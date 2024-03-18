#include "doctest.h"

#include <string>

#include "core/result.h"

using namespace sqltxt;

namespace {

Result<int> parsePositive(int v) {
    if (v < 0) return makeError(ErrorCode::TypeError, "negative");
    return v;
}

Result<void> check(bool ok) {
    if (!ok) return makeError(ErrorCode::IoError, "disk");
    return {};
}

// Vérifie que SQLTXT_TRY propage bien l'erreur et déballe bien la valeur.
Result<int> sumPositives(int a, int b) {
    SQLTXT_TRY(x, parsePositive(a));
    SQLTXT_TRY(y, parsePositive(b));
    SQLTXT_TRY_VOID(check(x + y < 1000));
    return x + y;
}

struct MoveOnly {
    std::string payload;
    MoveOnly(std::string p) : payload(std::move(p)) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
};

Result<MoveOnly> makeMoveOnly() { return MoveOnly{"hello"}; }

}  // namespace

TEST_CASE("Result<T> success path") {
    Result<int> r = parsePositive(5);
    REQUIRE(r.ok());
    REQUIRE(static_cast<bool>(r));
    CHECK(r.value() == 5);
    CHECK(r.valueOr(-1) == 5);
}

TEST_CASE("Result<T> error path") {
    Result<int> r = parsePositive(-1);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::TypeError);
    CHECK(r.error().message == "negative");
    CHECK(r.valueOr(-1) == -1);
}

TEST_CASE("Result<void>") {
    CHECK(check(true).ok());
    Result<void> bad = check(false);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::IoError);
}

TEST_CASE("SQLTXT_TRY propagates the first failure") {
    CHECK(sumPositives(1, 2).value() == 3);

    Result<int> e1 = sumPositives(-1, 2);
    REQUIRE_FALSE(e1.ok());
    CHECK(e1.error().code == ErrorCode::TypeError);

    Result<int> e2 = sumPositives(1, -2);
    REQUIRE_FALSE(e2.ok());
    CHECK(e2.error().code == ErrorCode::TypeError);

    Result<int> e3 = sumPositives(600, 600);
    REQUIRE_FALSE(e3.ok());
    CHECK(e3.error().code == ErrorCode::IoError);
}

TEST_CASE("Result<T> supports move-only types") {
    Result<MoveOnly> r = makeMoveOnly();
    REQUIRE(r.ok());
    MoveOnly m = std::move(r).value();
    CHECK(m.payload == "hello");
}

TEST_CASE("errorCodeName covers every code") {
    CHECK(errorCodeName(ErrorCode::SyntaxError) == "SyntaxError");
    CHECK(errorCodeName(ErrorCode::ConstraintViolation) == "ConstraintViolation");
    CHECK(errorCodeName(ErrorCode::Internal) == "Internal");
}
