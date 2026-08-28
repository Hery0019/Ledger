#include "semantic/eval.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ledger {

namespace {

using ast::BinaryOp;
using ast::UnaryOp;

constexpr auto kIntMin = std::numeric_limits<std::int64_t>::min();
constexpr auto kIntMax = std::numeric_limits<std::int64_t>::max();

Error overflow(std::string_view op) {
    return makeError(ErrorCode::TypeError, "integer overflow in '" + std::string(op) + "'");
}

Error divisionByZero() { return makeError(ErrorCode::TypeError, "division by zero"); }

double toDouble(const Value& v) noexcept {
    return v.type() == DataType::Int ? static_cast<double>(v.asInt()) : v.asFloat();
}

// Checked integer arithmetic. Portable (no __builtin_*_overflow): bounds are
// tested before the operation, never after a wrap.
Result<Value> intArith(BinaryOp op, std::int64_t a, std::int64_t b) {
    switch (op) {
        case BinaryOp::Add:
            if ((b > 0 && a > kIntMax - b) || (b < 0 && a < kIntMin - b)) return overflow("+");
            return Value::integer(a + b);
        case BinaryOp::Sub:
            if ((b < 0 && a > kIntMax + b) || (b > 0 && a < kIntMin + b)) return overflow("-");
            return Value::integer(a - b);
        case BinaryOp::Div:
            if (b == 0) return divisionByZero();
            if (a == kIntMin && b == -1) return overflow("/");
            return Value::integer(a / b);  // truncation toward zero, like SQL
        default:
            return makeError(ErrorCode::Internal, "intArith: not an arithmetic operator");
    }
}

Result<Value> floatArith(BinaryOp op, double a, double b) {
    switch (op) {
        case BinaryOp::Add: return Value::real(a + b);
        case BinaryOp::Sub: return Value::real(a - b);
        case BinaryOp::Mul: return Value::real(a * b);
        case BinaryOp::Div:
            if (b == 0.0) return divisionByZero();
            return Value::real(a / b);
        default:
            return makeError(ErrorCode::Internal, "floatArith: not an arithmetic operator");
    }
}

// Multiplication: go through unsigned (defined wrap) then verify by inverse
// division; a signed `a * b` that overflows would be UB.
Result<Value> checkedMul(std::int64_t a, std::int64_t b) {
    if (a == 0 || b == 0) return Value::integer(0);
    if ((a == -1 && b == kIntMin) || (b == -1 && a == kIntMin)) return overflow("*");
    const auto ua = static_cast<std::uint64_t>(a);
    const auto ub = static_cast<std::uint64_t>(b);
    const std::uint64_t ur = ua * ub;  // wrap is defined for unsigned
    const auto r = static_cast<std::int64_t>(ur);
    if (r / b != a) return overflow("*");
    return Value::integer(r);
}

bool comparisonHolds(BinaryOp op, Ordering ord) noexcept {
    switch (op) {
        case BinaryOp::Eq:    return ord == Ordering::Equal;
        case BinaryOp::NotEq: return ord != Ordering::Equal;
        case BinaryOp::Lt:    return ord == Ordering::Less;
        case BinaryOp::LtEq:  return ord != Ordering::Greater;
        case BinaryOp::Gt:    return ord == Ordering::Greater;
        case BinaryOp::GtEq:  return ord != Ordering::Less;
        default:              return false;
    }
}

bool isComparison(BinaryOp op) noexcept {
    return op == BinaryOp::Eq || op == BinaryOp::NotEq || op == BinaryOp::Lt ||
           op == BinaryOp::LtEq || op == BinaryOp::Gt || op == BinaryOp::GtEq;
}

Result<Value> evalUnary(const BoundUnary& u, const Row& row) {
    LEDGER_TRY(v, eval(*u.operand, row));
    if (v.isNull()) return Value::null();
    switch (u.op) {
        case UnaryOp::Not:
            return Value::boolean(!v.asBool());
        case UnaryOp::Neg:
            if (v.type() == DataType::Int) {
                if (v.asInt() == kIntMin) return overflow("-");
                return Value::integer(-v.asInt());
            }
            return Value::real(-v.asFloat());
    }
    return makeError(ErrorCode::Internal, "evalUnary: unknown operator");
}

Result<Value> evalLogical(BinaryOp op, const BoundBinary& b, const Row& row) {
    LEDGER_TRY(l, eval(*b.lhs, row));
    LEDGER_TRY(r, eval(*b.rhs, row));
    // Three-valued logic. Both sides are evaluated: no short-circuit, so that
    // a data error (division by zero) does not depend on operand order.
    const bool lNull = l.isNull();
    const bool rNull = r.isNull();
    if (op == BinaryOp::And) {
        if ((!lNull && !l.asBool()) || (!rNull && !r.asBool())) return Value::boolean(false);
        if (lNull || rNull) return Value::null();
        return Value::boolean(true);
    }
    if ((!lNull && l.asBool()) || (!rNull && r.asBool())) return Value::boolean(true);
    if (lNull || rNull) return Value::null();
    return Value::boolean(false);
}

Result<Value> evalBinary(const BoundBinary& b, const Row& row) {
    if (b.op == BinaryOp::And || b.op == BinaryOp::Or) return evalLogical(b.op, b, row);

    LEDGER_TRY(l, eval(*b.lhs, row));
    LEDGER_TRY(r, eval(*b.rhs, row));
    if (l.isNull() || r.isNull()) return Value::null();

    if (isComparison(b.op)) {
        LEDGER_TRY(ord, Value::compare(l, r));
        return Value::boolean(comparisonHolds(b.op, ord));
    }

    if (l.type() == DataType::Int && r.type() == DataType::Int) {
        if (b.op == BinaryOp::Mul) return checkedMul(l.asInt(), r.asInt());
        return intArith(b.op, l.asInt(), r.asInt());
    }
    return floatArith(b.op, toDouble(l), toDouble(r));
}

}  // namespace

// SQL LIKE: `%` matches any sequence, `_` any single character; everything
// else matches itself, byte for byte (case-sensitive). Backtracking on `%`
// only; patterns are short.
bool likeMatch(std::string_view text, std::string_view pattern) {
    std::size_t t = 0, p = 0;
    std::size_t starP = std::string_view::npos, starT = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '%') {
            starP = p++;
            starT = t;
        } else if (p < pattern.size() && (pattern[p] == '_' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (starP != std::string_view::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%') ++p;
    return p == pattern.size();
}

namespace {

Result<Value> evalInList(const BoundInList& in, const Row& row) {
    LEDGER_TRY(v, eval(*in.value, row));
    if (v.isNull()) return Value::null();
    bool sawNull = false;
    for (const auto& item : in.items) {
        LEDGER_TRY(x, eval(*item, row));
        if (x.isNull()) {
            sawNull = true;
            continue;
        }
        LEDGER_TRY(ord, Value::compare(v, x));
        if (ord == Ordering::Equal) return Value::boolean(!in.negated);
    }
    if (sawNull) return Value::null();  // no match, but an unknown item: unknown
    return Value::boolean(in.negated);
}

Result<Value> evalLike(const BoundLike& l, const Row& row) {
    LEDGER_TRY(v, eval(*l.value, row));
    LEDGER_TRY(p, eval(*l.pattern, row));
    if (v.isNull() || p.isNull()) return Value::null();
    return Value::boolean(likeMatch(v.asText(), p.asText()) != l.negated);
}

// ASCII-only case mapping: non-ASCII bytes pass through untouched, so a
// UTF-8 string is never corrupted (accented letters simply keep their case).
std::string mapAsciiCase(std::string s, bool upper) {
    for (char& c : s) {
        if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (!upper && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// Length in UTF-8 code points, consistent with the CLI's column widths.
std::int64_t codePoints(std::string_view s) noexcept {
    std::int64_t n = 0;
    for (const char ch : s) n += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
    return n;
}

Result<Value> evalCall(const BoundCall& c, const Row& row) {
    std::vector<Value> args;
    args.reserve(c.args.size());
    for (const auto& a : c.args) {
        LEDGER_TRY(v, eval(*a, row));
        args.push_back(std::move(v));
    }
    // COALESCE / NULLIF have their own NULL rules; every other function is
    // NULL as soon as an argument is.
    switch (c.func) {
        case ScalarFunc::Coalesce:
            for (auto& v : args) {
                if (!v.isNull()) return std::move(v);
            }
            return Value::null();
        case ScalarFunc::NullIf: {
            if (args[0].isNull() || args[1].isNull()) return args[0];
            LEDGER_TRY(ord, Value::compare(args[0], args[1]));
            return ord == Ordering::Equal ? Value::null() : args[0];
        }
        default:
            break;
    }
    for (const auto& v : args) {
        if (v.isNull()) return Value::null();
    }
    switch (c.func) {
        case ScalarFunc::Upper:  return Value::text(mapAsciiCase(args[0].asText(), true));
        case ScalarFunc::Lower:  return Value::text(mapAsciiCase(args[0].asText(), false));
        case ScalarFunc::Length: return Value::integer(codePoints(args[0].asText()));
        case ScalarFunc::Trim: {
            std::string_view s = args[0].asText();
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
            return Value::text(std::string(s));
        }
        case ScalarFunc::Abs:
            if (args[0].type() == DataType::Int) {
                if (args[0].asInt() == kIntMin) return overflow("abs");
                return Value::integer(args[0].asInt() < 0 ? -args[0].asInt() : args[0].asInt());
            }
            return Value::real(args[0].asFloat() < 0 ? -args[0].asFloat() : args[0].asFloat());
        case ScalarFunc::Round: {
            const double x = toDouble(args[0]);
            const std::int64_t digits = args.size() > 1 ? args[1].asInt() : 0;
            if (digits < -18 || digits > 18) return makeError(ErrorCode::TypeError, "round(): digits out of range");
            double scale = 1.0;
            for (std::int64_t i = 0; i < (digits < 0 ? -digits : digits); ++i) scale *= 10.0;
            const double scaled = digits >= 0 ? x * scale : x / scale;
            const double rounded = scaled < 0 ? -std::floor(-scaled + 0.5) : std::floor(scaled + 0.5);
            return Value::real(digits >= 0 ? rounded / scale : rounded * scale);
        }
        case ScalarFunc::Coalesce:
        case ScalarFunc::NullIf:
            break;  // handled above
    }
    return makeError(ErrorCode::Internal, "evalCall: unknown function");
}

Result<Value> evalCase(const BoundCase& c, const Row& row) {
    for (const auto& [cond, result] : c.whens) {
        LEDGER_TRY(v, eval(*cond, row));
        if (!v.isNull() && v.asBool()) return eval(*result, row);
    }
    if (c.elseExpr) return eval(*c.elseExpr, row);
    return Value::null();
}

Result<Value> evalCast(const BoundCast& c, const Row& row) {
    LEDGER_TRY(v, eval(*c.operand, row));
    if (v.isNull() || v.type() == c.to) return v;
    // The only allowed conversion: Int -> Float.
    return Value::real(static_cast<double>(v.asInt()));
}

}  // namespace

Result<Value> eval(const BoundExpr& expr, const Row& row) {
    return std::visit(
        [&](const auto& n) -> Result<Value> {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, Value>) {
                return n;
            } else if constexpr (std::is_same_v<N, BoundColumn>) {
                return row[n.index];
            } else if constexpr (std::is_same_v<N, BoundUnary>) {
                return evalUnary(n, row);
            } else if constexpr (std::is_same_v<N, BoundBinary>) {
                return evalBinary(n, row);
            } else if constexpr (std::is_same_v<N, BoundIsNull>) {
                LEDGER_TRY(v, eval(*n.operand, row));
                return Value::boolean(v.isNull() != n.negated);
            } else if constexpr (std::is_same_v<N, BoundInList>) {
                return evalInList(n, row);
            } else if constexpr (std::is_same_v<N, BoundLike>) {
                return evalLike(n, row);
            } else if constexpr (std::is_same_v<N, BoundCall>) {
                return evalCall(n, row);
            } else if constexpr (std::is_same_v<N, BoundCase>) {
                return evalCase(n, row);
            } else {
                return evalCast(n, row);
            }
        },
        expr.node);
}

bool isConstant(const BoundExpr& expr) noexcept {
    return std::visit(
        [](const auto& n) -> bool {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, Value>) {
                return true;
            } else if constexpr (std::is_same_v<N, BoundColumn>) {
                return false;
            } else if constexpr (std::is_same_v<N, BoundBinary>) {
                return isConstant(*n.lhs) && isConstant(*n.rhs);
            } else if constexpr (std::is_same_v<N, BoundInList>) {
                if (!isConstant(*n.value)) return false;
                for (const auto& item : n.items) {
                    if (!isConstant(*item)) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<N, BoundLike>) {
                return isConstant(*n.value) && isConstant(*n.pattern);
            } else if constexpr (std::is_same_v<N, BoundCall>) {
                for (const auto& a : n.args) {
                    if (!isConstant(*a)) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<N, BoundCase>) {
                for (const auto& [c, r] : n.whens) {
                    if (!isConstant(*c) || !isConstant(*r)) return false;
                }
                return !n.elseExpr || isConstant(*n.elseExpr);
            } else {
                return isConstant(*n.operand);
            }
        },
        expr.node);
}

}  // namespace ledger
