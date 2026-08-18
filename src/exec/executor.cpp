#include "exec/executor.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <utility>

#include "core/uuid.h"
#include "semantic/binder.h"
#include "sql/parser.h"

namespace ledger {

namespace {

Error rowError(RowId id, const Error& e) {
    return makeError(e.code, "row " + std::to_string(id) + ": " + e.message);
}

// Rows with an id (straight from a table) get the "row N:" prefix.
Error atRow(RowId id, const Error& e) { return id ? rowError(id, e) : e; }

// Total order for sorting: NULL before everything, otherwise Value::compare.
// Types are homogeneous within a column, so compare cannot fail here.
bool lessForSort(const Value& a, const Value& b) {
    if (a.isNull()) return !b.isNull();
    if (b.isNull()) return false;
    return Value::compare(a, b).value() == Ordering::Less;
}

// Lexicographic order on rows (group keys, DISTINCT, UNION): NULLs group
// together, before values.
struct RowLess {
    bool operator()(const Row& a, const Row& b) const {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (lessForSort(a[i], b[i])) return true;
            if (lessForSort(b[i], a[i])) return false;
        }
        return false;
    }
};

}  // namespace

Executor::~Executor() {
    // A session closing mid-transaction abandons it, like a disconnect.
    if (undo_) (void)rollback();
}

// ---- transactions ----------------------------------------------------------

Result<void> Executor::noDdlInTransaction() const {
    if (!undo_) return {};
    return makeError(ErrorCode::SyntaxError,
                     "CREATE, DROP and ALTER are not transactional; COMMIT or ROLLBACK first");
}

Result<QueryResult> Executor::run(const BoundBegin&) {
    if (undo_) return makeError(ErrorCode::SyntaxError, "a transaction is already in progress");
    undo_.emplace();
    return QueryResult{};
}

Result<QueryResult> Executor::run(const BoundCommit&) {
    if (!undo_) return makeError(ErrorCode::SyntaxError, "no transaction in progress");
    // Every write was already flushed: committing only forgets the undo log.
    undo_.reset();
    return QueryResult{};
}

Result<QueryResult> Executor::run(const BoundRollback&) {
    if (!undo_) return makeError(ErrorCode::SyntaxError, "no transaction in progress");
    LEDGER_TRY_VOID(rollback());
    return QueryResult{};
}

// Replays the undo log backwards. Stops at the first failing step (an
// IoError): the remaining entries are kept so that ROLLBACK can be retried.
Result<void> Executor::rollback() {
    std::vector<Undo>& log = *undo_;
    while (!log.empty()) {
        const Undo& u = log.back();
        Result<void> r;
        switch (u.kind) {
            case Undo::Kind::Insert: r = engine_.remove(u.table, u.id); break;
            case Undo::Kind::Delete: r = engine_.restore(u.table, u.id, u.row); break;
            case Undo::Kind::Update: r = engine_.update(u.table, u.id, u.row); break;
        }
        if (!r.ok()) {
            return makeError(r.error().code, "rollback failed: " + r.error().message +
                                                 " (" + std::to_string(log.size()) + " changes still pending)");
        }
        log.pop_back();
    }
    undo_.reset();
    return {};
}

Result<QueryResult> Executor::execute(std::string_view sql) {
    LEDGER_TRY(stmt, parse(sql));
    LEDGER_TRY(bound, ledger::bind(stmt, catalog_));
    return execute(bound);
}

Result<QueryResult> Executor::execute(const BoundStatement& stmt) {
    return std::visit([&](const auto& s) { return run(s); }, stmt);
}

// ---- DDL -------------------------------------------------------------------

Result<QueryResult> Executor::run(const BoundCreateTable& s) {
    LEDGER_TRY_VOID(noDdlInTransaction());
    LEDGER_TRY_VOID(engine_.createTable(s.schema));
    auto added = catalog_.add(s.schema);
    if (!added.ok()) {
        return makeError(ErrorCode::Internal,
                         "catalog out of sync after CREATE TABLE: " + added.error().message);
    }
    return QueryResult{};
}

Result<QueryResult> Executor::run(const BoundDropTable& s) {
    LEDGER_TRY_VOID(noDdlInTransaction());
    LEDGER_TRY_VOID(engine_.dropTable(s.table));
    auto removed = catalog_.remove(s.table);
    if (!removed.ok()) {
        return makeError(ErrorCode::Internal,
                         "catalog out of sync after DROP TABLE: " + removed.error().message);
    }
    // The table's indexes go with it (the engine already forgot them). A
    // failed list rewrite is survivable: loadIndexes skips declarations
    // whose table is gone, with a warning.
    const auto dropped = catalog_.indexesOn(s.table);
    if (!dropped.empty()) {
        for (const auto& d : dropped) (void)catalog_.removeIndex(d.name);
        LEDGER_TRY_VOID(engine_.saveIndexes(catalog_.indexes()));
    }
    return QueryResult{};
}

// Views live in the catalog; the engine only persists the list. The catalog
// is updated first, then the whole list is saved; on a save failure the
// catalog change is rolled back so that memory and disk never disagree.
Result<QueryResult> Executor::run(const BoundCreateView& s) {
    LEDGER_TRY_VOID(noDdlInTransaction());
    LEDGER_TRY_VOID(catalog_.addView(s.def, s.sources));
    auto saved = engine_.saveViews(catalog_.views());
    if (!saved.ok()) {
        (void)catalog_.removeView(s.def.name);
        return saved.error();
    }
    return QueryResult{};
}

Result<QueryResult> Executor::run(const BoundDropView& s) {
    LEDGER_TRY_VOID(noDdlInTransaction());
    const ViewEntry* entry = catalog_.findView(s.name);
    if (!entry) return makeError(ErrorCode::NotFound, "unknown view '" + s.name + "'");
    const ViewEntry backup = *entry;
    LEDGER_TRY_VOID(catalog_.removeView(s.name));
    auto saved = engine_.saveViews(catalog_.views());
    if (!saved.ok()) {
        (void)catalog_.addView(backup.def, backup.sources);
        return saved.error();
    }
    return QueryResult{};
}

// ---- users -----------------------------------------------------------------
//
// Same shape as views: the catalog first, then the whole list to the engine,
// catalog rolled back if the save fails. The password is hashed here — the
// engine and the files only ever see the salted hash.

Result<QueryResult> Executor::run(const BoundCreateUser& s) {
    LEDGER_TRY_VOID(noDdlInTransaction());
    LEDGER_TRY_VOID(catalog_.addUser(makeUser(s.name, s.password)));
    auto saved = engine_.saveUsers(catalog_.users());
    if (!saved.ok()) {
        (void)catalog_.removeUser(s.name);
        return saved.error();
    }
    return QueryResult{};
}

Result<QueryResult> Executor::run(const BoundAlterUser& s) {
    LEDGER_TRY_VOID(noDdlInTransaction());
    const UserDef* existing = catalog_.findUser(s.name);
    if (!existing) return makeError(ErrorCode::NotFound, "unknown user '" + s.name + "'");
    const UserDef backup = *existing;
    LEDGER_TRY_VOID(catalog_.replaceUser(makeUser(s.name, s.password)));
    auto saved = engine_.saveUsers(catalog_.users());
    if (!saved.ok()) {
        (void)catalog_.replaceUser(backup);
        return saved.error();
    }
    return QueryResult{};
}

// ---- user indexes ------------------------------------------------------------
//
// Same shape as views and users: the catalog first, the engine next, the
// declaration list saved last, everything unwound on failure.

Result<QueryResult> Executor::run(const BoundCreateIndex& s) {
    LEDGER_TRY_VOID(noDdlInTransaction());
    LEDGER_TRY_VOID(catalog_.addIndex(s.def));
    auto built = engine_.createIndex(s.def.table, s.column);
    if (!built.ok()) {
        (void)catalog_.removeIndex(s.def.name);
        return built.error();
    }
    auto saved = engine_.saveIndexes(catalog_.indexes());
    if (!saved.ok()) {
        (void)engine_.dropIndex(s.def.table, s.column);
        (void)catalog_.removeIndex(s.def.name);
        return saved.error();
    }
    return QueryResult{};
}

Result<QueryResult> Executor::run(const BoundDropIndex& s) {
    LEDGER_TRY_VOID(noDdlInTransaction());
    const IndexDef* def = catalog_.findIndex(s.name);
    if (!def) return makeError(ErrorCode::NotFound, "unknown index '" + s.name + "'");
    const IndexDef backup = *def;
    LEDGER_TRY_VOID(catalog_.removeIndex(s.name));
    LEDGER_TRY_VOID(engine_.dropIndex(s.table, s.column));
    auto saved = engine_.saveIndexes(catalog_.indexes());
    if (!saved.ok()) {
        (void)engine_.createIndex(s.table, s.column);
        (void)catalog_.addIndex(backup);
        return saved.error();
    }
    return QueryResult{};
}

Result<QueryResult> Executor::run(const BoundDropUser& s) {
    LEDGER_TRY_VOID(noDdlInTransaction());
    const UserDef* existing = catalog_.findUser(s.name);
    if (!existing) return makeError(ErrorCode::NotFound, "unknown user '" + s.name + "'");
    const UserDef backup = *existing;
    LEDGER_TRY_VOID(catalog_.removeUser(s.name));
    auto saved = engine_.saveUsers(catalog_.users());
    if (!saved.ok()) {
        (void)catalog_.addUser(backup);
        return saved.error();
    }
    return QueryResult{};
}

// ---- helpers ---------------------------------------------------------------

Result<std::vector<std::pair<RowId, Row>>> Executor::filter(const TableSchema& table,
                                                            const BoundExpr* where) {
    std::vector<std::pair<RowId, Row>> out;
    // WHERE of type Null (NULL literal): no row, no need to scan.
    if (where && where->type == DataType::Null) return out;

    std::optional<Error> failure;
    LEDGER_TRY_VOID(engine_.scan(table.name, [&](RowId id, const Row& row) {
        if (where) {
            auto v = ev(*where, row);
            if (!v.ok()) {
                failure = rowError(id, v.error());
                return false;
            }
            const Value& b = v.value();
            if (b.isNull() || !b.asBool()) return true;
        }
        out.emplace_back(id, row);
        return true;
    }));
    if (failure) return *failure;
    return out;
}

// PRIMARY KEY and UNIQUE columns: no other live row (outside `ignore`) may
// hold the same non-NULL value. One index lookup per constrained column.
Result<void> Executor::checkUnique(const TableSchema& table, const Row& row,
                                   const std::vector<RowId>& ignore) {
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        const ColumnSchema& col = table.columns[c];
        if (!(col.primaryKey || col.unique) || row[c].isNull()) continue;
        LEDGER_TRY(hit, engine_.lookup(table.name, c, row[c]));
        if (hit && std::find(ignore.begin(), ignore.end(), hit->first) == ignore.end()) {
            return makeError(ErrorCode::ConstraintViolation,
                             col.primaryKey
                                 ? "duplicate primary key " + row[c].toText() + " in table '" + table.name + "'"
                                 : "duplicate value " + row[c].toText() + " for UNIQUE column '" + col.name +
                                       "' in table '" + table.name + "'");
        }
    }
    return {};
}

namespace {

// `indexed_column = constant` (either side) on a plain table scan: the shape
// an index can answer directly. Returns {column, constant}, or nullopt.
std::optional<std::pair<std::size_t, const Value*>> pointLookupKey(const BoundSelect& s,
                                                                   const IStorageEngine& engine) {
    if (!s.where || s.aggregated) return std::nullopt;
    const auto* scan = std::get_if<RelScan>(&s.relation->node);
    if (!scan) return std::nullopt;
    const auto* eq = std::get_if<BoundBinary>(&s.where->node);
    if (!eq || eq->op != ast::BinaryOp::Eq) return std::nullopt;
    const auto* lcol = std::get_if<BoundColumn>(&eq->lhs->node);
    const auto* rcol = std::get_if<BoundColumn>(&eq->rhs->node);
    const auto* lval = std::get_if<Value>(&eq->lhs->node);
    const auto* rval = std::get_if<Value>(&eq->rhs->node);
    const BoundColumn* col = lcol && rval ? lcol : (rcol && lval ? rcol : nullptr);
    const Value* key = lcol && rval ? rval : (rcol && lval ? lval : nullptr);
    if (!col || key->isNull() || !engine.indexed(scan->table->name, col->index)) return std::nullopt;
    return std::pair{col->index, key};
}

}  // namespace

// ---- DML -------------------------------------------------------------------

// CHECK constraints: a row is refused only when a constraint evaluates to
// FALSE; NULL (unknown) passes, as in SQL.
Result<void> Executor::runChecks(const TableSchema& table, const std::vector<BoundCheck>& checks,
                                 const Row& row) {
    for (const auto& [column, expr] : checks) {
        LEDGER_TRY(v, ev(*expr, row));
        if (v.type() == DataType::Bool && !v.asBool()) {
            return makeError(ErrorCode::ConstraintViolation, "CHECK constraint on column '" +
                                                                 table.columns[column].name + "' failed: " +
                                                                 table.columns[column].check);
        }
    }
    return {};
}

Result<void> Executor::checkForeignKeys(const TableSchema& table, const Row& row) {
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        const ColumnSchema& col = table.columns[c];
        if (!col.reference || row[c].isNull()) continue;
        // The binder guaranteed the parent and its indexed column exist.
        const TableSchema* parent = catalog_.find(col.reference->table);
        const auto target = parent ? parent->columnIndex(col.reference->column) : std::nullopt;
        if (!target) return makeError(ErrorCode::Internal, "dangling REFERENCES on column '" + col.name + "'");
        LEDGER_TRY(hit, engine_.lookup(parent->name, *target, row[c]));
        if (!hit) {
            return makeError(ErrorCode::ConstraintViolation,
                             "foreign key on column '" + col.name + "': no row in '" + parent->name + "' with " +
                                 col.reference->column + " = " + row[c].toText());
        }
    }
    return {};
}

Result<std::vector<std::pair<RowId, Row>>> Executor::rowsWithValue(const TableSchema& table, std::size_t column,
                                                                   const Value& key) {
    std::vector<std::pair<RowId, Row>> out;
    LEDGER_TRY_VOID(engine_.scan(table.name, [&](RowId id, const Row& row) {
        if (Value::compare(row[column], key).value() == Ordering::Equal) out.emplace_back(id, row);
        return true;
    }));
    return out;
}

Result<QueryResult> Executor::run(const BoundInsert& s) {
    Row row = s.row;
    if (s.autoColumn) {
        if (s.table->columns[*s.autoColumn].type == DataType::Uuid) {
            // UUID PRIMARY KEY: a fresh random UUID. Collisions are
            // theoretical, and the PK check below would catch one anyway.
            row[*s.autoColumn] = Value::uuid(generateUuidV4());
        } else {
            // AUTOINCREMENT: largest live key + 1 (so a deleted maximum can
            // be handed out again), 1 for an empty table. The PK index
            // answers directly.
            LEDGER_TRY(max, engine_.maxKey(s.table->name, *s.autoColumn));
            const std::int64_t last = max ? max->asInt() : 0;
            if (last == std::numeric_limits<std::int64_t>::max()) {
                return makeError(ErrorCode::TypeError, "AUTOINCREMENT column '" +
                                                          s.table->columns[*s.autoColumn].name + "' is exhausted");
            }
            row[*s.autoColumn] = Value::integer(last < 0 ? 1 : last + 1);
        }
    }
    LEDGER_TRY_VOID(runChecks(*s.table, s.checks, row));
    LEDGER_TRY_VOID(checkForeignKeys(*s.table, row));
    LEDGER_TRY_VOID(checkUnique(*s.table, row, {}));
    LEDGER_TRY(id, engine_.insert(s.table->name, row));
    if (undo_) undo_->push_back(Undo{Undo::Kind::Insert, s.table->name, id, {}});
    QueryResult out{{}, {}, 1, ResultKind::Dml, {}};
    if (s.autoColumn) out.key = row[*s.autoColumn];
    return out;
}

namespace {

// Running state of one aggregate over one group.
struct AggState {
    std::int64_t count = 0;  // non-NULL values seen (rows, for COUNT(*))
    std::int64_t isum = 0;   // SUM over Int, checked
    double fsum = 0.0;       // SUM/AVG over Float, or after an Int overflow to Float
    bool useFloat = false;
    std::optional<Value> minv, maxv;
};

Result<void> accumulate(AggState& st, AggFunc f, const Value& v) {
    if (v.isNull()) return {};
    ++st.count;
    switch (f) {
        case AggFunc::Count:
            break;
        case AggFunc::Sum:
            if (v.type() == DataType::Float) {
                if (!st.useFloat) {
                    st.useFloat = true;
                    st.fsum = static_cast<double>(st.isum);
                }
                st.fsum += v.asFloat();
            } else if (st.useFloat) {
                st.fsum += static_cast<double>(v.asInt());
            } else {
                const std::int64_t a = st.isum, b = v.asInt();
                constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
                constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
                if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) {
                    return makeError(ErrorCode::TypeError, "integer overflow in sum()");
                }
                st.isum = a + b;
            }
            break;
        case AggFunc::Avg:
            st.fsum += v.type() == DataType::Int ? static_cast<double>(v.asInt()) : v.asFloat();
            break;
        case AggFunc::Min:
        case AggFunc::Max: {
            std::optional<Value>& best = f == AggFunc::Min ? st.minv : st.maxv;
            if (!best) {
                best = v;
                break;
            }
            LEDGER_TRY(ord, Value::compare(v, *best));
            if ((f == AggFunc::Min && ord == Ordering::Less) || (f == AggFunc::Max && ord == Ordering::Greater)) {
                best = v;
            }
            break;
        }
    }
    return {};
}

Result<Value> finalize(const AggState& st, AggFunc f) {
    switch (f) {
        case AggFunc::Count: return Value::integer(st.count);
        case AggFunc::Sum:
            if (st.count == 0) return Value::null();
            return st.useFloat ? Value::real(st.fsum) : Value::integer(st.isum);
        case AggFunc::Avg:
            if (st.count == 0) return Value::null();
            return Value::real(st.fsum / static_cast<double>(st.count));
        case AggFunc::Min: return st.minv ? *st.minv : Value::null();
        case AggFunc::Max: return st.maxv ? *st.maxv : Value::null();
    }
    return makeError(ErrorCode::Internal, "finalize: unknown aggregate");
}

}  // namespace

// Groups the filtered rows and returns one row per surviving group:
// [group keys..., aggregate results...], in order of first appearance.
Result<std::vector<Row>> Executor::aggregate(const BoundSelect& s,
                                             const std::vector<std::pair<RowId, Row>>& matches) {
    struct Group {
        Row keys;
        std::vector<AggState> states;
    };
    std::vector<Group> groups;
    std::map<Row, std::size_t, RowLess> index;

    for (const auto& [id, row] : matches) {
        Row keys;
        keys.reserve(s.groupBy.size());
        for (const auto& k : s.groupBy) {
            auto v = ev(*k, row);
            if (!v.ok()) return atRow(id, v.error());
            keys.push_back(std::move(v).value());
        }
        auto it = index.find(keys);
        if (it == index.end()) {
            it = index.emplace(keys, groups.size()).first;
            groups.push_back(Group{std::move(keys), std::vector<AggState>(s.aggregates.size())});
        }
        Group& g = groups[it->second];
        for (std::size_t a = 0; a < s.aggregates.size(); ++a) {
            const auto& agg = s.aggregates[a];
            if (!agg.arg) {
                ++g.states[a].count;  // COUNT(*)
                continue;
            }
            auto v = ev(*agg.arg, row);
            if (!v.ok()) return atRow(id, v.error());
            auto acc = accumulate(g.states[a], agg.func, v.value());
            if (!acc.ok()) return atRow(id, acc.error());
        }
    }
    // No GROUP BY: the whole input is one group, even when empty.
    if (groups.empty() && s.groupBy.empty()) {
        groups.push_back(Group{{}, std::vector<AggState>(s.aggregates.size())});
    }

    std::vector<Row> out;
    out.reserve(groups.size());
    for (const auto& g : groups) {
        Row groupRow = g.keys;
        for (std::size_t a = 0; a < s.aggregates.size(); ++a) {
            LEDGER_TRY(v, finalize(g.states[a], s.aggregates[a].func));
            groupRow.push_back(std::move(v));
        }
        if (s.having) {
            LEDGER_TRY(keep, ev(*s.having, groupRow));
            if (keep.isNull() || !keep.asBool()) continue;
        }
        out.push_back(std::move(groupRow));
    }
    return out;
}

Result<std::vector<std::pair<RowId, Row>>> Executor::evaluate(const BoundRelation& rel) {
    using Rows = std::vector<std::pair<RowId, Row>>;
    return std::visit(
        [&](const auto& n) -> Result<Rows> {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, RelScan>) {
                return filter(*n.table, nullptr);
            } else if constexpr (std::is_same_v<N, RelFilter>) {
                LEDGER_TRY(input, evaluate(*n.input));
                Rows out;
                for (auto& [id, row] : input) {
                    auto v = ev(*n.predicate, row);
                    if (!v.ok()) return atRow(id, v.error());
                    if (!v.value().isNull() && v.value().asBool()) out.emplace_back(id, std::move(row));
                }
                return out;
            } else if constexpr (std::is_same_v<N, RelProject>) {
                LEDGER_TRY(input, evaluate(*n.input));
                Rows out;
                out.reserve(input.size());
                for (const auto& [id, row] : input) {
                    Row projected;
                    projected.reserve(n.exprs.size());
                    for (const auto& e : n.exprs) {
                        auto v = ev(*e, row);
                        if (!v.ok()) return atRow(id, v.error());
                        projected.push_back(std::move(v).value());
                    }
                    out.emplace_back(id, std::move(projected));
                }
                return out;
            } else {
                // Nested-loop join: every pair is tested against ON. Fine for
                // personal-sized tables; an index-driven join can come later.
                LEDGER_TRY(left, evaluate(*n.left));
                LEDGER_TRY(right, evaluate(*n.right));
                const std::size_t rightWidth = n.right->columns.size();
                Rows out;
                for (const auto& [lid, lrow] : left) {
                    bool matched = false;
                    for (const auto& [rid, rrow] : right) {
                        Row combined = lrow;
                        combined.insert(combined.end(), rrow.begin(), rrow.end());
                        auto v = ev(*n.on, combined);
                        if (!v.ok()) return v.error();
                        if (v.value().isNull() || !v.value().asBool()) continue;
                        matched = true;
                        out.emplace_back(0, std::move(combined));
                    }
                    if (!matched && n.kind == JoinKind::Left) {
                        Row padded = lrow;
                        padded.resize(padded.size() + rightWidth, Value::null());
                        out.emplace_back(0, std::move(padded));
                    }
                }
                return out;
            }
        },
        rel.node);
}

// Runs one SELECT body (no UNION members, no OFFSET/LIMIT): rows of the
// relation -> WHERE -> grouping -> projection and sort keys -> stable sort
// -> DISTINCT. The result is the projected rows, in order.
Result<std::vector<Row>> Executor::collect(const BoundSelect& s) {
    // Subqueries first, each run once; their rows are visible to every
    // expression of this SELECT through subs_.
    SubqueryRows subqueryRows;
    subqueryRows.reserve(s.subqueries.size());
    for (const auto& sub : s.subqueries) {
        LEDGER_TRY(r, run(*sub));
        subqueryRows.push_back(std::move(r.rows));
    }
    const SubqueryRows* const saved = subs_;
    subs_ = &subqueryRows;
    struct Restore {
        const SubqueryRows*& slot;
        const SubqueryRows* value;
        ~Restore() { slot = value; }
    } restore{subs_, saved};

    // WHERE over the relation's rows. `WHERE indexed_column = value` on a
    // table is answered by that column's index without materializing the
    // table — one row for the primary key, possibly several for a
    // user-declared index.
    std::vector<std::pair<RowId, Row>> matches;
    std::vector<std::pair<RowId, Row>> source;
    if (const auto point = pointLookupKey(s, engine_)) {
        const auto* scan = std::get_if<RelScan>(&s.relation->node);
        LEDGER_TRY(hits, engine_.lookupAll(scan->table->name, point->first, *point->second));
        matches = std::move(hits);
    } else {
        LEDGER_TRY(rows, evaluate(*s.relation));
        source = std::move(rows);
    }
    if (pointLookupKey(s, engine_)) {
        // done above
    } else if (s.where && s.where->type == DataType::Null) {
        // A WHERE that is always NULL keeps nothing.
    } else if (s.where) {
        for (auto& [id, row] : source) {
            auto v = ev(*s.where, row);
            if (!v.ok()) return atRow(id, v.error());
            if (!v.value().isNull() && v.value().asBool()) matches.emplace_back(id, std::move(row));
        }
    } else {
        matches = std::move(source);
    }

    // The rows the projection and ORDER BY read: source rows, or one group
    // row per group when aggregating.
    std::vector<std::pair<RowId, Row>> inputs;
    if (s.aggregated) {
        LEDGER_TRY(groups, aggregate(s, matches));
        inputs.reserve(groups.size());
        for (auto& g : groups) inputs.emplace_back(0, std::move(g));
    } else {
        inputs = std::move(matches);
    }

    // Sort keys and projected values are both computed from the input row,
    // so ORDER BY may use columns the projection does not keep. With UNION
    // members the keys are computed later, on the output rows.
    const bool keysOnOutput = !s.unions.empty();
    struct Item {
        Row keys;
        Row projected;
    };
    std::vector<Item> items;
    items.reserve(inputs.size());
    for (const auto& [id, row] : inputs) {
        Item item;
        if (!keysOnOutput) {
            item.keys.reserve(s.orderBy.size());
            for (const auto& ob : s.orderBy) {
                auto v = ev(*ob.expr, row);
                if (!v.ok()) return atRow(id, v.error());
                item.keys.push_back(std::move(v).value());
            }
        }
        item.projected.reserve(s.projection.size());
        for (const auto& e : s.projection) {
            auto v = ev(*e, row);
            if (!v.ok()) return atRow(id, v.error());
            item.projected.push_back(std::move(v).value());
        }
        items.push_back(std::move(item));
    }

    if (!s.orderBy.empty() && !keysOnOutput) {
        std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
            for (std::size_t k = 0; k < s.orderBy.size(); ++k) {
                const bool desc = s.orderBy[k].descending;
                const Value& x = desc ? b.keys[k] : a.keys[k];
                const Value& y = desc ? a.keys[k] : b.keys[k];
                if (lessForSort(x, y)) return true;
                if (lessForSort(y, x)) return false;
            }
            return false;
        });
    }

    std::vector<Row> out;
    out.reserve(items.size());
    if (s.distinct) {
        // Keep the first occurrence of every projected row (after the sort,
        // so "first" is well defined when ORDER BY is present).
        std::map<Row, bool, RowLess> seen;
        for (auto& item : items) {
            if (seen.emplace(item.projected, true).second) out.push_back(std::move(item.projected));
        }
    } else {
        for (auto& item : items) out.push_back(std::move(item.projected));
    }
    return out;
}

Result<QueryResult> Executor::run(const BoundSelect& s) {
    LEDGER_TRY(rows, collect(s));

    if (!s.unions.empty()) {
        // Concatenate every member; UNION (without ALL) removes duplicates
        // across the whole result, first occurrence kept.
        bool dedupe = false;
        for (const auto& member : s.unions) {
            LEDGER_TRY(more, collect(*member.select));
            rows.insert(rows.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
            dedupe = dedupe || !member.all;
        }
        if (dedupe) {
            std::map<Row, bool, RowLess> seen;
            std::vector<Row> unique;
            for (auto& row : rows) {
                if (seen.emplace(row, true).second) unique.push_back(std::move(row));
            }
            rows = std::move(unique);
        }
        if (!s.orderBy.empty()) {
            // Keys are output columns: evaluated on the result row itself.
            std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
                for (const auto& ob : s.orderBy) {
                    const std::size_t idx = std::get<BoundColumn>(ob.expr->node).index;
                    const Value& x = ob.descending ? b[idx] : a[idx];
                    const Value& y = ob.descending ? a[idx] : b[idx];
                    if (lessForSort(x, y)) return true;
                    if (lessForSort(y, x)) return false;
                }
                return false;
            });
        }
    }

    if (s.offset) {
        const auto skip = std::min(static_cast<std::size_t>(*s.offset), rows.size());
        rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(skip));
    }
    if (s.limit && static_cast<std::size_t>(*s.limit) < rows.size()) {
        rows.resize(static_cast<std::size_t>(*s.limit));
    }

    QueryResult out;
    out.kind = ResultKind::Select;
    out.columns = s.columnNames;
    out.rows = std::move(rows);
    return out;
}

Result<QueryResult> Executor::run(const BoundUpdate& s) {
    // Pass 1: compute every new row, writing nothing. Each assignment is
    // evaluated on the ORIGINAL row, so `SET a = b, b = a` really swaps.
    LEDGER_TRY(matches, filter(*s.table, s.where.get()));
    std::vector<std::pair<RowId, Row>> updated;
    updated.reserve(matches.size());
    for (const auto& [id, original] : matches) {
        Row next = original;
        for (const auto& [col, expr] : s.assignments) {
            auto v = ev(*expr, original);
            if (!v.ok()) return rowError(id, v.error());
            // The binder refused a guaranteed NULL; here we catch a NULL that
            // comes from a nullable column (`SET name = other_nullable`).
            if (v.value().isNull() && s.table->columns[col].notNull) {
                return makeError(ErrorCode::ConstraintViolation,
                                 "row " + std::to_string(id) + ": column '" +
                                     s.table->columns[col].name + "' cannot be NULL");
            }
            next[col] = std::move(v).value();
        }
        if (auto c = runChecks(*s.table, s.checks, next); !c.ok()) return rowError(id, c.error());
        if (auto c = checkForeignKeys(*s.table, next); !c.ok()) return rowError(id, c.error());
        updated.emplace_back(id, std::move(next));
    }

    // A referenced key cannot change while a row still points at it (there
    // is no ON UPDATE CASCADE).
    for (const auto& [child, column] : catalog_.referencing(s.table->name)) {
        const auto target = s.table->columnIndex(child->columns[column].reference->column);
        for (std::size_t i = 0; i < updated.size(); ++i) {
            const Value& before = matches[i].second[*target];
            if (before.isNull() || Value::compare(before, updated[i].second[*target]).value() == Ordering::Equal) {
                continue;
            }
            LEDGER_TRY(users, rowsWithValue(*child, column, before));
            if (!users.empty()) {
                return makeError(ErrorCode::ConstraintViolation,
                                 "row " + std::to_string(matches[i].first) + ": " + s.table->columns[*target].name +
                                     " = " + before.toText() + " is referenced by " + child->name + "." +
                                     child->columns[column].name);
            }
        }
    }

    // PRIMARY KEY / UNIQUE: every new value against the untouched rows and
    // against the other modified rows.
    std::vector<RowId> touched;
    touched.reserve(updated.size());
    for (const auto& [id, row] : updated) touched.push_back(id);
    for (std::size_t i = 0; i < updated.size(); ++i) {
        LEDGER_TRY_VOID(checkUnique(*s.table, updated[i].second, touched));
        for (std::size_t c = 0; c < s.table->columns.size(); ++c) {
            const ColumnSchema& col = s.table->columns[c];
            if (!(col.primaryKey || col.unique) || updated[i].second[c].isNull()) continue;
            for (std::size_t j = 0; j < i; ++j) {
                if (updated[j].second[c].isNull()) continue;
                if (Value::compare(updated[i].second[c], updated[j].second[c]).value() == Ordering::Equal) {
                    return makeError(ErrorCode::ConstraintViolation,
                                     col.primaryKey
                                         ? "duplicate primary key " + updated[i].second[c].toText() +
                                               " in table '" + s.table->name + "'"
                                         : "duplicate value " + updated[i].second[c].toText() +
                                               " for UNIQUE column '" + col.name + "' in table '" +
                                               s.table->name + "'");
                }
            }
        }
    }

    // Pass 2: write. The undo entry (previous version) is recorded before
    // each write, so a failing write is still undone by ROLLBACK.
    for (std::size_t i = 0; i < updated.size(); ++i) {
        const auto& [id, row] = updated[i];
        if (undo_) undo_->push_back(Undo{Undo::Kind::Update, s.table->name, id, matches[i].second});
        LEDGER_TRY_VOID(engine_.update(s.table->name, id, row));
    }
    return QueryResult{{}, {}, updated.size(), ResultKind::Dml, {}};
}

Result<void> Executor::cascade(std::vector<Doomed>& doomed, std::size_t from) {
    const auto isDoomed = [&](const TableSchema* table, RowId id) {
        return std::any_of(doomed.begin(), doomed.end(),
                           [&](const Doomed& d) { return d.table == table && d.id == id; });
    };
    // `doomed` grows while it is walked: rows added by a cascade are examined
    // in their turn, so a chain of cascades goes all the way down.
    for (std::size_t i = from; i < doomed.size(); ++i) {
        for (const auto& [child, column] : catalog_.referencing(doomed[i].table->name)) {
            const ForeignKey& fk = *child->columns[column].reference;
            const auto target = doomed[i].table->columnIndex(fk.column);
            const Value key = doomed[i].row[*target];  // a copy: doomed may reallocate
            if (key.isNull()) continue;
            LEDGER_TRY(users, rowsWithValue(*child, column, key));
            for (auto& [id, row] : users) {
                // Rows already going (deleted by the statement itself, or by
                // an earlier cascade) never hold a parent back.
                if (isDoomed(child, id)) continue;
                if (!fk.cascade) {
                    return makeError(ErrorCode::ConstraintViolation,
                                     "row " + std::to_string(doomed[i].id) + " of '" + doomed[i].table->name +
                                         "' is referenced by " + child->name + "." + child->columns[column].name +
                                         " (row " + std::to_string(id) + ")");
                }
                doomed.push_back(Doomed{child, id, std::move(row)});
            }
        }
    }
    return {};
}

Result<QueryResult> Executor::run(const BoundDelete& s) {
    LEDGER_TRY(matches, filter(*s.table, s.where.get()));
    // Pass 1: the full set of rows to delete (ON DELETE CASCADE included),
    // and every RESTRICT violation, before anything is written.
    std::vector<Doomed> doomed;
    doomed.reserve(matches.size());
    for (auto& [id, row] : matches) doomed.push_back(Doomed{s.table, id, std::move(row)});
    LEDGER_TRY_VOID(cascade(doomed, 0));
    // Pass 2: children first, so that a crash mid-way never leaves a
    // referencing row without its parent.
    for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) {
        if (undo_) undo_->push_back(Undo{Undo::Kind::Delete, it->table->name, it->id, it->row});
        LEDGER_TRY_VOID(engine_.remove(it->table->name, it->id));
    }
    return QueryResult{{}, {}, matches.size(), ResultKind::Dml, {}};
}

}  // namespace ledger
