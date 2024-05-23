#pragma once

#include "core/result.h"
#include "semantic/bound.h"
#include "semantic/catalog.h"
#include "sql/ast.h"

namespace ledger {

// Vérifie et résout une instruction contre le catalogue. Ne modifie ni l'AST
// ni le catalogue (un CREATE/DROP lié n'est appliqué au catalogue qu'après
// exécution réussie, par l'exécuteur).
//
// Erreurs :
//  - NotFound            table ou colonne inconnue
//  - AlreadyExists       CREATE TABLE sur une table existante
//  - TypeError           opérande ou valeur d'un type incompatible
//  - ConstraintViolation NULL dans une colonne NOT NULL / PRIMARY KEY
//  - SyntaxError         forme invalide non détectable par le parser (colonne en
//                        double, plusieurs PRIMARY KEY, mauvais nombre de valeurs,
//                        référence de colonne dans VALUES)
// Les erreurs issues d'une expression sont préfixées `ligne:col: `.
//
// Règles de typage :
//  - + - * /  : Int×Int -> Int ; numérique×numérique -> Float ; sinon TypeError
//  - = <> < <= > >= : numériques entre eux, Text×Text, Bool×Bool -> Bool
//  - AND OR NOT : Bool uniquement (pas de « 1 est vrai »)
//  - NULL littéral : compatible avec tout, le résultat est de type Null si les
//    deux opérandes le sont, sinon du type de l'autre opérande
//  - WHERE : Bool ou Null
//  - INSERT / UPDATE : type identique à la colonne, ou Int vers Float (cast
//    inséré). Aucune conversion Text <-> nombre.
//  - toute sous-expression sans colonne est pliée en constante (une erreur de
//    données, ex. 1/0, est alors signalée dès le binding).
Result<BoundStatement> bind(const ast::Statement& stmt, const Catalog& catalog);

}  // namespace ledger
