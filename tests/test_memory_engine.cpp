#include "doctest.h"

#include <vector>

#include "storage/memory_engine.h"

using namespace ledger;

namespace {

TableSchema schema() {
    return TableSchema{"t",
                       {ColumnSchema{"id", DataType::Int, true, true},
                        ColumnSchema{"name", DataType::Text, false, false}}};
}

Row row(std::int64_t id, const char* name) { return Row{Value::integer(id), Value::text(name)}; }

std::vector<std::pair<RowId, Row>> all(IStorageEngine& e, std::string_view table) {
    std::vector<std::pair<RowId, Row>> out;
    REQUIRE(e.scan(table, [&](RowId id, const Row& r) { out.emplace_back(id, r); return true; }).ok());
    return out;
}

}  // namespace

TEST_CASE("MemoryEngine: create, insert, scan") {
    MemoryEngine e;
    REQUIRE(e.createTable(schema()).ok());
    CHECK(e.insert("t", row(1, "a")).value() == 1);
    CHECK(e.insert("t", row(2, "b")).value() == 2);
    const auto rows = all(e, "t");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].first == 1);
    CHECK(rows[0].second == row(1, "a"));
    CHECK(rows[1].first == 2);
}

TEST_CASE("MemoryEngine: update keeps the rowid, remove drops it, ids are never reused") {
    MemoryEngine e;
    REQUIRE(e.createTable(schema()).ok());
    REQUIRE(e.insert("t", row(1, "a")).ok());
    REQUIRE(e.insert("t", row(2, "b")).ok());
    REQUIRE(e.update("t", 1, row(1, "A")).ok());
    REQUIRE(e.remove("t", 2).ok());
    auto rows = all(e, "t");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].first == 1);
    CHECK(rows[0].second == row(1, "A"));
    CHECK(e.insert("t", row(3, "c")).value() == 3);
}

TEST_CASE("MemoryEngine: scan stops when the visitor returns false") {
    MemoryEngine e;
    REQUIRE(e.createTable(schema()).ok());
    for (int i = 1; i <= 5; ++i) REQUIRE(e.insert("t", row(i, "x")).ok());
    int seen = 0;
    REQUIRE(e.scan("t", [&](RowId, const Row&) { return ++seen < 2; }).ok());
    CHECK(seen == 2);
}

TEST_CASE("MemoryEngine: errors") {
    MemoryEngine e;
    REQUIRE(e.createTable(schema()).ok());
    CHECK(e.createTable(schema()).error().code == ErrorCode::AlreadyExists);
    CHECK(e.insert("nope", row(1, "a")).error().code == ErrorCode::NotFound);
    CHECK(e.scan("nope", [](RowId, const Row&) { return true; }).error().code == ErrorCode::NotFound);
    CHECK(e.update("t", 99, row(1, "a")).error().code == ErrorCode::NotFound);
    CHECK(e.remove("t", 99).error().code == ErrorCode::NotFound);
    CHECK(e.dropTable("nope").error().code == ErrorCode::NotFound);
    CHECK(e.insert("t", Row{Value::integer(1)}).error().code == ErrorCode::Internal);
    REQUIRE(e.insert("t", row(1, "a")).ok());
    CHECK(e.update("t", 1, Row{Value::integer(1)}).error().code == ErrorCode::Internal);
}

TEST_CASE("MemoryEngine: dropTable, loadSchemas, compact") {
    MemoryEngine e;
    REQUIRE(e.createTable(schema()).ok());
    TableSchema other{"a", {ColumnSchema{"x", DataType::Bool, false, false}}};
    REQUIRE(e.createTable(other).ok());
    auto schemas = e.loadSchemas().value();
    REQUIRE(schemas.size() == 2);
    CHECK(schemas[0].name == "a");  // ordre alphabétique
    CHECK(schemas[1].name == "t");
    CHECK(e.compact("t").ok());
    REQUIRE(e.dropTable("t").ok());
    CHECK(e.loadSchemas().value().size() == 1);
    CHECK(e.insert("t", row(1, "a")).error().code == ErrorCode::NotFound);
}
