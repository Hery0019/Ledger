#include "doctest.h"

#include <filesystem>
#include <memory>
#include <set>
#include <string>

#include "cli/database.h"
#include "core/uuid.h"
#include "core/value.h"
#include "exec/executor.h"
#include "storage/memory_engine.h"

using namespace ledger;
namespace fs = std::filesystem;

namespace {

// Engine + catalog + executor over MemoryEngine, like the executor tests.
struct Fixture {
    MemoryEngine engine;
    Catalog catalog;
    Executor exec{engine, catalog};

    Result<QueryResult> run(std::string_view sql) { return exec.execute(sql); }
    // Precondition: `sql` succeeds and returns rows.
    QueryResult rows(std::string_view sql) {
        auto r = run(sql);
        REQUIRE_MESSAGE(r.ok(), "unexpected error: " << (r.ok() ? "" : r.error().message));
        return std::move(r).value();
    }
};

constexpr std::string_view kA = "00000000-0000-4000-8000-00000000000a";
constexpr std::string_view kB = "00000000-0000-4000-8000-00000000000b";

}  // namespace

// ---- parse / format ------------------------------------------------------------

TEST_CASE("parseUuid / formatUuid round-trip, uppercase folded to canonical") {
    const auto id = parseUuid("550e8400-e29b-41d4-a716-446655440000");
    REQUIRE(id.ok());
    CHECK(formatUuid(id.value()) == "550e8400-e29b-41d4-a716-446655440000");

    const auto upper = parseUuid("550E8400-E29B-41D4-A716-446655440000");
    REQUIRE(upper.ok());
    CHECK(upper.value() == id.value());
    CHECK(formatUuid(upper.value()) == "550e8400-e29b-41d4-a716-446655440000");
}

TEST_CASE("parseUuid refuses anything but the canonical shape") {
    for (const auto* bad : {
             "",
             "550e8400e29b41d4a716446655440000",       // no hyphens
             "550e8400-e29b-41d4-a716-44665544000",    // too short
             "550e8400-e29b-41d4-a716-4466554400000",  // too long
             "550e8400-e29b-41d4-a716_446655440000",   // wrong separator
             "550e8400-e29b-41d4-a716-44665544000g",   // not hex
             "-50e8400-e29b-41d4-a716-446655440000",   // hyphen out of place
         }) {
        const auto r = parseUuid(bad);
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK(r.error().code == ErrorCode::TypeError);
    }
}

// ---- generation ----------------------------------------------------------------

TEST_CASE("generateUuidV4 sets the version and variant bits, and does not repeat") {
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        const Uuid id = generateUuidV4();
        CHECK((id.bytes[6] >> 4) == 0x4);          // version 4
        CHECK((id.bytes[8] & 0xc0) == 0x80);       // variant 10xx
        CHECK(seen.insert(formatUuid(id)).second);  // all distinct
        // The canonical form parses back to the same bytes.
        CHECK(parseUuid(formatUuid(id)).value() == id);
    }
}

// ---- Value integration ------------------------------------------------------------

TEST_CASE("Value carries UUIDs like any other type") {
    const Uuid raw = parseUuid("550e8400-e29b-41d4-a716-446655440000").value();
    const Value v = Value::uuid(raw);
    CHECK(v.type() == DataType::Uuid);
    CHECK(v.asUuid() == raw);
    CHECK(v.toText() == "550e8400-e29b-41d4-a716-446655440000");
    CHECK(Value::fromText(DataType::Uuid, v.toText()).value() == v);
    CHECK_FALSE(Value::fromText(DataType::Uuid, "not-a-uuid").ok());
}

TEST_CASE("UUID comparison: bytewise order, NULL unknown, other types refused") {
    const Value a = Value::fromText(DataType::Uuid, "00000000-0000-4000-8000-000000000001").value();
    const Value b = Value::fromText(DataType::Uuid, "ffffffff-ffff-4fff-bfff-ffffffffffff").value();
    CHECK(Value::compare(a, b).value() == Ordering::Less);
    CHECK(Value::compare(b, a).value() == Ordering::Greater);
    CHECK(Value::compare(a, a).value() == Ordering::Equal);
    CHECK(Value::compare(a, Value::null()).value() == Ordering::Unknown);
    CHECK_FALSE(Value::compare(a, Value::integer(1)).ok());
    CHECK_FALSE(Value::compare(a, Value::text("00000000-0000-4000-8000-000000000001")).ok());
}

// ---- SQL surface ---------------------------------------------------------------

TEST_CASE("UUID columns: explicit text literals convert at bind time") {
    Fixture f;
    REQUIRE(f.run("CREATE TABLE t (id UUID PRIMARY KEY, tag TEXT)").ok());
    REQUIRE(f.run("INSERT INTO t VALUES ('00000000-0000-4000-8000-00000000000a', 'a')").ok());
    // Uppercase input, canonical lowercase out.
    REQUIRE(f.run("INSERT INTO t VALUES ('00000000-0000-4000-8000-00000000000B', 'b')").ok());
    const auto r = f.rows("SELECT id FROM t ORDER BY id");
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0][0].toText() == "00000000-0000-4000-8000-00000000000a");
    CHECK(r.rows[1][0].toText() == "00000000-0000-4000-8000-00000000000b");

    // A malformed literal fails at bind, before any write.
    auto bad = f.run("INSERT INTO t VALUES ('not-a-uuid', 'x')");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::TypeError);
    CHECK(f.rows("SELECT COUNT(*) AS n FROM t").rows[0][0].asInt() == 2);
}

TEST_CASE("a UUID PRIMARY KEY generates a fresh v4 when omitted or NULL") {
    Fixture f;
    REQUIRE(f.run("CREATE TABLE t (id UUID PRIMARY KEY, n INT)").ok());
    REQUIRE(f.run("INSERT INTO t (n) VALUES (1)").ok());
    REQUIRE(f.run("INSERT INTO t VALUES (NULL, 2)").ok());
    const auto r = f.rows("SELECT id, n FROM t ORDER BY n");
    REQUIRE(r.rows.size() == 2);
    for (const auto& row : r.rows) {
        CHECK(row[0].type() == DataType::Uuid);
        CHECK((row[0].asUuid().bytes[6] >> 4) == 0x4);  // version 4
    }
    CHECK_FALSE(r.rows[0][0] == r.rows[1][0]);  // distinct keys
}

TEST_CASE("UUID comparisons, IN and BETWEEN accept text constants") {
    Fixture f;
    REQUIRE(f.run("CREATE TABLE t (id UUID PRIMARY KEY, n INT)").ok());
    REQUIRE(f.run(std::string("INSERT INTO t VALUES ('") + std::string(kA) + "', 1)").ok());
    REQUIRE(f.run(std::string("INSERT INTO t VALUES ('") + std::string(kB) + "', 2)").ok());

    CHECK(f.rows(std::string("SELECT n FROM t WHERE id = '") + std::string(kA) + "'").rows.size() == 1);
    CHECK(f.rows(std::string("SELECT n FROM t WHERE '") + std::string(kB) + "' = id").rows.size() == 1);
    CHECK(f.rows(std::string("SELECT n FROM t WHERE id IN ('") + std::string(kA) + "', '" +
                 std::string(kB) + "')").rows.size() == 2);
    CHECK(f.rows(std::string("SELECT n FROM t WHERE id BETWEEN '") + std::string(kA) + "' AND '" +
                 std::string(kB) + "'").rows.size() == 2);

    // The coercion only applies to constants that parse: garbage stays TEXT
    // and the comparison is refused at bind time.
    auto bad = f.run("SELECT n FROM t WHERE id = 'nope'");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::TypeError);
    // A TEXT column never silently compares with a UUID column.
    REQUIRE(f.run("CREATE TABLE s (label TEXT)").ok());
    CHECK_FALSE(f.run("SELECT * FROM t JOIN s ON t.id = s.label").ok());
}

TEST_CASE("UUID keys go through the PK index and foreign keys") {
    Fixture f;
    REQUIRE(f.run("CREATE TABLE parent (id UUID PRIMARY KEY)").ok());
    REQUIRE(f.run(std::string("INSERT INTO parent VALUES ('") + std::string(kA) + "')").ok());
    // Duplicate key refused (through the index).
    CHECK(f.run(std::string("INSERT INTO parent VALUES ('") + std::string(kA) + "')").error().code ==
          ErrorCode::ConstraintViolation);
    // REFERENCES a UUID parent works, and blocks a missing key.
    REQUIRE(f.run("CREATE TABLE child (id INT PRIMARY KEY, pid UUID REFERENCES parent(id))").ok());
    CHECK(f.run(std::string("INSERT INTO child VALUES (1, '") + std::string(kA) + "')").ok());
    CHECK(f.run(std::string("INSERT INTO child VALUES (2, '") + std::string(kB) + "')").error().code ==
          ErrorCode::ConstraintViolation);
}

TEST_CASE("UUID restrictions: AUTOINCREMENT, DEFAULT on the key, arithmetic, SUM") {
    Fixture f;
    CHECK_FALSE(f.run("CREATE TABLE t (id UUID PRIMARY KEY AUTOINCREMENT)").ok());
    CHECK_FALSE(f.run(std::string("CREATE TABLE t (id UUID PRIMARY KEY DEFAULT '") +
                      std::string(kA) + "')").ok());
    REQUIRE(f.run("CREATE TABLE t (id UUID PRIMARY KEY, n INT)").ok());
    CHECK_FALSE(f.run("SELECT id + 1 FROM t").ok());
    CHECK_FALSE(f.run("SELECT SUM(id) FROM t").ok());
    // MIN/MAX follow the bytewise order and stay allowed.
    REQUIRE(f.run(std::string("INSERT INTO t VALUES ('") + std::string(kB) + "', 1)").ok());
    REQUIRE(f.run(std::string("INSERT INTO t VALUES ('") + std::string(kA) + "', 2)").ok());
    CHECK(f.rows("SELECT MIN(id) AS lo FROM t").rows[0][0].toText() == kA);
}

TEST_CASE("a plain UUID column (not a key) stays optional and un-generated") {
    Fixture f;
    REQUIRE(f.run("CREATE TABLE t (id INT PRIMARY KEY, ref UUID)").ok());
    REQUIRE(f.run("INSERT INTO t (id) VALUES (1)").ok());
    CHECK(f.rows("SELECT ref FROM t").rows[0][0].isNull());
    REQUIRE(f.run(std::string("UPDATE t SET ref = '") + std::string(kA) + "' WHERE id = 1").ok());
    CHECK(f.rows("SELECT ref FROM t").rows[0][0].toText() == kA);
}

TEST_CASE("UUID rows and schema survive a database reopen") {
    const fs::path dir = fs::temp_directory_path() / "ledger_test_uuid_persist";
    fs::remove_all(dir);
    std::string generated;
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("CREATE TABLE t (id UUID PRIMARY KEY, n INT)").ok());
        REQUIRE(db->execute("INSERT INTO t (n) VALUES (1)").ok());
        generated = db->execute("SELECT id FROM t").value().rows[0][0].toText();
    }
    {
        auto db = Database::open(dir).value();
        const auto r = db->execute(std::string("SELECT n FROM t WHERE id = '") + generated + "'");
        REQUIRE(r.ok());
        REQUIRE(r.value().rows.size() == 1);
        CHECK(r.value().rows[0][0].asInt() == 1);
    }
    fs::remove_all(dir);
}

// ---- generated keys are reported -------------------------------------------------

TEST_CASE("INSERT reports the key it generated (UUID and AUTOINCREMENT alike)") {
    Fixture f;
    REQUIRE(f.run("CREATE TABLE u (id UUID PRIMARY KEY, n INT)").ok());
    const auto gen = f.run("INSERT INTO u (n) VALUES (1)");
    REQUIRE(gen.ok());
    REQUIRE(gen.value().key.has_value());
    CHECK(gen.value().key->type() == DataType::Uuid);
    // The reported key is the row's actual key.
    CHECK(f.rows(std::string("SELECT n FROM u WHERE id = '") + gen.value().key->toText() + "'")
              .rows.size() == 1);

    REQUIRE(f.run("CREATE TABLE a (id INT PRIMARY KEY AUTOINCREMENT, n INT)").ok());
    const auto next = f.run("INSERT INTO a (n) VALUES (1)");
    REQUIRE(next.ok());
    REQUIRE(next.value().key.has_value());
    CHECK(next.value().key->asInt() == 1);

    // An explicit key is the caller's own: nothing to report.
    const auto explicitKey = f.run(std::string("INSERT INTO u VALUES ('") + std::string(kA) + "', 2)");
    REQUIRE(explicitKey.ok());
    CHECK_FALSE(explicitKey.value().key.has_value());
    CHECK_FALSE(f.run("UPDATE u SET n = 3").value().key.has_value());
}
