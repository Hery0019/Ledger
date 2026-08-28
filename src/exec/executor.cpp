#include "exec/executor.h"

#include <algorithm>
#include <utility>

#include "semantic/binder.h"
#include "semantic/eval.h"
#include "sql/parser.h"

namespace ledger {

namespace {

Error rowError(RowId id, const Error& e) {
    return makeError(e.code, "row " + std::to_string(id) + ": " + e.message);
}

// Ordre total pour le tri : NULL avant tout, sinon Value::compare. Les types
// sont homogènes dans une colonne, compare ne peut pas échouer ici.
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

// ---- aides -----------------------------------------------------------------

Result<std::vector<std::pair<RowId, Row>>> Executor::filter(const TableSchema& table,
                                                            const BoundExpr* where) {
    std::vector<std::pair<RowId, Row>> out;
    // WHERE de type Null (littéral NULL) : aucune ligne, inutile de scanner.
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
        // PK => NOT NULL : aucun des deux n'est NULL, compare ne renvoie
        // jamais Unknown ni erreur (même type).
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
    return QueryResult{{}, {}, 1};
}

Result<QueryResult> Executor::run(const BoundSelect& s) {
    LEDGER_TRY(matches, filter(*s.table, s.where.get()));

    if (s.orderBy) {
        const std::size_t col = s.orderBy->column;
        const bool desc = s.orderBy->descending;
        std::stable_sort(matches.begin(), matches.end(), [&](const auto& a, const auto& b) {
            return desc ? lessForSort(b.second[col], a.second[col])
                        : lessForSort(a.second[col], b.second[col]);
        });
    }
    if (s.limit && static_cast<std::size_t>(*s.limit) < matches.size()) {
        matches.resize(static_cast<std::size_t>(*s.limit));
    }

    QueryResult out;
    for (const std::size_t idx : s.projection) out.columns.push_back(s.table->columns[idx].name);
    out.rows.reserve(matches.size());
    for (auto& [id, row] : matches) {
        Row projected;
        projected.reserve(s.projection.size());
        for (const std::size_t idx : s.projection) projected.push_back(row[idx]);
        out.rows.push_back(std::move(projected));
    }
    return out;
}

Result<QueryResult> Executor::run(const BoundUpdate& s) {
    // Passe 1 : calcul de toutes les nouvelles lignes, sans rien écrire.
    // Chaque assignation est évaluée sur la ligne d'ORIGINE, donc
    // `SET a = b, b = a` échange bien les deux.
    LEDGER_TRY(matches, filter(*s.table, s.where.get()));
    std::vector<std::pair<RowId, Row>> updated;
    updated.reserve(matches.size());
    for (const auto& [id, original] : matches) {
        Row next = original;
        for (const auto& [col, expr] : s.assignments) {
            auto v = eval(*expr, original);
            if (!v.ok()) return rowError(id, v.error());
            // Le binder a refusé un NULL garanti ; ici on attrape un NULL
            // issu d'une colonne nullable (`SET name = other_nullable`).
            if (v.value().isNull() && s.table->columns[col].notNull) {
                return makeError(ErrorCode::ConstraintViolation,
                                 "row " + std::to_string(id) + ": column '" +
                                     s.table->columns[col].name + "' cannot be NULL");
            }
            next[col] = std::move(v).value();
        }
        updated.emplace_back(id, std::move(next));
    }

    // Contrainte PK : chaque nouvelle clé contre les lignes non modifiées et
    // contre les autres lignes modifiées.
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

    // Passe 2 : écriture.
    for (const auto& [id, row] : updated) LEDGER_TRY_VOID(engine_.update(s.table->name, id, row));
    return QueryResult{{}, {}, updated.size()};
}

Result<QueryResult> Executor::run(const BoundDelete& s) {
    LEDGER_TRY(matches, filter(*s.table, s.where.get()));
    for (const auto& [id, row] : matches) LEDGER_TRY_VOID(engine_.remove(s.table->name, id));
    return QueryResult{{}, {}, matches.size()};
}

}  // namespace ledger
