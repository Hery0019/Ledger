#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/value.h"

// Syntax tree produced by the parser. Pure data: no logic, no semantic checks
// (table existence, column types...), that is the binder's job. All
// identifiers are already folded to lowercase by the lexer.
//
// Choice: std::variant + unique_ptr rather than a virtual hierarchy. The
// binder and the executor do an exhaustive std::visit: adding a node without
// handling it everywhere becomes a compile error.
namespace ledger::ast {

// ---- expressions -----------------------------------------------------------

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class UnaryOp { Not, Neg };
enum class BinaryOp { Or, And, Eq, NotEq, Lt, LtEq, Gt, GtEq, Add, Sub, Mul, Div };

std::string_view unaryOpName(UnaryOp op) noexcept;
std::string_view binaryOpName(BinaryOp op) noexcept;

// Renders an expression back to SQL-like text: `a + 1`, `NOT (x IS NULL)`.
// Used to name unaliased SELECT columns and in error messages.
std::string exprToString(const Expr& e);

struct Literal {
    Value value;  // Int/Float/Text/Bool/Null, already converted by the parser
};

struct ColumnRef {
    std::string qualifier;  // table alias, empty when unqualified
    std::string name;
};

struct Unary {
    UnaryOp op;
    ExprPtr operand;
};

struct Binary {
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
};

// `x IS NULL` / `x IS NOT NULL`. A node of its own: this is not a comparison
// (comparing with NULL would give Unknown; here we want a real boolean).
struct IsNull {
    ExprPtr operand;
    bool negated;
};

// Function call: `count(*)`, `sum(x)`, `upper(name)`. Aggregates and scalar
// functions share this node; the binder tells them apart by name.
struct Call {
    std::string name;  // lowercase
    std::vector<ExprPtr> args;
    bool star;  // `count(*)`: no argument, counts rows
};

// `x [NOT] IN (a, b, ...)`
struct InList {
    ExprPtr value;
    std::vector<ExprPtr> items;  // never empty
    bool negated;
};

// `x [NOT] BETWEEN low AND high` (inclusive)
struct Between {
    ExprPtr value;
    ExprPtr low;
    ExprPtr high;
    bool negated;
};

// `x [NOT] LIKE pattern` (`%` any sequence, `_` any single character)
struct Like {
    ExprPtr value;
    ExprPtr pattern;
    bool negated;
};

// `CASE [x] WHEN a THEN b [WHEN ...] [ELSE c] END`. With an operand, each
// WHEN compares `x = a`; without, each WHEN is a boolean condition. Missing
// ELSE yields NULL.
struct Case {
    ExprPtr operand;  // nullptr for the searched form
    std::vector<std::pair<ExprPtr, ExprPtr>> whens;  // (condition or value, result); never empty
    ExprPtr elseExpr;  // nullptr if absent
};

struct Select;

// `x [NOT] IN (SELECT ...)`: the subquery must project one column.
struct InSelect {
    ExprPtr value;
    std::unique_ptr<Select> select;
    bool negated;
};

// `[NOT] EXISTS (SELECT ...)`
struct Exists {
    std::unique_ptr<Select> select;
    bool negated;
};

// `(SELECT ...)` used as a value: one column, at most one row (NULL if none).
struct ScalarSubquery {
    std::unique_ptr<Select> select;
};

struct Expr {
    std::variant<Literal, ColumnRef, Unary, Binary, IsNull, Call, InList, Between, Like, Case, InSelect,
                 Exists, ScalarSubquery>
        node;
    // Position of the expression's first token, for binder error messages.
    std::size_t line;
    std::size_t column;
};

// ---- statements ------------------------------------------------------------

struct ColumnDef {
    std::string name;
    DataType type;  // never DataType::Null
    bool primaryKey;
    bool notNull;
};

struct CreateTable {
    std::string table;
    std::vector<ColumnDef> columns;  // never empty
};

struct DropTable {
    std::string table;
};

struct Insert {
    std::string table;
    std::vector<std::string> columns;  // empty = every column, in schema order
    std::vector<ExprPtr> values;       // never empty; a single row in v1
};

// One item of the SELECT list: an expression and its output name.
// `alias` is empty when none was given; the binder then names the column
// after the expression (a bare column keeps its name).
struct SelectItem {
    ExprPtr expr;         // nullptr for `t.*`
    std::string alias;
    std::string starOf;   // `t.*`: the qualifier; empty otherwise
};

// A table or view in FROM, with its alias (defaults to the name).
struct TableRef {
    std::string name;
    std::string alias;
};

enum class JoinKind { Inner, Left };

struct Join {
    JoinKind kind;
    TableRef table;
    ExprPtr on;
};

struct OrderBy {
    ExprPtr expr;  // an output alias, a column, or any expression
    bool descending;
};

struct Select {
    bool distinct;                  // `SELECT DISTINCT`
    bool star;                      // `SELECT *`
    std::vector<SelectItem> items;  // empty when star
    TableRef from;
    std::vector<Join> joins;        // applied left to right
    ExprPtr where;  // nullptr if absent
    std::vector<ExprPtr> groupBy;   // empty if absent
    ExprPtr having;                 // nullptr if absent
    std::vector<OrderBy> orderBy;   // empty if absent
    std::optional<std::int64_t> limit;   // >= 0 if present
    std::optional<std::int64_t> offset;  // >= 0 if present

    // UNION [ALL] members, in order. ORDER BY / LIMIT / OFFSET written after
    // the last member apply to the whole union and are stored on the head.
    struct UnionMember {
        bool all;
        std::unique_ptr<Select> select;
    };
    std::vector<UnionMember> unions;
};

struct Update {
    std::string table;
    std::vector<std::pair<std::string, ExprPtr>> assignments;  // never empty
    ExprPtr where;  // nullptr if absent
};

struct Delete {
    std::string table;
    ExprPtr where;  // nullptr if absent
};

// CREATE VIEW name AS SELECT ... — the SELECT is kept both parsed (to
// validate it) and verbatim (to persist it). ORDER BY and LIMIT are refused in
// a view: a view is a table + projection + filter, nothing more, so that
// `SELECT ... FROM view WHERE ...` composes without surprises.
struct CreateView {
    std::string name;
    Select query;
    std::string queryText;  // the SELECT as written, without the trailing `;`
};

struct DropView {
    std::string name;
};

// BEGIN [TRANSACTION] / COMMIT / ROLLBACK
struct Begin {};
struct Commit {};
struct Rollback {};

using Statement = std::variant<CreateTable, DropTable, CreateView, DropView, Insert, Select, Update,
                               Delete, Begin, Commit, Rollback>;

}  // namespace ledger::ast
