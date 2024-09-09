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

struct Literal {
    Value value;  // Int/Float/Text/Bool/Null, already converted by the parser
};

struct ColumnRef {
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

struct Expr {
    std::variant<Literal, ColumnRef, Unary, Binary, IsNull> node;
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

struct OrderBy {
    std::string column;
    bool descending;
};

struct Select {
    std::vector<std::string> columns;  // empty = `*`
    std::string table;
    ExprPtr where;  // nullptr if absent
    std::optional<OrderBy> orderBy;
    std::optional<std::int64_t> limit;  // >= 0 if present
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

using Statement = std::variant<CreateTable, DropTable, Insert, Select, Update, Delete>;

}  // namespace ledger::ast
