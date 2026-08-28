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

struct BoundOrderBy {
    std::size_t column;
    bool descending;
};

struct BoundSelect {
    const TableSchema* table;
    std::vector<std::size_t> projection;  // never empty (`*` is expanded)
    BoundExprPtr where;                   // nullptr = every row; type Bool or Null
    std::optional<BoundOrderBy> orderBy;
    std::optional<std::int64_t> limit;
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
    ViewDef def;         // validated: the SELECT binds against the current catalog
    std::string source;  // table or view it reads from
};

struct BoundDropView {
    std::string name;
};

using BoundStatement = std::variant<BoundCreateTable, BoundDropTable, BoundCreateView,
                                    BoundDropView, BoundInsert, BoundSelect, BoundUpdate,
                                    BoundDelete>;

}  // namespace ledger
