#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace ledger {

// One enum value per keyword: the parser can switch exhaustively, and a typo
// in a keyword name becomes a compile error.
enum class TokenKind {
    // Keywords
    KwSelect, KwFrom, KwWhere, KwInsert, KwInto, KwValues, KwCreate, KwDrop,
    KwTable, KwUpdate, KwSet, KwDelete, KwOrder, KwBy, KwAsc, KwDesc, KwLimit,
    KwAnd, KwOr, KwNot, KwNull, KwTrue, KwFalse, KwIs, KwInt, KwFloat, KwText,
    KwBool, KwPrimary, KwKey, KwView, KwAs, KwGroup, KwHaving, KwJoin, KwInner,
    KwLeft, KwOuter, KwOn, KwDistinct, KwOffset, KwIn, KwBetween, KwLike,
    KwCase, KwWhen, KwThen, KwElse, KwEnd, KwUnion, KwAll, KwExists, KwBegin,
    KwCommit, KwRollback, KwTransaction,

    // Lexemes carrying text
    Identifier,  // folded to ASCII lowercase
    Integer,     // literal text, not converted (see Value::fromText)
    Float,       // same
    String,      // unescaped content ('' -> ')

    // Symbols
    LParen, RParen, Comma, Semicolon, Star, Dot,
    Plus, Minus, Slash,
    Eq, NotEq, Lt, LtEq, Gt, GtEq,

    End,  // always the last token of the list
};

std::string_view tokenKindName(TokenKind kind) noexcept;

struct Token {
    TokenKind kind;
    // Identifier: folded name. Integer/Float: source text. String: unescaped
    // content. Empty for keywords and symbols.
    std::string text;
    std::size_t line;    // 1-based
    std::size_t column;  // 1-based, in bytes, position of the first character
    std::size_t offset = 0;  // byte offset of the first character in the input

    [[nodiscard]] bool isKeyword() const noexcept {
        return kind >= TokenKind::KwSelect && kind <= TokenKind::KwTransaction;
    }
};

// Splits a complete SQL query into tokens, in a single pass. The list always
// ends with an End token. Any lexical error (unknown character, unterminated
// string, malformed number, quoted identifier) is a SyntaxError giving
// line:column.
//
// v1 choices:
//  - identifiers: [A-Za-z_][A-Za-z0-9_]*, folded to lowercase; non-ASCII is
//    rejected;
//  - "identifier" in double quotes: rejected (it would preserve case, which
//    contradicts the "everything lowercase" decision);
//  - numbers: the sign is never lexed with the number ('-' is a Minus, the
//    parser handles unary), otherwise `a-5` would be split wrongly;
//  - strings: 'single quotes', '' for an inner quote, no backslash;
//  - comments: `-- to end of line`.
Result<std::vector<Token>> tokenize(std::string_view sql);

}  // namespace ledger
