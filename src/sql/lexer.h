#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace ledger {

// Un mot-clé par valeur d'enum : le parser peut faire un switch exhaustif et
// une faute de frappe dans un nom de mot-clé devient une erreur de compilation.
enum class TokenKind {
    // Mots-clés
    KwSelect, KwFrom, KwWhere, KwInsert, KwInto, KwValues, KwCreate, KwDrop,
    KwTable, KwUpdate, KwSet, KwDelete, KwOrder, KwBy, KwAsc, KwDesc, KwLimit,
    KwAnd, KwOr, KwNot, KwNull, KwTrue, KwFalse, KwIs, KwInt, KwFloat, KwText,
    KwBool, KwPrimary, KwKey,

    // Lexèmes porteurs de texte
    Identifier,  // replié en minuscules ASCII
    Integer,     // texte du littéral, non converti (voir Value::fromText)
    Float,       // idem
    String,      // contenu déséchappé ('' -> ')

    // Symboles
    LParen, RParen, Comma, Semicolon, Star,
    Plus, Minus, Slash,
    Eq, NotEq, Lt, LtEq, Gt, GtEq,

    End,  // toujours dernier token de la liste
};

std::string_view tokenKindName(TokenKind kind) noexcept;

struct Token {
    TokenKind kind;
    // Identifier : nom replié. Integer/Float : texte source. String : contenu
    // déséchappé. Vide pour les mots-clés et symboles.
    std::string text;
    std::size_t line;    // 1-based
    std::size_t column;  // 1-based, en octets, position du premier caractère

    [[nodiscard]] bool isKeyword() const noexcept {
        return kind >= TokenKind::KwSelect && kind <= TokenKind::KwKey;
    }
};

// Découpe une requête SQL complète en tokens, en un seul passage. La liste se
// termine toujours par un token End. Toute erreur lexicale (caractère inconnu,
// chaîne non terminée, nombre mal formé, identifiant entre guillemets) est un
// SyntaxError qui indique ligne:colonne.
//
// Choix v1 :
//  - identifiants : [A-Za-z_][A-Za-z0-9_]*, repliés en minuscules ; le non-ASCII
//    est rejeté ;
//  - "identifiant" entre guillemets doubles : rejeté (préserverait la casse, ce
//    qui contredit la décision « tout minuscule ») ;
//  - nombres : le signe n'est jamais lexé avec le nombre ('-' est un Minus, le
//    parser gère l'unaire), sinon `a-5` serait mal découpé ;
//  - chaînes : 'quotes simples', '' pour un quote interne, pas d'antislash ;
//  - commentaires : `-- jusqu'à la fin de ligne`.
Result<std::vector<Token>> tokenize(std::string_view sql);

}  // namespace ledger
