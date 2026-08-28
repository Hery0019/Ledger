#include "sql/parser.h"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "sql/lexer.h"

namespace ledger {

namespace {

using namespace ast;

class Parser {
public:
    Parser(std::vector<Token> tokens, std::string_view src) noexcept
        : tokens_(std::move(tokens)), src_(src) {}

    Result<Statement> run() {
        LEDGER_TRY(stmt, statement());
        if (at(TokenKind::Semicolon)) advance();
        if (!at(TokenKind::End)) return unexpected("end of input");
        return stmt;
    }

private:
    // ---- token access ------------------------------------------------------

    [[nodiscard]] const Token& peek() const noexcept { return tokens_[pos_]; }
    [[nodiscard]] bool at(TokenKind kind) const noexcept { return peek().kind == kind; }

    // The last token is always End: we never run past it.
    const Token& advance() noexcept {
        const Token& t = tokens_[pos_];
        if (!at(TokenKind::End)) ++pos_;
        return t;
    }

    bool accept(TokenKind kind) noexcept {
        if (!at(kind)) return false;
        advance();
        return true;
    }

    Result<const Token*> expect(TokenKind kind) {
        if (!at(kind)) return unexpected("'" + std::string(tokenKindName(kind)) + "'");
        return &advance();
    }

    Result<std::string> identifier() {
        if (!at(TokenKind::Identifier)) return unexpected("identifier");
        return advance().text;
    }

    // ---- errors ------------------------------------------------------------

    static std::string describe(const Token& t) {
        switch (t.kind) {
            case TokenKind::Identifier: return "identifier '" + t.text + "'";
            case TokenKind::Integer:    return "integer '" + t.text + "'";
            case TokenKind::Float:      return "float '" + t.text + "'";
            case TokenKind::String:     return "string '" + t.text + "'";
            default:                    return "'" + std::string(tokenKindName(t.kind)) + "'";
        }
    }

    static Error errorAt(const Token& t, const std::string& what) {
        return makeError(ErrorCode::SyntaxError,
                         std::to_string(t.line) + ":" + std::to_string(t.column) + ": " + what);
    }

    Error unexpected(const std::string& expected) const {
        return errorAt(peek(), "expected " + expected + ", got " + describe(peek()));
    }

    // ---- statements --------------------------------------------------------

    Result<Statement> statement() {
        switch (peek().kind) {
            case TokenKind::KwCreate: return create();
            case TokenKind::KwDrop:   return drop();
            case TokenKind::KwInsert: return insert();
            case TokenKind::KwSelect: return select();
            case TokenKind::KwUpdate: return update();
            case TokenKind::KwDelete: return del();
            default: return unexpected("statement (CREATE, DROP, INSERT, SELECT, UPDATE or DELETE)");
        }
    }

    Result<Statement> create() {
        advance();  // CREATE
        if (at(TokenKind::KwView)) return createView();
        if (at(TokenKind::KwTable)) return createTable();
        return unexpected("'TABLE' or 'VIEW'");
    }

    Result<Statement> drop() {
        advance();  // DROP
        if (at(TokenKind::KwView)) return dropView();
        if (at(TokenKind::KwTable)) return dropTable();
        return unexpected("'TABLE' or 'VIEW'");
    }

    // CREATE TABLE name ( col type [PRIMARY KEY] [NOT NULL] {, ...} )
    Result<Statement> createTable() {
        advance();  // TABLE
        LEDGER_TRY(table, identifier());
        LEDGER_TRY_VOID(expect(TokenKind::LParen));

        std::vector<ColumnDef> columns;
        do {
            LEDGER_TRY(col, columnDef());
            columns.push_back(std::move(col));
        } while (accept(TokenKind::Comma));

        LEDGER_TRY_VOID(expect(TokenKind::RParen));
        return Statement{CreateTable{std::move(table), std::move(columns)}};
    }

    Result<ColumnDef> columnDef() {
        LEDGER_TRY(name, identifier());
        LEDGER_TRY(type, dataType());

        ColumnDef col{std::move(name), type, false, false};
        // Constraints in any order, each at most once.
        for (;;) {
            if (at(TokenKind::KwPrimary)) {
                if (col.primaryKey) return unexpected("a single PRIMARY KEY constraint");
                advance();
                LEDGER_TRY_VOID(expect(TokenKind::KwKey));
                col.primaryKey = true;
            } else if (at(TokenKind::KwNot)) {
                if (col.notNull) return unexpected("a single NOT NULL constraint");
                advance();
                LEDGER_TRY_VOID(expect(TokenKind::KwNull));
                col.notNull = true;
            } else {
                return col;
            }
        }
    }

    Result<DataType> dataType() {
        switch (peek().kind) {
            case TokenKind::KwInt:   advance(); return DataType::Int;
            case TokenKind::KwFloat: advance(); return DataType::Float;
            case TokenKind::KwText:  advance(); return DataType::Text;
            case TokenKind::KwBool:  advance(); return DataType::Bool;
            default: return unexpected("column type (INT, FLOAT, TEXT or BOOL)");
        }
    }

    // DROP TABLE name
    Result<Statement> dropTable() {
        advance();  // TABLE
        LEDGER_TRY(table, identifier());
        return Statement{DropTable{std::move(table)}};
    }

    // CREATE VIEW name AS SELECT ...
    Result<Statement> createView() {
        advance();  // VIEW
        LEDGER_TRY(name, identifier());
        LEDGER_TRY_VOID(expect(TokenKind::KwAs));
        if (!at(TokenKind::KwSelect)) return unexpected("SELECT after AS");
        const std::size_t start = peek().offset;
        LEDGER_TRY(stmt, select());
        // The view text runs up to the `;` or the end of input.
        std::string text(src_.substr(start, peek().offset - start));
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                                 text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        Select query = std::get<Select>(std::move(stmt));
        if (!query.orderBy.empty() || query.limit) {
            return makeError(ErrorCode::SyntaxError,
                             "view '" + name + "': ORDER BY and LIMIT are not allowed in a view");
        }
        if (!query.groupBy.empty() || query.having) {
            return makeError(ErrorCode::SyntaxError,
                             "view '" + name + "': GROUP BY and HAVING are not allowed in a view");
        }
        return Statement{CreateView{std::move(name), std::move(query), std::move(text)}};
    }

    // DROP VIEW name
    Result<Statement> dropView() {
        advance();  // VIEW
        LEDGER_TRY(name, identifier());
        return Statement{DropView{std::move(name)}};
    }

    // INSERT INTO name [( col {, col} )] VALUES ( expr {, expr} )
    Result<Statement> insert() {
        advance();  // INSERT
        LEDGER_TRY_VOID(expect(TokenKind::KwInto));
        LEDGER_TRY(table, identifier());

        std::vector<std::string> columns;
        if (accept(TokenKind::LParen)) {
            do {
                LEDGER_TRY(col, identifier());
                columns.push_back(std::move(col));
            } while (accept(TokenKind::Comma));
            LEDGER_TRY_VOID(expect(TokenKind::RParen));
        }

        LEDGER_TRY_VOID(expect(TokenKind::KwValues));
        LEDGER_TRY_VOID(expect(TokenKind::LParen));
        std::vector<ExprPtr> values;
        do {
            LEDGER_TRY(e, expression());
            values.push_back(std::move(e));
        } while (accept(TokenKind::Comma));
        LEDGER_TRY_VOID(expect(TokenKind::RParen));

        return Statement{Insert{std::move(table), std::move(columns), std::move(values)}};
    }

    // SELECT (* | item {, item}) FROM name [WHERE expr]
    //        [GROUP BY expr {, expr}] [HAVING expr]
    //        [ORDER BY expr [ASC|DESC] {, expr [ASC|DESC]}] [LIMIT n]
    // item := expr [[AS] alias]
    Result<Statement> select() {
        advance();  // SELECT
        Select s;
        s.star = accept(TokenKind::Star);
        if (!s.star) {
            do {
                LEDGER_TRY(e, expression());
                std::string alias;
                if (accept(TokenKind::KwAs)) {
                    LEDGER_TRY(name, identifier());
                    alias = std::move(name);
                } else if (at(TokenKind::Identifier)) {
                    alias = advance().text;  // `expr alias` without AS
                }
                s.items.push_back(SelectItem{std::move(e), std::move(alias)});
            } while (accept(TokenKind::Comma));
        }

        LEDGER_TRY_VOID(expect(TokenKind::KwFrom));
        LEDGER_TRY(table, identifier());
        s.table = std::move(table);

        LEDGER_TRY(where, optionalWhere());
        s.where = std::move(where);

        if (accept(TokenKind::KwGroup)) {
            LEDGER_TRY_VOID(expect(TokenKind::KwBy));
            do {
                LEDGER_TRY(e, expression());
                s.groupBy.push_back(std::move(e));
            } while (accept(TokenKind::Comma));
        }
        if (accept(TokenKind::KwHaving)) {
            LEDGER_TRY(e, expression());
            s.having = std::move(e);
        }

        if (accept(TokenKind::KwOrder)) {
            LEDGER_TRY_VOID(expect(TokenKind::KwBy));
            do {
                LEDGER_TRY(e, expression());
                bool descending = false;
                if (accept(TokenKind::KwDesc)) descending = true;
                else accept(TokenKind::KwAsc);
                s.orderBy.push_back(OrderBy{std::move(e), descending});
            } while (accept(TokenKind::Comma));
        }

        if (accept(TokenKind::KwLimit)) {
            // Unsigned integer literal only: `LIMIT -1` or `LIMIT n` makes no
            // sense in v1, and refusing it here saves the binder a case.
            if (!at(TokenKind::Integer)) return unexpected("non-negative integer after LIMIT");
            const Token& tok = advance();
            auto v = Value::fromText(DataType::Int, tok.text);
            if (!v.ok()) return errorAt(tok, v.error().message);
            s.limit = v.value().asInt();
        }

        return Statement{std::move(s)};
    }

    // UPDATE name SET col = expr {, col = expr} [WHERE expr]
    Result<Statement> update() {
        advance();  // UPDATE
        Update u;
        LEDGER_TRY(table, identifier());
        u.table = std::move(table);
        LEDGER_TRY_VOID(expect(TokenKind::KwSet));
        do {
            LEDGER_TRY(col, identifier());
            LEDGER_TRY_VOID(expect(TokenKind::Eq));
            LEDGER_TRY(e, expression());
            u.assignments.emplace_back(std::move(col), std::move(e));
        } while (accept(TokenKind::Comma));
        LEDGER_TRY(where, optionalWhere());
        u.where = std::move(where);
        return Statement{std::move(u)};
    }

    // DELETE FROM name [WHERE expr]
    Result<Statement> del() {
        advance();  // DELETE
        LEDGER_TRY_VOID(expect(TokenKind::KwFrom));
        LEDGER_TRY(table, identifier());
        LEDGER_TRY(where, optionalWhere());
        return Statement{Delete{std::move(table), std::move(where)}};
    }

    Result<ExprPtr> optionalWhere() {
        if (!accept(TokenKind::KwWhere)) return ExprPtr{};
        return expression();
    }

    // ---- expressions -------------------------------------------------------

    static ExprPtr make(decltype(Expr::node) node, const Token& start) {
        return std::make_unique<Expr>(Expr{std::move(node), start.line, start.column});
    }

    Result<ExprPtr> expression() { return orExpr(); }

    // Left-associative binary levels share the same loop.
    template <typename Next>
    Result<ExprPtr> leftAssoc(Next next, std::initializer_list<std::pair<TokenKind, BinaryOp>> ops) {
        const Token& start = peek();
        LEDGER_TRY(lhs, (this->*next)());
        for (;;) {
            const BinaryOp* op = nullptr;
            for (const auto& [kind, candidate] : ops) {
                if (at(kind)) { op = &candidate; break; }
            }
            if (!op) return lhs;
            advance();
            LEDGER_TRY(rhs, (this->*next)());
            lhs = make(Binary{*op, std::move(lhs), std::move(rhs)}, start);
        }
    }

    Result<ExprPtr> orExpr() {
        return leftAssoc(&Parser::andExpr, {{TokenKind::KwOr, BinaryOp::Or}});
    }

    Result<ExprPtr> andExpr() {
        return leftAssoc(&Parser::notExpr, {{TokenKind::KwAnd, BinaryOp::And}});
    }

    Result<ExprPtr> notExpr() {
        const Token& start = peek();
        if (accept(TokenKind::KwNot)) {
            LEDGER_TRY(operand, notExpr());
            return make(Unary{UnaryOp::Not, std::move(operand)}, start);
        }
        return comparison();
    }

    static const BinaryOp* comparisonOp(TokenKind kind) noexcept {
        static constexpr BinaryOp kEq = BinaryOp::Eq, kNotEq = BinaryOp::NotEq, kLt = BinaryOp::Lt,
                                  kLtEq = BinaryOp::LtEq, kGt = BinaryOp::Gt, kGtEq = BinaryOp::GtEq;
        switch (kind) {
            case TokenKind::Eq:    return &kEq;
            case TokenKind::NotEq: return &kNotEq;
            case TokenKind::Lt:    return &kLt;
            case TokenKind::LtEq:  return &kLtEq;
            case TokenKind::Gt:    return &kGt;
            case TokenKind::GtEq:  return &kGtEq;
            default:               return nullptr;
        }
    }

    // Non-associative: `a = b = c` and `a IS NULL = b` are rejected.
    Result<ExprPtr> comparison() {
        const Token& start = peek();
        LEDGER_TRY(lhs, additive());

        if (accept(TokenKind::KwIs)) {
            const bool negated = accept(TokenKind::KwNot);
            LEDGER_TRY_VOID(expect(TokenKind::KwNull));
            lhs = make(IsNull{std::move(lhs), negated}, start);
        } else if (const BinaryOp* op = comparisonOp(peek().kind)) {
            advance();
            LEDGER_TRY(rhs, additive());
            lhs = make(Binary{*op, std::move(lhs), std::move(rhs)}, start);
        } else {
            return lhs;
        }

        if (comparisonOp(peek().kind) || at(TokenKind::KwIs)) {
            return errorAt(peek(), "comparison operators cannot be chained; use parentheses");
        }
        return lhs;
    }

    Result<ExprPtr> additive() {
        return leftAssoc(&Parser::multiplicative,
                         {{TokenKind::Plus, BinaryOp::Add}, {TokenKind::Minus, BinaryOp::Sub}});
    }

    Result<ExprPtr> multiplicative() {
        return leftAssoc(&Parser::unary,
                         {{TokenKind::Star, BinaryOp::Mul}, {TokenKind::Slash, BinaryOp::Div}});
    }

    Result<ExprPtr> unary() {
        const Token& start = peek();
        if (accept(TokenKind::Minus)) {
            LEDGER_TRY(operand, unary());
            return make(Unary{UnaryOp::Neg, std::move(operand)}, start);
        }
        return primary();
    }

    Result<ExprPtr> literal(DataType type) {
        const Token& tok = advance();
        auto v = Value::fromText(type, tok.text);
        if (!v.ok()) return errorAt(tok, v.error().message);
        return make(Literal{std::move(v).value()}, tok);
    }

    Result<ExprPtr> primary() {
        const Token& tok = peek();
        switch (tok.kind) {
            case TokenKind::Integer: return literal(DataType::Int);
            case TokenKind::Float:   return literal(DataType::Float);
            case TokenKind::String:  advance(); return make(Literal{Value::text(tok.text)}, tok);
            case TokenKind::KwTrue:  advance(); return make(Literal{Value::boolean(true)}, tok);
            case TokenKind::KwFalse: advance(); return make(Literal{Value::boolean(false)}, tok);
            case TokenKind::KwNull:  advance(); return make(Literal{Value::null()}, tok);
            case TokenKind::Identifier: {
                advance();
                if (!accept(TokenKind::LParen)) return make(ColumnRef{tok.text}, tok);
                // name( * ) | name( [expr {, expr}] )
                Call call{tok.text, {}, false};
                if (accept(TokenKind::Star)) {
                    call.star = true;
                } else if (!at(TokenKind::RParen)) {
                    do {
                        LEDGER_TRY(arg, expression());
                        call.args.push_back(std::move(arg));
                    } while (accept(TokenKind::Comma));
                }
                LEDGER_TRY_VOID(expect(TokenKind::RParen));
                return make(std::move(call), tok);
            }
            case TokenKind::LParen: {
                advance();
                LEDGER_TRY(inner, expression());
                LEDGER_TRY_VOID(expect(TokenKind::RParen));
                return inner;
            }
            default: return unexpected("expression");
        }
    }

    std::vector<Token> tokens_;
    std::string_view src_;
    std::size_t pos_ = 0;
};

}  // namespace

Result<ast::Statement> parse(std::string_view sql) {
    LEDGER_TRY(tokens, tokenize(sql));
    return Parser{std::move(tokens), sql}.run();
}

}  // namespace ledger
