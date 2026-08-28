#include "doctest.h"

#include <cmath>
#include <limits>
#include <string>

#include "core/value.h"

using namespace ledger;

namespace {
Value f(double d) { return Value::real(d).value(); }
Ordering cmp(const Value& a, const Value& b) { return Value::compare(a, b).value(); }
}  // namespace

// ---- construction & inspection ---------------------------------------------

TEST_CASE("Value reports its type") {
    CHECK(Value::null().type() == DataType::Null);
    CHECK(Value::null().isNull());
    CHECK(Value::integer(1).type() == DataType::Int);
    CHECK(f(1.5).type() == DataType::Float);
    CHECK(Value::text("a").type() == DataType::Text);
    CHECK(Value::boolean(true).type() == DataType::Bool);
    CHECK_FALSE(Value::integer(0).isNull());
}

TEST_CASE("Value typed accessors") {
    CHECK(Value::integer(-7).asInt() == -7);
    CHECK(f(2.5).asFloat() == 2.5);
    CHECK(Value::text("abc").asText() == "abc");
    CHECK(Value::boolean(false).asBool() == false);
}

TEST_CASE("Value::real rejects non-finite doubles") {
    CHECK_FALSE(Value::real(std::numeric_limits<double>::quiet_NaN()).ok());
    CHECK_FALSE(Value::real(std::numeric_limits<double>::infinity()).ok());
    CHECK_FALSE(Value::real(-std::numeric_limits<double>::infinity()).ok());
    CHECK(Value::real(0.0).ok());
    CHECK(Value::real(std::numeric_limits<double>::max()).ok());
}

// ---- fromText : erreurs -----------------------------------------------------

TEST_CASE("fromText rejects DataType::Null") {
    Result<Value> r = Value::fromText(DataType::Null, "anything");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Internal);
}

TEST_CASE("fromText INT rejects malformed input") {
    for (const char* s : {"", " 42", "42 ", "4 2", "abc", "42abc", "4.0", "0x1F", "+42", "--1", "1e3"}) {
        CAPTURE(s);
        Result<Value> r = Value::fromText(DataType::Int, s);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::TypeError);
    }
}

TEST_CASE("fromText INT rejects out-of-range") {
    CHECK_FALSE(Value::fromText(DataType::Int, "9223372036854775808").ok());   // 2^63
    CHECK_FALSE(Value::fromText(DataType::Int, "-9223372036854775809").ok());  // -2^63-1
    CHECK(Value::fromText(DataType::Int, "9223372036854775807").value().asInt() ==
          std::numeric_limits<std::int64_t>::max());
    CHECK(Value::fromText(DataType::Int, "-9223372036854775808").value().asInt() ==
          std::numeric_limits<std::int64_t>::min());
}

TEST_CASE("fromText FLOAT rejects malformed and non-finite input") {
    for (const char* s : {"", " 1.5", "1.5 ", "abc", "1.5abc", "nan", "inf", "-inf", "NaN", "1e400"}) {
        CAPTURE(s);
        Result<Value> r = Value::fromText(DataType::Float, s);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::TypeError);
    }
}

TEST_CASE("fromText BOOL rejects anything but true/false") {
    for (const char* s : {"", "1", "0", "yes", "no", "t", "f", " true", "vrai"}) {
        CAPTURE(s);
        Result<Value> r = Value::fromText(DataType::Bool, s);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::TypeError);
    }
}

// ---- fromText : succès ------------------------------------------------------

TEST_CASE("fromText parses valid literals") {
    CHECK(Value::fromText(DataType::Int, "0").value().asInt() == 0);
    CHECK(Value::fromText(DataType::Int, "-42").value().asInt() == -42);
    CHECK(Value::fromText(DataType::Float, "1.5").value().asFloat() == 1.5);
    CHECK(Value::fromText(DataType::Float, "-0.25").value().asFloat() == -0.25);
    CHECK(Value::fromText(DataType::Float, "1e3").value().asFloat() == 1000.0);
    CHECK(Value::fromText(DataType::Float, "3").value().asFloat() == 3.0);
    CHECK(Value::fromText(DataType::Bool, "true").value().asBool() == true);
    CHECK(Value::fromText(DataType::Bool, "FALSE").value().asBool() == false);
    CHECK(Value::fromText(DataType::Bool, "True").value().asBool() == true);
}

TEST_CASE("fromText TEXT keeps the string verbatim, including whitespace and empty") {
    CHECK(Value::fromText(DataType::Text, "").value().asText() == "");
    CHECK(Value::fromText(DataType::Text, "  spaced  ").value().asText() == "  spaced  ");
    CHECK(Value::fromText(DataType::Text, "NULL").value().asText() == "NULL");
    CHECK(Value::fromText(DataType::Text, "42").value().type() == DataType::Text);
}

// ---- round-trip ---------------------------------------------------------------

TEST_CASE("toText/fromText round-trips for every non-null type") {
    const Value samples[] = {
        Value::integer(0),
        Value::integer(std::numeric_limits<std::int64_t>::min()),
        Value::integer(std::numeric_limits<std::int64_t>::max()),
        f(0.0), f(-0.0), f(0.1), f(1.0 / 3.0), f(1e-300), f(1e300),
        f(std::numeric_limits<double>::max()),
        f(std::numeric_limits<double>::denorm_min()),
        Value::text(""), Value::text("héllo wörld"), Value::text("a\tb\nc"),
        Value::boolean(true), Value::boolean(false),
    };
    for (const Value& v : samples) {
        CAPTURE(v.toText());
        Result<Value> back = Value::fromText(v.type(), v.toText());
        REQUIRE(back.ok());
        CHECK(back.value() == v);
    }
}

TEST_CASE("toText produces canonical forms") {
    CHECK(Value::null().toText() == "NULL");
    CHECK(Value::integer(-5).toText() == "-5");
    CHECK(f(2.5).toText() == "2.5");
    CHECK(f(1.0).toText() == "1");
    CHECK(Value::boolean(true).toText() == "true");
    CHECK(Value::text("x").toText() == "x");
}

// ---- comparaison : NULL -------------------------------------------------------

TEST_CASE("compare: NULL with anything is Unknown, never an error") {
    const Value others[] = {Value::null(), Value::integer(1), f(1.0), Value::text("a"), Value::boolean(true)};
    for (const Value& o : others) {
        CAPTURE(o.toText());
        CHECK(cmp(Value::null(), o) == Ordering::Unknown);
        CHECK(cmp(o, Value::null()) == Ordering::Unknown);
    }
}

TEST_CASE("compare: NULL vs incompatible type is still Unknown (NULL wins over type check)") {
    // Un NULL de colonne TEXT comparé à un INT ne doit pas lever TypeError :
    // le binder a déjà validé les types de colonnes, ici NULL absorbe tout.
    CHECK(cmp(Value::null(), Value::text("x")) == Ordering::Unknown);
}

// ---- comparaison : même type ------------------------------------------------

TEST_CASE("compare: INT") {
    CHECK(cmp(Value::integer(1), Value::integer(2)) == Ordering::Less);
    CHECK(cmp(Value::integer(2), Value::integer(1)) == Ordering::Greater);
    CHECK(cmp(Value::integer(-3), Value::integer(-3)) == Ordering::Equal);
    CHECK(cmp(Value::integer(std::numeric_limits<std::int64_t>::min()),
              Value::integer(std::numeric_limits<std::int64_t>::max())) == Ordering::Less);
}

TEST_CASE("compare: FLOAT") {
    CHECK(cmp(f(1.5), f(2.5)) == Ordering::Less);
    CHECK(cmp(f(-0.0), f(0.0)) == Ordering::Equal);
    CHECK(cmp(f(1e300), f(1e-300)) == Ordering::Greater);
}

TEST_CASE("compare: TEXT is bytewise, case-sensitive") {
    CHECK(cmp(Value::text("a"), Value::text("b")) == Ordering::Less);
    CHECK(cmp(Value::text("B"), Value::text("a")) == Ordering::Less);  // 'B' (0x42) < 'a' (0x61)
    CHECK(cmp(Value::text("abc"), Value::text("ab")) == Ordering::Greater);
    CHECK(cmp(Value::text(""), Value::text("")) == Ordering::Equal);
    CHECK(cmp(Value::text(""), Value::text("a")) == Ordering::Less);
}

TEST_CASE("compare: BOOL false < true") {
    CHECK(cmp(Value::boolean(false), Value::boolean(true)) == Ordering::Less);
    CHECK(cmp(Value::boolean(true), Value::boolean(true)) == Ordering::Equal);
}

// ---- comparaison : types mixtes ---------------------------------------------

TEST_CASE("compare: INT vs FLOAT is numeric in both directions") {
    CHECK(cmp(Value::integer(2), f(2.0)) == Ordering::Equal);
    CHECK(cmp(f(2.0), Value::integer(2)) == Ordering::Equal);
    CHECK(cmp(Value::integer(2), f(2.5)) == Ordering::Less);
    CHECK(cmp(f(2.5), Value::integer(2)) == Ordering::Greater);
    CHECK(cmp(Value::integer(-1), f(-0.5)) == Ordering::Less);
}

TEST_CASE("compare: incompatible types are a TypeError, not false") {
    const Value i = Value::integer(1);
    const Value t = Value::text("1");
    const Value b = Value::boolean(true);
    const Value d = f(1.0);

    for (auto [a, c] : {std::pair{i, t}, std::pair{t, i}, std::pair{i, b}, std::pair{b, i},
                        std::pair{t, b}, std::pair{b, t}, std::pair{d, t}, std::pair{d, b}}) {
        CAPTURE(a.toText());
        CAPTURE(c.toText());
        Result<Ordering> r = Value::compare(a, c);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::TypeError);
    }
}

// ---- égalité structurelle ---------------------------------------------------

TEST_CASE("operator== is strict structural equality") {
    CHECK(Value::null() == Value::null());
    CHECK(Value::integer(2) == Value::integer(2));
    CHECK_FALSE(Value::integer(2) == f(2.0));       // types différents : pas égaux
    CHECK_FALSE(Value::text("1") == Value::integer(1));
    CHECK_FALSE(Value::boolean(true) == Value::integer(1));
}
