#pragma once

#include <string_view>

#include "core/result.h"
#include "sql/ast.h"

namespace ledger {

// Analyse une instruction SQL complète (lexer + parser) et produit son AST.
//
//  - une seule instruction par appel ; le `;` final est facultatif, tout ce
//    qui suit est une erreur (la CLI se charge de découper le multi-instructions) ;
//  - toute erreur est un SyntaxError positionné `ligne:col: expected X, got Y` ;
//    on s'arrête à la première, pas de récupération ;
//  - les littéraux sont convertis en Value ici (Value::fromText), donc un
//    entier hors plage est signalé avec sa position ;
//  - `-5` reste Unary(Neg, 5) : le pliage de constantes est le rôle du binder.
//
// Précédence des expressions, de la plus faible à la plus forte :
//   OR  <  AND  <  NOT  <  comparaison (= <> < <= > >=, IS [NOT] NULL ;
//   non associative : `a = b = c` est rejeté)  <  + -  <  * /  <  - unaire.
Result<ast::Statement> parse(std::string_view sql);

}  // namespace ledger
