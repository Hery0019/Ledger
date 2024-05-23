#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "core/row.h"
#include "core/value.h"
#include "semantic/catalog.h"
#include "sql/ast.h"

// Plan lié : ce que produit le binder à partir d'un ast::Statement. Toutes les
// références sont résolues (colonnes -> indices, tables -> schémas) et tous les
// types sont vérifiés. L'exécuteur déroule ce plan sans jamais avoir à lever
// une erreur sémantique ; les seules erreurs possibles à l'exécution dépendent
// des données (division par zéro, débordement, contrainte violée).
namespace ledger {

struct BoundExpr;
using BoundExprPtr = std::unique_ptr<BoundExpr>;

struct BoundColumn {
    std::size_t index;  // dans la Row de la table courante
};

struct BoundUnary {
    ast::UnaryOp op;
    BoundExprPtr operand;
};

struct BoundBinary {
    ast::BinaryOp op;
    BoundExprPtr lhs;
    BoundExprPtr rhs;
};

struct BoundIsNull {
    BoundExprPtr operand;
    bool negated;
};

// Seule conversion implicite du moteur : Int -> Float (promotion sans perte
// notable). Insérée par le binder quand une expression Int alimente une
// colonne Float. NULL traverse inchangé.
struct BoundCast {
    BoundExprPtr operand;
    DataType to;  // toujours DataType::Float en v1
};

struct BoundExpr {
    std::variant<Value, BoundColumn, BoundUnary, BoundBinary, BoundIsNull, BoundCast> node;
    // Type statique. DataType::Null signifie « toujours NULL » (ex. le littéral
    // NULL, ou NULL + NULL). Une expression de type Int peut quand même
    // produire NULL à l'exécution (colonne nullable) : Null n'est pas un
    // sous-type, c'est le type de l'absence garantie.
    DataType type;
};

struct BoundCreateTable {
    TableSchema schema;  // validé : noms uniques, <= 1 PRIMARY KEY, PK => NOT NULL
};

struct BoundDropTable {
    std::string table;
};

struct BoundInsert {
    const TableSchema* table;
    Row row;  // complète, dans l'ordre du schéma, types déjà conformes
};

struct BoundOrderBy {
    std::size_t column;
    bool descending;
};

struct BoundSelect {
    const TableSchema* table;
    std::vector<std::size_t> projection;  // jamais vide (`*` est développé)
    BoundExprPtr where;                   // nullptr = toutes les lignes ; type Bool ou Null
    std::optional<BoundOrderBy> orderBy;
    std::optional<std::int64_t> limit;
};

struct BoundUpdate {
    const TableSchema* table;
    std::vector<std::pair<std::size_t, BoundExprPtr>> assignments;  // type conforme à la colonne
    BoundExprPtr where;
};

struct BoundDelete {
    const TableSchema* table;
    BoundExprPtr where;
};

using BoundStatement = std::variant<BoundCreateTable, BoundDropTable, BoundInsert, BoundSelect,
                                    BoundUpdate, BoundDelete>;

}  // namespace ledger
