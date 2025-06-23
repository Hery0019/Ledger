#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "core/row.h"
#include "semantic/bound.h"
#include "semantic/catalog.h"
#include "semantic/eval.h"
#include "storage/engine.h"

namespace ledger {

enum class ResultKind { Ddl, Dml, Select };

struct QueryResult {
    std::vector<std::string> columns;  // headers (SELECT only)
    std::vector<Row> rows;             // projected rows (SELECT only)
    std::size_t affected = 0;          // INSERT/UPDATE/DELETE: rows touched
    ResultKind kind = ResultKind::Ddl;
};

// Runs a BoundStatement on a storage engine. The only place that modifies the
// Catalog (after a successful CREATE/DROP on the storage side) and that
// enforces the PRIMARY KEY constraint (by scan: no index in v1).
//
// No transactions: every predictable error (types, PK, evaluation error on a
// row) is raised BEFORE the first write, in two passes for UPDATE/DELETE.
// Only an IoError in the middle of the writes can leave a partial state; it
// is propagated as is.
//
// SELECT: the FROM relation is materialized, then WHERE (true only), then
// grouping if the query aggregates, then projection / ORDER BY keys, stable
// sort (NULL smaller than everything), DISTINCT, OFFSET, LIMIT. UNION members
// are run the same way and concatenated (deduplicated unless ALL) before the
// final ORDER BY / OFFSET / LIMIT. Subqueries are run once, up front.
class Executor {
public:
    Executor(IStorageEngine& engine, Catalog& catalog) noexcept
        : engine_(engine), catalog_(catalog) {}

    // SQL text -> parse -> bind -> execute.
    Result<QueryResult> execute(std::string_view sql);
    Result<QueryResult> execute(const BoundStatement& stmt);

private:
    Result<QueryResult> run(const BoundCreateTable& s);
    Result<QueryResult> run(const BoundDropTable& s);
    Result<QueryResult> run(const BoundCreateView& s);
    Result<QueryResult> run(const BoundDropView& s);
    Result<QueryResult> run(const BoundInsert& s);
    Result<QueryResult> run(const BoundSelect& s);
    Result<QueryResult> run(const BoundUpdate& s);
    Result<QueryResult> run(const BoundDelete& s);

    // eval() with the current statement's subquery rows.
    Result<Value> ev(const BoundExpr& e, const Row& row) const { return eval(e, row, subs_); }

    // Live rows satisfying `where` (nullptr = all of them).
    Result<std::vector<std::pair<RowId, Row>>> filter(const TableSchema& table,
                                                      const BoundExpr* where);
    // Materializes a bound relation (FROM clause). Rows that come straight
    // from one table keep their rowid for error messages; joined rows use 0.
    Result<std::vector<std::pair<RowId, Row>>> evaluate(const BoundRelation& rel);
    // GROUP BY + aggregates + HAVING: one group row per surviving group.
    Result<std::vector<Row>> aggregate(const BoundSelect& s,
                                       const std::vector<std::pair<RowId, Row>>& matches);
    // A SELECT (without its UNION members) down to sorted, distinct rows:
    // everything but OFFSET / LIMIT.
    Result<std::vector<Row>> collect(const BoundSelect& s);
    Result<void> checkPrimaryKey(const TableSchema& table, const Value& key,
                                 const std::vector<RowId>& ignore);

    IStorageEngine& engine_;
    Catalog& catalog_;
    const SubqueryRows* subs_ = nullptr;  // rows of the SELECT being run
};

}  // namespace ledger
