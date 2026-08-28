#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "core/row.h"
#include "core/value.h"
#include "semantic/catalog.h"
#include "sql/ast.h"

// Bound plan: what the binder produces from an ast::Statement. Every
// reference is resolved (columns -> indices, tables -> schemas) and every type
// is checked. The executor runs this plan without ever having to raise a
// semantic error; the only possible runtime errors depend on the data
// (division by zero, overflow, violated constraint).
namespace ledger {

struct BoundExpr;
using BoundExprPtr = std::unique_ptr<BoundExpr>;

struct BoundColumn {
    std::size_t index;  // into the current table's Row
};

struct BoundUnary {
    ast::UnaryOp op;
    BoundExprPtr operand;
};

struct BoundBinary {
    ast::BinaryOp op;
    BoundExprPtr lhs;
    BoundExprPtr rhs;
};

struct BoundIsNull {
    BoundExprPtr operand;
    bool negated;
};

// The engine's only implicit conversion: Int -> Float (promotion with no
// notable loss). Inserted by the binder when an Int expression feeds a Float
// column. NULL passes through unchanged.
struct BoundCast {
    BoundExprPtr operand;
    DataType to;  // always DataType::Float in v1
};

// `value [NOT] IN (items...)`: NULL if value is NULL, or if no item matches
// and one of them is NULL.
struct BoundInList {
    BoundExprPtr value;
    std::vector<BoundExprPtr> items;
    bool negated;
};

enum class ScalarFunc { Upper, Lower, Length, Trim, Abs, Round, Coalesce, NullIf };

std::string_view scalarFuncName(ScalarFunc f) noexcept;

// A scalar function call, arguments already type-checked by the binder.
struct BoundCall {
    ScalarFunc func;
    std::vector<BoundExprPtr> args;
};

// Searched CASE (the simple form is bound as `operand = value` conditions).
struct BoundCase {
    std::vector<std::pair<BoundExprPtr, BoundExprPtr>> whens;  // (Bool condition, result)
    BoundExprPtr elseExpr;  // nullptr = NULL
};

// Subquery references. The nested SELECT lives in the enclosing
// BoundSelect::subqueries at index `slot`; the executor runs it once (they
// are never correlated) and hands the rows to eval() through the
// SubqueryRows table.
struct BoundInSubquery {
    BoundExprPtr value;
    std::size_t slot;
    bool negated;
};

struct BoundExists {
    std::size_t slot;
    bool negated;
};

struct BoundScalarSubquery {
    std::size_t slot;
};

// `value [NOT] LIKE pattern` over TEXT: `%` any sequence, `_` one character.
struct BoundLike {
    BoundExprPtr value;
    BoundExprPtr pattern;
    bool negated;
};

struct BoundExpr {
    std::variant<Value, BoundColumn, BoundUnary, BoundBinary, BoundIsNull, BoundCast, BoundInList, BoundLike,
                 BoundCall, BoundCase, BoundInSubquery, BoundExists, BoundScalarSubquery>
        node;
    // Static type. DataType::Null means "always NULL" (e.g. the NULL literal,
    // or NULL + NULL). An expression of type Int can still produce NULL at
    // runtime (nullable column): Null is not a subtype, it is the type of
    // guaranteed absence.
    DataType type;
};

struct BoundCreateTable {
    TableSchema schema;  // validated: unique names, <= 1 PRIMARY KEY, PK => NOT NULL
};

struct BoundDropTable {
    std::string table;
};

// A table's CHECK constraint, bound over the table's own columns.
struct BoundCheck {
    std::size_t column;  // the column carrying the constraint (for messages)
    BoundExprPtr expr;   // BOOL or NULL typed; evaluated on the complete row
};

struct BoundInsert {
    const TableSchema* table;
    Row row;  // complete, in schema order, types already conforming
    std::vector<BoundCheck> checks;
    // Generated-key column left to the executor to fill (its row slot is
    // NULL): AUTOINCREMENT takes the next INT key, a UUID PRIMARY KEY takes
    // a fresh version-4 UUID.
    std::optional<std::size_t> autoColumn;
};

// Deep copy. Bound expressions are trees of unique_ptr; a view column's
// expression is cloned every time a query refers to it.
BoundExprPtr cloneExpr(const BoundExpr& e);

struct BoundOrderBy {
    BoundExprPtr expr;  // evaluated on the source row, before projection
    bool descending;
};

// ---- relations -------------------------------------------------------------
//
// A bound FROM clause is a small tree of relational operators that the
// executor materializes into rows. Views are sub-trees, so they work on
// either side of a join without special cases.
//
//   Scan    : the live rows of one table
//   Filter  : keeps the input rows whose predicate is true
//   Project : one expression per output column, evaluated on the input row
//   Join    : left row ++ right row for every pair satisfying `on`; a LEFT
//             join also emits left ++ NULLs for a left row with no match
//
// Every expression inside a node indexes the row of the node's input(s); a
// join's `on` indexes the concatenated row.

struct BoundRelation;
using BoundRelationPtr = std::unique_ptr<BoundRelation>;

// An output column of a relation: how queries refer to it, and its type.
struct RelColumn {
    std::string qualifier;  // table alias or view alias
    std::string name;
    DataType type;
};

struct RelScan {
    const TableSchema* table;
};

struct RelFilter {
    BoundRelationPtr input;
    BoundExprPtr predicate;
};

struct RelProject {
    BoundRelationPtr input;
    std::vector<BoundExprPtr> exprs;
};

enum class JoinKind { Inner, Left };

struct RelJoin {
    JoinKind kind;
    BoundRelationPtr left;
    BoundRelationPtr right;
    BoundExprPtr on;  // Bool or Null, over left ++ right
};

struct BoundRelation {
    std::variant<RelScan, RelFilter, RelProject, RelJoin> node;
    std::vector<RelColumn> columns;  // output layout; the row width
};

enum class AggFunc { Count, Sum, Avg, Min, Max };

std::string_view aggFuncName(AggFunc f) noexcept;

// One aggregate computed per group. `arg` is evaluated on every source row of
// the group; nullptr for COUNT(*).
struct BoundAggregate {
    AggFunc func;
    BoundExprPtr arg;
};

// A SELECT is either a plain row pipeline or an aggregating one:
//
//   plain      : source rows -> WHERE -> projection / ORDER BY keys
//   aggregated : source rows -> WHERE -> grouped by `groupBy` -> one group
//                row [key0.., agg0..] per group -> HAVING -> projection /
//                ORDER BY keys
//
// In the aggregated case `projection`, `having` and `orderBy` expressions are
// bound against the group row (BoundColumn indices into it), never against
// the source row. Without GROUP BY, every row is one group (an empty source
// still yields one group: COUNT(*) = 0, other aggregates NULL).
struct BoundSelect {
    BoundRelationPtr relation;              // the FROM clause (tables, views, joins)
    std::vector<std::string> columnNames;   // output header, one per projection
    std::vector<BoundExprPtr> projection;   // never empty (`*` is expanded)
    BoundExprPtr where;                     // nullptr = every row; type Bool or Null
    std::vector<BoundOrderBy> orderBy;      // lexicographic, first key first
    std::optional<std::int64_t> limit;
    std::optional<std::int64_t> offset;
    bool distinct = false;                  // drop duplicate projected rows (first kept)

    bool aggregated = false;
    std::vector<BoundExprPtr> groupBy;      // evaluated on source rows
    std::vector<BoundAggregate> aggregates;
    BoundExprPtr having;                    // nullptr = keep every group

    // Uncorrelated subqueries used by this SELECT's expressions (WHERE,
    // projection, HAVING, ORDER BY, join conditions, view bodies), each run
    // once before the rows are evaluated.
    std::vector<std::unique_ptr<BoundSelect>> subqueries;

    // UNION [ALL] members; their output must match the head column by column
    // (same count, comparable types). With unions, `orderBy` reads the output
    // row (BoundColumn indices into the projected row), not the source row.
    struct UnionMember {
        bool all;
        std::unique_ptr<BoundSelect> select;
    };
    std::vector<UnionMember> unions;
};

struct BoundUpdate {
    const TableSchema* table;
    std::vector<std::pair<std::size_t, BoundExprPtr>> assignments;  // type conforms to the column
    BoundExprPtr where;
    std::vector<BoundCheck> checks;
};

struct BoundDelete {
    const TableSchema* table;
    BoundExprPtr where;
};

struct BoundCreateView {
    ViewDef def;                       // validated: the SELECT binds against the current catalog
    std::vector<std::string> sources;  // tables and views it reads from (FROM and JOINs)
};

struct BoundDropView {
    std::string name;
};

struct BoundBegin {};
struct BoundCommit {};
struct BoundRollback {};

// User statements carry the plain password up to the executor, which hashes
// it (semantic checks need the catalog only; hashing is an effect).
struct BoundCreateUser {
    std::string name;
    std::string password;
};

struct BoundAlterUser {
    std::string name;
    std::string password;
};

struct BoundDropUser {
    std::string name;
};

using BoundStatement = std::variant<BoundCreateTable, BoundDropTable, BoundCreateView,
                                    BoundDropView, BoundInsert, BoundSelect, BoundUpdate,
                                    BoundDelete, BoundBegin, BoundCommit, BoundRollback,
                                    BoundCreateUser, BoundAlterUser, BoundDropUser>;

}  // namespace ledger
