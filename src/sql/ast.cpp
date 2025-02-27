#include "sql/ast.h"

namespace ledger::ast {

std::string_view unaryOpName(UnaryOp op) noexcept {
    switch (op) {
        case UnaryOp::Not: return "NOT";
        case UnaryOp::Neg: return "-";
    }
    return "?";
}

std::string_view binaryOpName(BinaryOp op) noexcept {
    switch (op) {
        case BinaryOp::Or:    return "OR";
        case BinaryOp::And:   return "AND";
        case BinaryOp::Eq:    return "=";
        case BinaryOp::NotEq: return "<>";
        case BinaryOp::Lt:    return "<";
        case BinaryOp::LtEq:  return "<=";
        case BinaryOp::Gt:    return ">";
        case BinaryOp::GtEq:  return ">=";
        case BinaryOp::Add:   return "+";
        case BinaryOp::Sub:   return "-";
        case BinaryOp::Mul:   return "*";
        case BinaryOp::Div:   return "/";
    }
    return "?";
}

namespace {

// Precedence levels, higher binds tighter; used to parenthesize only when
// needed so that `a + b * c` prints as written.
int precedence(const Expr& e) {
    return std::visit(
        [](const auto& n) -> int {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, Binary>) {
                switch (n.op) {
                    case BinaryOp::Or:  return 1;
                    case BinaryOp::And: return 2;
                    case BinaryOp::Eq: case BinaryOp::NotEq: case BinaryOp::Lt:
                    case BinaryOp::LtEq: case BinaryOp::Gt: case BinaryOp::GtEq: return 4;
                    case BinaryOp::Add: case BinaryOp::Sub: return 5;
                    case BinaryOp::Mul: case BinaryOp::Div: return 6;
                }
                return 0;
            } else if constexpr (std::is_same_v<N, Unary>) {
                return n.op == UnaryOp::Not ? 3 : 7;
            } else if constexpr (std::is_same_v<N, IsNull>) {
                return 4;
            } else {
                return 8;  // Literal, ColumnRef, Call
            }
        },
        e.node);
}

void render(const Expr& e, std::string& out, int parentPrecedence) {
    const int mine = precedence(e);
    const bool parens = mine < parentPrecedence;
    if (parens) out += '(';
    std::visit(
        [&](const auto& n) {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, Literal>) {
                if (n.value.type() == DataType::Text) {
                    out += '\'';
                    for (const char c : n.value.asText()) {
                        out += c;
                        if (c == '\'') out += '\'';
                    }
                    out += '\'';
                } else {
                    out += n.value.toText();
                }
            } else if constexpr (std::is_same_v<N, ColumnRef>) {
                out += n.name;
            } else if constexpr (std::is_same_v<N, Unary>) {
                out += unaryOpName(n.op);
                if (n.op == UnaryOp::Not) out += ' ';
                render(*n.operand, out, mine);
            } else if constexpr (std::is_same_v<N, Binary>) {
                // Comparisons are non-associative: a comparison (or IS NULL)
                // on either side must be parenthesized to parse back.
                render(*n.lhs, out, mine == 4 ? mine + 1 : mine);
                out += ' ';
                out += binaryOpName(n.op);
                out += ' ';
                // Left-associative: the right operand needs parentheses at
                // the same level (`a - (b - c)`).
                render(*n.rhs, out, mine + 1);
            } else if constexpr (std::is_same_v<N, IsNull>) {
                render(*n.operand, out, mine + 1);
                out += n.negated ? " IS NOT NULL" : " IS NULL";
            } else {
                out += n.name;
                out += '(';
                if (n.star) out += '*';
                for (std::size_t i = 0; i < n.args.size(); ++i) {
                    if (i) out += ", ";
                    render(*n.args[i], out, 0);
                }
                out += ')';
            }
        },
        e.node);
    if (parens) out += ')';
}

}  // namespace

std::string exprToString(const Expr& e) {
    std::string out;
    render(e, out, 0);
    return out;
}

}  // namespace ledger::ast
