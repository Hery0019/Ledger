#include "doctest.h"

#include <string>
#include <variant>

#include "sql/parser.h"

using namespace ledger;
using namespace ledger::ast;

namespace {

// Precondition: parse succeeds and produces a statement of type T.
template <typename T>
T parseAs(std::string_view sql) {
    auto r = parse(sql);
    REQUIRE_MESSAGE(r.ok(), "unexpected error: " << (r.ok() ? "" : r.error().message));
    REQUIRE(std::holds_alternative<T>(r.value()));
    return std::move(std::get<T>(r.value()));
}

std::string errorOf(std::string_view sql) {
    auto r = parse(sql);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::SyntaxError);
    return r.error().message;
}

// Parses an expression through a WHERE, to test expressions on their own.
ExprPtr expr(std::string_view e) {
    return std::move(parseAs<Select>("SELECT * FROM t WHERE " + std::string(e)).where);
}

// Parenthesized serialization: makes the tree shape readable and comparable.
std::string show(const Expr& e) {
    return std::visit(
        [](const auto& n) -> std::string {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, Literal>) {
                if (n.value.isNull()) return "NULL";
                if (n.value.type() == DataType::Text) return "'" + n.value.toText() + "'";
                return n.value.toText();
            } else if constexpr (std::is_same_v<N, ColumnRef>) {
                return n.name;
            } else if constexpr (std::is_same_v<N, Unary>) {
                return "(" + std::string(unaryOpName(n.op)) + " " + show(*n.operand) + ")";
            } else if constexpr (std::is_same_v<N, Binary>) {
                return "(" + show(*n.lhs) + " " + std::string(binaryOpName(n.op)) + " " +
                       show(*n.rhs) + ")";
            } else {
                return "(" + show(*n.operand) + (n.negated ? " IS NOT NULL)" : " IS NULL)");
            }
        },
        e.node);
}

std::string show(std::string_view e) { return show(*expr(e)); }

}  // namespace

// ---- CREATE TABLE ----------------------------------------------------------

TEST_CASE("CREATE TABLE with all column types and constraints") {
    const auto s = parseAs<CreateTable>(
        "CREATE TABLE Users (id INT PRIMARY KEY, Name TEXT NOT NULL, score FLOAT, ok BOOL)");
    CHECK(s.table == "users");
    REQUIRE(s.columns.size() == 4);
    CHECK(s.columns[0].name == "id");
    CHECK(s.columns[0].type == DataType::Int);
    CHECK(s.columns[0].primaryKey);
    CHECK_FALSE(s.columns[0].notNull);
    CHECK(s.columns[1].name == "name");
    CHECK(s.columns[1].type == DataType::Text);
    CHECK_FALSE(s.columns[1].primaryKey);
    CHECK(s.columns[1].notNull);
    CHECK(s.columns[2].type == DataType::Float);
    CHECK(s.columns[3].type == DataType::Bool);
}

TEST_CASE("CREATE TABLE constraints in any order, each at most once") {
    const auto s = parseAs<CreateTable>("CREATE TABLE t (a INT NOT NULL PRIMARY KEY)");
    CHECK(s.columns[0].primaryKey);
    CHECK(s.columns[0].notNull);
    CHECK(errorOf("CREATE TABLE t (a INT PRIMARY KEY PRIMARY KEY)") ==
          "1:35: expected a single PRIMARY KEY constraint, got 'PRIMARY'");
    CHECK(errorOf("CREATE TABLE t (a INT NOT NULL NOT NULL)") ==
          "1:32: expected a single NOT NULL constraint, got 'NOT'");
}

TEST_CASE("CREATE TABLE syntax errors") {
    CHECK(errorOf("CREATE t (a INT)") == "1:8: expected 'TABLE' or 'VIEW', got identifier 't'");
    CHECK(errorOf("CREATE TABLE t") == "1:15: expected '(', got 'end of input'");
    CHECK(errorOf("CREATE TABLE t ()") == "1:17: expected identifier, got ')'");
    CHECK(errorOf("CREATE TABLE t (a)") == "1:18: expected column type (INT, FLOAT, TEXT or BOOL), got ')'");
    CHECK(errorOf("CREATE TABLE t (a INT,)") == "1:23: expected identifier, got ')'");
    CHECK(errorOf("CREATE TABLE t (a INT") == "1:22: expected ')', got 'end of input'");
    CHECK(errorOf("CREATE TABLE t (a INT PRIMARY)") == "1:30: expected 'KEY', got ')'");
    CHECK(errorOf("CREATE TABLE t (a INT NOT)") == "1:26: expected 'NULL', got ')'");
}

// ---- DROP TABLE ------------------------------------------------------------

TEST_CASE("DROP TABLE") {
    CHECK(parseAs<DropTable>("DROP TABLE Users;").table == "users");
    CHECK(errorOf("DROP Users") == "1:6: expected 'TABLE' or 'VIEW', got identifier 'users'");
    CHECK(errorOf("DROP TABLE") == "1:11: expected identifier, got 'end of input'");
}

// ---- INSERT ----------------------------------------------------------------

TEST_CASE("INSERT without column list") {
    const auto s = parseAs<Insert>("INSERT INTO t VALUES (1, 'a', 2.5, TRUE, NULL)");
    CHECK(s.table == "t");
    CHECK(s.columns.empty());
    REQUIRE(s.values.size() == 5);
    CHECK(show(*s.values[0]) == "1");
    CHECK(show(*s.values[1]) == "'a'");
    CHECK(show(*s.values[2]) == "2.5");
    CHECK(show(*s.values[3]) == "true");
    CHECK(show(*s.values[4]) == "NULL");
}

TEST_CASE("INSERT with column list and expressions") {
    const auto s = parseAs<Insert>("INSERT INTO t (A, b) VALUES (-1, 2 * 3)");
    REQUIRE(s.columns.size() == 2);
    CHECK(s.columns[0] == "a");
    CHECK(s.columns[1] == "b");
    REQUIRE(s.values.size() == 2);
    CHECK(show(*s.values[0]) == "(- 1)");
    CHECK(show(*s.values[1]) == "(2 * 3)");
}

TEST_CASE("INSERT syntax errors") {
    CHECK(errorOf("INSERT t VALUES (1)") == "1:8: expected 'INTO', got identifier 't'");
    CHECK(errorOf("INSERT INTO t (1)") == "1:16: expected identifier, got integer '1'");
    CHECK(errorOf("INSERT INTO t (a) (1)") == "1:19: expected 'VALUES', got '('");
    CHECK(errorOf("INSERT INTO t VALUES ()") == "1:23: expected expression, got ')'");
    CHECK(errorOf("INSERT INTO t VALUES (1,)") == "1:25: expected expression, got ')'");
    CHECK(errorOf("INSERT INTO t VALUES (1), (2)") == "1:25: expected end of input, got ','");
}

// ---- SELECT ----------------------------------------------------------------

TEST_CASE("SELECT * is an empty column list") {
    const auto s = parseAs<Select>("SELECT * FROM t");
    CHECK(s.columns.empty());
    CHECK(s.table == "t");
    CHECK(s.where == nullptr);
    CHECK_FALSE(s.orderBy.has_value());
    CHECK_FALSE(s.limit.has_value());
}

TEST_CASE("SELECT with explicit columns") {
    const auto s = parseAs<Select>("SELECT A, b, C FROM T");
    REQUIRE(s.columns.size() == 3);
    CHECK(s.columns[0] == "a");
    CHECK(s.columns[1] == "b");
    CHECK(s.columns[2] == "c");
    CHECK(s.table == "t");
}

TEST_CASE("SELECT with every clause") {
    const auto s = parseAs<Select>("SELECT a FROM t WHERE a > 1 ORDER BY B DESC LIMIT 10;");
    REQUIRE(s.where != nullptr);
    CHECK(show(*s.where) == "(a > 1)");
    REQUIRE(s.orderBy.has_value());
    CHECK(s.orderBy->column == "b");
    CHECK(s.orderBy->descending);
    REQUIRE(s.limit.has_value());
    CHECK(*s.limit == 10);
}

TEST_CASE("ORDER BY defaults to ascending; ASC is accepted") {
    CHECK_FALSE(parseAs<Select>("SELECT * FROM t ORDER BY a").orderBy->descending);
    CHECK_FALSE(parseAs<Select>("SELECT * FROM t ORDER BY a ASC").orderBy->descending);
}

TEST_CASE("LIMIT accepts only a non-negative integer literal") {
    CHECK(*parseAs<Select>("SELECT * FROM t LIMIT 0").limit == 0);
    CHECK(errorOf("SELECT * FROM t LIMIT -1") == "1:23: expected non-negative integer after LIMIT, got '-'");
    CHECK(errorOf("SELECT * FROM t LIMIT n") == "1:23: expected non-negative integer after LIMIT, got identifier 'n'");
    CHECK(errorOf("SELECT * FROM t LIMIT 1.5") == "1:23: expected non-negative integer after LIMIT, got float '1.5'");
    CHECK(errorOf("SELECT * FROM t LIMIT 99999999999999999999") ==
          "1:23: value out of range for INT: '99999999999999999999'");
}

TEST_CASE("SELECT syntax errors") {
    CHECK(errorOf("SELECT FROM t") == "1:8: expected identifier, got 'FROM'");
    CHECK(errorOf("SELECT a b FROM t") == "1:10: expected 'FROM', got identifier 'b'");
    CHECK(errorOf("SELECT *, a FROM t") == "1:9: expected 'FROM', got ','");
    CHECK(errorOf("SELECT * FROM") == "1:14: expected identifier, got 'end of input'");
    CHECK(errorOf("SELECT * FROM t WHERE") == "1:22: expected expression, got 'end of input'");
    CHECK(errorOf("SELECT * FROM t ORDER a") == "1:23: expected 'BY', got identifier 'a'");
    CHECK(errorOf("SELECT * FROM t LIMIT 1 WHERE a") == "1:25: expected end of input, got 'WHERE'");
}

// ---- UPDATE ----------------------------------------------------------------

TEST_CASE("UPDATE with several assignments and WHERE") {
    const auto s = parseAs<Update>("UPDATE t SET A = 1, b = b + 1 WHERE id = 3");
    CHECK(s.table == "t");
    REQUIRE(s.assignments.size() == 2);
    CHECK(s.assignments[0].first == "a");
    CHECK(show(*s.assignments[0].second) == "1");
    CHECK(s.assignments[1].first == "b");
    CHECK(show(*s.assignments[1].second) == "(b + 1)");
    REQUIRE(s.where != nullptr);
    CHECK(show(*s.where) == "(id = 3)");
}

TEST_CASE("UPDATE without WHERE") {
    CHECK(parseAs<Update>("UPDATE t SET a = NULL").where == nullptr);
}

TEST_CASE("UPDATE syntax errors") {
    CHECK(errorOf("UPDATE t a = 1") == "1:10: expected 'SET', got identifier 'a'");
    CHECK(errorOf("UPDATE t SET a 1") == "1:16: expected '=', got integer '1'");
    CHECK(errorOf("UPDATE t SET a = 1,") == "1:20: expected identifier, got 'end of input'");
}

// ---- DELETE ----------------------------------------------------------------

TEST_CASE("DELETE with and without WHERE") {
    const auto d = parseAs<Delete>("DELETE FROM t WHERE a IS NULL");
    CHECK(d.table == "t");
    REQUIRE(d.where != nullptr);
    CHECK(show(*d.where) == "(a IS NULL)");
    CHECK(parseAs<Delete>("DELETE FROM t").where == nullptr);
    CHECK(errorOf("DELETE t") == "1:8: expected 'FROM', got identifier 't'");
}

// ---- general structure -----------------------------------------------------

TEST_CASE("unknown statement and trailing garbage") {
    CHECK(errorOf("") == "1:1: expected statement (CREATE, DROP, INSERT, SELECT, UPDATE or DELETE), got 'end of input'");
    CHECK(errorOf("EXPLAIN SELECT * FROM t") ==
          "1:1: expected statement (CREATE, DROP, INSERT, SELECT, UPDATE or DELETE), got identifier 'explain'");
    CHECK(errorOf("SELECT * FROM t; SELECT * FROM t") == "1:18: expected end of input, got 'SELECT'");
    CHECK(errorOf("SELECT * FROM t;;") == "1:17: expected end of input, got ';'");
}

TEST_CASE("lexer errors are propagated") {
    CHECK(errorOf("SELECT * FROM t WHERE a = 'oops") == "1:27: unterminated string literal");
}

TEST_CASE("statements carry positions on multiple lines") {
    CHECK(errorOf("SELECT *\nFROM t\nWHERE") == "3:6: expected expression, got 'end of input'");
}

// ---- expressions: literals -------------------------------------------------

TEST_CASE("literals become Values") {
    CHECK(show("42") == "42");
    CHECK(show("2.5") == "2.5");
    CHECK(show("'it''s'") == "'it's'");
    CHECK(show("TRUE") == "true");
    CHECK(show("false") == "false");
    CHECK(show("NULL") == "NULL");
    CHECK(show("col") == "col");
}

TEST_CASE("out-of-range integer literal is reported with its position") {
    CHECK(errorOf("SELECT * FROM t WHERE a = 99999999999999999999") ==
          "1:27: value out of range for INT: '99999999999999999999'");
}

TEST_CASE("expressions carry the position of their first token") {
    const auto e = expr("a + 1");
    CHECK(e->line == 1);
    CHECK(e->column == 23);
    CHECK(std::get<Binary>(e->node).rhs->column == 27);
}

// ---- expressions: precedence and associativity -----------------------------

TEST_CASE("arithmetic precedence and left associativity") {
    CHECK(show("1 + 2 * 3") == "(1 + (2 * 3))");
    CHECK(show("(1 + 2) * 3") == "((1 + 2) * 3)");
    CHECK(show("a - b - c") == "((a - b) - c)");
    CHECK(show("a / b * c") == "((a / b) * c)");
    CHECK(show("a + b - c") == "((a + b) - c)");
}

TEST_CASE("unary minus binds tighter than multiplication") {
    CHECK(show("-a * b") == "((- a) * b)");
    CHECK(show("- -1") == "(- (- 1))");
    CHECK(show("-(a + b)") == "(- (a + b))");
    CHECK(show("a - -b") == "(a - (- b))");
}

TEST_CASE("comparison binds looser than arithmetic") {
    CHECK(show("a + 1 > b * 2") == "((a + 1) > (b * 2))");
    CHECK(show("a <> b") == "(a <> b)");
    CHECK(show("a != b") == "(a <> b)");
    CHECK(show("a <= b") == "(a <= b)");
    CHECK(show("a >= b") == "(a >= b)");
    CHECK(show("a < b") == "(a < b)");
}

TEST_CASE("comparison is not associative") {
    CHECK(errorOf("SELECT * FROM t WHERE a = b = c") ==
          "1:29: comparison operators cannot be chained; use parentheses");
    CHECK(errorOf("SELECT * FROM t WHERE a < b > c") ==
          "1:29: comparison operators cannot be chained; use parentheses");
    CHECK(errorOf("SELECT * FROM t WHERE a IS NULL = b") ==
          "1:33: comparison operators cannot be chained; use parentheses");
    CHECK(errorOf("SELECT * FROM t WHERE a = b IS NULL") ==
          "1:29: comparison operators cannot be chained; use parentheses");
    CHECK(show("(a = b) = c") == "((a = b) = c)");
}

TEST_CASE("IS NULL and IS NOT NULL") {
    CHECK(show("a IS NULL") == "(a IS NULL)");
    CHECK(show("a IS NOT NULL") == "(a IS NOT NULL)");
    CHECK(show("a + 1 IS NULL") == "((a + 1) IS NULL)");
    CHECK(show("NOT a IS NULL") == "(NOT (a IS NULL))");
    CHECK(errorOf("SELECT * FROM t WHERE a IS 1") == "1:28: expected 'NULL', got integer '1'");
    CHECK(errorOf("SELECT * FROM t WHERE a IS NOT 1") == "1:32: expected 'NULL', got integer '1'");
}

TEST_CASE("logical precedence: NOT > AND > OR") {
    CHECK(show("a OR b AND c") == "(a OR (b AND c))");
    CHECK(show("a AND b OR c") == "((a AND b) OR c)");
    CHECK(show("NOT a AND b") == "((NOT a) AND b)");
    CHECK(show("NOT a = b") == "(NOT (a = b))");
    CHECK(show("NOT NOT a") == "(NOT (NOT a))");
    CHECK(show("a OR b OR c") == "((a OR b) OR c)");
    CHECK(show("a AND b AND c") == "((a AND b) AND c)");
    CHECK(show("(a OR b) AND c") == "((a OR b) AND c)");
}

TEST_CASE("a realistic WHERE clause") {
    CHECK(show("age >= 18 AND (name = 'Bob' OR name IS NULL) AND NOT deleted") ==
          "(((age >= 18) AND ((name = 'Bob') OR (name IS NULL))) AND (NOT deleted))");
}

TEST_CASE("expression syntax errors") {
    CHECK(errorOf("SELECT * FROM t WHERE (a") == "1:25: expected ')', got 'end of input'");
    CHECK(errorOf("SELECT * FROM t WHERE ()") == "1:24: expected expression, got ')'");
    CHECK(errorOf("SELECT * FROM t WHERE a +") == "1:26: expected expression, got 'end of input'");
    CHECK(errorOf("SELECT * FROM t WHERE * 2") == "1:23: expected expression, got '*'");
    CHECK(errorOf("SELECT * FROM t WHERE a b") == "1:25: expected end of input, got identifier 'b'");
    CHECK(errorOf("SELECT * FROM t WHERE NOT") == "1:26: expected expression, got 'end of input'");
    CHECK(errorOf("SELECT * FROM t WHERE SELECT") == "1:23: expected expression, got 'SELECT'");
}

// ---- operator names --------------------------------------------------------

TEST_CASE("operator names") {
    CHECK(unaryOpName(UnaryOp::Not) == "NOT");
    CHECK(unaryOpName(UnaryOp::Neg) == "-");
    CHECK(binaryOpName(BinaryOp::GtEq) == ">=");
    CHECK(binaryOpName(BinaryOp::Div) == "/");
}
