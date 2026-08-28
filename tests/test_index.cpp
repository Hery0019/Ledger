#include "doctest.h"

#include <filesystem>
#include <string>
#include <vector>

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
    Result<void> compact(std::string_view t) override { return inner_.compact(t); }
    Result<std::vector<TableSchema>> loadSchemas() override { return inner_.loadSchemas(); }
    Result<void> saveViews(const std::vector<ViewDef>& v) override { return inner_.saveViews(v); }
    Result<std::vector<ViewDef>> loadViews() override { return inner_.loadViews(); }

private:
    IStorageEngine& inner_;
};

}  // namespace

// ---- PkIndex ---------------------------------------------------------------

TEST_CASE("PkIndex tracks add, replace and remove; no index without a PRIMARY KEY") {
    PkIndex idx(schema());
    CHECK(idx.column() == 0);
    idx.add(10, row(1, "a"));
    idx.add(11, row(2, "b"));
    CHECK(idx.find(i(1)) == 10);
    CHECK(idx.find(i(2)) == 11);
    CHECK_FALSE(idx.find(i(3)).has_value());
    CHECK_FALSE(idx.find(Value::null()).has_value());
    idx.replace(10, row(1, "a"), row(5, "a"));  // key changed
    CHECK_FALSE(idx.find(i(1)).has_value());
    CHECK(idx.find(i(5)) == 10);
    idx.remove(row(2, "b"));
    CHECK_FALSE(idx.find(i(2)).has_value());
    // Int key looked up with a Float value: numeric comparison.
    CHECK(idx.find(Value::real(5.0).value()) == 10);

    PkIndex none(TableSchema{"u", {ColumnSchema{"a", DataType::Int, false, false}}});
    CHECK_FALSE(none.column().has_value());
    none.add(1, Row{i(1)});
    CHECK_FALSE(none.find(i(1)).has_value());
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
