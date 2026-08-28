#include "doctest.h"

#include <string>
#include <vector>

#include "sql/lexer.h"

using namespace ledger;

namespace {

std::vector<Token> lex(std::string_view sql) { return tokenize(sql).value(); }

std::vector<TokenKind> kinds(std::string_view sql) {
    std::vector<TokenKind> out;
    for (const auto& t : lex(sql)) out.push_back(t.kind);
    return out;
}

// Error message of an invalid input. Precondition: tokenize fails.
std::string errorOf(std::string_view sql) {
    auto r = tokenize(sql);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::SyntaxError);
    return r.error().message;
}

}  // namespace

// ---- general structure -----------------------------------------------------

TEST_CASE("empty input yields only End") {
    CHECK(kinds("") == std::vector<TokenKind>{TokenKind::End});
    CHECK(kinds("   \n\t ") == std::vector<TokenKind>{TokenKind::End});
}

TEST_CASE("a full statement is tokenized in order") {
    CHECK(kinds("SELECT a, b FROM t WHERE x >= 10 ORDER BY a DESC LIMIT 5;") ==
          std::vector<TokenKind>{
              TokenKind::KwSelect, TokenKind::Identifier, TokenKind::Comma, TokenKind::Identifier,
              TokenKind::KwFrom, TokenKind::Identifier, TokenKind::KwWhere, TokenKind::Identifier,
              TokenKind::GtEq, TokenKind::Integer, TokenKind::KwOrder, TokenKind::KwBy,
              TokenKind::Identifier, TokenKind::KwDesc, TokenKind::KwLimit, TokenKind::Integer,
              TokenKind::Semicolon, TokenKind::End});
}

// ---- keywords and identifiers ---------------------------------------------

TEST_CASE("keywords are recognized regardless of case") {
    CHECK(kinds("select")[0] == TokenKind::KwSelect);
    CHECK(kinds("SELECT")[0] == TokenKind::KwSelect);
    CHECK(kinds("SeLeCt")[0] == TokenKind::KwSelect);
    CHECK(lex("SELECT")[0].text.empty());
}

TEST_CASE("every keyword maps to its own kind") {
    const struct { const char* sql; TokenKind kind; } cases[] = {
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
    };
    for (const auto& c : cases) {
        CAPTURE(c.sql);
        const auto toks = lex(c.sql);
        CHECK(toks[0].kind == c.kind);
        CHECK(toks[0].isKeyword());
    }
}

TEST_CASE("identifiers are folded to lowercase") {
    const auto toks = lex("Users user_ID _x a1B2");
    REQUIRE(toks.size() == 5);
    CHECK(toks[0].text == "users");
    CHECK(toks[1].text == "user_id");
    CHECK(toks[2].text == "_x");
    CHECK(toks[3].text == "a1b2");
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(toks[i].kind == TokenKind::Identifier);
        CHECK_FALSE(toks[i].isKeyword());
    }
}

TEST_CASE("a keyword prefix does not make an identifier a keyword") {
    CHECK(lex("selected")[0].kind == TokenKind::Identifier);
    CHECK(lex("from_table")[0].kind == TokenKind::Identifier);
    CHECK(lex("nullable")[0].kind == TokenKind::Identifier);
}

TEST_CASE("quoted identifiers are rejected") {
    CHECK(errorOf("SELECT \"Users\" FROM t") == "1:8: quoted identifiers are not supported; "
                                                "identifiers are case-insensitive and folded to lowercase");
}

TEST_CASE("non-ASCII bytes are rejected") {
    CHECK(errorOf("SELECT \xC3\xA9 FROM t") == "1:8: unexpected character 0xC3");
}

// ---- numbers ---------------------------------------------------------------

TEST_CASE("integer literals keep their source text") {
    const auto toks = lex("0 42 007 99999999999999999999");
    REQUIRE(toks.size() == 5);
    for (std::size_t i = 0; i < 4; ++i) CHECK(toks[i].kind == TokenKind::Integer);
    CHECK(toks[0].text == "0");
    CHECK(toks[1].text == "42");
    CHECK(toks[2].text == "007");
    // Overflow is not the lexer's problem: Value::fromText will report it.
    CHECK(toks[3].text == "99999999999999999999");
}

TEST_CASE("float literals: fraction and exponent forms") {
    const auto toks = lex("1.5 .5 1e3 1E-3 2.5e+10 0.0");
    REQUIRE(toks.size() == 7);
    for (std::size_t i = 0; i < 6; ++i) CHECK(toks[i].kind == TokenKind::Float);
    CHECK(toks[0].text == "1.5");
    CHECK(toks[1].text == ".5");
    CHECK(toks[2].text == "1e3");
    CHECK(toks[3].text == "1E-3");
    CHECK(toks[4].text == "2.5e+10");
    CHECK(toks[5].text == "0.0");
}

TEST_CASE("sign is never part of the number") {
    CHECK(kinds("-5") == std::vector<TokenKind>{TokenKind::Minus, TokenKind::Integer, TokenKind::End});
    CHECK(kinds("a-5") == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::Minus,
                                                 TokenKind::Integer, TokenKind::End});
    CHECK(kinds("+1.5") == std::vector<TokenKind>{TokenKind::Plus, TokenKind::Float, TokenKind::End});
}

TEST_CASE("malformed numbers are errors, not split tokens") {
    CHECK(errorOf("1.") == "1:1: malformed number '1.'");
    CHECK(errorOf("1e") == "1:1: malformed number '1e'");
    CHECK(errorOf("1e+") == "1:1: malformed number '1e+'");
    CHECK(errorOf("1.5.2") == "1:1: malformed number '1.5': unexpected '.'");
    CHECK(errorOf("12abc") == "1:1: malformed number '12': unexpected 'a'");
    CHECK(errorOf("1.5e3x") == "1:1: malformed number '1.5e3': unexpected 'x'");
}

TEST_CASE("a lone dot is an error") {
    CHECK(errorOf(".") == "1:1: unexpected character '.'");
    CHECK(errorOf("a.b") == "1:2: unexpected character '.'");
}

// ---- strings ---------------------------------------------------------------

TEST_CASE("string literals are unquoted and unescaped") {
    const auto toks = lex("'hello' '' 'it''s' 'a''''b'");
    REQUIRE(toks.size() == 5);
    for (std::size_t i = 0; i < 4; ++i) CHECK(toks[i].kind == TokenKind::String);
    CHECK(toks[0].text == "hello");
    CHECK(toks[1].text.empty());
    CHECK(toks[2].text == "it's");
    CHECK(toks[3].text == "a''b");
}

TEST_CASE("string literals preserve case, spaces and non-ASCII content") {
    CHECK(lex("'Hello World'")[0].text == "Hello World");
    CHECK(lex("'caf\xC3\xA9'")[0].text == "caf\xC3\xA9");
    CHECK(lex("'SELECT'")[0].kind == TokenKind::String);
}

TEST_CASE("string literals may span lines and positions stay correct") {
    const auto toks = lex("'a\nb' x");
    REQUIRE(toks.size() == 3);
    CHECK(toks[0].text == "a\nb");
    CHECK(toks[1].line == 2);
    CHECK(toks[1].column == 4);
}

TEST_CASE("unterminated strings are errors reported at the opening quote") {
    CHECK(errorOf("'abc") == "1:1: unterminated string literal");
    CHECK(errorOf("SELECT 'a''") == "1:8: unterminated string literal");
    CHECK(errorOf("x = 'a\n") == "1:5: unterminated string literal");
}

// ---- symbols ---------------------------------------------------------------

TEST_CASE("every symbol is recognized") {
    CHECK(kinds("( ) , ; * + - / = <> != < <= > >=") ==
          std::vector<TokenKind>{TokenKind::LParen, TokenKind::RParen, TokenKind::Comma,
                                 TokenKind::Semicolon, TokenKind::Star, TokenKind::Plus,
                                 TokenKind::Minus, TokenKind::Slash, TokenKind::Eq,
                                 TokenKind::NotEq, TokenKind::NotEq, TokenKind::Lt,
                                 TokenKind::LtEq, TokenKind::Gt, TokenKind::GtEq, TokenKind::End});
}

TEST_CASE("two-character symbols win over single ones, without whitespace") {
    CHECK(kinds("a<=b") == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::LtEq,
                                                  TokenKind::Identifier, TokenKind::End});
    CHECK(kinds("a<>b") == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::NotEq,
                                                  TokenKind::Identifier, TokenKind::End});
    CHECK(kinds("a<-1") == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::Lt,
                                                  TokenKind::Minus, TokenKind::Integer, TokenKind::End});
    CHECK(kinds("(1,2)") == std::vector<TokenKind>{TokenKind::LParen, TokenKind::Integer,
                                                   TokenKind::Comma, TokenKind::Integer,
                                                   TokenKind::RParen, TokenKind::End});
}

TEST_CASE("a lone '!' is an error") {
    CHECK(errorOf("a ! b") == "1:3: unexpected character '!'");
}

TEST_CASE("unknown characters are errors") {
    CHECK(errorOf("a @ b") == "1:3: unexpected character '@'");
    CHECK(errorOf("a # b") == "1:3: unexpected character '#'");
    CHECK(errorOf("\x01") == "1:1: unexpected character 0x01");
}

// ---- comments --------------------------------------------------------------

TEST_CASE("line comments are ignored") {
    CHECK(kinds("SELECT -- everything after is ignored\n 1") ==
          std::vector<TokenKind>{TokenKind::KwSelect, TokenKind::Integer, TokenKind::End});
    CHECK(kinds("-- only a comment") == std::vector<TokenKind>{TokenKind::End});
    CHECK(kinds("x --") == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::End});
}

TEST_CASE("a comment marker inside a string is content") {
    CHECK(lex("'-- not a comment'")[0].text == "-- not a comment");
}

TEST_CASE("'-' followed by something else is Minus") {
    CHECK(kinds("a - b") == std::vector<TokenKind>{TokenKind::Identifier, TokenKind::Minus,
                                                   TokenKind::Identifier, TokenKind::End});
}

// ---- positions -------------------------------------------------------------

TEST_CASE("tokens carry 1-based line and column") {
    const auto toks = lex("SELECT a\n  FROM t\n\nWHERE x");
    REQUIRE(toks.size() == 7);
    CHECK((toks[0].line == 1 && toks[0].column == 1));   // SELECT
    CHECK((toks[1].line == 1 && toks[1].column == 8));   // a
    CHECK((toks[2].line == 2 && toks[2].column == 3));   // FROM
    CHECK((toks[3].line == 2 && toks[3].column == 8));   // t
    CHECK((toks[4].line == 4 && toks[4].column == 1));   // WHERE
    CHECK((toks[5].line == 4 && toks[5].column == 7));   // x
    CHECK((toks[6].line == 4 && toks[6].column == 8));   // End
}

TEST_CASE("CRLF line endings count as one line break") {
    const auto toks = lex("a\r\nb");
    REQUIRE(toks.size() == 3);
    CHECK(toks[1].line == 2);
    CHECK(toks[1].column == 1);
}

TEST_CASE("error positions point at the offending token on later lines") {
    CHECK(errorOf("SELECT a\nFROM \"t\"") == "2:6: quoted identifiers are not supported; "
                                             "identifiers are case-insensitive and folded to lowercase");
}

// ---- token names -----------------------------------------------------------

TEST_CASE("tokenKindName covers keywords, symbols and categories") {
    CHECK(tokenKindName(TokenKind::KwSelect) == "SELECT");
    CHECK(tokenKindName(TokenKind::LtEq) == "<=");
    CHECK(tokenKindName(TokenKind::Identifier) == "identifier");
    CHECK(tokenKindName(TokenKind::End) == "end of input");
}
