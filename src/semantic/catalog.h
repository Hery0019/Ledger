#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "core/value.h"

namespace ledger {

struct ColumnSchema {
    std::string name;  // minuscules
    DataType type;     // jamais DataType::Null
    bool primaryKey;
    bool notNull;      // toujours true si primaryKey
};

struct TableSchema {
    std::string name;  // minuscules
    std::vector<ColumnSchema> columns;  // jamais vide

    [[nodiscard]] std::optional<std::size_t> columnIndex(std::string_view column) const noexcept;
    // Index de la colonne PRIMARY KEY, s'il y en a une (au plus une par table).
    [[nodiscard]] std::optional<std::size_t> primaryKeyIndex() const noexcept;
};

// Ensemble des schémas connus. En mémoire, mono-processus : la couche stockage
// le remplit au démarrage (lecture des schema.txt) et le tient à jour après
// chaque CREATE/DROP effectif. Le catalogue lui-même ne touche jamais au disque.
//
// Les pointeurs renvoyés par find() restent valides tant que la table n'est
// pas retirée (std::map ne déplace pas ses nœuds).
class Catalog {
public:
    [[nodiscard]] const TableSchema* find(std::string_view table) const noexcept;
    [[nodiscard]] bool contains(std::string_view table) const noexcept { return find(table) != nullptr; }

    Result<void> add(TableSchema schema);          // AlreadyExists
    Result<void> remove(std::string_view table);   // NotFound

    [[nodiscard]] std::vector<std::string_view> tableNames() const;  // triés
    [[nodiscard]] std::size_t size() const noexcept { return tables_.size(); }

private:
    // std::less<> : find() par string_view sans construire de std::string.
    std::map<std::string, TableSchema, std::less<>> tables_;
};

}  // namespace ledger
