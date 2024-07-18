#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "exec/executor.h"
#include "semantic/catalog.h"
#include "storage/file_engine.h"

namespace ledger {

// Assemble FileEngine + Catalog + Executor sur un dossier. C'est l'objet que
// manipule la CLI ; main.cpp ne fait que l'entrée/sortie.
class Database {
public:
    // Ouvre (ou crée) la base et charge les schémas dans le catalogue.
    static Result<std::unique_ptr<Database>> open(const std::filesystem::path& dir);

    Result<QueryResult> execute(std::string_view sql) { return executor_.execute(sql); }

    [[nodiscard]] const Catalog& catalog() const noexcept { return catalog_; }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return engine_->directory(); }

    // Avertissements du moteur (lignes tronquées ignorées...), vidés à l'appel.
    std::vector<std::string> takeWarnings() { return engine_->takeWarnings(); }

private:
    explicit Database(std::unique_ptr<FileEngine> engine)
        : engine_(std::move(engine)), executor_(*engine_, catalog_) {}

    std::unique_ptr<FileEngine> engine_;
    Catalog catalog_;
    Executor executor_;
};

// Découpe un texte en instructions sur les `;` situés hors chaîne '...' et
// hors commentaire `--`. Le `;` n'est pas conservé. Les instructions vides
// (blancs / commentaires seuls) sont omises. Un `;` final est facultatif.
//
// `sql` commence au premier caractère utile (blancs et commentaires de tête
// retirés), `line` est sa ligne (1-based) dans le texte d'origine : une
// position `L:C` du parser correspond donc à la ligne `line + L - 1`.
struct ScriptStatement {
    std::string sql;
    std::size_t line;
};
std::vector<ScriptStatement> splitStatements(std::string_view text);

// Vrai si le texte se termine par une instruction complète (dernier `;` hors
// chaîne et hors commentaire, suivi seulement de blancs / commentaires). Sert
// au REPL pour savoir s'il faut continuer à lire.
[[nodiscard]] bool endsWithCompleteStatement(std::string_view text);

// Tableau ASCII aligné d'un résultat SELECT. Vide si pas de colonnes.
std::string formatTable(const QueryResult& result);

// Ligne de statut : "3 rows", "1 row affected", "ok".
std::string formatSummary(const QueryResult& result);

}  // namespace ledger
