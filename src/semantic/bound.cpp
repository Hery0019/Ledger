#include "semantic/bound.h"

namespace ledger {

std::string_view scalarFuncName(ScalarFunc f) noexcept {
    switch (f) {
        case ScalarFunc::Upper:    return "upper";
        case ScalarFunc::Lower:    return "lower";
        case ScalarFunc::Length:   return "length";
        case ScalarFunc::Trim:     return "trim";
        case ScalarFunc::Abs:      return "abs";
        case ScalarFunc::Round:    return "round";
        case ScalarFunc::Coalesce: return "coalesce";
        case ScalarFunc::NullIf:   return "nullif";
    }
    return "?";
}

std::string_view aggFuncName(AggFunc f) noexcept {
    switch (f) {
        case AggFunc::Count: return "count";
        case AggFunc::Sum:   return "sum";
        case AggFunc::Avg:   return "avg";
        case AggFunc::Min:   return "min";
        case AggFunc::Max:   return "max";
    }
    return "?";
}

BoundExprPtr cloneExpr(const BoundExpr& e) {
    auto node = std::visit(
        [](const auto& n) -> decltype(BoundExpr::node) {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, Value>) {
                return n;
            } else if constexpr (std::is_same_v<N, BoundColumn>) {
                return n;
            } else if constexpr (std::is_same_v<N, BoundUnary>) {
                return BoundUnary{n.op, cloneExpr(*n.operand)};
            } else if constexpr (std::is_same_v<N, BoundBinary>) {
                return BoundBinary{n.op, cloneExpr(*n.lhs), cloneExpr(*n.rhs)};
            } else if constexpr (std::is_same_v<N, BoundIsNull>) {
                return BoundIsNull{cloneExpr(*n.operand), n.negated};
            } else if constexpr (std::is_same_v<N, BoundCast>) {
                return BoundCast{cloneExpr(*n.operand), n.to};
            } else if constexpr (std::is_same_v<N, BoundInList>) {
                BoundInList out{cloneExpr(*n.value), {}, n.negated};
                for (const auto& item : n.items) out.items.push_back(cloneExpr(*item));
                return out;
            } else if constexpr (std::is_same_v<N, BoundLike>) {
                return BoundLike{cloneExpr(*n.value), cloneExpr(*n.pattern), n.negated};
            } else if constexpr (std::is_same_v<N, BoundCall>) {
                BoundCall out{n.func, {}};
                for (const auto& a : n.args) out.args.push_back(cloneExpr(*a));
                return out;
            } else if constexpr (std::is_same_v<N, BoundCase>) {
                BoundCase out{{}, n.elseExpr ? cloneExpr(*n.elseExpr) : nullptr};
                for (const auto& [c, r] : n.whens) out.whens.emplace_back(cloneExpr(*c), cloneExpr(*r));
                return out;
            } else if constexpr (std::is_same_v<N, BoundInSubquery>) {
                return BoundInSubquery{cloneExpr(*n.value), n.slot, n.negated};
            } else {
                return n;  // BoundExists, BoundScalarSubquery: a slot number
            }
        },
        e.node);
    return std::make_unique<BoundExpr>(BoundExpr{std::move(node), e.type});
}

}  // namespace ledger
