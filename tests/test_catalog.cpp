#include "doctest.h"

#include "semantic/catalog.h"

using namespace ledger;

namespace {
TableSchema users() {
    return TableSchema{"users",
                       {ColumnSchema{"id", DataType::Int, true, true},
                        ColumnSchema{"name", DataType::Text, false, true},
                        ColumnSchema{"score", DataType::Float, false, false}}};
}
}  // namespace

TEST_CASE("TableSchema resolves columns by name") {
    const auto t = users();
    CHECK(t.columnIndex("id") == 0);
    CHECK(t.columnIndex("score") == 2);
    CHECK_FALSE(t.columnIndex("missing").has_value());
    CHECK_FALSE(t.columnIndex("ID").has_value());  // folding happens upstream
    CHECK(t.primaryKeyIndex() == 0);
    CHECK_FALSE(TableSchema{"t", {ColumnSchema{"a", DataType::Int, false, false}}}
                    .primaryKeyIndex().has_value());
}

TEST_CASE("Catalog add / find / remove") {
    Catalog c;
    CHECK(c.size() == 0);
    CHECK(c.find("users") == nullptr);

    REQUIRE(c.add(users()).ok());
    CHECK(c.size() == 1);
    const TableSchema* t = c.find("users");
    REQUIRE(t != nullptr);
    CHECK(t->name == "users");
    CHECK(t->columns.size() == 3);
    CHECK(c.contains("users"));

    REQUIRE(c.remove("users").ok());
    CHECK(c.size() == 0);
    CHECK_FALSE(c.contains("users"));
}

TEST_CASE("Catalog rejects duplicates and unknown tables") {
    Catalog c;
    REQUIRE(c.add(users()).ok());
    auto dup = c.add(users());
    REQUIRE_FALSE(dup.ok());
    CHECK(dup.error().code == ErrorCode::AlreadyExists);
    CHECK(dup.error().message == "table 'users' already exists");

    auto missing = c.remove("nope");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(missing.error().message == "unknown table 'nope'");
}

TEST_CASE("Catalog pointers stay valid across other insertions") {
    Catalog c;
    REQUIRE(c.add(users()).ok());
    const TableSchema* before = c.find("users");
    for (int i = 0; i < 100; ++i) {
        REQUIRE(c.add(TableSchema{"t" + std::to_string(i),
                                  {ColumnSchema{"a", DataType::Int, false, false}}}).ok());
    }
    CHECK(c.find("users") == before);
    CHECK(before->name == "users");
}

TEST_CASE("Catalog lists table names sorted") {
    Catalog c;
    REQUIRE(c.add(TableSchema{"zeta", {ColumnSchema{"a", DataType::Int, false, false}}}).ok());
    REQUIRE(c.add(TableSchema{"alpha", {ColumnSchema{"a", DataType::Int, false, false}}}).ok());
    const auto names = c.tableNames();
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "alpha");
    CHECK(names[1] == "zeta");
}
