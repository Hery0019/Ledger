#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "core/row.h"
#include "semantic/bound.h"
#include "semantic/catalog.h"
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
// SELECT: filter (WHERE must be true; NULL and false reject), stable sort by
// Value::compare (NULL smaller than everything: first in ASC, last in DESC),
// LIMIT, then projection. Without ORDER BY, rowid order.
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

    // Live rows satisfying `where` (nullptr = all of them).
    Result<std::vector<std::pair<RowId, Row>>> filter(const TableSchema& table,
                                                      const BoundExpr* where);
    // Materializes a bound relation (FROM clause). Rows that come straight
    // from one table keep their rowid for error messages; joined rows use 0.
    Result<std::vector<std::pair<RowId, Row>>> evaluate(const BoundRelation& rel);
    // GROUP BY + aggregates + HAVING: one group row per surviving group.
    Result<std::vector<Row>> aggregate(const BoundSelect& s,
                                       const std::vector<std::pair<RowId, Row>>& matches);
    Result<void> checkPrimaryKey(const TableSchema& table, const Value& key,
                                 const std::vector<RowId>& ignore);

    IStorageEngine& engine_;
    Catalog& catalog_;
};

}  // namespace ledger
