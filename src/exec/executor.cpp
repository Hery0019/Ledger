#include "exec/executor.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <utility>

#include "semantic/binder.h"
#include "semantic/eval.h"
#include "sql/parser.h"

namespace ledger {

namespace {

Error rowError(RowId id, const Error& e) {
    return makeError(e.code, "row " + std::to_string(id) + ": " + e.message);
}

// Total order for sorting: NULL before everything, otherwise Value::compare.
// Types are homogeneous within a column, so compare cannot fail here.
bool lessForSort(const Value& a, const Value& b) {
    if (a.isNull()) return !b.isNull();
    if (b.isNull()) return false;
    return Value::compare(a, b).value() == Ordering::Less;
}

}  // namespace

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
    LEDGER_TRY_VOID(engine_.createTable(s.schema));
    auto added = catalog_.add(s.schema);
    if (!added.ok()) {
        return makeError(ErrorCode::Internal,
                         "catalog out of sync after CREATE TABLE: " + added.error().message);
    }
    return QueryResult{};
}

Result<QueryResult> Executor::run(const BoundDropTable& s) {
    LEDGER_TRY_VOID(engine_.dropTable(s.table));
    auto removed = catalog_.remove(s.table);
    if (!removed.ok()) {
        return makeError(ErrorCode::Internal,
                         "catalog out of sync after DROP TABLE: " + removed.error().message);
    }
    return QueryResult{};
}

// Views live in the catalog; the engine only persists the list. The catalog
// is updated first, then the whole list is saved; on a save failure the
// catalog change is rolled back so that memory and disk never disagree.
Result<QueryResult> Executor::run(const BoundCreateView& s) {
    LEDGER_TRY_VOID(catalog_.addView(s.def, s.source));
    auto saved = engine_.saveViews(catalog_.views());
    if (!saved.ok()) {
        (void)catalog_.removeView(s.def.name);
        return saved.error();
    }
    return QueryResult{};
}

Result<QueryResult> Executor::run(const BoundDropView& s) {
    const ViewEntry* entry = catalog_.findView(s.name);
    if (!entry) return makeError(ErrorCode::NotFound, "unknown view '" + s.name + "'");
    const ViewEntry backup = *entry;
    LEDGER_TRY_VOID(catalog_.removeView(s.name));
    auto saved = engine_.saveViews(catalog_.views());
    if (!saved.ok()) {
        (void)catalog_.addView(backup.def, backup.source);
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
            auto v = eval(*where, row);
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

Result<void> Executor::checkPrimaryKey(const TableSchema& table, const Value& key,
                                       const std::vector<RowId>& ignore) {
    const auto pk = table.primaryKeyIndex();
    if (!pk) return {};
    bool duplicate = false;
    LEDGER_TRY_VOID(engine_.scan(table.name, [&](RowId id, const Row& row) {
        if (std::find(ignore.begin(), ignore.end(), id) != ignore.end()) return true;
        // PK => NOT NULL: neither side is NULL, compare never returns Unknown
        // nor an error (same type).
        if (Value::compare(row[*pk], key).value() == Ordering::Equal) {
            duplicate = true;
            return false;
        }
        return true;
    }));
    if (duplicate) {
        return makeError(ErrorCode::ConstraintViolation,
                         "duplicate primary key " + key.toText() + " in table '" + table.name + "'");
    }
    return {};
}

// ---- DML -------------------------------------------------------------------

Result<QueryResult> Executor::run(const BoundInsert& s) {
    if (const auto pk = s.table->primaryKeyIndex()) {
        LEDGER_TRY_VOID(checkPrimaryKey(*s.table, s.row[*pk], {}));
    }
    LEDGER_TRY_VOID(engine_.insert(s.table->name, s.row));
    return QueryResult{{}, {}, 1, ResultKind::Dml};
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

// Lexicographic order on group keys (NULLs group together, before values).
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
            auto v = eval(*k, row);
            if (!v.ok()) return rowError(id, v.error());
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
            auto v = eval(*agg.arg, row);
            if (!v.ok()) return rowError(id, v.error());
            auto acc = accumulate(g.states[a], agg.func, v.value());
            if (!acc.ok()) return rowError(id, acc.error());
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
            LEDGER_TRY(keep, eval(*s.having, groupRow));
            if (keep.isNull() || !keep.asBool()) continue;
        }
        out.push_back(std::move(groupRow));
    }
    return out;
}

Result<QueryResult> Executor::run(const BoundSelect& s) {
    LEDGER_TRY(matches, filter(*s.table, s.where.get()));

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
    // so ORDER BY may use columns the projection does not keep.
    struct Item {
        Row keys;
        Row projected;
    };
    std::vector<Item> items;
    items.reserve(inputs.size());
    for (const auto& [id, row] : inputs) {
        Item item;
        item.keys.reserve(s.orderBy.size());
        for (const auto& ob : s.orderBy) {
            auto v = eval(*ob.expr, row);
            if (!v.ok()) return s.aggregated ? v.error() : rowError(id, v.error());
            item.keys.push_back(std::move(v).value());
        }
        item.projected.reserve(s.projection.size());
        for (const auto& e : s.projection) {
            auto v = eval(*e, row);
            if (!v.ok()) return s.aggregated ? v.error() : rowError(id, v.error());
            item.projected.push_back(std::move(v).value());
        }
        items.push_back(std::move(item));
    }

    if (!s.orderBy.empty()) {
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
    if (s.limit && static_cast<std::size_t>(*s.limit) < items.size()) {
        items.resize(static_cast<std::size_t>(*s.limit));
    }

    QueryResult out;
    out.kind = ResultKind::Select;
    out.columns = s.columnNames;
    out.rows.reserve(items.size());
    for (auto& item : items) out.rows.push_back(std::move(item.projected));
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
            auto v = eval(*expr, original);
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
        updated.emplace_back(id, std::move(next));
    }

    // PK constraint: every new key against the untouched rows and against the
    // other modified rows.
    if (const auto pk = s.table->primaryKeyIndex()) {
        std::vector<RowId> touched;
        touched.reserve(updated.size());
        for (const auto& [id, row] : updated) touched.push_back(id);
        for (std::size_t i = 0; i < updated.size(); ++i) {
            LEDGER_TRY_VOID(checkPrimaryKey(*s.table, updated[i].second[*pk], touched));
            for (std::size_t j = 0; j < i; ++j) {
                if (Value::compare(updated[i].second[*pk], updated[j].second[*pk]).value() ==
                    Ordering::Equal) {
                    return makeError(ErrorCode::ConstraintViolation,
                                     "duplicate primary key " + updated[i].second[*pk].toText() +
                                         " in table '" + s.table->name + "'");
                }
            }
        }
    }

    // Pass 2: write.
    for (const auto& [id, row] : updated) LEDGER_TRY_VOID(engine_.update(s.table->name, id, row));
    return QueryResult{{}, {}, updated.size(), ResultKind::Dml};
}

Result<QueryResult> Executor::run(const BoundDelete& s) {
    LEDGER_TRY(matches, filter(*s.table, s.where.get()));
    for (const auto& [id, row] : matches) LEDGER_TRY_VOID(engine_.remove(s.table->name, id));
    return QueryResult{{}, {}, matches.size(), ResultKind::Dml};
}

}  // namespace ledger
