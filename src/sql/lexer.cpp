#include "sql/lexer.h"

#include <array>
#include <utility>

namespace ledger {

namespace {

// Keyword table, lowercase keys. Linear search: ~30 entries, negligible next
// to everything else, and zero allocation.
struct KeywordEntry {
    std::string_view name;
    TokenKind kind;
};

constexpr std::array<KeywordEntry, 52> kKeywords{{
    {"select", TokenKind::KwSelect},   {"from", TokenKind::KwFrom},
    {"where", TokenKind::KwWhere},     {"insert", TokenKind::KwInsert},
    {"into", TokenKind::KwInto},       {"values", TokenKind::KwValues},
    {"create", TokenKind::KwCreate},   {"drop", TokenKind::KwDrop},
    {"table", TokenKind::KwTable},     {"update", TokenKind::KwUpdate},
    {"set", TokenKind::KwSet},         {"delete", TokenKind::KwDelete},
    {"order", TokenKind::KwOrder},     {"by", TokenKind::KwBy},
    {"asc", TokenKind::KwAsc},         {"desc", TokenKind::KwDesc},
    {"limit", TokenKind::KwLimit},     {"and", TokenKind::KwAnd},
    {"or", TokenKind::KwOr},           {"not", TokenKind::KwNot},
    {"null", TokenKind::KwNull},       {"true", TokenKind::KwTrue},
    {"false", TokenKind::KwFalse},     {"is", TokenKind::KwIs},
    {"int", TokenKind::KwInt},         {"float", TokenKind::KwFloat},
    {"text", TokenKind::KwText},       {"bool", TokenKind::KwBool},
    {"primary", TokenKind::KwPrimary}, {"key", TokenKind::KwKey},
    {"view", TokenKind::KwView},       {"as", TokenKind::KwAs},
    {"group", TokenKind::KwGroup},     {"having", TokenKind::KwHaving},
    {"join", TokenKind::KwJoin},       {"inner", TokenKind::KwInner},
    {"left", TokenKind::KwLeft},       {"outer", TokenKind::KwOuter},
    {"on", TokenKind::KwOn},           {"distinct", TokenKind::KwDistinct},
    {"offset", TokenKind::KwOffset},   {"in", TokenKind::KwIn},
    {"between", TokenKind::KwBetween}, {"like", TokenKind::KwLike},
    {"case", TokenKind::KwCase},       {"when", TokenKind::KwWhen},
    {"then", TokenKind::KwThen},       {"else", TokenKind::KwElse},
    {"end", TokenKind::KwEnd},         {"union", TokenKind::KwUnion},
    {"all", TokenKind::KwAll},         {"exists", TokenKind::KwExists},
}};

// Explicit ASCII classification: we don't want to depend on the locale, and
// std::isalpha & co take an int that must be a valid unsigned char.
constexpr bool isAsciiLetter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isIdentStart(char c) noexcept { return isAsciiLetter(c) || c == '_'; }
constexpr bool isIdentChar(char c) noexcept { return isIdentStart(c) || isDigit(c); }
constexpr bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
constexpr char toLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

class Lexer {
public:
    explicit Lexer(std::string_view src) noexcept : src_(src) {}

    Result<std::vector<Token>> run() {
        std::vector<Token> out;
        for (;;) {
            skipSpaceAndComments();
            if (atEnd()) {
                out.push_back(Token{TokenKind::End, {}, line_, column_, pos_});
                return out;
            }
            const std::size_t start = pos_;
            LEDGER_TRY(tok, next());
            tok.offset = start;
            out.push_back(std::move(tok));
        }
    }

private:
    [[nodiscard]] bool atEnd() const noexcept { return pos_ >= src_.size(); }
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }

    // Consumes one character while tracking line/column. The only way input is
    // consumed: guarantees consistent positions.
    char advance() noexcept {
        const char c = src_[pos_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }

    void skipSpaceAndComments() noexcept {
        for (;;) {
            if (!atEnd() && isSpace(peek())) {
                advance();
            } else if (peek() == '-' && peek(1) == '-') {
                while (!atEnd() && peek() != '\n') advance();
            } else {
                return;
            }
        }
    }

    Error errorHere(std::size_t line, std::size_t column, std::string what) const {
        return makeError(ErrorCode::SyntaxError, std::to_string(line) + ":" +
                                                     std::to_string(column) + ": " + what);
    }

    Result<Token> next() {
        const std::size_t line = line_;
        const std::size_t column = column_;
        const char c = peek();

        if (isIdentStart(c)) return identifierOrKeyword(line, column);
        if (isDigit(c) || (c == '.' && isDigit(peek(1)))) return number(line, column);
        if (c == '\'') return string(line, column);
        if (c == '"') {
            return errorHere(line, column,
                             "quoted identifiers are not supported; identifiers are "
                             "case-insensitive and folded to lowercase");
        }
        return symbol(line, column);
    }

    Result<Token> identifierOrKeyword(std::size_t line, std::size_t column) {
        std::string text;
        while (!atEnd() && isIdentChar(peek())) text.push_back(toLowerAscii(advance()));
        for (const auto& kw : kKeywords) {
            if (kw.name == text) return Token{kw.kind, {}, line, column};
        }
        return Token{TokenKind::Identifier, std::move(text), line, column};
    }

    // Grammar: digits [ '.' digits ] [ ('e'|'E') ['+'|'-'] digits ]
    //        |  '.' digits [ exponent ]
    // A dot or an exponent with no digit behind it is an error, not a silent
    // split into two tokens: `1.` or `1e` makes no sense in SQL. An identifier
    // glued to the number (`12abc`) is rejected for the same reason.
    Result<Token> number(std::size_t line, std::size_t column) {
        std::string text;
        bool isFloat = false;

        while (isDigit(peek())) text.push_back(advance());

        if (peek() == '.') {
            isFloat = true;
            text.push_back(advance());
            if (!isDigit(peek())) return errorHere(line, column, "malformed number '" + text + "'");
            while (isDigit(peek())) text.push_back(advance());
        }

        if (peek() == 'e' || peek() == 'E') {
            isFloat = true;
            text.push_back(advance());
            if (peek() == '+' || peek() == '-') text.push_back(advance());
            if (!isDigit(peek())) return errorHere(line, column, "malformed number '" + text + "'");
            while (isDigit(peek())) text.push_back(advance());
        }

        // `1.5.2` or `12abc`: a number directly followed by a dot or a letter
        // is never valid; refuse rather than split in two.
        if (peek() == '.' || isIdentStart(peek())) {
            return errorHere(line, column, "malformed number '" + text + "': unexpected '" +
                                               std::string(1, peek()) + "'");
        }

        return Token{isFloat ? TokenKind::Float : TokenKind::Integer, std::move(text), line, column};
    }

    Result<Token> string(std::size_t line, std::size_t column) {
        advance();  // opening quote
        std::string text;
        for (;;) {
            if (atEnd()) return errorHere(line, column, "unterminated string literal");
            const char c = advance();
            if (c != '\'') {
                text.push_back(c);
            } else if (peek() == '\'') {
                advance();  // '' -> '
                text.push_back('\'');
            } else {
                return Token{TokenKind::String, std::move(text), line, column};
            }
        }
    }

    Result<Token> symbol(std::size_t line, std::size_t column) {
        const char c = advance();
        // Two-character symbols first: `<=` must not become `<` `=`.
        if (c == '<' && peek() == '=') { advance(); return Token{TokenKind::LtEq, {}, line, column}; }
        if (c == '<' && peek() == '>') { advance(); return Token{TokenKind::NotEq, {}, line, column}; }
        if (c == '>' && peek() == '=') { advance(); return Token{TokenKind::GtEq, {}, line, column}; }
        if (c == '!' && peek() == '=') { advance(); return Token{TokenKind::NotEq, {}, line, column}; }

        switch (c) {
            case '(': return Token{TokenKind::LParen, {}, line, column};
            case ')': return Token{TokenKind::RParen, {}, line, column};
            case ',': return Token{TokenKind::Comma, {}, line, column};
            case ';': return Token{TokenKind::Semicolon, {}, line, column};
            case '*': return Token{TokenKind::Star, {}, line, column};
            case '.': return Token{TokenKind::Dot, {}, line, column};
            case '+': return Token{TokenKind::Plus, {}, line, column};
            case '-': return Token{TokenKind::Minus, {}, line, column};
            case '/': return Token{TokenKind::Slash, {}, line, column};
            case '=': return Token{TokenKind::Eq, {}, line, column};
            case '<': return Token{TokenKind::Lt, {}, line, column};
            case '>': return Token{TokenKind::Gt, {}, line, column};
            default: break;
        }

        // Lone `!`, non-ASCII byte, control character... Show the code rather
        // than the character: an isolated UTF-8 byte is unreadable.
        const auto code = static_cast<unsigned>(static_cast<unsigned char>(c));
        std::string what = "unexpected character ";
        if (code >= 0x20 && code < 0x7F) {
            what += '\'';
            what += c;
            what += '\'';
        } else {
            what += "0x";
            constexpr char kHex[] = "0123456789ABCDEF";
            what += kHex[code >> 4];
            what += kHex[code & 0xF];
        }
        return errorHere(line, column, std::move(what));
    }

    std::string_view src_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

}  // namespace

std::string_view tokenKindName(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::KwSelect:   return "SELECT";
        case TokenKind::KwFrom:     return "FROM";
        case TokenKind::KwWhere:    return "WHERE";
        case TokenKind::KwInsert:   return "INSERT";
        case TokenKind::KwInto:     return "INTO";
        case TokenKind::KwValues:   return "VALUES";
        case TokenKind::KwCreate:   return "CREATE";
        case TokenKind::KwDrop:     return "DROP";
        case TokenKind::KwTable:    return "TABLE";
        case TokenKind::KwUpdate:   return "UPDATE";
        case TokenKind::KwSet:      return "SET";
        case TokenKind::KwDelete:   return "DELETE";
        case TokenKind::KwOrder:    return "ORDER";
        case TokenKind::KwBy:       return "BY";
        case TokenKind::KwAsc:      return "ASC";
        case TokenKind::KwDesc:     return "DESC";
        case TokenKind::KwLimit:    return "LIMIT";
        case TokenKind::KwAnd:      return "AND";
        case TokenKind::KwOr:       return "OR";
        case TokenKind::KwNot:      return "NOT";
        case TokenKind::KwNull:     return "NULL";
        case TokenKind::KwTrue:     return "TRUE";
        case TokenKind::KwFalse:    return "FALSE";
        case TokenKind::KwIs:       return "IS";
        case TokenKind::KwInt:      return "INT";
        case TokenKind::KwFloat:    return "FLOAT";
        case TokenKind::KwText:     return "TEXT";
        case TokenKind::KwBool:     return "BOOL";
        case TokenKind::KwPrimary:  return "PRIMARY";
        case TokenKind::KwKey:      return "KEY";
        case TokenKind::KwView:     return "VIEW";
        case TokenKind::KwAs:       return "AS";
        case TokenKind::KwGroup:    return "GROUP";
        case TokenKind::KwHaving:   return "HAVING";
        case TokenKind::KwJoin:     return "JOIN";
        case TokenKind::KwInner:    return "INNER";
        case TokenKind::KwLeft:     return "LEFT";
        case TokenKind::KwOuter:    return "OUTER";
        case TokenKind::KwOn:       return "ON";
        case TokenKind::KwDistinct: return "DISTINCT";
        case TokenKind::KwOffset:   return "OFFSET";
        case TokenKind::KwIn:       return "IN";
        case TokenKind::KwBetween:  return "BETWEEN";
        case TokenKind::KwLike:     return "LIKE";
        case TokenKind::KwCase:     return "CASE";
        case TokenKind::KwWhen:     return "WHEN";
        case TokenKind::KwThen:     return "THEN";
        case TokenKind::KwElse:     return "ELSE";
        case TokenKind::KwEnd:      return "END";
        case TokenKind::KwUnion:    return "UNION";
        case TokenKind::KwAll:      return "ALL";
        case TokenKind::KwExists:   return "EXISTS";
        case TokenKind::Identifier: return "identifier";
        case TokenKind::Integer:    return "integer";
        case TokenKind::Float:      return "float";
        case TokenKind::String:     return "string";
        case TokenKind::LParen:     return "(";
        case TokenKind::RParen:     return ")";
        case TokenKind::Comma:      return ",";
        case TokenKind::Semicolon:  return ";";
        case TokenKind::Star:       return "*";
        case TokenKind::Dot:        return ".";
        case TokenKind::Plus:       return "+";
        case TokenKind::Minus:      return "-";
        case TokenKind::Slash:      return "/";
        case TokenKind::Eq:         return "=";
        case TokenKind::NotEq:      return "<>";
        case TokenKind::Lt:         return "<";
        case TokenKind::LtEq:       return "<=";
        case TokenKind::Gt:         return ">";
        case TokenKind::GtEq:       return ">=";
        case TokenKind::End:        return "end of input";
    }
    return "?";
}

Result<std::vector<Token>> tokenize(std::string_view sql) { return Lexer{sql}.run(); }

}  // namespace ledger
