#include "semantic/binder.h"

#include <set>
#include <string>
#include <utility>

#include "semantic/eval.h"
#include "sql/parser.h"

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
    return makeError(ErrorCode::NotFound, "unknown table or view '" + name + "'");
}

// Bound to a view nesting level: a view cannot read from itself (creation
// requires the source to exist first), so this only guards against a
// hand-edited views file.
constexpr int kMaxViewDepth = 32;

std::optional<AggFunc> aggregateByName(std::string_view name) noexcept {
    if (name == "count") return AggFunc::Count;
    if (name == "sum") return AggFunc::Sum;
    if (name == "avg") return AggFunc::Avg;
    if (name == "min") return AggFunc::Min;
    if (name == "max") return AggFunc::Max;
    return std::nullopt;
}

// True if the expression contains an aggregate call anywhere.
bool containsAggregate(const ast::Expr& e) {
    return std::visit(
        [](const auto& n) -> bool {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, ast::Call>) {
                if (aggregateByName(n.name)) return true;
                for (const auto& a : n.args) {
                    if (containsAggregate(*a)) return true;
                }
                return false;
            } else if constexpr (std::is_same_v<N, ast::Unary>) {
                return containsAggregate(*n.operand);
            } else if constexpr (std::is_same_v<N, ast::Binary>) {
                return containsAggregate(*n.lhs) || containsAggregate(*n.rhs);
            } else if constexpr (std::is_same_v<N, ast::IsNull>) {
                return containsAggregate(*n.operand);
            } else {
                return false;
            }
        },
        e.node);
}

// One column visible to an expression: its output name, its type, and the
// bound expression that computes it over the underlying table's row. For a
// table that is a plain BoundColumn; for a view it is whatever the view
// selected (a column, or a computed expression).
struct Slot {
    std::string name;
    DataType type;
    BoundExprPtr expr;
};

// What a FROM clause resolves to: the underlying table, the columns visible
// through the (possibly nested) views, and the filters those views apply.
struct Source {
    const TableSchema* table;
    std::vector<Slot> slots;
    std::vector<BoundExprPtr> filters;  // one per view level with a WHERE
    std::string name;                   // as written in the FROM
    bool isView;
};

// Column scope of an expression. nullptr scope = no column allowed
// (INSERT ... VALUES).
struct Scope {
    const std::vector<Slot>* slots;
    std::string_view name;
    bool isView;

    [[nodiscard]] const Slot* resolve(std::string_view column) const noexcept {
        for (const auto& s : *slots) {
            if (s.name == column) return &s;
        }
        return nullptr;
    }

    // Position of a column in the slot list (= its index in the table's row
    // for a table scope).
    [[nodiscard]] std::optional<std::size_t> position(std::string_view column) const noexcept {
        for (std::size_t i = 0; i < slots->size(); ++i) {
            if ((*slots)[i].name == column) return i;
        }
        return std::nullopt;
    }

    [[nodiscard]] Error unknownColumn(const std::string& column) const {
        return makeError(ErrorCode::NotFound, "unknown column '" + column + "' in " +
                                                  (isView ? "view '" : "table '") +
                                                  std::string(name) + "'");
    }
};

// Binding context of an aggregated SELECT. Expressions bound with a group
// context read the group row: [group keys..., aggregate results...].
//  - a subtree textually equal to a GROUP BY expression -> key column;
//  - an aggregate call -> its argument is bound against the source scope and
//    the call becomes an aggregate-result column;
//  - any other column reference is an error.
struct GroupContext {
    std::vector<std::string> keyTexts;
    std::vector<DataType> keyTypes;
    const Scope* source;
    std::vector<BoundAggregate> aggregates;
    std::vector<DataType> aggTypes;
};

std::vector<Slot> tableSlots(const TableSchema& table) {
    std::vector<Slot> slots;
    slots.reserve(table.columns.size());
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        slots.push_back(Slot{table.columns[i].name, table.columns[i].type,
                             std::make_unique<BoundExpr>(BoundExpr{BoundColumn{i}, table.columns[i].type})});
    }
    return slots;
}

// Output name of a SELECT item without alias: a bare column keeps its name,
// anything else is named after its text (`a + 1`).
std::string itemName(const ast::SelectItem& item) {
    if (!item.alias.empty()) return item.alias;
    if (const auto* c = std::get_if<ast::ColumnRef>(&item.expr->node)) return c->name;
    return ast::exprToString(*item.expr);
}

class Binder {
public:
    explicit Binder(const Catalog& catalog) noexcept : catalog_(catalog) {}

    Result<BoundStatement> run(const ast::Statement& stmt) {
        return std::visit([&](const auto& s) -> Result<BoundStatement> { return bindStatement(s); },
                          stmt);
    }

private:
    // ---- DDL ---------------------------------------------------------------

    Result<BoundStatement> bindStatement(const ast::CreateTable& s) {
        if (catalog_.hasName(s.table)) {
            return makeError(ErrorCode::AlreadyExists, "'" + s.table + "' already exists");
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
        if (!catalog_.contains(s.table)) {
            if (catalog_.findView(s.table)) {
                return makeError(ErrorCode::SyntaxError,
                                 "'" + s.table + "' is a view; use DROP VIEW");
            }
            return makeError(ErrorCode::NotFound, "unknown table '" + s.table + "'");
        }
        LEDGER_TRY_VOID(checkNoDependents(s.table, "table"));
        return BoundStatement{BoundDropTable{s.table}};
    }

    Result<BoundStatement> bindStatement(const ast::CreateView& s) {
        if (catalog_.hasName(s.name)) {
            return makeError(ErrorCode::AlreadyExists, "'" + s.name + "' already exists");
        }
        // Binding the view's own SELECT validates it against the catalog.
        LEDGER_TRY(bound, bindStatement(s.query));
        const auto& sel = std::get<BoundSelect>(bound);
        if (sel.aggregated) {
            return makeError(ErrorCode::SyntaxError,
                             "view '" + s.name + "': aggregate functions are not allowed in a view");
        }
        // Every output column must be nameable and unique: they become the
        // columns other queries refer to.
        std::set<std::string> seen;
        for (std::size_t i = 0; i < s.query.items.size(); ++i) {
            const auto& item = s.query.items[i];
            if (item.alias.empty() && !std::holds_alternative<ast::ColumnRef>(item.expr->node)) {
                return makeError(ErrorCode::SyntaxError,
                                 "view '" + s.name + "': column " + std::to_string(i + 1) + " (" +
                                     ast::exprToString(*item.expr) + ") needs an alias (use AS)");
            }
        }
        for (const auto& name : sel.columnNames) {
            if (!seen.insert(name).second) {
                return makeError(ErrorCode::SyntaxError,
                                 "view '" + s.name + "': duplicate column name '" + name + "'");
            }
        }
        return BoundStatement{BoundCreateView{ViewDef{s.name, s.queryText}, s.query.table}};
    }

    Result<BoundStatement> bindStatement(const ast::DropView& s) {
        if (!catalog_.findView(s.name)) {
            if (catalog_.contains(s.name)) {
                return makeError(ErrorCode::SyntaxError,
                                 "'" + s.name + "' is a table; use DROP TABLE");
            }
            return makeError(ErrorCode::NotFound, "unknown view '" + s.name + "'");
        }
        LEDGER_TRY_VOID(checkNoDependents(s.name, "view"));
        return BoundStatement{BoundDropView{s.name}};
    }

    Result<void> checkNoDependents(const std::string& name, const char* what) const {
        const auto deps = catalog_.dependents(name);
        if (deps.empty()) return {};
        return makeError(ErrorCode::ConstraintViolation,
                         std::string(what) + " '" + name + "' is used by view '" +
                             std::string(deps.front()) + "'");
    }

    // ---- DML ---------------------------------------------------------------

    // Tables only: views are read-only.
    Result<const TableSchema*> writableTable(const std::string& name) const {
        if (const TableSchema* t = catalog_.find(name)) return t;
        if (catalog_.findView(name)) {
            return makeError(ErrorCode::SyntaxError, "'" + name + "' is a view; views are read-only");
        }
        return makeError(ErrorCode::NotFound, "unknown table '" + name + "'");
    }

    Result<BoundStatement> bindStatement(const ast::Insert& s) {
        LEDGER_TRY(table, writableTable(s.table));
        const std::vector<Slot> slots = tableSlots(*table);
        const Scope scope{&slots, table->name, false};

        // Target columns: the explicit list, or the whole schema in order.
        std::vector<std::size_t> targets;
        if (s.columns.empty()) {
            for (std::size_t i = 0; i < table->columns.size(); ++i) targets.push_back(i);
        } else {
            std::set<std::size_t> seen;
            for (const auto& name : s.columns) {
                LEDGER_TRY(idx, resolveColumn(scope, name));
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
        LEDGER_TRY(src, resolveSource(s.table, 0));
        const Scope scope{&src.slots, src.name, src.isView};

        BoundSelect out;
        out.table = src.table;
        out.limit = s.limit;

        // WHERE = every view filter AND the query's own filter. Never
        // aggregated: it runs before grouping.
        LEDGER_TRY(where, bindWhere(s.where, scope));
        std::vector<BoundExprPtr> filters = std::move(src.filters);
        if (where) filters.push_back(std::move(where));
        out.where = conjunction(std::move(filters));

        // Aggregated as soon as GROUP BY is present or an aggregate appears in
        // the projection, HAVING or ORDER BY.
        bool aggregated = !s.groupBy.empty() || (s.having != nullptr);
        for (const auto& item : s.items) aggregated = aggregated || containsAggregate(*item.expr);
        for (const auto& ob : s.orderBy) aggregated = aggregated || containsAggregate(*ob.expr);

        GroupContext group{{}, {}, &scope, {}, {}};
        GroupContext* g = aggregated ? &group : nullptr;
        if (aggregated) {
            if (s.star) {
                return makeError(ErrorCode::SyntaxError,
                                 "SELECT * cannot be used with GROUP BY or aggregate functions");
            }
            for (const auto& key : s.groupBy) {
                if (containsAggregate(*key)) {
                    return errorAt(*key, ErrorCode::SyntaxError,
                                   "aggregate functions are not allowed in GROUP BY");
                }
                LEDGER_TRY(e, bindExpr(*key, &scope));
                group.keyTexts.push_back(ast::exprToString(*key));
                group.keyTypes.push_back(e->type);
                out.groupBy.push_back(std::move(e));
            }
        }

        if (s.star) {
            for (const auto& slot : src.slots) {
                out.columnNames.push_back(slot.name);
                out.projection.push_back(cloneExpr(*slot.expr));
            }
        } else {
            for (const auto& item : s.items) {
                LEDGER_TRY(e, bindExpr(*item.expr, &scope, g));
                out.columnNames.push_back(itemName(item));
                out.projection.push_back(std::move(e));
            }
        }

        if (s.having) {
            LEDGER_TRY(h, bindExpr(*s.having, &scope, g));
            if (h->type != DataType::Bool && h->type != DataType::Null) {
                return errorAt(*s.having, ErrorCode::TypeError,
                               "HAVING must be BOOL, got " + typeName(h->type));
            }
            out.having = std::move(h);
        }

        // ORDER BY: an output alias first, then anything visible in the source
        // (so a hidden column can still order the result).
        for (const auto& ob : s.orderBy) {
            BoundExprPtr key;
            if (const auto* c = std::get_if<ast::ColumnRef>(&ob.expr->node)) {
                for (std::size_t i = 0; i < out.columnNames.size() && !key; ++i) {
                    if (out.columnNames[i] == c->name) key = cloneExpr(*out.projection[i]);
                }
            }
            if (!key) {
                LEDGER_TRY(e, bindExpr(*ob.expr, &scope, g));
                key = std::move(e);
            }
            out.orderBy.push_back(BoundOrderBy{std::move(key), ob.descending});
        }

        out.aggregated = aggregated;
        out.aggregates = std::move(group.aggregates);
        return BoundStatement{std::move(out)};
    }

    Result<BoundStatement> bindStatement(const ast::Update& s) {
        LEDGER_TRY(table, writableTable(s.table));
        const std::vector<Slot> slots = tableSlots(*table);
        const Scope scope{&slots, table->name, false};

        BoundUpdate out{table, {}, nullptr};
        std::set<std::size_t> seen;
        for (const auto& [name, expr] : s.assignments) {
            LEDGER_TRY(idx, resolveColumn(scope, name));
            if (!seen.insert(idx).second) {
                return makeError(ErrorCode::SyntaxError, "column '" + name + "' assigned twice");
            }
            LEDGER_TRY(e, bindExpr(*expr, &scope));
            LEDGER_TRY(fitted, fitToColumn(std::move(e), table->columns[idx], *expr));
            out.assignments.emplace_back(idx, std::move(fitted));
        }
        LEDGER_TRY(where, bindWhere(s.where, scope));
        out.where = std::move(where);
        return BoundStatement{std::move(out)};
    }

    Result<BoundStatement> bindStatement(const ast::Delete& s) {
        LEDGER_TRY(table, writableTable(s.table));
        const std::vector<Slot> slots = tableSlots(*table);
        const Scope scope{&slots, table->name, false};
        LEDGER_TRY(where, bindWhere(s.where, scope));
        return BoundStatement{BoundDelete{table, std::move(where)}};
    }

    // ---- views -------------------------------------------------------------

    // Resolves a FROM name through any number of views down to a table.
    Result<Source> resolveSource(const std::string& name, int depth) {
        if (const TableSchema* t = catalog_.find(name)) {
            return Source{t, tableSlots(*t), {}, name, false};
        }
        const ViewEntry* view = catalog_.findView(name);
        if (!view) return notFoundTable(name);
        if (depth >= kMaxViewDepth) {
            return makeError(ErrorCode::Corruption, "view '" + name + "': nesting too deep");
        }

        auto parsed = parse(view->def.sql);
        if (!parsed.ok()) return inView(name, parsed.error());
        const auto* query = std::get_if<ast::Select>(&parsed.value());
        if (!query) {
            return makeError(ErrorCode::Corruption, "view '" + name + "': definition is not a SELECT");
        }

        auto inner = resolveSource(query->table, depth + 1);
        if (!inner.ok()) return inView(name, inner.error());
        Source& base = inner.value();
        const Scope innerScope{&base.slots, base.name, base.isView};

        Source out{base.table, {}, std::move(base.filters), name, true};
        if (query->star) {
            for (const auto& slot : base.slots) {
                out.slots.push_back(Slot{slot.name, slot.type, cloneExpr(*slot.expr)});
            }
        } else {
            for (const auto& item : query->items) {
                auto e = bindExpr(*item.expr, &innerScope);
                if (!e.ok()) return inView(name, e.error());
                const DataType type = e.value()->type;
                out.slots.push_back(Slot{itemName(item), type, std::move(e).value()});
            }
        }
        if (query->where) {
            auto filter = bindWhere(query->where, innerScope);
            if (!filter.ok()) return inView(name, filter.error());
            out.filters.push_back(std::move(filter).value());
        }
        return out;
    }

    static Error inView(const std::string& view, const Error& e) {
        return makeError(e.code, "in view '" + view + "': " + e.message);
    }

    // AND of every filter; nullptr when there is none.
    static BoundExprPtr conjunction(std::vector<BoundExprPtr> filters) {
        BoundExprPtr acc;
        for (auto& f : filters) {
            if (!acc) {
                acc = std::move(f);
                continue;
            }
            const DataType type = (acc->type == DataType::Null && f->type == DataType::Null)
                                      ? DataType::Null
                                      : DataType::Bool;
            acc = make(BoundBinary{BinaryOp::And, std::move(acc), std::move(f)}, type);
        }
        return acc;
    }

    // ---- helpers -----------------------------------------------------------

    // Position of a column in a table scope (= its index in the row).
    static Result<std::size_t> resolveColumn(const Scope& scope, const std::string& name) {
        if (auto idx = scope.position(name)) return *idx;
        return scope.unknownColumn(name);
    }

    Result<BoundExprPtr> bindWhere(const ast::ExprPtr& where, const Scope& scope) {
        if (!where) return BoundExprPtr{};
        LEDGER_TRY(e, bindExpr(*where, &scope));
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

    // `scope`: the columns visible to the expression, or nullptr (VALUES).
    // `group`: set inside an aggregated SELECT (see GroupContext).
    Result<BoundExprPtr> bindExpr(const ast::Expr& e, const Scope* scope, GroupContext* group = nullptr) {
        LEDGER_TRY(bound, bindNode(e, scope, group));
        // Folding: every column-free subtree becomes a constant. A data error
        // (1/0, overflow) surfaces here, with its position.
        if (!std::holds_alternative<Value>(bound->node) && isConstant(*bound)) {
            auto v = eval(*bound, Row{});
            if (!v.ok()) return errorAt(e, v.error().code, v.error().message);
            return make(std::move(v).value(), bound->type);
        }
        return bound;
    }

    Result<BoundExprPtr> bindNode(const ast::Expr& e, const Scope* scope, GroupContext* group) {
        // In a group context, a subtree written exactly like a GROUP BY key
        // reads that key from the group row.
        if (group) {
            const std::string text = ast::exprToString(e);
            for (std::size_t i = 0; i < group->keyTexts.size(); ++i) {
                if (group->keyTexts[i] == text) return make(BoundColumn{i}, group->keyTypes[i]);
            }
        }
        return std::visit(
            [&](const auto& n) -> Result<BoundExprPtr> {
                using N = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<N, ast::Literal>) {
                    return make(n.value, n.value.type());
                } else if constexpr (std::is_same_v<N, ast::ColumnRef>) {
                    return bindColumn(e, n, scope, group);
                } else if constexpr (std::is_same_v<N, ast::Unary>) {
                    return bindUnary(e, n, scope, group);
                } else if constexpr (std::is_same_v<N, ast::Binary>) {
                    return bindBinary(e, n, scope, group);
                } else if constexpr (std::is_same_v<N, ast::IsNull>) {
                    LEDGER_TRY(operand, bindExpr(*n.operand, scope, group));
                    return make(BoundIsNull{std::move(operand), n.negated}, DataType::Bool);
                } else {
                    return bindCall(e, n, group);
                }
            },
            e.node);
    }

    Result<BoundExprPtr> bindColumn(const ast::Expr& e, const ast::ColumnRef& c, const Scope* scope,
                                    const GroupContext* group) {
        if (!scope) {
            return errorAt(e, ErrorCode::SyntaxError,
                           "column reference '" + c.name + "' is not allowed here");
        }
        const Slot* slot = scope->resolve(c.name);
        if (!slot) {
            const Error err = scope->unknownColumn(c.name);
            return errorAt(e, err.code, err.message);
        }
        if (group) {
            return errorAt(e, ErrorCode::SyntaxError,
                           "column '" + c.name +
                               "' must appear in GROUP BY or be used in an aggregate function");
        }
        return cloneExpr(*slot->expr);
    }

    // Aggregate arguments read source rows through the group's source scope;
    // scalar functions will bind their arguments in the caller's scope.
    Result<BoundExprPtr> bindCall(const ast::Expr& e, const ast::Call& call, GroupContext* group) {
        const auto agg = aggregateByName(call.name);
        if (!agg) {
            return errorAt(e, ErrorCode::SyntaxError, "unknown function '" + call.name + "'");
        }
        if (!group) {
            return errorAt(e, ErrorCode::SyntaxError,
                           "aggregate function '" + call.name + "()' is not allowed here");
        }
        if (call.star && *agg != AggFunc::Count) {
            return errorAt(e, ErrorCode::SyntaxError, call.name + "(*) is not valid; only count(*) is");
        }
        if (!call.star && call.args.size() != 1) {
            return errorAt(e, ErrorCode::SyntaxError,
                           call.name + "() takes exactly one argument, got " +
                               std::to_string(call.args.size()));
        }

        BoundAggregate bound{*agg, nullptr};
        DataType argType = DataType::Null;
        if (!call.star) {
            if (containsAggregate(*call.args[0])) {
                return errorAt(e, ErrorCode::SyntaxError, "aggregate functions cannot be nested");
            }
            // The argument reads source rows: bound against the source scope,
            // outside the group context.
            LEDGER_TRY(arg, bindExpr(*call.args[0], group->source));
            argType = arg->type;
            bound.arg = std::move(arg);
        }

        DataType result = DataType::Null;
        switch (*agg) {
            case AggFunc::Count:
                result = DataType::Int;
                break;
            case AggFunc::Sum:
            case AggFunc::Avg:
                if (!isNumeric(argType) && argType != DataType::Null) {
                    return errorAt(e, ErrorCode::TypeError,
                                   call.name + "() requires INT or FLOAT, got " + typeName(argType));
                }
                result = argType == DataType::Null ? DataType::Null
                         : (*agg == AggFunc::Avg)  ? DataType::Float
                                                   : argType;
                break;
            case AggFunc::Min:
            case AggFunc::Max:
                result = argType;
                break;
        }

        const std::size_t index = group->keyTexts.size() + group->aggregates.size();
        group->aggregates.push_back(std::move(bound));
        group->aggTypes.push_back(result);
        return make(BoundColumn{index}, result);
    }

    Result<BoundExprPtr> bindUnary(const ast::Expr& e, const ast::Unary& u, const Scope* scope,
                                   GroupContext* group) {
        LEDGER_TRY(operand, bindExpr(*u.operand, scope, group));
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

    Result<BoundExprPtr> bindBinary(const ast::Expr& e, const ast::Binary& b, const Scope* scope,
                                    GroupContext* group) {
        LEDGER_TRY(lhs, bindExpr(*b.lhs, scope, group));
        LEDGER_TRY(rhs, bindExpr(*b.rhs, scope, group));
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
