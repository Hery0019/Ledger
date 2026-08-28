#include "doctest.h"

#include <string>

#include "semantic/binder.h"
#include "semantic/eval.h"
#include "sql/parser.h"

using namespace ledger;

namespace {

Catalog catalog() {
    Catalog c;
    REQUIRE(c.add(TableSchema{"users",
                              {ColumnSchema{"id", DataType::Int, true, true},
                               ColumnSchema{"name", DataType::Text, false, true},
                               ColumnSchema{"score", DataType::Float, false, false},
                               ColumnSchema{"active", DataType::Bool, false, false}}})
                .ok());
    return c;
}

template <typename T>
T bindAs(std::string_view sql, const Catalog& cat) {
    auto parsed = parse(sql);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    auto bound = ledger::bind(parsed.value(), cat);  // qualified: ADL on std::variant would find std::bind
    REQUIRE_MESSAGE(bound.ok(), "unexpected error: " << (bound.ok() ? "" : bound.error().message));
    REQUIRE(std::holds_alternative<T>(bound.value()));
    return std::move(std::get<T>(bound.value()));
}

Error errorOf(std::string_view sql, const Catalog& cat) {
    auto parsed = parse(sql);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    auto bound = ledger::bind(parsed.value(), cat);  // qualified: ADL on std::variant would find std::bind
    REQUIRE_FALSE(bound.ok());
    return bound.error();
}

// Binds an expression through a WHERE on users.
BoundExprPtr where(std::string_view e, const Catalog& cat) {
    return std::move(bindAs<BoundSelect>("SELECT * FROM users WHERE " + std::string(e), cat).where);
}

std::string whereError(std::string_view e, const Catalog& cat) {
    return errorOf("SELECT * FROM users WHERE " + std::string(e), cat).message;
}

}  // namespace

// ---- CREATE / DROP ---------------------------------------------------------

TEST_CASE("bind CREATE TABLE builds a validated schema") {
    const Catalog cat = catalog();
    const auto s = bindAs<BoundCreateTable>(
        "CREATE TABLE t (id INT PRIMARY KEY, a TEXT NOT NULL, b FLOAT)", cat);
    CHECK(s.schema.name == "t");
    REQUIRE(s.schema.columns.size() == 3);
    CHECK(s.schema.columns[0].primaryKey);
    CHECK(s.schema.columns[0].notNull);  // PK => NOT NULL
    CHECK(s.schema.columns[1].notNull);
    CHECK_FALSE(s.schema.columns[2].notNull);
}

TEST_CASE("bind CREATE TABLE errors") {
    const Catalog cat = catalog();
    auto e = errorOf("CREATE TABLE users (a INT)", cat);
    CHECK(e.code == ErrorCode::AlreadyExists);
    CHECK(e.message == "'users' already exists");

    e = errorOf("CREATE TABLE t (a INT, A TEXT)", cat);
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "duplicate column 'a'");

    e = errorOf("CREATE TABLE t (a INT PRIMARY KEY, b INT PRIMARY KEY)", cat);
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "table 't' has more than one PRIMARY KEY");
}

TEST_CASE("bind DROP TABLE") {
    const Catalog cat = catalog();
    CHECK(bindAs<BoundDropTable>("DROP TABLE users", cat).table == "users");
    auto e = errorOf("DROP TABLE nope", cat);
    CHECK(e.code == ErrorCode::NotFound);
    CHECK(e.message == "unknown table 'nope'");
}

// ---- INSERT ----------------------------------------------------------------

TEST_CASE("bind INSERT without column list fills the row in schema order") {
    const Catalog cat = catalog();
    const auto s = bindAs<BoundInsert>("INSERT INTO users VALUES (1, 'bob', 2, TRUE)", cat);
    CHECK(s.table == cat.find("users"));
    REQUIRE(s.row.size() == 4);
    CHECK(s.row[0] == Value::integer(1));
    CHECK(s.row[1] == Value::text("bob"));
    CHECK(s.row[2] == Value::real(2.0).value());  // Int -> Float
    CHECK(s.row[3] == Value::boolean(true));
}

TEST_CASE("bind INSERT with column list leaves missing nullable columns NULL") {
    const Catalog cat = catalog();
    const auto s = bindAs<BoundInsert>("INSERT INTO users (name, id) VALUES ('a', 2 * 3)", cat);
    CHECK(s.row[0] == Value::integer(6));  // folded expression
    CHECK(s.row[1] == Value::text("a"));
    CHECK(s.row[2].isNull());
    CHECK(s.row[3].isNull());
}

TEST_CASE("bind INSERT errors") {
    const Catalog cat = catalog();
    CHECK(errorOf("INSERT INTO nope VALUES (1)", cat).message == "unknown table 'nope'");

    auto e = errorOf("INSERT INTO users (id, nope) VALUES (1, 2)", cat);
    CHECK(e.code == ErrorCode::NotFound);
    CHECK(e.message == "unknown column 'nope' in table 'users'");

    e = errorOf("INSERT INTO users (id, id) VALUES (1, 2)", cat);
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "duplicate column 'id'");

    e = errorOf("INSERT INTO users VALUES (1, 'a')", cat);
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "expected 4 values, got 2");
    CHECK(errorOf("INSERT INTO users (id) VALUES (1, 2)", cat).message == "expected 1 value, got 2");

    e = errorOf("INSERT INTO users (id, name) VALUES (1, 2)", cat);
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:41: column 'name' expects TEXT, got INT");

    e = errorOf("INSERT INTO users (id, name) VALUES ('1', 'a')", cat);
    CHECK(e.message == "1:38: column 'id' expects INT, got TEXT");

    e = errorOf("INSERT INTO users (id, name) VALUES (1.5, 'a')", cat);
    CHECK(e.message == "1:38: column 'id' expects INT, got FLOAT");  // no Float -> Int

    e = errorOf("INSERT INTO users (id, name) VALUES (1, NULL)", cat);
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "1:41: column 'name' cannot be NULL");

    e = errorOf("INSERT INTO users (id) VALUES (1)", cat);
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "column 'name' cannot be NULL");

    e = errorOf("INSERT INTO users (id, name) VALUES (id, 'a')", cat);
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "1:38: column reference 'id' is not allowed here");

    e = errorOf("INSERT INTO users (id, name) VALUES (1 / 0, 'a')", cat);
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:38: division by zero");
}

// ---- SELECT ----------------------------------------------------------------

TEST_CASE("bind SELECT * expands the projection") {
    const Catalog cat = catalog();
    const auto s = bindAs<BoundSelect>("SELECT * FROM users", cat);
    CHECK(s.table == cat.find("users"));
    CHECK(s.projection == std::vector<std::size_t>{0, 1, 2, 3});
    CHECK(s.where == nullptr);
    CHECK_FALSE(s.orderBy.has_value());
    CHECK_FALSE(s.limit.has_value());
}

TEST_CASE("bind SELECT resolves columns, ORDER BY and LIMIT") {
    const Catalog cat = catalog();
    const auto s = bindAs<BoundSelect>(
        "SELECT name, id, name FROM users WHERE active ORDER BY score DESC LIMIT 3", cat);
    CHECK(s.projection == std::vector<std::size_t>{1, 0, 1});
    REQUIRE(s.where != nullptr);
    CHECK(s.where->type == DataType::Bool);
    REQUIRE(s.orderBy.has_value());
    CHECK(s.orderBy->column == 2);
    CHECK(s.orderBy->descending);
    CHECK(s.limit == 3);
}

TEST_CASE("bind SELECT errors") {
    const Catalog cat = catalog();
    CHECK(errorOf("SELECT * FROM nope", cat).message == "unknown table or view 'nope'");
    CHECK(errorOf("SELECT nope FROM users", cat).message == "unknown column 'nope' in table 'users'");
    CHECK(errorOf("SELECT * FROM users ORDER BY nope", cat).message ==
          "unknown column 'nope' in table 'users'");
    auto e = errorOf("SELECT * FROM users WHERE nope = 1", cat);
    CHECK(e.code == ErrorCode::NotFound);
    CHECK(e.message == "1:27: unknown column 'nope' in table 'users'");
}

TEST_CASE("bind WHERE must be boolean") {
    const Catalog cat = catalog();
    CHECK(whereError("1", cat) == "1:27: WHERE must be BOOL, got INT");
    CHECK(whereError("name", cat) == "1:27: WHERE must be BOOL, got TEXT");
    CHECK(whereError("id + 1", cat) == "1:27: WHERE must be BOOL, got INT");
    CHECK(where("active", cat)->type == DataType::Bool);
    CHECK(where("NULL", cat)->type == DataType::Null);  // accepted: no row
    CHECK(where("TRUE", cat)->type == DataType::Bool);
}

// ---- UPDATE / DELETE -------------------------------------------------------

TEST_CASE("bind UPDATE resolves assignments and inserts casts") {
    const Catalog cat = catalog();
    const auto s = bindAs<BoundUpdate>(
        "UPDATE users SET score = id, name = 'x', active = NULL WHERE id = 1", cat);
    CHECK(s.table == cat.find("users"));
    REQUIRE(s.assignments.size() == 3);
    CHECK(s.assignments[0].first == 2);
    CHECK(s.assignments[0].second->type == DataType::Float);
    CHECK(std::holds_alternative<BoundCast>(s.assignments[0].second->node));  // Int col -> Float
    CHECK(s.assignments[1].first == 1);
    CHECK(std::holds_alternative<Value>(s.assignments[1].second->node));
    CHECK(s.assignments[2].first == 3);
    CHECK(s.assignments[2].second->type == DataType::Null);
    REQUIRE(s.where != nullptr);
}

TEST_CASE("bind UPDATE errors") {
    const Catalog cat = catalog();
    CHECK(errorOf("UPDATE nope SET a = 1", cat).message == "unknown table 'nope'");
    CHECK(errorOf("UPDATE users SET nope = 1", cat).message == "unknown column 'nope' in table 'users'");
    auto e = errorOf("UPDATE users SET id = 1, id = 2", cat);
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "column 'id' assigned twice");
    e = errorOf("UPDATE users SET name = NULL", cat);
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "1:25: column 'name' cannot be NULL");
    e = errorOf("UPDATE users SET id = score", cat);
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:23: column 'id' expects INT, got FLOAT");
    CHECK(errorOf("UPDATE users SET id = 1 WHERE name", cat).message == "1:31: WHERE must be BOOL, got TEXT");
}

TEST_CASE("bind DELETE") {
    const Catalog cat = catalog();
    const auto d = bindAs<BoundDelete>("DELETE FROM users WHERE id > 3", cat);
    CHECK(d.table == cat.find("users"));
    REQUIRE(d.where != nullptr);
    CHECK(bindAs<BoundDelete>("DELETE FROM users", cat).where == nullptr);
    CHECK(errorOf("DELETE FROM nope", cat).message == "unknown table 'nope'");
}

// ---- expression typing -----------------------------------------------------

TEST_CASE("bind expression types: arithmetic") {
    const Catalog cat = catalog();
    auto sel = [&](std::string_view e) {
        return bindAs<BoundUpdate>("UPDATE users SET score = " + std::string(e), cat)
            .assignments[0].second->type;
    };
    // score is FLOAT: the result is cast/folded to Float. So we look at the
    // type through an INT column when we want to check Int.
    CHECK(sel("id + 1") == DataType::Float);
    CHECK(bindAs<BoundUpdate>("UPDATE users SET id = id + 1", cat).assignments[0].second->type == DataType::Int);
    CHECK(bindAs<BoundUpdate>("UPDATE users SET id = id + NULL", cat).assignments[0].second->type == DataType::Int);
    CHECK(sel("score * 2") == DataType::Float);
    CHECK(sel("id / 2") == DataType::Float);  // cast inserted around an Int
    CHECK(sel("NULL") == DataType::Null);
}

TEST_CASE("bind expression type errors") {
    const Catalog cat = catalog();
    CHECK(whereError("name + 1 > 0", cat) == "1:27: cannot apply '+' to TEXT and INT");
    CHECK(whereError("active * 2 > 0", cat) == "1:27: cannot apply '*' to BOOL and INT");
    CHECK(whereError("-name = 'a'", cat) == "1:27: unary '-' requires INT or FLOAT, got TEXT");
    CHECK(whereError("NOT id", cat) == "1:27: NOT requires BOOL, got INT");
    CHECK(whereError("id AND active", cat) == "1:27: AND requires BOOL, got INT");
    CHECK(whereError("active OR name", cat) == "1:27: OR requires BOOL, got TEXT");
    CHECK(whereError("name = 1", cat) == "1:27: cannot compare TEXT with INT");
    CHECK(whereError("active = 1", cat) == "1:27: cannot compare BOOL with INT");
    CHECK(whereError("id = 'a'", cat) == "1:27: cannot compare INT with TEXT");
    // Position of the offending sub-expression, not of the root.
    CHECK(whereError("active AND (name + 1 > 0)", cat) == "1:39: cannot apply '+' to TEXT and INT");
}

TEST_CASE("bind expression types: comparisons and logic") {
    const Catalog cat = catalog();
    CHECK(where("id = 1", cat)->type == DataType::Bool);
    CHECK(where("id = 1.5", cat)->type == DataType::Bool);
    CHECK(where("score < id", cat)->type == DataType::Bool);
    CHECK(where("name = 'a'", cat)->type == DataType::Bool);
    CHECK(where("active = TRUE", cat)->type == DataType::Bool);
    CHECK(where("id = NULL", cat)->type == DataType::Bool);
    CHECK(where("name IS NULL", cat)->type == DataType::Bool);
    CHECK(where("NOT active", cat)->type == DataType::Bool);
    CHECK(where("active AND NULL", cat)->type == DataType::Bool);
    CHECK(where("NOT NULL", cat)->type == DataType::Null);
    CHECK(where("NULL AND NULL", cat)->type == DataType::Null);
    CHECK(where("NULL = NULL", cat)->type == DataType::Null);
}

// ---- constant folding ------------------------------------------------------

TEST_CASE("bind folds constant sub-expressions") {
    const Catalog cat = catalog();
    auto e = where("id > 1 + 2 * 3", cat);
    const auto& cmp = std::get<BoundBinary>(e->node);
    const Value* rhs = std::get_if<Value>(&cmp.rhs->node);
    REQUIRE(rhs != nullptr);
    CHECK(*rhs == Value::integer(7));
    CHECK(std::holds_alternative<BoundColumn>(cmp.lhs->node));

    e = where("TRUE AND NOT FALSE", cat);
    REQUIRE(std::holds_alternative<Value>(e->node));
    CHECK(std::get<Value>(e->node) == Value::boolean(true));

    e = where("1 IS NULL", cat);
    REQUIRE(std::holds_alternative<Value>(e->node));
    CHECK(std::get<Value>(e->node) == Value::boolean(false));
}

TEST_CASE("bind reports data errors in constants with their position") {
    const Catalog cat = catalog();
    auto e = errorOf("SELECT * FROM users WHERE id = 1 / 0", cat);
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:32: division by zero");
    e = errorOf("SELECT * FROM users WHERE id = 9223372036854775807 + 1", cat);
    CHECK(e.message == "1:32: integer overflow in '+'");
    // -9223372036854775808 cannot be written as a negated positive literal:
    // the parser reads 9223372036854775808 (out of range) before the binder runs.
    auto p = parse("SELECT * FROM users WHERE id = -9223372036854775808");
    CHECK_FALSE(p.ok());
}

TEST_CASE("bound expressions evaluate correctly over a row") {
    const Catalog cat = catalog();
    const Row row{Value::integer(5), Value::text("bob"), Value::real(1.5).value(), Value::null()};
    auto e = where("id * 2 > 9 AND name = 'bob' AND active IS NULL", cat);
    CHECK(eval(*e, row).value() == Value::boolean(true));
    e = where("active OR id = 5", cat);
    CHECK(eval(*e, row).value() == Value::boolean(true));  // NULL OR TRUE = TRUE
    e = where("active AND id = 5", cat);
    CHECK(eval(*e, row).value().isNull());  // NULL AND TRUE = NULL
}
