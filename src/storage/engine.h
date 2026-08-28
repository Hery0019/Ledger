#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "core/row.h"
#include "core/schema.h"

namespace ledger {

// Identifiant d'une ligne, unique dans sa table, monotone, jamais réutilisé.
// Un update conserve le rowid : la ligne garde son identité.
using RowId = std::uint64_t;

// Contrat du moteur de stockage. L'exécuteur ne connaît que cette interface ;
// FileEngine (fichiers texte) et MemoryEngine (tests) l'implémentent.
//
// Le moteur ne vérifie PAS les types des valeurs (c'est le binder) ; il
// vérifie seulement que la Row a le bon nombre de colonnes (sinon Internal :
// c'est un bug de l'appelant, pas une erreur attendue).
class IStorageEngine {
public:
    virtual ~IStorageEngine() = default;

    virtual Result<void> createTable(const TableSchema& schema) = 0;   // AlreadyExists
    virtual Result<void> dropTable(std::string_view table) = 0;        // NotFound

    virtual Result<RowId> insert(std::string_view table, const Row& row) = 0;

    // Parcourt les lignes vivantes par rowid croissant. `visit` renvoie false
    // pour arrêter. Aucune copie de la table entière : c'est pour ça qu'on
    // passe un callback et non un vector.
    // Précondition : ne pas modifier la table pendant le parcours.
    virtual Result<void> scan(std::string_view table,
                              const std::function<bool(RowId, const Row&)>& visit) = 0;

    virtual Result<void> update(std::string_view table, RowId id, const Row& row) = 0;  // NotFound
    virtual Result<void> remove(std::string_view table, RowId id) = 0;                  // NotFound

    // Réécrit le fichier de lignes sans les tombstones. Le moteur peut aussi le
    // déclencher de lui-même.
    virtual Result<void> compact(std::string_view table) = 0;

    // Schémas de toutes les tables existantes, pour remplir le Catalog au
    // démarrage.
    virtual Result<std::vector<TableSchema>> loadSchemas() = 0;
};

}  // namespace ledger
