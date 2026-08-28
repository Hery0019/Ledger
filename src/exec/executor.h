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

struct QueryResult {
    std::vector<std::string> columns;  // en-têtes (SELECT uniquement)
    std::vector<Row> rows;             // lignes projetées (SELECT uniquement)
    std::size_t affected = 0;          // INSERT/UPDATE/DELETE : lignes touchées
};

// Déroule un BoundStatement sur un moteur de stockage. Seul endroit qui
// modifie le Catalog (après un CREATE/DROP réussi côté stockage) et qui
// applique la contrainte PRIMARY KEY (par scan : pas d'index en v1).
//
// Pas de transactions : toutes les erreurs prévisibles (types, PK, erreur
// d'évaluation sur une ligne) sont levées AVANT la première écriture, en deux
// passes pour UPDATE/DELETE. Seule une IoError en cours d'écriture peut
// laisser un état partiel ; elle est remontée telle quelle.
//
// SELECT : filtre (WHERE doit valoir true ; NULL et false rejettent), tri
// stable par Value::compare (NULL plus petit que tout : premier en ASC,
// dernier en DESC), LIMIT, puis projection. Sans ORDER BY, ordre des rowid.
class Executor {
public:
    Executor(IStorageEngine& engine, Catalog& catalog) noexcept
        : engine_(engine), catalog_(catalog) {}

    // Texte SQL -> parse -> bind -> exécution.
    Result<QueryResult> execute(std::string_view sql);
    Result<QueryResult> execute(const BoundStatement& stmt);

private:
    Result<QueryResult> run(const BoundCreateTable& s);
    Result<QueryResult> run(const BoundDropTable& s);
    Result<QueryResult> run(const BoundInsert& s);
    Result<QueryResult> run(const BoundSelect& s);
    Result<QueryResult> run(const BoundUpdate& s);
    Result<QueryResult> run(const BoundDelete& s);

    // Lignes vivantes satisfaisant `where` (nullptr = toutes).
    Result<std::vector<std::pair<RowId, Row>>> filter(const TableSchema& table,
                                                      const BoundExpr* where);
    Result<void> checkPrimaryKey(const TableSchema& table, const Value& key,
                                 const std::vector<RowId>& ignore);

    IStorageEngine& engine_;
    Catalog& catalog_;
};

}  // namespace ledger
