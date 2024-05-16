#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/value.h"

// Arbre syntaxique produit par le parser. Données pures : aucune logique, pas
// de vérification sémantique (existence des tables, types des colonnes...),
// c'est le rôle du binder. Tous les identifiants sont déjà repliés en
// minuscules par le lexer.
//
// Choix : std::variant + unique_ptr plutôt qu'une hiérarchie virtuelle. Le
// binder et l'exécuteur font un std::visit exhaustif : ajouter un nœud sans
// le traiter partout devient une erreur de compilation.
namespace ledger::ast {

// ---- expressions -----------------------------------------------------------

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class UnaryOp { Not, Neg };
enum class BinaryOp { Or, And, Eq, NotEq, Lt, LtEq, Gt, GtEq, Add, Sub, Mul, Div };

std::string_view unaryOpName(UnaryOp op) noexcept;
std::string_view binaryOpName(BinaryOp op) noexcept;

struct Literal {
    Value value;  // Int/Float/Text/Bool/Null, déjà convertie par le parser
};

struct ColumnRef {
    std::string name;
};

struct Unary {
    UnaryOp op;
    ExprPtr operand;
};

struct Binary {
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
};

// `x IS NULL` / `x IS NOT NULL`. Nœud à part : ce n'est pas une comparaison
// (une comparaison avec NULL donnerait Unknown, ici on veut un vrai booléen).
struct IsNull {
    ExprPtr operand;
    bool negated;
};

struct Expr {
    std::variant<Literal, ColumnRef, Unary, Binary, IsNull> node;
    // Position du premier token de l'expression, pour les erreurs du binder.
    std::size_t line;
    std::size_t column;
};

// ---- instructions ----------------------------------------------------------

struct ColumnDef {
    std::string name;
    DataType type;  // jamais DataType::Null
    bool primaryKey;
    bool notNull;
};

struct CreateTable {
    std::string table;
    std::vector<ColumnDef> columns;  // jamais vide
};

struct DropTable {
    std::string table;
};

struct Insert {
    std::string table;
    std::vector<std::string> columns;  // vide = toutes les colonnes, dans l'ordre du schéma
    std::vector<ExprPtr> values;       // jamais vide ; une seule ligne en v1
};

struct OrderBy {
    std::string column;
    bool descending;
};

struct Select {
    std::vector<std::string> columns;  // vide = `*`
    std::string table;
    ExprPtr where;  // nullptr si absent
    std::optional<OrderBy> orderBy;
    std::optional<std::int64_t> limit;  // >= 0 si présent
};

struct Update {
    std::string table;
    std::vector<std::pair<std::string, ExprPtr>> assignments;  // jamais vide
    ExprPtr where;  // nullptr si absent
};

struct Delete {
    std::string table;
    ExprPtr where;  // nullptr si absent
};

using Statement = std::variant<CreateTable, DropTable, Insert, Select, Update, Delete>;

}  // namespace ledger::ast
