#include "doctest.h"

#include <filesystem>
#include <string>
#include <vector>

#include "cli/database.h"
#include "exec/executor.h"
#include "sql/parser.h"
#include "storage/codec.h"
#include "storage/memory_engine.h"

using namespace ledger;

// Column constraints: DEFAULT, UNIQUE, CHECK, REFERENCES, AUTOINCREMENT.
namespace {

struct Db {
    MemoryEngine engine;
    Catalog catalog;
    Executor exec{engine, catalog};

    QueryResult run(std::string_view sql) {
        auto r = exec.execute(sql);
        REQUIRE_MESSAGE(r.ok(), "unexpected error for [" << sql << "]: "
                                    << (r.ok() ? "" : r.error().message));
        return std::move(r).value();
    }
    Error fail(std::string_view sql) {
        auto r = exec.execute(sql);
        REQUIRE_MESSAGE(!r.ok(), "expected an error for [" << sql << "]");
        return r.error();
    }
    std::vector<Row> rows(std::string_view sql) { return run(sql).rows; }
};

Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }
Value f(double d) { return Value::real(d).value(); }
Value b(bool v) { return Value::boolean(v); }
const Value N = Value::null();

}  // namespace

// ---- DEFAULT ---------------------------------------------------------------

TEST_CASE("DEFAULT fills omitted columns on INSERT") {
    Db db;
    db.run("CREATE TABLE t (id INT PRIMARY KEY, status TEXT DEFAULT 'new', qty INT DEFAULT 1 + 1, "
           "price FLOAT DEFAULT 5, flag BOOL DEFAULT TRUE, note TEXT DEFAULT NULL, plain TEXT)");
    db.run("INSERT INTO t (id) VALUES (1)");
    db.run("INSERT INTO t (id, status, qty) VALUES (2, 'old', 9)");
    db.run("INSERT INTO t VALUES (3, 'x', 0, 1.5, FALSE, 'n', 'p')");  // explicit values win
    const auto r = db.rows("SELECT * FROM t ORDER BY id");
    REQUIRE(r.size() == 3);
    CHECK(r[0] == Row{i(1), t("new"), i(2), f(5.0), b(true), N, N});  // 1 + 1 folded, 5 -> 5.0
    CHECK(r[1] == Row{i(2), t("old"), i(9), f(5.0), b(true), N, N});
    CHECK(r[2] == Row{i(3), t("x"), i(0), f(1.5), b(false), t("n"), t("p")});
    CHECK(db.catalog.find("t")->columns[1].defaultValue == t("new"));
    CHECK_FALSE(db.catalog.find("t")->columns[6].defaultValue.has_value());
}

TEST_CASE("DEFAULT combines with NOT NULL and PRIMARY KEY") {
    Db db;
    db.run("CREATE TABLE t (id INT PRIMARY KEY, name TEXT NOT NULL DEFAULT 'anon', n INT DEFAULT 0 NOT NULL)");
    db.run("INSERT INTO t (id) VALUES (1)");
    CHECK(db.rows("SELECT name, n FROM t")[0] == Row{t("anon"), i(0)});
    // An explicit NULL is still refused: DEFAULT only applies to omitted columns.
    CHECK(db.fail("INSERT INTO t (id, name) VALUES (2, NULL)").code == ErrorCode::ConstraintViolation);
}

TEST_CASE("DEFAULT errors at CREATE TABLE") {
    Db db;
    auto e = db.fail("CREATE TABLE t (a INT DEFAULT 'x')");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:31: column 'a' expects INT, got TEXT");
    e = db.fail("CREATE TABLE t (a INT NOT NULL DEFAULT NULL)");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "1:40: column 'a' cannot be NULL");
    e = db.fail("CREATE TABLE t (a INT DEFAULT b)");
    CHECK(e.message == "1:31: column reference 'b' is not allowed here");
    e = db.fail("CREATE TABLE t (a INT DEFAULT 1 DEFAULT 2)");
    CHECK(e.message == "1:33: expected a single DEFAULT, got 'DEFAULT'");
    e = db.fail("CREATE TABLE t (a INT DEFAULT 1 / 0)");
    CHECK(e.message == "1:31: division by zero");
    CHECK(db.fail("CREATE TABLE t (a INT DEFAULT count(*))").message ==
          "1:31: aggregate function 'count()' is not allowed here");
}

// ---- UNIQUE ----------------------------------------------------------------

TEST_CASE("UNIQUE refuses duplicate non-NULL values on INSERT and UPDATE, allows NULLs") {
    Db db;
    db.run("CREATE TABLE u (id INT PRIMARY KEY, email TEXT UNIQUE, code INT UNIQUE NOT NULL DEFAULT 0)");
    db.run("INSERT INTO u VALUES (1, 'a@x', 1)");
    db.run("INSERT INTO u VALUES (2, 'b@x', 2)");
    db.run("INSERT INTO u VALUES (3, NULL, 3)");
    db.run("INSERT INTO u VALUES (4, NULL, 4)");  // several NULLs are fine
    auto e = db.fail("INSERT INTO u VALUES (5, 'a@x', 5)");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "duplicate value a@x for UNIQUE column 'email' in table 'u'");
    CHECK(db.fail("INSERT INTO u VALUES (5, 'c@x', 2)").message ==
          "duplicate value 2 for UNIQUE column 'code' in table 'u'");
    db.run("INSERT INTO u (id, email) VALUES (5, 'c@x')");  // code takes DEFAULT 0
    CHECK(db.fail("INSERT INTO u (id, email) VALUES (6, 'd@x')").message ==
          "duplicate value 0 for UNIQUE column 'code' in table 'u'");  // the DEFAULT collides too
    CHECK(db.rows("SELECT * FROM u").size() == 5);

    // UPDATE: against other rows, against rows modified in the same statement,
    // and a row keeping its own value is fine.
    db.run("UPDATE u SET email = 'a@x' WHERE id = 1");
    CHECK(db.fail("UPDATE u SET email = 'a@x' WHERE id = 2").message ==
          "duplicate value a@x for UNIQUE column 'email' in table 'u'");
    CHECK(db.fail("UPDATE u SET code = 9 WHERE id > 2").message ==
          "duplicate value 9 for UNIQUE column 'code' in table 'u'");
    db.run("UPDATE u SET email = NULL WHERE id IN (1, 2)");  // NULLs never collide
    db.run("UPDATE u SET code = code + 10");                 // all distinct, all move at once
    CHECK(db.rows("SELECT * FROM u WHERE code = 11").size() == 1);
    CHECK(db.catalog.find("u")->columns[1].unique);
}

TEST_CASE("UNIQUE columns are indexed: point lookups and reopen") {
    Db db;
    db.run("CREATE TABLE u (id INT PRIMARY KEY, email TEXT UNIQUE)");
    db.run("INSERT INTO u VALUES (1, 'a@x')");
    CHECK(db.engine.indexed("u", 1));
    CHECK(db.engine.lookup("u", 1, t("a@x")).value()->first == 1);
    CHECK(db.rows("SELECT id FROM u WHERE email = 'a@x'")[0] == Row{i(1)});
    CHECK(db.rows("SELECT id FROM u WHERE 'zz' = email").empty());

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_unique";
    fs::remove_all(dir);
    {
        auto d = Database::open(dir).value();
        REQUIRE(d->execute("CREATE TABLE u (id INT PRIMARY KEY, email TEXT UNIQUE)").ok());
        REQUIRE(d->execute("INSERT INTO u VALUES (1, 'a@x')").ok());
    }
    {
        auto d = Database::open(dir).value();
        CHECK(d->catalog().find("u")->columns[1].unique);
        CHECK(d->execute("INSERT INTO u VALUES (2, 'a@x')").error().code == ErrorCode::ConstraintViolation);
        REQUIRE(d->execute("INSERT INTO u VALUES (2, 'b@x')").ok());
    }
    fs::remove_all(dir);
    CHECK(codec::encodeSchema(TableSchema{"u", {ColumnSchema{"e", DataType::Text, false, true, std::nullopt, true}}}) ==
          "ledger-schema 1\ne TEXT NN UQ\n");
    CHECK(db.fail("CREATE TABLE v (a INT UNIQUE UNIQUE)").message == "1:30: expected a single UNIQUE constraint, got 'UNIQUE'");
}

TEST_CASE("DEFAULT is persisted in schema.txt and survives reopen") {
    using namespace ledger::codec;
    TableSchema s{"t", {ColumnSchema{"a", DataType::Text, false, false, t("two words\tand\\tab")},
                        ColumnSchema{"b", DataType::Int, false, true, i(-3)},
                        ColumnSchema{"c", DataType::Float, false, false, N},
                        ColumnSchema{"d", DataType::Bool, true, true, std::nullopt}}};
    const std::string encoded = encodeSchema(s);
    CHECK(encoded == "ledger-schema 1\n"
                     "a TEXT DEF:two\\swords\\tand\\\\tab\n"
                     "b INT NN DEF:-3\n"
                     "c FLOAT DEFNULL\n"
                     "d BOOL PK\n");
    const auto back = decodeSchema("t", encoded).value();
    CHECK(back.columns[0].defaultValue == t("two words\tand\\tab"));
    CHECK(back.columns[1].defaultValue == i(-3));
    CHECK(back.columns[2].defaultValue == N);
    CHECK_FALSE(back.columns[3].defaultValue.has_value());
    // A TEXT default that reads "NULL" stays text.
    const auto weird = decodeSchema("t", "ledger-schema 1\na TEXT DEF:NULL\nb TEXT DEF:\\\\N\n").value();
    CHECK(weird.columns[0].defaultValue == t("NULL"));
    CHECK(weird.columns[1].defaultValue == t("\\N"));
    CHECK(decodeSchema("t", "ledger-schema 1\na INT DEF:x\n").error().code == ErrorCode::Corruption);
    CHECK(decodeSchema("t", "ledger-schema 1\na INT NN DEF:1 FOO\n").error().message ==
          "schema.txt:2: unknown constraint 'FOO'");

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_defaults";
    fs::remove_all(dir);
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("CREATE TABLE t (id INT PRIMARY KEY, tag TEXT DEFAULT 'a b', n INT DEFAULT 7)").ok());
    }
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("INSERT INTO t (id) VALUES (1)").ok());
        CHECK(db->execute("SELECT tag, n FROM t").value().rows[0] == Row{t("a b"), i(7)});
    }
    fs::remove_all(dir);
}
