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

}  // namespace ledger::ast
