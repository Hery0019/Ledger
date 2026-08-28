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

struct BoundExpr {
    std::variant<Value, BoundColumn, BoundUnary, BoundBinary, BoundIsNull, BoundCast> node;
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

struct BoundInsert {
    const TableSchema* table;
    Row row;  // complete, in schema order, types already conforming
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

    bool aggregated = false;
    std::vector<BoundExprPtr> groupBy;      // evaluated on source rows
    std::vector<BoundAggregate> aggregates;
    BoundExprPtr having;                    // nullptr = keep every group
};

struct BoundUpdate {
    const TableSchema* table;
    std::vector<std::pair<std::size_t, BoundExprPtr>> assignments;  // type conforms to the column
    BoundExprPtr where;
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

using BoundStatement = std::variant<BoundCreateTable, BoundDropTable, BoundCreateView,
                                    BoundDropView, BoundInsert, BoundSelect, BoundUpdate,
                                    BoundDelete>;

}  // namespace ledger
