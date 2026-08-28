#include "semantic/binder.h"

#include <cstdint>
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
            } else if constexpr (std::is_same_v<N, ast::InList>) {
                if (containsAggregate(*n.value)) return true;
                for (const auto& item : n.items) {
                    if (containsAggregate(*item)) return true;
                }
                return false;
            } else if constexpr (std::is_same_v<N, ast::Between>) {
                return containsAggregate(*n.value) || containsAggregate(*n.low) || containsAggregate(*n.high);
            } else if constexpr (std::is_same_v<N, ast::Like>) {
                return containsAggregate(*n.value) || containsAggregate(*n.pattern);
            } else if constexpr (std::is_same_v<N, ast::Case>) {
                if (n.operand && containsAggregate(*n.operand)) return true;
                if (n.elseExpr && containsAggregate(*n.elseExpr)) return true;
                for (const auto& [c, r] : n.whens) {
                    if (containsAggregate(*c) || containsAggregate(*r)) return true;
                }
                return false;
            } else if constexpr (std::is_same_v<N, ast::InSelect>) {
                return containsAggregate(*n.value);  // the subquery's own aggregates are its own
            } else {
                return false;  // Literal, ColumnRef, Exists, ScalarSubquery
            }
        },
        e.node);
}

std::optional<ScalarFunc> scalarByName(std::string_view name) noexcept {
    if (name == "upper") return ScalarFunc::Upper;
    if (name == "lower") return ScalarFunc::Lower;
    if (name == "length") return ScalarFunc::Length;
    if (name == "trim") return ScalarFunc::Trim;
    if (name == "abs") return ScalarFunc::Abs;
    if (name == "round") return ScalarFunc::Round;
    if (name == "coalesce") return ScalarFunc::Coalesce;
    if (name == "nullif") return ScalarFunc::NullIf;
    return std::nullopt;
}

// Types that `=` accepts between each other (see bindBinary).
bool comparable(DataType a, DataType b) noexcept {
    return a == DataType::Null || b == DataType::Null || a == b ||
           (isNumeric(a) && isNumeric(b));
}

// Column scope of an expression: the output columns of a relation. nullptr
// scope = no column allowed (INSERT ... VALUES).
struct Scope {
    const std::vector<RelColumn>* columns;
    std::string_view name;  // single source: its alias, for messages
    bool isView;            // single source: table or view, for messages
    bool multi;             // several sources (joins): messages omit the source

    Result<std::size_t> resolve(const ast::ColumnRef& c) const {
        std::vector<std::size_t> found;
        bool qualifierSeen = false;
        for (std::size_t i = 0; i < columns->size(); ++i) {
            const RelColumn& col = (*columns)[i];
            if (!c.qualifier.empty()) {
                if (col.qualifier != c.qualifier) continue;
                qualifierSeen = true;
            }
            if (col.name == c.name) found.push_back(i);
        }
        if (found.size() == 1) return found[0];
        if (found.size() > 1) {
            return makeError(ErrorCode::SyntaxError,
                             "ambiguous column '" + c.name + "' (use " + (*columns)[found[0]].qualifier +
                                 "." + c.name + " or " + (*columns)[found[1]].qualifier + "." + c.name + ")");
        }
        if (!c.qualifier.empty()) {
            if (!qualifierSeen) {
                return makeError(ErrorCode::NotFound, "unknown table alias '" + c.qualifier + "'");
            }
            return makeError(ErrorCode::NotFound, "unknown column '" + c.qualifier + "." + c.name + "'");
        }
        if (multi) return makeError(ErrorCode::NotFound, "unknown column '" + c.name + "'");
        return makeError(ErrorCode::NotFound, "unknown column '" + c.name + "' in " +
                                                  (isView ? "view '" : "table '") + std::string(name) + "'");
    }

    // Unqualified lookup by name only, for INSERT/UPDATE column lists.
    Result<std::size_t> resolve(const std::string& column) const {
        return resolve(ast::ColumnRef{{}, column});
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

std::vector<RelColumn> tableColumns(const TableSchema& table, const std::string& qualifier) {
    std::vector<RelColumn> out;
    out.reserve(table.columns.size());
    for (const auto& c : table.columns) out.push_back(RelColumn{qualifier, c.name, c.type});
    return out;
}

// Output name of a SELECT item without alias: a bare column keeps its name,
// anything else is named after its text (`a + 1`).
std::string itemName(const ast::SelectItem& item) {
    if (!item.alias.empty()) return item.alias;
    if (const auto* c = std::get_if<ast::ColumnRef>(&item.expr->node)) return c->name;
    return ast::exprToString(*item.expr);
}

BoundRelationPtr makeRelation(decltype(BoundRelation::node) node, std::vector<RelColumn> columns) {
    return std::make_unique<BoundRelation>(BoundRelation{std::move(node), std::move(columns)});
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
            if (!item.expr) continue;  // t.*: plain columns
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
        std::vector<std::string> sources{s.query.from.name};
        for (const auto& j : s.query.joins) sources.push_back(j.table.name);
        return BoundStatement{BoundCreateView{ViewDef{s.name, s.queryText}, std::move(sources)}};
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
        const std::vector<RelColumn> columns = tableColumns(*table, table->name);
        const Scope scope{&columns, table->name, false, false};

        // Target columns: the explicit list, or the whole schema in order.
        std::vector<std::size_t> targets;
        if (s.columns.empty()) {
            for (std::size_t i = 0; i < table->columns.size(); ++i) targets.push_back(i);
        } else {
            std::set<std::size_t> seen;
            for (const auto& name : s.columns) {
                LEDGER_TRY(idx, scope.resolve(name));
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
        BoundSelect out;
        // Subqueries met while binding this SELECT register themselves here.
        std::vector<std::unique_ptr<BoundSelect>>* const savedSubqueries = subqueries_;
        subqueries_ = &out.subqueries;
        auto result = bindSelectBody(s, out);
        subqueries_ = savedSubqueries;
        LEDGER_TRY_VOID(result);

        // UNION members: bound as independent SELECTs, matched column by
        // column. ORDER BY then reads the output row.
        for (const auto& member : s.unions) {
            LEDGER_TRY(bound, bindStatement(*member.select));
            auto sel = std::make_unique<BoundSelect>(std::get<BoundSelect>(std::move(bound)));
            if (sel->columnNames.size() != out.columnNames.size()) {
                return makeError(ErrorCode::SyntaxError,
                                 "UNION members must have the same number of columns (" +
                                     std::to_string(out.columnNames.size()) + " and " +
                                     std::to_string(sel->columnNames.size()) + ")");
            }
            for (std::size_t i = 0; i < sel->projection.size(); ++i) {
                const DataType a = out.projection[i]->type, b = sel->projection[i]->type;
                if (!comparable(a, b)) {
                    return makeError(ErrorCode::TypeError,
                                     "UNION column " + std::to_string(i + 1) + " mixes " + typeName(a) +
                                         " and " + typeName(b));
                }
            }
            out.unions.push_back(BoundSelect::UnionMember{member.all, std::move(sel)});
        }
        if (!s.unions.empty()) {
            // ORDER BY over the union result: output names only.
            out.orderBy.clear();
            for (const auto& ob : s.orderBy) {
                const auto* c = std::get_if<ast::ColumnRef>(&ob.expr->node);
                std::optional<std::size_t> idx;
                if (c && c->qualifier.empty()) {
                    for (std::size_t i = 0; i < out.columnNames.size() && !idx; ++i) {
                        if (out.columnNames[i] == c->name) idx = i;
                    }
                }
                if (!idx) {
                    return errorAt(*ob.expr, ErrorCode::SyntaxError,
                                   "ORDER BY on a UNION must name an output column");
                }
                out.orderBy.push_back(BoundOrderBy{make(BoundColumn{*idx}, out.projection[*idx]->type),
                                                   ob.descending});
            }
        }
        return BoundStatement{std::move(out)};
    }

    Result<void> bindSelectBody(const ast::Select& s, BoundSelect& out) {
        LEDGER_TRY(relation, fromClause(s, 0));
        const Scope scope{&relation->columns, s.from.alias, catalog_.findView(s.from.name) != nullptr,
                          !s.joins.empty()};

        out.limit = s.limit;
        out.offset = s.offset;
        out.distinct = s.distinct;

        // WHERE runs on the relation's rows, before any grouping.
        LEDGER_TRY(where, bindWhere(s.where, scope));
        out.where = std::move(where);

        // Aggregated as soon as GROUP BY is present or an aggregate appears in
        // the projection, HAVING or ORDER BY.
        bool aggregated = !s.groupBy.empty() || (s.having != nullptr);
        for (const auto& item : s.items) aggregated = aggregated || (item.expr && containsAggregate(*item.expr));
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
            for (std::size_t i = 0; i < relation->columns.size(); ++i) {
                out.columnNames.push_back(relation->columns[i].name);
                out.projection.push_back(make(BoundColumn{i}, relation->columns[i].type));
            }
        } else {
            for (const auto& item : s.items) {
                if (!item.expr) {
                    // t.*: every column of that source.
                    if (aggregated) {
                        return makeError(ErrorCode::SyntaxError,
                                         item.starOf + ".* cannot be used with GROUP BY or aggregate functions");
                    }
                    bool any = false;
                    for (std::size_t i = 0; i < relation->columns.size(); ++i) {
                        if (relation->columns[i].qualifier != item.starOf) continue;
                        any = true;
                        out.columnNames.push_back(relation->columns[i].name);
                        out.projection.push_back(make(BoundColumn{i}, relation->columns[i].type));
                    }
                    if (!any) return makeError(ErrorCode::NotFound, "unknown table alias '" + item.starOf + "'");
                    continue;
                }
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
            if (const auto* c = std::get_if<ast::ColumnRef>(&ob.expr->node); c && c->qualifier.empty()) {
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

        out.relation = std::move(relation);
        out.aggregated = aggregated;
        out.aggregates = std::move(group.aggregates);
        return {};
    }

    Result<BoundStatement> bindStatement(const ast::Update& s) {
        LEDGER_TRY(table, writableTable(s.table));
        const std::vector<RelColumn> columns = tableColumns(*table, table->name);
        const Scope scope{&columns, table->name, false, false};

        BoundUpdate out{table, {}, nullptr};
        std::set<std::size_t> seen;
        for (const auto& [name, expr] : s.assignments) {
            LEDGER_TRY(idx, scope.resolve(name));
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
        const std::vector<RelColumn> columns = tableColumns(*table, table->name);
        const Scope scope{&columns, table->name, false, false};
        LEDGER_TRY(where, bindWhere(s.where, scope));
        return BoundStatement{BoundDelete{table, std::move(where)}};
    }

    // ---- relations ---------------------------------------------------------

    // One FROM/JOIN entry: a table scan, or a view expanded into its own
    // relation, with every column qualified by the entry's alias.
    Result<BoundRelationPtr> relationFor(const ast::TableRef& ref, int depth) {
        if (const TableSchema* t = catalog_.find(ref.name)) {
            return makeRelation(RelScan{t}, tableColumns(*t, ref.alias));
        }
        const ViewEntry* view = catalog_.findView(ref.name);
        if (!view) return makeError(ErrorCode::NotFound, "unknown table or view '" + ref.name + "'");
        if (depth >= kMaxViewDepth) {
            return makeError(ErrorCode::Corruption, "view '" + ref.name + "': nesting too deep");
        }

        auto parsed = parse(view->def.sql);
        if (!parsed.ok()) return inView(ref.name, parsed.error());
        const auto* query = std::get_if<ast::Select>(&parsed.value());
        if (!query) {
            return makeError(ErrorCode::Corruption, "view '" + ref.name + "': definition is not a SELECT");
        }
        if (!query->groupBy.empty() || query->having) {
            return makeError(ErrorCode::Corruption, "view '" + ref.name + "': definition aggregates");
        }

        auto rel = viewRelation(*query, depth + 1);
        if (!rel.ok()) return inView(ref.name, rel.error());
        for (auto& col : rel.value()->columns) col.qualifier = ref.alias;
        return std::move(rel).value();
    }

    // FROM tableRef {JOIN tableRef ON expr}: scans / views combined by joins.
    Result<BoundRelationPtr> fromClause(const ast::Select& s, int depth) {
        std::set<std::string> aliases{s.from.alias};
        LEDGER_TRY(rel, relationFor(s.from, depth));
        for (const auto& j : s.joins) {
            if (!aliases.insert(j.table.alias).second) {
                return makeError(ErrorCode::SyntaxError, "duplicate table alias '" + j.table.alias + "'");
            }
            LEDGER_TRY(right, relationFor(j.table, depth));
            std::vector<RelColumn> columns = rel->columns;
            columns.insert(columns.end(), right->columns.begin(), right->columns.end());
            const Scope scope{&columns, {}, false, true};
            LEDGER_TRY(on, bindExpr(*j.on, &scope));
            if (on->type != DataType::Bool && on->type != DataType::Null) {
                return errorAt(*j.on, ErrorCode::TypeError, "ON must be BOOL, got " + typeName(on->type));
            }
            if (containsAggregate(*j.on)) {
                return errorAt(*j.on, ErrorCode::SyntaxError, "aggregate functions are not allowed in ON");
            }
            const JoinKind kind = j.kind == ast::JoinKind::Left ? JoinKind::Left : JoinKind::Inner;
            rel = makeRelation(RelJoin{kind, std::move(rel), std::move(right), std::move(on)},
                               std::move(columns));
        }
        return rel;
    }

    // A view's body as a relation: FROM/JOINs, then WHERE, then its projection.
    Result<BoundRelationPtr> viewRelation(const ast::Select& q, int depth) {
        LEDGER_TRY(rel, fromClause(q, depth));
        const Scope scope{&rel->columns, q.from.alias, catalog_.findView(q.from.name) != nullptr,
                          !q.joins.empty()};
        LEDGER_TRY(where, bindWhere(q.where, scope));
        if (where) {
            std::vector<RelColumn> columns = rel->columns;
            rel = makeRelation(RelFilter{std::move(rel), std::move(where)}, std::move(columns));
        }
        // Re-bind against the filtered relation (same layout).
        const Scope filtered{&rel->columns, q.from.alias, false, !q.joins.empty()};
        std::vector<BoundExprPtr> exprs;
        std::vector<RelColumn> columns;
        auto addColumn = [&](std::size_t i) {
            columns.push_back(RelColumn{{}, rel->columns[i].name, rel->columns[i].type});
            exprs.push_back(make(BoundColumn{i}, rel->columns[i].type));
        };
        if (q.star) {
            for (std::size_t i = 0; i < rel->columns.size(); ++i) addColumn(i);
        } else {
            for (const auto& item : q.items) {
                if (!item.expr) {
                    bool any = false;
                    for (std::size_t i = 0; i < rel->columns.size(); ++i) {
                        if (rel->columns[i].qualifier == item.starOf) {
                            addColumn(i);
                            any = true;
                        }
                    }
                    if (!any) return makeError(ErrorCode::NotFound, "unknown table alias '" + item.starOf + "'");
                    continue;
                }
                LEDGER_TRY(e, bindExpr(*item.expr, &filtered));
                columns.push_back(RelColumn{{}, itemName(item), e->type});
                exprs.push_back(std::move(e));
            }
        }
        return makeRelation(RelProject{std::move(rel), std::move(exprs)}, std::move(columns));
    }

    static Error inView(const std::string& view, const Error& e) {
        return makeError(e.code, "in view '" + view + "': " + e.message);
    }

    // ---- helpers -----------------------------------------------------------

    Result<BoundExprPtr> bindWhere(const ast::ExprPtr& where, const Scope& scope) {
        if (!where) return BoundExprPtr{};
        if (containsAggregate(*where)) {
            return errorAt(*where, ErrorCode::SyntaxError, "aggregate functions are not allowed in WHERE");
        }
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
                } else if constexpr (std::is_same_v<N, ast::InList>) {
                    return bindInList(e, n, scope, group);
                } else if constexpr (std::is_same_v<N, ast::Between>) {
                    return bindBetween(e, n, scope, group);
                } else if constexpr (std::is_same_v<N, ast::Like>) {
                    return bindLike(e, n, scope, group);
                } else if constexpr (std::is_same_v<N, ast::Case>) {
                    return bindCase(e, n, scope, group);
                } else if constexpr (std::is_same_v<N, ast::InSelect>) {
                    LEDGER_TRY(value, bindExpr(*n.value, scope, group));
                    LEDGER_TRY(slot, bindSubquery(e, *n.select, 1));
                    const DataType colType = subqueries_->at(slot)->projection[0]->type;
                    if (!comparable(value->type, colType)) {
                        return errorAt(e, ErrorCode::TypeError,
                                       "cannot compare " + typeName(value->type) + " with the subquery's " +
                                           typeName(colType));
                    }
                    const DataType type = value->type == DataType::Null ? DataType::Null : DataType::Bool;
                    return make(BoundInSubquery{std::move(value), slot, n.negated}, type);
                } else if constexpr (std::is_same_v<N, ast::Exists>) {
                    LEDGER_TRY(slot, bindSubquery(e, *n.select, 0));
                    return make(BoundExists{slot, n.negated}, DataType::Bool);
                } else if constexpr (std::is_same_v<N, ast::ScalarSubquery>) {
                    LEDGER_TRY(slot, bindSubquery(e, *n.select, 1));
                    return make(BoundScalarSubquery{slot}, subqueries_->at(slot)->projection[0]->type);
                } else {
                    if (scalarByName(n.name)) return bindScalar(e, n, scope, group);
                    return bindCall(e, n, group);
                }
            },
            e.node);
    }

    // Binds a nested SELECT on its own (no access to the enclosing scope:
    // correlated subqueries are not supported) and registers it in the
    // current statement's subquery list. `columns` = required output width,
    // 0 for "any" (EXISTS).
    Result<std::size_t> bindSubquery(const ast::Expr& e, const ast::Select& select, std::size_t columns) {
        if (!subqueries_) {
            return errorAt(e, ErrorCode::SyntaxError, "subqueries are only allowed inside a SELECT");
        }
        std::vector<std::unique_ptr<BoundSelect>>* outer = subqueries_;
        auto bound = bindStatement(select);  // sets and restores subqueries_ for the nested level
        subqueries_ = outer;
        if (!bound.ok()) {
            return makeError(bound.error().code, "in subquery: " + bound.error().message);
        }
        auto sub = std::make_unique<BoundSelect>(std::get<BoundSelect>(std::move(bound).value()));
        if (columns && sub->columnNames.size() != columns) {
            return errorAt(e, ErrorCode::SyntaxError,
                           "subquery must return exactly " + std::to_string(columns) + " column, got " +
                               std::to_string(sub->columnNames.size()));
        }
        subqueries_->push_back(std::move(sub));
        return subqueries_->size() - 1;
    }

    // Common type of several branches (CASE results, COALESCE arguments):
    // NULL-typed branches are ignored; numeric branches may mix (-> Float);
    // anything else must be one single type.
    Result<DataType> commonType(const ast::Expr& e, const std::vector<DataType>& types, const char* what) {
        DataType result = DataType::Null;
        for (const DataType t : types) {
            if (t == DataType::Null) continue;
            if (result == DataType::Null) {
                result = t;
            } else if (result == t) {
                continue;
            } else if (isNumeric(result) && isNumeric(t)) {
                result = DataType::Float;
            } else {
                return errorAt(e, ErrorCode::TypeError,
                               std::string(what) + " mixes " + typeName(result) + " and " + typeName(t));
            }
        }
        return result;
    }

    Result<BoundExprPtr> bindScalar(const ast::Expr& e, const ast::Call& call, const Scope* scope,
                                    GroupContext* group) {
        const ScalarFunc func = *scalarByName(call.name);
        if (call.star) return errorAt(e, ErrorCode::SyntaxError, call.name + "(*) is not valid");

        BoundCall bound{func, {}};
        std::vector<DataType> types;
        for (const auto& a : call.args) {
            LEDGER_TRY(x, bindExpr(*a, scope, group));
            types.push_back(x->type);
            bound.args.push_back(std::move(x));
        }
        const std::size_t n = bound.args.size();
        auto arity = [&](std::size_t lo, std::size_t hi) -> Result<void> {
            if (n >= lo && n <= hi) return {};
            const std::string want = lo == hi ? std::to_string(lo)
                                     : hi == SIZE_MAX ? "at least " + std::to_string(lo)
                                                      : std::to_string(lo) + " to " + std::to_string(hi);
            return errorAt(e, ErrorCode::SyntaxError,
                           call.name + "() takes " + want + " argument" + (lo == 1 && hi == 1 ? "" : "s") +
                               ", got " + std::to_string(n));
        };
        auto require = [&](std::size_t i, bool ok, const char* what) -> Result<void> {
            if (ok || types[i] == DataType::Null) return {};
            return errorAt(e, ErrorCode::TypeError,
                           call.name + "() requires " + what + ", got " + typeName(types[i]));
        };

        DataType result = DataType::Null;
        switch (func) {
            case ScalarFunc::Upper:
            case ScalarFunc::Lower:
            case ScalarFunc::Trim:
                LEDGER_TRY_VOID(arity(1, 1));
                LEDGER_TRY_VOID(require(0, types[0] == DataType::Text, "TEXT"));
                result = types[0] == DataType::Null ? DataType::Null : DataType::Text;
                break;
            case ScalarFunc::Length:
                LEDGER_TRY_VOID(arity(1, 1));
                LEDGER_TRY_VOID(require(0, types[0] == DataType::Text, "TEXT"));
                result = types[0] == DataType::Null ? DataType::Null : DataType::Int;
                break;
            case ScalarFunc::Abs:
                LEDGER_TRY_VOID(arity(1, 1));
                LEDGER_TRY_VOID(require(0, isNumeric(types[0]), "INT or FLOAT"));
                result = types[0];
                break;
            case ScalarFunc::Round:
                LEDGER_TRY_VOID(arity(1, 2));
                LEDGER_TRY_VOID(require(0, isNumeric(types[0]), "INT or FLOAT"));
                if (n == 2) LEDGER_TRY_VOID(require(1, types[1] == DataType::Int, "an INT digit count"));
                result = types[0] == DataType::Null ? DataType::Null : DataType::Float;
                break;
            case ScalarFunc::Coalesce: {
                LEDGER_TRY_VOID(arity(1, SIZE_MAX));
                LEDGER_TRY(t, commonType(e, types, "coalesce()"));
                result = t;
                break;
            }
            case ScalarFunc::NullIf:
                LEDGER_TRY_VOID(arity(2, 2));
                if (!comparable(types[0], types[1])) {
                    return errorAt(e, ErrorCode::TypeError,
                                   "nullif(): cannot compare " + typeName(types[0]) + " with " + typeName(types[1]));
                }
                result = types[0];
                break;
        }
        return make(std::move(bound), result);
    }

    Result<BoundExprPtr> bindCase(const ast::Expr& e, const ast::Case& c, const Scope* scope,
                                  GroupContext* group) {
        BoundExprPtr operand;
        if (c.operand) {
            LEDGER_TRY(op, bindExpr(*c.operand, scope, group));
            operand = std::move(op);
        }
        BoundCase bound{{}, nullptr};
        std::vector<DataType> types;
        for (const auto& [when, then] : c.whens) {
            LEDGER_TRY(cond, bindExpr(*when, scope, group));
            if (operand) {
                // Simple form: CASE x WHEN a ... is CASE WHEN x = a ...
                if (!comparable(operand->type, cond->type)) {
                    return errorAt(*when, ErrorCode::TypeError,
                                   "cannot compare " + typeName(operand->type) + " with " +
                                       typeName(cond->type) + " in CASE");
                }
                const DataType t = (operand->type == DataType::Null && cond->type == DataType::Null)
                                       ? DataType::Null
                                       : DataType::Bool;
                cond = make(BoundBinary{BinaryOp::Eq, cloneExpr(*operand), std::move(cond)}, t);
            } else if (cond->type != DataType::Bool && cond->type != DataType::Null) {
                return errorAt(*when, ErrorCode::TypeError, "WHEN requires BOOL, got " + typeName(cond->type));
            }
            LEDGER_TRY(result, bindExpr(*then, scope, group));
            types.push_back(result->type);
            bound.whens.emplace_back(std::move(cond), std::move(result));
        }
        if (c.elseExpr) {
            LEDGER_TRY(el, bindExpr(*c.elseExpr, scope, group));
            types.push_back(el->type);
            bound.elseExpr = std::move(el);
        }
        LEDGER_TRY(type, commonType(e, types, "CASE"));
        return make(std::move(bound), type);
    }

    Result<BoundExprPtr> bindInList([[maybe_unused]] const ast::Expr& e, const ast::InList& in, const Scope* scope,
                                    GroupContext* group) {
        LEDGER_TRY(value, bindExpr(*in.value, scope, group));
        BoundInList out{std::move(value), {}, in.negated};
        for (const auto& item : in.items) {
            LEDGER_TRY(x, bindExpr(*item, scope, group));
            if (!comparable(out.value->type, x->type)) {
                return errorAt(*item, ErrorCode::TypeError,
                               "cannot compare " + typeName(out.value->type) + " with " + typeName(x->type) +
                                   " in IN list");
            }
            out.items.push_back(std::move(x));
        }
        const DataType type = out.value->type == DataType::Null ? DataType::Null : DataType::Bool;
        return make(std::move(out), type);
    }

    // `x BETWEEN lo AND hi` is exactly `x >= lo AND x <= hi` (x evaluated
    // twice; fine, expressions are pure). NOT BETWEEN wraps it in NOT.
    Result<BoundExprPtr> bindBetween(const ast::Expr& e, const ast::Between& b, const Scope* scope,
                                     GroupContext* group) {
        LEDGER_TRY(value, bindExpr(*b.value, scope, group));
        LEDGER_TRY(low, bindExpr(*b.low, scope, group));
        LEDGER_TRY(high, bindExpr(*b.high, scope, group));
        for (const auto* bound : {&low, &high}) {
            if (!comparable(value->type, (*bound)->type)) {
                return errorAt(e, ErrorCode::TypeError,
                               "cannot compare " + typeName(value->type) + " with " +
                                   typeName((*bound)->type) + " in BETWEEN");
            }
        }
        const bool anyNull = value->type == DataType::Null && low->type == DataType::Null &&
                             high->type == DataType::Null;
        const DataType type = anyNull ? DataType::Null : DataType::Bool;
        auto ge = make(BoundBinary{BinaryOp::GtEq, cloneExpr(*value), std::move(low)}, type);
        auto le = make(BoundBinary{BinaryOp::LtEq, std::move(value), std::move(high)}, type);
        auto both = make(BoundBinary{BinaryOp::And, std::move(ge), std::move(le)}, type);
        if (!b.negated) return both;
        return make(BoundUnary{UnaryOp::Not, std::move(both)}, type);
    }

    Result<BoundExprPtr> bindLike(const ast::Expr& e, const ast::Like& l, const Scope* scope,
                                  GroupContext* group) {
        LEDGER_TRY(value, bindExpr(*l.value, scope, group));
        LEDGER_TRY(pattern, bindExpr(*l.pattern, scope, group));
        for (const auto* side : {&value, &pattern}) {
            const DataType t = (*side)->type;
            if (t != DataType::Text && t != DataType::Null) {
                return errorAt(e, ErrorCode::TypeError, "LIKE requires TEXT, got " + typeName(t));
            }
        }
        const DataType type = (value->type == DataType::Null || pattern->type == DataType::Null)
                                  ? DataType::Null
                                  : DataType::Bool;
        return make(BoundLike{std::move(value), std::move(pattern), l.negated}, type);
    }

    Result<BoundExprPtr> bindColumn(const ast::Expr& e, const ast::ColumnRef& c, const Scope* scope,
                                    const GroupContext* group) {
        const std::string shown = c.qualifier.empty() ? c.name : c.qualifier + "." + c.name;
        if (!scope) {
            return errorAt(e, ErrorCode::SyntaxError,
                           "column reference '" + shown + "' is not allowed here");
        }
        auto idx = scope->resolve(c);
        if (!idx.ok()) return errorAt(e, idx.error().code, idx.error().message);
        if (group) {
            return errorAt(e, ErrorCode::SyntaxError,
                           "column '" + shown +
                               "' must appear in GROUP BY or be used in an aggregate function");
        }
        return make(BoundColumn{idx.value()}, (*scope->columns)[idx.value()].type);
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
    // Subquery list of the SELECT being bound; nullptr outside a SELECT.
    std::vector<std::unique_ptr<BoundSelect>>* subqueries_ = nullptr;
};

}  // namespace

Result<BoundStatement> bind(const ast::Statement& stmt, const Catalog& catalog) {
    return Binder{catalog}.run(stmt);
}

}  // namespace ledger
