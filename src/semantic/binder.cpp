#include "semantic/binder.h"

#include <set>
#include <string>
#include <utility>

#include "semantic/eval.h"

namespace ledger {

namespace {

using ast::BinaryOp;
using ast::UnaryOp;

std::string typeName(DataType t) { return std::string(dataTypeName(t)); }

bool isNumeric(DataType t) noexcept { return t == DataType::Int || t == DataType::Float; }

bool isComparison(BinaryOp op) noexcept {
    return op == BinaryOp::Eq || op == BinaryOp::NotEq || op == BinaryOp::Lt ||
           op == BinaryOp::LtEq || op == BinaryOp::Gt || op == BinaryOp::GtEq;
}

bool isLogical(BinaryOp op) noexcept { return op == BinaryOp::And || op == BinaryOp::Or; }

Error notFoundTable(const std::string& name) {
    return makeError(ErrorCode::NotFound, "unknown table '" + name + "'");
}

class Binder {
public:
    explicit Binder(const Catalog& catalog) noexcept : catalog_(catalog) {}

    Result<BoundStatement> run(const ast::Statement& stmt) {
        return std::visit([&](const auto& s) -> Result<BoundStatement> { return bindStatement(s); },
                          stmt);
    }

private:
    // ---- statements --------------------------------------------------------

    Result<BoundStatement> bindStatement(const ast::CreateTable& s) {
        if (catalog_.contains(s.table)) {
            return makeError(ErrorCode::AlreadyExists, "table '" + s.table + "' already exists");
        }
        TableSchema schema{s.table, {}};
        std::set<std::string> seen;
        bool hasPrimaryKey = false;
        for (const auto& c : s.columns) {
            if (!seen.insert(c.name).second) {
                return makeError(ErrorCode::SyntaxError, "duplicate column '" + c.name + "'");
            }
            if (c.primaryKey) {
                if (hasPrimaryKey) {
                    return makeError(ErrorCode::SyntaxError,
                                     "table '" + s.table + "' has more than one PRIMARY KEY");
                }
                hasPrimaryKey = true;
            }
            schema.columns.push_back(ColumnSchema{c.name, c.type, c.primaryKey,
                                                  c.notNull || c.primaryKey});
        }
        return BoundStatement{BoundCreateTable{std::move(schema)}};
    }

    Result<BoundStatement> bindStatement(const ast::DropTable& s) {
        if (!catalog_.contains(s.table)) return notFoundTable(s.table);
        return BoundStatement{BoundDropTable{s.table}};
    }

    Result<BoundStatement> bindStatement(const ast::Insert& s) {
        const TableSchema* table = catalog_.find(s.table);
        if (!table) return notFoundTable(s.table);

        // Target columns: the explicit list, or the whole schema in order.
        std::vector<std::size_t> targets;
        if (s.columns.empty()) {
            for (std::size_t i = 0; i < table->columns.size(); ++i) targets.push_back(i);
        } else {
            std::set<std::size_t> seen;
            for (const auto& name : s.columns) {
                LEDGER_TRY(idx, resolveColumn(*table, name));
                if (!seen.insert(idx).second) {
                    return makeError(ErrorCode::SyntaxError, "duplicate column '" + name + "'");
                }
                targets.push_back(idx);
            }
        }
        if (s.values.size() != targets.size()) {
            return makeError(ErrorCode::SyntaxError,
                             "expected " + std::to_string(targets.size()) + " value" +
                                 (targets.size() == 1 ? "" : "s") + ", got " +
                                 std::to_string(s.values.size()));
        }

        Row row(table->columns.size(), Value::null());
        std::vector<bool> provided(table->columns.size(), false);
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const ColumnSchema& col = table->columns[targets[i]];
            // No current row inside VALUES: every column is forbidden there.
            LEDGER_TRY(e, bindExpr(*s.values[i], nullptr));
            LEDGER_TRY(fitted, fitToColumn(std::move(e), col, *s.values[i]));
            // Without a column, the expression was folded: it is a Value.
            const Value* v = std::get_if<Value>(&fitted->node);
            if (!v) return makeError(ErrorCode::Internal, "INSERT value did not fold to a constant");
            row[targets[i]] = *v;
            provided[targets[i]] = true;
        }
        for (std::size_t i = 0; i < table->columns.size(); ++i) {
            if (!provided[i] && table->columns[i].notNull) {
                return makeError(ErrorCode::ConstraintViolation,
                                 "column '" + table->columns[i].name + "' cannot be NULL");
            }
        }
        return BoundStatement{BoundInsert{table, std::move(row)}};
    }

    Result<BoundStatement> bindStatement(const ast::Select& s) {
        const TableSchema* table = catalog_.find(s.table);
        if (!table) return notFoundTable(s.table);

        BoundSelect out{table, {}, nullptr, std::nullopt, s.limit};
        if (s.columns.empty()) {
            for (std::size_t i = 0; i < table->columns.size(); ++i) out.projection.push_back(i);
        } else {
            for (const auto& name : s.columns) {
                LEDGER_TRY(idx, resolveColumn(*table, name));
                out.projection.push_back(idx);
            }
        }
        LEDGER_TRY(where, bindWhere(s.where, *table));
        out.where = std::move(where);
        if (s.orderBy) {
            LEDGER_TRY(idx, resolveColumn(*table, s.orderBy->column));
            out.orderBy = BoundOrderBy{idx, s.orderBy->descending};
        }
        return BoundStatement{std::move(out)};
    }

    Result<BoundStatement> bindStatement(const ast::Update& s) {
        const TableSchema* table = catalog_.find(s.table);
        if (!table) return notFoundTable(s.table);

        BoundUpdate out{table, {}, nullptr};
        std::set<std::size_t> seen;
        for (const auto& [name, expr] : s.assignments) {
            LEDGER_TRY(idx, resolveColumn(*table, name));
            if (!seen.insert(idx).second) {
                return makeError(ErrorCode::SyntaxError, "column '" + name + "' assigned twice");
            }
            LEDGER_TRY(e, bindExpr(*expr, table));
            LEDGER_TRY(fitted, fitToColumn(std::move(e), table->columns[idx], *expr));
            out.assignments.emplace_back(idx, std::move(fitted));
        }
        LEDGER_TRY(where, bindWhere(s.where, *table));
        out.where = std::move(where);
        return BoundStatement{std::move(out)};
    }

    Result<BoundStatement> bindStatement(const ast::Delete& s) {
        const TableSchema* table = catalog_.find(s.table);
        if (!table) return notFoundTable(s.table);
        LEDGER_TRY(where, bindWhere(s.where, *table));
        return BoundStatement{BoundDelete{table, std::move(where)}};
    }

    // ---- helpers -----------------------------------------------------------

    static Result<std::size_t> resolveColumn(const TableSchema& table, const std::string& name) {
        if (auto idx = table.columnIndex(name)) return *idx;
        return makeError(ErrorCode::NotFound,
                         "unknown column '" + name + "' in table '" + table.name + "'");
    }

    Result<BoundExprPtr> bindWhere(const ast::ExprPtr& where, const TableSchema& table) {
        if (!where) return BoundExprPtr{};
        LEDGER_TRY(e, bindExpr(*where, &table));
        if (e->type != DataType::Bool && e->type != DataType::Null) {
            return errorAt(*where, ErrorCode::TypeError,
                           "WHERE must be BOOL, got " + typeName(e->type));
        }
        return e;
    }

    // Checks that an expression can feed a column; inserts the Int -> Float
    // cast when needed; refuses a guaranteed NULL on a NOT NULL column.
    Result<BoundExprPtr> fitToColumn(BoundExprPtr e, const ColumnSchema& col, const ast::Expr& src) {
        if (e->type == DataType::Null) {
            if (col.notNull) {
                return errorAt(src, ErrorCode::ConstraintViolation,
                               "column '" + col.name + "' cannot be NULL");
            }
            return e;
        }
        if (e->type == col.type) return e;
        if (e->type == DataType::Int && col.type == DataType::Float) {
            if (const Value* v = std::get_if<Value>(&e->node)) {
                // Constant: converted right away rather than cast on every row.
                LEDGER_TRY(f, Value::real(static_cast<double>(v->asInt())));
                return std::make_unique<BoundExpr>(BoundExpr{std::move(f), DataType::Float});
            }
            return std::make_unique<BoundExpr>(
                BoundExpr{BoundCast{std::move(e), DataType::Float}, DataType::Float});
        }
        return errorAt(src, ErrorCode::TypeError,
                       "column '" + col.name + "' expects " + typeName(col.type) + ", got " +
                           typeName(e->type));
    }

    static Error errorAt(const ast::Expr& e, ErrorCode code, const std::string& what) {
        return makeError(code, std::to_string(e.line) + ":" + std::to_string(e.column) + ": " + what);
    }

    static BoundExprPtr make(decltype(BoundExpr::node) node, DataType type) {
        return std::make_unique<BoundExpr>(BoundExpr{std::move(node), type});
    }

    // ---- expressions -------------------------------------------------------

    // `scope`: the table whose columns are visible, or nullptr (VALUES).
    Result<BoundExprPtr> bindExpr(const ast::Expr& e, const TableSchema* scope) {
        LEDGER_TRY(bound, bindNode(e, scope));
        // Folding: every column-free subtree becomes a constant. A data error
        // (1/0, overflow) surfaces here, with its position.
        if (!std::holds_alternative<Value>(bound->node) && isConstant(*bound)) {
            auto v = eval(*bound, Row{});
            if (!v.ok()) return errorAt(e, v.error().code, v.error().message);
            return make(std::move(v).value(), bound->type);
        }
        return bound;
    }

    Result<BoundExprPtr> bindNode(const ast::Expr& e, const TableSchema* scope) {
        return std::visit(
            [&](const auto& n) -> Result<BoundExprPtr> {
                using N = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<N, ast::Literal>) {
                    return make(n.value, n.value.type());
                } else if constexpr (std::is_same_v<N, ast::ColumnRef>) {
                    return bindColumn(e, n, scope);
                } else if constexpr (std::is_same_v<N, ast::Unary>) {
                    return bindUnary(e, n, scope);
                } else if constexpr (std::is_same_v<N, ast::Binary>) {
                    return bindBinary(e, n, scope);
                } else {
                    LEDGER_TRY(operand, bindExpr(*n.operand, scope));
                    return make(BoundIsNull{std::move(operand), n.negated}, DataType::Bool);
                }
            },
            e.node);
    }

    Result<BoundExprPtr> bindColumn(const ast::Expr& e, const ast::ColumnRef& c,
                                    const TableSchema* scope) {
        if (!scope) {
            return errorAt(e, ErrorCode::SyntaxError,
                           "column reference '" + c.name + "' is not allowed here");
        }
        auto idx = scope->columnIndex(c.name);
        if (!idx) {
            return errorAt(e, ErrorCode::NotFound,
                           "unknown column '" + c.name + "' in table '" + scope->name + "'");
        }
        return make(BoundColumn{*idx}, scope->columns[*idx].type);
    }

    Result<BoundExprPtr> bindUnary(const ast::Expr& e, const ast::Unary& u, const TableSchema* scope) {
        LEDGER_TRY(operand, bindExpr(*u.operand, scope));
        const DataType t = operand->type;
        switch (u.op) {
            case UnaryOp::Not:
                if (t != DataType::Bool && t != DataType::Null) {
                    return errorAt(e, ErrorCode::TypeError, "NOT requires BOOL, got " + typeName(t));
                }
                break;
            case UnaryOp::Neg:
                if (!isNumeric(t) && t != DataType::Null) {
                    return errorAt(e, ErrorCode::TypeError,
                                   "unary '-' requires INT or FLOAT, got " + typeName(t));
                }
                break;
        }
        return make(BoundUnary{u.op, std::move(operand)}, t);
    }

    Result<BoundExprPtr> bindBinary(const ast::Expr& e, const ast::Binary& b, const TableSchema* scope) {
        LEDGER_TRY(lhs, bindExpr(*b.lhs, scope));
        LEDGER_TRY(rhs, bindExpr(*b.rhs, scope));
        const DataType lt = lhs->type;
        const DataType rt = rhs->type;
        const std::string opName(ast::binaryOpName(b.op));

        DataType result;
        if (isLogical(b.op)) {
            for (DataType t : {lt, rt}) {
                if (t != DataType::Bool && t != DataType::Null) {
                    return errorAt(e, ErrorCode::TypeError, opName + " requires BOOL, got " + typeName(t));
                }
            }
            result = (lt == DataType::Null && rt == DataType::Null) ? DataType::Null : DataType::Bool;
        } else if (isComparison(b.op)) {
            const bool compatible = lt == DataType::Null || rt == DataType::Null || lt == rt ||
                                    (isNumeric(lt) && isNumeric(rt));
            if (!compatible) {
                return errorAt(e, ErrorCode::TypeError,
                               "cannot compare " + typeName(lt) + " with " + typeName(rt));
            }
            result = (lt == DataType::Null && rt == DataType::Null) ? DataType::Null : DataType::Bool;
        } else {
            for (DataType t : {lt, rt}) {
                if (!isNumeric(t) && t != DataType::Null) {
                    return errorAt(e, ErrorCode::TypeError,
                                   "cannot apply '" + opName + "' to " + typeName(lt) + " and " +
                                       typeName(rt));
                }
            }
            if (lt == DataType::Null) result = rt;
            else if (rt == DataType::Null) result = lt;
            else if (lt == DataType::Int && rt == DataType::Int) result = DataType::Int;
            else result = DataType::Float;
        }
        return make(BoundBinary{b.op, std::move(lhs), std::move(rhs)}, result);
    }

    const Catalog& catalog_;
};

}  // namespace

Result<BoundStatement> bind(const ast::Statement& stmt, const Catalog& catalog) {
    return Binder{catalog}.run(stmt);
}

}  // namespace ledger
