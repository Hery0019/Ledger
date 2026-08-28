#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli/database.h"
#include "exec/executor.h"
#include "storage/file_engine.h"
#include "storage/memory_engine.h"
#include "storage/pk_index.h"

using namespace ledger;

// The primary-key index: maintained by the engines, used by the executor.
namespace {

TableSchema schema() {
    return TableSchema{"t",
                       {ColumnSchema{"id", DataType::Int, true, true},
                        ColumnSchema{"name", DataType::Text, false, false}}};
}

Row row(std::int64_t id, const char* name) { return Row{Value::integer(id), Value::text(name)}; }
Value i(std::int64_t v) { return Value::integer(v); }

// Wraps an engine and counts scans, so tests can prove the index was used.
class CountingEngine final : public IStorageEngine {
public:
    explicit CountingEngine(IStorageEngine& inner) : inner_(inner) {}
    std::size_t scans = 0;
    std::size_t lookups = 0;

    Result<void> createTable(const TableSchema& s) override { return inner_.createTable(s); }
    Result<void> dropTable(std::string_view t) override { return inner_.dropTable(t); }
    Result<RowId> insert(std::string_view t, const Row& r) override { return inner_.insert(t, r); }
    Result<void> scan(std::string_view t, const std::function<bool(RowId, const Row&)>& v) override {
        ++scans;
        return inner_.scan(t, v);
    }
    Result<void> update(std::string_view t, RowId id, const Row& r) override { return inner_.update(t, id, r); }
    Result<void> remove(std::string_view t, RowId id) override { return inner_.remove(t, id); }
    Result<void> restore(std::string_view t, RowId id, const Row& r) override { return inner_.restore(t, id, r); }
    bool indexed(std::string_view t, std::size_t c) const noexcept override { return inner_.indexed(t, c); }
    Result<std::optional<std::pair<RowId, Row>>> lookup(std::string_view t, std::size_t c, const Value& k) override {
        ++lookups;
        return inner_.lookup(t, c, k);
    }
    Result<std::vector<std::pair<RowId, Row>>> lookupAll(std::string_view t, std::size_t c, const Value& k) override {
        ++lookups;
        return inner_.lookupAll(t, c, k);
    }
    Result<std::optional<Value>> maxKey(std::string_view t, std::size_t c) override { return inner_.maxKey(t, c); }
    Result<void> compact(std::string_view t) override { return inner_.compact(t); }
    Result<std::vector<TableSchema>> loadSchemas() override { return inner_.loadSchemas(); }
    Result<void> saveViews(const std::vector<ViewDef>& v) override { return inner_.saveViews(v); }
    Result<std::vector<ViewDef>> loadViews() override { return inner_.loadViews(); }
    Result<void> saveUsers(const std::vector<UserDef>& u) override { return inner_.saveUsers(u); }
    Result<std::vector<UserDef>> loadUsers() override { return inner_.loadUsers(); }
    Result<void> createIndex(std::string_view t, std::size_t c) override { return inner_.createIndex(t, c); }
    Result<void> dropIndex(std::string_view t, std::size_t c) override { return inner_.dropIndex(t, c); }
    Result<void> saveIndexes(const std::vector<IndexDef>& d) override { return inner_.saveIndexes(d); }
    Result<std::vector<IndexDef>> loadIndexes() override { return inner_.loadIndexes(); }

private:
    IStorageEngine& inner_;
};

}  // namespace

// ---- ColumnIndex / TableIndexes --------------------------------------------

TEST_CASE("TableIndexes tracks add, replace and remove; no index without a PRIMARY KEY or UNIQUE") {
    TableIndexes idx(schema());
    REQUIRE(idx.has(0));
    CHECK_FALSE(idx.has(1));
    idx.add(10, row(1, "a"));
    idx.add(11, row(2, "b"));
    CHECK(idx.on(0)->find(i(1)) == 10);
    CHECK(idx.on(0)->find(i(2)) == 11);
    CHECK_FALSE(idx.on(0)->find(i(3)).has_value());
    CHECK_FALSE(idx.on(0)->find(Value::null()).has_value());
    CHECK(idx.on(0)->maxKey() == i(2));
    idx.replace(10, row(1, "a"), row(5, "a"));  // key changed
    CHECK_FALSE(idx.on(0)->find(i(1)).has_value());
    CHECK(idx.on(0)->find(i(5)) == 10);
    idx.remove(11, row(2, "b"));
    CHECK_FALSE(idx.on(0)->find(i(2)).has_value());
    // Int key looked up with a Float value: numeric comparison.
    CHECK(idx.on(0)->find(Value::real(5.0).value()) == 10);

    TableIndexes none(TableSchema{"u", {ColumnSchema{"a", DataType::Int, false, false}}});
    CHECK_FALSE(none.has(0));
    none.add(1, Row{i(1)});
    CHECK(none.on(0) == nullptr);

    // A UNIQUE column is indexed too, and NULL keys are skipped.
    TableIndexes uq(TableSchema{"v", {ColumnSchema{"a", DataType::Int, true, true},
                                       ColumnSchema{"e", DataType::Text, false, false, std::nullopt, true}}});
    REQUIRE(uq.has(1));
    uq.add(1, Row{i(1), Value::text("x")});
    uq.add(2, Row{i(2), Value::null()});
    CHECK(uq.on(1)->find(Value::text("x")) == 1);
    CHECK(uq.on(0)->find(i(2)) == 2);
    CHECK_FALSE(uq.on(1)->maxKey() == Value::null());
}

TEST_CASE("a non-unique index keeps every duplicate and removes one entry at a time") {
    TableIndexes idx(TableSchema{"u", {ColumnSchema{"a", DataType::Int, false, false}}});
    REQUIRE(idx.addIndex(0));
    CHECK_FALSE(idx.addIndex(0));  // one index per column
    CHECK_FALSE(idx.on(0)->unique());

    idx.add(10, Row{i(5)});
    idx.add(11, Row{i(5)});
    idx.add(12, Row{i(7)});
    CHECK(idx.on(0)->findAll(i(5)) == std::vector<RowId>{10, 11});
    CHECK(idx.on(0)->findAll(i(7)) == std::vector<RowId>{12});
    CHECK(idx.on(0)->findAll(i(9)).empty());
    CHECK(idx.on(0)->findAll(Value::null()).empty());

    // Removing one duplicate leaves the other in place.
    idx.remove(10, Row{i(5)});
    CHECK(idx.on(0)->findAll(i(5)) == std::vector<RowId>{11});

    // A schema-born unique index cannot be dropped; a user one can.
    TableIndexes pk(schema());
    CHECK_FALSE(pk.removeIndex(0));
    CHECK(idx.removeIndex(0));
    CHECK_FALSE(idx.has(0));
}

// ---- engines ---------------------------------------------------------------

TEST_CASE("MemoryEngine keeps the index in step with every write") {
    MemoryEngine e;
    REQUIRE(e.createTable(schema()).ok());
    CHECK(e.indexed("t", 0));
    CHECK_FALSE(e.indexed("t", 1));
    const RowId a = e.insert("t", row(1, "a")).value();
    const RowId b = e.insert("t", row(2, "b")).value();
    auto hit = e.lookup("t", 0, i(2)).value();
    REQUIRE(hit.has_value());
    CHECK(hit->first == b);
    CHECK(hit->second == row(2, "b"));
    REQUIRE(e.update("t", a, row(7, "a")).ok());
    CHECK_FALSE(e.lookup("t", 0, i(1)).value().has_value());
    CHECK(e.lookup("t", 0, i(7)).value()->first == a);
    REQUIRE(e.remove("t", b).ok());
    CHECK_FALSE(e.lookup("t", 0, i(2)).value().has_value());
    REQUIRE(e.restore("t", b, row(2, "b")).ok());
    CHECK(e.lookup("t", 0, i(2)).value()->first == b);
    CHECK(e.lookup("t", 1, i(2)).error().code == ErrorCode::Internal);
}

TEST_CASE("FileEngine rebuilds the index from the replayed rows") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_index";
    fs::remove_all(dir);
    {
        auto e = FileEngine::open(dir).value();
        REQUIRE(e->createTable(schema()).ok());
        REQUIRE(e->insert("t", row(1, "a")).ok());
        const RowId b = e->insert("t", row(2, "b")).value();
        REQUIRE(e->insert("t", row(3, "c")).ok());
        REQUIRE(e->update("t", b, row(20, "b")).ok());   // tombstone + new version
        REQUIRE(e->remove("t", 1).ok());                  // tombstone
        CHECK(e->lookup("t", 0, i(20)).value()->first == b);
    }
    {
        auto e = FileEngine::open(dir).value();
        CHECK(e->indexed("t", 0));
        CHECK_FALSE(e->lookup("t", 0, i(1)).value().has_value());   // deleted
        CHECK_FALSE(e->lookup("t", 0, i(2)).value().has_value());   // old key of the updated row
        CHECK(e->lookup("t", 0, i(20)).value()->second == row(20, "b"));
        CHECK(e->lookup("t", 0, i(3)).value()->second == row(3, "c"));
        REQUIRE(e->compact("t").ok());
        CHECK(e->lookup("t", 0, i(3)).value()->second == row(3, "c"));
    }
    fs::remove_all(dir);
}

// ---- executor --------------------------------------------------------------

TEST_CASE("the executor answers WHERE pk = value and PK uniqueness through the index") {
    MemoryEngine inner;
    CountingEngine engine(inner);
    Catalog catalog;
    Executor exec(engine, catalog);
    auto run = [&](std::string_view sql) {
        auto r = exec.execute(sql);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        return std::move(r).value();
    };
    run("CREATE TABLE t (id INT PRIMARY KEY, name TEXT)");
    run("CREATE TABLE u (id INT, name TEXT)");  // no PK: always scanned
    for (int k = 1; k <= 5; ++k) {
        run("INSERT INTO t VALUES (" + std::to_string(k) + ", 'n')");
        run("INSERT INTO u VALUES (" + std::to_string(k) + ", 'n')");
    }
    CHECK(engine.scans == 0);      // PK checks used lookups, never a scan
    CHECK(engine.lookups == 5);

    engine.scans = engine.lookups = 0;
    auto r = run("SELECT name FROM t WHERE id = 3");
    CHECK(r.rows.size() == 1);
    CHECK(engine.scans == 0);
    CHECK(engine.lookups == 1);
    r = run("SELECT * FROM t WHERE 4 = id");
    CHECK(r.rows[0] == row(4, "n"));
    CHECK(engine.scans == 0);
    CHECK(run("SELECT * FROM t WHERE id = 99").rows.empty());
    CHECK(run("SELECT * FROM t WHERE id = 2.0").rows.size() == 1);  // numeric key match
    CHECK(engine.scans == 0);

    // Anything else still scans: other columns, ranges, tables without PK,
    // aggregates, joins.
    engine.scans = 0;
    run("SELECT * FROM t WHERE name = 'n'");
    run("SELECT * FROM t WHERE id > 3");
    run("SELECT * FROM u WHERE id = 3");
    run("SELECT count(*) FROM t WHERE id = 3");
    CHECK(engine.scans == 4);

    // UPDATE / DELETE go through filter(): still a scan, but the PK check of
    // UPDATE uses the index.
    engine.scans = engine.lookups = 0;
    run("UPDATE t SET id = 10 WHERE id = 1");
    CHECK(engine.lookups == 1);
    auto e = exec.execute("UPDATE t SET id = 10 WHERE id = 2");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().code == ErrorCode::ConstraintViolation);
    // Rows stay consistent after the index-guided operations.
    CHECK(run("SELECT id FROM t ORDER BY id").rows.size() == 5);
    CHECK(run("SELECT id FROM t WHERE id = 10").rows.size() == 1);
}

// ---- CREATE INDEX / DROP INDEX -----------------------------------------------

namespace {

// Engine + catalog + executor, with the engine wrapped to count accesses.
struct IndexFixture {
    MemoryEngine inner;
    CountingEngine engine{inner};
    Catalog catalog;
    Executor exec{engine, catalog};

    Result<QueryResult> run(std::string_view sql) { return exec.execute(sql); }
    QueryResult rows(std::string_view sql) {
        auto r = run(sql);
        REQUIRE_MESSAGE(r.ok(), "unexpected error: " << (r.ok() ? "" : r.error().message));
        return std::move(r).value();
    }
};

}  // namespace

TEST_CASE("CREATE INDEX turns an equality scan into a lookup, duplicates included") {
    IndexFixture f;
    REQUIRE(f.run("CREATE TABLE t (id INT PRIMARY KEY, score INT)").ok());
    for (int i = 1; i <= 6; ++i) {
        REQUIRE(f.run("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 3) + ")").ok());
    }
    // Without an index: a scan answers.
    f.engine.scans = f.engine.lookups = 0;
    CHECK(f.rows("SELECT id FROM t WHERE score = 1").rows.size() == 2);
    CHECK(f.engine.scans == 1);
    CHECK(f.engine.lookups == 0);

    REQUIRE(f.run("CREATE INDEX idx_score ON t (score)").ok());
    f.engine.scans = f.engine.lookups = 0;
    const auto hits = f.rows("SELECT id FROM t WHERE score = 1 ORDER BY id");
    REQUIRE(hits.rows.size() == 2);
    CHECK(hits.rows[0][0].asInt() == 1);
    CHECK(hits.rows[1][0].asInt() == 4);
    CHECK(f.engine.scans == 0);
    CHECK(f.engine.lookups == 1);

    // Writes keep the index in step.
    REQUIRE(f.run("INSERT INTO t VALUES (7, 1)").ok());
    REQUIRE(f.run("UPDATE t SET score = 1 WHERE id = 2").ok());
    REQUIRE(f.run("DELETE FROM t WHERE id = 4").ok());
    f.engine.scans = f.engine.lookups = 0;
    CHECK(f.rows("SELECT id FROM t WHERE score = 1").rows.size() == 3);  // 1, 2, 7
    CHECK(f.engine.scans == 0);

    // DROP INDEX goes back to scanning.
    REQUIRE(f.run("DROP INDEX idx_score").ok());
    f.engine.scans = f.engine.lookups = 0;
    CHECK(f.rows("SELECT id FROM t WHERE score = 1").rows.size() == 3);
    CHECK(f.engine.scans == 1);
    CHECK(f.engine.lookups == 0);
}

TEST_CASE("CREATE INDEX / DROP INDEX validation") {
    IndexFixture f;
    REQUIRE(f.run("CREATE TABLE t (id INT PRIMARY KEY, tag TEXT UNIQUE, score INT)").ok());
    REQUIRE(f.run("CREATE VIEW v AS SELECT score FROM t").ok());

    CHECK(f.run("CREATE INDEX i ON missing (score)").error().code == ErrorCode::NotFound);
    CHECK(f.run("CREATE INDEX i ON t (missing)").error().code == ErrorCode::NotFound);
    CHECK(f.run("CREATE INDEX i ON v (score)").error().code == ErrorCode::SyntaxError);
    CHECK(f.run("CREATE INDEX i ON t (id)").error().code == ErrorCode::AlreadyExists);   // PRIMARY KEY
    CHECK(f.run("CREATE INDEX i ON t (tag)").error().code == ErrorCode::AlreadyExists);  // UNIQUE

    REQUIRE(f.run("CREATE INDEX i ON t (score)").ok());
    CHECK(f.run("CREATE INDEX i ON t (score)").error().code == ErrorCode::AlreadyExists);   // name taken
    CHECK(f.run("CREATE INDEX i2 ON t (score)").error().code == ErrorCode::AlreadyExists);  // column taken
    CHECK(f.run("DROP INDEX missing").error().code == ErrorCode::NotFound);

    // Refused inside a transaction, like the other DDL.
    REQUIRE(f.run("BEGIN").ok());
    CHECK_FALSE(f.run("CREATE INDEX i3 ON t (score)").ok());
    CHECK_FALSE(f.run("DROP INDEX i").ok());
    REQUIRE(f.run("ROLLBACK").ok());

    // Parse errors.
    CHECK_FALSE(f.run("CREATE INDEX i ON t (a, b)").ok());  // multi-column
    CHECK_FALSE(f.run("CREATE INDEX i ON t").ok());
    CHECK_FALSE(f.run("CREATE INDEX ON t (a)").ok());
}

TEST_CASE("DROP TABLE takes its indexes along") {
    IndexFixture f;
    REQUIRE(f.run("CREATE TABLE t (id INT PRIMARY KEY, score INT)").ok());
    REQUIRE(f.run("CREATE INDEX idx_score ON t (score)").ok());
    REQUIRE(f.run("DROP TABLE t").ok());
    CHECK(f.catalog.findIndex("idx_score") == nullptr);
    CHECK(f.inner.loadIndexes().value().empty());
    // The name is free again.
    REQUIRE(f.run("CREATE TABLE t (id INT PRIMARY KEY, score INT)").ok());
    CHECK(f.run("CREATE INDEX idx_score ON t (score)").ok());
}

TEST_CASE("user indexes survive a database reopen and are rebuilt from the rows") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_index_persist";
    fs::remove_all(dir);
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("CREATE TABLE t (id INT PRIMARY KEY, score INT)").ok());
        REQUIRE(db->execute("INSERT INTO t VALUES (1, 5)").ok());
        REQUIRE(db->execute("INSERT INTO t VALUES (2, 5)").ok());
        REQUIRE(db->execute("CREATE INDEX idx_score ON t (score)").ok());
    }
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->catalog().findIndex("idx_score") != nullptr);
        const auto r = db->execute("SELECT id FROM t WHERE score = 5");
        REQUIRE(r.ok());
        CHECK(r.value().rows.size() == 2);
        REQUIRE(db->execute("DROP INDEX idx_score").ok());
    }
    {
        auto db = Database::open(dir).value();
        CHECK(db->catalog().indexes().empty());
    }
    fs::remove_all(dir);
}

TEST_CASE("a stale indexes.txt declaration is skipped with a warning") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_index_stale";
    fs::remove_all(dir);
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("CREATE TABLE t (id INT PRIMARY KEY)").ok());
    }
    {
        std::ofstream(dir / "indexes.txt") << "ledger-indexes 1\nghost\tgone\tscore\n";
    }
    {
        auto db = Database::open(dir).value();
        CHECK(db->catalog().indexes().empty());
        const auto warnings = db->takeWarnings();
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].find("ghost") != std::string::npos);
    }
    fs::remove_all(dir);
}
