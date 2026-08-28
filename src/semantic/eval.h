#pragma once

#include "core/result.h"
#include "core/row.h"
#include "semantic/bound.h"

namespace ledger {

// Évalue une expression liée sur une ligne. Unique implémentation de la
// sémantique des opérateurs : utilisée par le binder (pliage de constantes)
// et par l'exécuteur (ligne par ligne).
//
// Précondition : l'expression a été produite par le binder, donc les types
// sont compatibles. Les seules erreurs possibles dépendent des données :
//  - division par zéro                      -> TypeError
//  - débordement d'un Int (+ - * / négation) -> TypeError, jamais de wrap
//  - résultat Float non fini                 -> TypeError (via Value::real)
//
// Sémantique de NULL (logique à trois états SQL) :
//  - arithmétique, comparaison, négation : NULL si un opérande est NULL ;
//  - AND : false si un opérande est false, sinon NULL si un est NULL, sinon true ;
//  - OR  : true si un opérande est true, sinon NULL si un est NULL, sinon false ;
//  - NOT NULL = NULL ; x IS NULL est toujours un vrai booléen.
Result<Value> eval(const BoundExpr& expr, const Row& row);

// Vrai si l'expression ne référence aucune colonne (donc évaluable sans ligne).
[[nodiscard]] bool isConstant(const BoundExpr& expr) noexcept;

}  // namespace ledger
