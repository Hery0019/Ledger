#include "doctest.h"

#include <limits>

#include "semantic/eval.h"

using namespace ledger;
using ast::BinaryOp;
using ast::UnaryOp;

// Construction directe d'expressions liées, sans passer par le binder : on
// teste ici la sémantique des opérateurs seule.
namespace {

BoundExprPtr lit(Value v) {
    const DataType t = v.type();
    return std::make_unique<BoundExpr>(BoundExpr{std::move(v), t});
}
BoundExprPtr i(std::int64_t v) { return lit(Value::integer(v)); }
BoundExprPtr f(double v) { return lit(Value::real(v).value()); }
BoundExprPtr b(bool v) { return lit(Value::boolean(v)); }
BoundExprPtr txt(const char* v) { return lit(Value::text(v)); }
BoundExprPtr null() { return lit(Value::null()); }
BoundExprPtr col(std::size_t idx, DataType t) {
    return std::make_unique<BoundExpr>(BoundExpr{BoundColumn{idx}, t});
}
BoundExprPtr bin(BinaryOp op, BoundExprPtr l, BoundExprPtr r) {
    return std::make_unique<BoundExpr>(BoundExpr{BoundBinary{op, std::move(l), std::move(r)}, DataType::Null});
}
BoundExprPtr un(UnaryOp op, BoundExprPtr e) {
    return std::make_unique<BoundExpr>(BoundExpr{BoundUnary{op, std::move(e)}, DataType::Null});
}
BoundExprPtr isNull(BoundExprPtr e, bool negated) {
    return std::make_unique<BoundExpr>(BoundExpr{BoundIsNull{std::move(e), negated}, DataType::Bool});
}
BoundExprPtr cast(BoundExprPtr e) {
    return std::make_unique<BoundExpr>(BoundExpr{BoundCast{std::move(e), DataType::Float}, DataType::Float});
}

Value ev(const BoundExprPtr& e, const Row& row = {}) {
    auto r = eval(*e, row);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r).value();
}

Error err(const BoundExprPtr& e, const Row& row = {}) {
    auto r = eval(*e, row);
    REQUIRE_FALSE(r.ok());
    return r.error();
}

constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

}  // namespace

// ---- feuilles --------------------------------------------------------------

TEST_CASE("eval: constants and columns") {
    CHECK(ev(i(7)) == Value::integer(7));
    CHECK(ev(txt("a")) == Value::text("a"));
    CHECK(ev(null()).isNull());
    const Row row{Value::integer(1), Value::text("x")};
    CHECK(ev(col(0, DataType::Int), row) == Value::integer(1));
    CHECK(ev(col(1, DataType::Text), row) == Value::text("x"));
}

// ---- arithmétique ----------------------------------------------------------

TEST_CASE("eval: integer arithmetic") {
    CHECK(ev(bin(BinaryOp::Add, i(2), i(3))) == Value::integer(5));
    CHECK(ev(bin(BinaryOp::Sub, i(2), i(3))) == Value::integer(-1));
    CHECK(ev(bin(BinaryOp::Mul, i(-4), i(3))) == Value::integer(-12));
    CHECK(ev(bin(BinaryOp::Div, i(7), i(2))) == Value::integer(3));
    CHECK(ev(bin(BinaryOp::Div, i(-7), i(2))) == Value::integer(-3));  // troncature vers zéro
    CHECK(ev(bin(BinaryOp::Mul, i(0), i(kMin))) == Value::integer(0));
}

TEST_CASE("eval: integer overflow is an error, never a wrap") {
    CHECK(err(bin(BinaryOp::Add, i(kMax), i(1))).message == "integer overflow in '+'");
    CHECK(err(bin(BinaryOp::Add, i(kMin), i(-1))).message == "integer overflow in '+'");
    CHECK(err(bin(BinaryOp::Sub, i(kMin), i(1))).message == "integer overflow in '-'");
    CHECK(err(bin(BinaryOp::Sub, i(kMax), i(-1))).message == "integer overflow in '-'");
    CHECK(err(bin(BinaryOp::Mul, i(kMax), i(2))).message == "integer overflow in '*'");
    CHECK(err(bin(BinaryOp::Mul, i(kMin), i(-1))).message == "integer overflow in '*'");
    CHECK(err(bin(BinaryOp::Mul, i(-1), i(kMin))).message == "integer overflow in '*'");
    CHECK(err(bin(BinaryOp::Div, i(kMin), i(-1))).message == "integer overflow in '/'");
    CHECK(err(un(UnaryOp::Neg, i(kMin))).message == "integer overflow in '-'");
    CHECK(err(bin(BinaryOp::Add, i(kMax), i(1))).code == ErrorCode::TypeError);
    // Les bornes exactes passent.
    CHECK(ev(bin(BinaryOp::Add, i(kMax - 1), i(1))) == Value::integer(kMax));
    CHECK(ev(bin(BinaryOp::Sub, i(kMin + 1), i(1))) == Value::integer(kMin));
    CHECK(ev(bin(BinaryOp::Mul, i(kMax), i(1))) == Value::integer(kMax));
    CHECK(ev(bin(BinaryOp::Mul, i(kMin), i(1))) == Value::integer(kMin));
    CHECK(ev(bin(BinaryOp::Mul, i(kMax), i(-1))) == Value::integer(-kMax));
    CHECK(ev(un(UnaryOp::Neg, i(kMax))) == Value::integer(-kMax));
}

TEST_CASE("eval: division by zero") {
    CHECK(err(bin(BinaryOp::Div, i(1), i(0))).message == "division by zero");
    CHECK(err(bin(BinaryOp::Div, f(1.0), f(0.0))).message == "division by zero");
    CHECK(err(bin(BinaryOp::Div, i(1), f(0.0))).message == "division by zero");
    CHECK(err(bin(BinaryOp::Div, i(0), i(0))).code == ErrorCode::TypeError);
}

TEST_CASE("eval: mixed Int/Float arithmetic promotes to Float") {
    CHECK(ev(bin(BinaryOp::Add, i(1), f(0.5))) == Value::real(1.5).value());
    CHECK(ev(bin(BinaryOp::Div, i(7), f(2.0))) == Value::real(3.5).value());
    CHECK(ev(bin(BinaryOp::Mul, f(1.5), i(2))) == Value::real(3.0).value());
    CHECK(ev(bin(BinaryOp::Sub, f(1.0), f(0.25))) == Value::real(0.75).value());
    CHECK(ev(bin(BinaryOp::Add, i(1), f(0.5))).type() == DataType::Float);
}

TEST_CASE("eval: float overflow to infinity is an error") {
    const double big = std::numeric_limits<double>::max();
    CHECK(err(bin(BinaryOp::Mul, f(big), f(2.0))).code == ErrorCode::TypeError);
    CHECK(err(bin(BinaryOp::Add, f(big), f(big))).code == ErrorCode::TypeError);
}

TEST_CASE("eval: unary minus") {
    CHECK(ev(un(UnaryOp::Neg, i(5))) == Value::integer(-5));
    CHECK(ev(un(UnaryOp::Neg, f(2.5))) == Value::real(-2.5).value());
    CHECK(ev(un(UnaryOp::Neg, null())).isNull());
}

TEST_CASE("eval: arithmetic with NULL yields NULL") {
    CHECK(ev(bin(BinaryOp::Add, null(), i(1))).isNull());
    CHECK(ev(bin(BinaryOp::Div, i(1), null())).isNull());
    CHECK(ev(bin(BinaryOp::Div, null(), i(0))).isNull());  // NULL avant la division
}

// ---- comparaisons ----------------------------------------------------------

TEST_CASE("eval: comparisons") {
    CHECK(ev(bin(BinaryOp::Eq, i(1), i(1))) == Value::boolean(true));
    CHECK(ev(bin(BinaryOp::NotEq, i(1), i(1))) == Value::boolean(false));
    CHECK(ev(bin(BinaryOp::Lt, i(1), i(2))) == Value::boolean(true));
    CHECK(ev(bin(BinaryOp::LtEq, i(2), i(2))) == Value::boolean(true));
    CHECK(ev(bin(BinaryOp::Gt, i(1), i(2))) == Value::boolean(false));
    CHECK(ev(bin(BinaryOp::GtEq, i(1), i(2))) == Value::boolean(false));
    CHECK(ev(bin(BinaryOp::Eq, i(1), f(1.0))) == Value::boolean(true));
    CHECK(ev(bin(BinaryOp::Lt, txt("abc"), txt("abd"))) == Value::boolean(true));
    CHECK(ev(bin(BinaryOp::Lt, b(false), b(true))) == Value::boolean(true));
}

TEST_CASE("eval: comparison with NULL is NULL, IS NULL is a real boolean") {
    CHECK(ev(bin(BinaryOp::Eq, null(), null())).isNull());
    CHECK(ev(bin(BinaryOp::Eq, i(1), null())).isNull());
    CHECK(ev(isNull(null(), false)) == Value::boolean(true));
    CHECK(ev(isNull(null(), true)) == Value::boolean(false));
    CHECK(ev(isNull(i(1), false)) == Value::boolean(false));
    CHECK(ev(isNull(i(1), true)) == Value::boolean(true));
}

// ---- logique à trois états -------------------------------------------------

TEST_CASE("eval: three-valued AND") {
    CHECK(ev(bin(BinaryOp::And, b(true), b(true))) == Value::boolean(true));
    CHECK(ev(bin(BinaryOp::And, b(true), b(false))) == Value::boolean(false));
    CHECK(ev(bin(BinaryOp::And, b(false), null())) == Value::boolean(false));
    CHECK(ev(bin(BinaryOp::And, null(), b(false))) == Value::boolean(false));
    CHECK(ev(bin(BinaryOp::And, b(true), null())).isNull());
    CHECK(ev(bin(BinaryOp::And, null(), null())).isNull());
}

TEST_CASE("eval: three-valued OR") {
    CHECK(ev(bin(BinaryOp::Or, b(false), b(false))) == Value::boolean(false));
    CHECK(ev(bin(BinaryOp::Or, b(false), b(true))) == Value::boolean(true));
    CHECK(ev(bin(BinaryOp::Or, b(true), null())) == Value::boolean(true));
    CHECK(ev(bin(BinaryOp::Or, null(), b(true))) == Value::boolean(true));
    CHECK(ev(bin(BinaryOp::Or, b(false), null())).isNull());
    CHECK(ev(bin(BinaryOp::Or, null(), null())).isNull());
}

TEST_CASE("eval: NOT") {
    CHECK(ev(un(UnaryOp::Not, b(true))) == Value::boolean(false));
    CHECK(ev(un(UnaryOp::Not, b(false))) == Value::boolean(true));
    CHECK(ev(un(UnaryOp::Not, null())).isNull());
}

TEST_CASE("eval: logical operators do not short-circuit data errors") {
    CHECK(err(bin(BinaryOp::And, b(false), bin(BinaryOp::Div, i(1), i(0)))).message == "division by zero");
    CHECK(err(bin(BinaryOp::Or, b(true), bin(BinaryOp::Div, i(1), i(0)))).message == "division by zero");
}

// ---- cast et constantes ----------------------------------------------------

TEST_CASE("eval: Int -> Float cast") {
    CHECK(ev(cast(i(3))) == Value::real(3.0).value());
    CHECK(ev(cast(f(2.5))) == Value::real(2.5).value());
    CHECK(ev(cast(null())).isNull());
}

TEST_CASE("eval: nested expression over a row") {
    // (a + 1) * b > 10 AND c IS NOT NULL
    const Row row{Value::integer(4), Value::integer(3), Value::text("x")};
    auto e = bin(BinaryOp::And,
                 bin(BinaryOp::Gt,
                     bin(BinaryOp::Mul, bin(BinaryOp::Add, col(0, DataType::Int), i(1)), col(1, DataType::Int)),
                     i(10)),
                 isNull(col(2, DataType::Text), true));
    CHECK(ev(e, row) == Value::boolean(true));
    CHECK_FALSE(isConstant(*e));
}

TEST_CASE("isConstant") {
    CHECK(isConstant(*i(1)));
    CHECK(isConstant(*bin(BinaryOp::Add, i(1), un(UnaryOp::Neg, i(2)))));
    CHECK(isConstant(*isNull(null(), false)));
    CHECK(isConstant(*cast(i(1))));
    CHECK_FALSE(isConstant(*col(0, DataType::Int)));
    CHECK_FALSE(isConstant(*bin(BinaryOp::Add, i(1), col(0, DataType::Int))));
    CHECK_FALSE(isConstant(*cast(col(0, DataType::Int))));
}
