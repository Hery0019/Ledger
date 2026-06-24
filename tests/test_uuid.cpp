#include "doctest.h"

#include <set>
#include <string>

#include "core/uuid.h"
#include "core/value.h"

using namespace ledger;

// ---- parse / format ------------------------------------------------------------

TEST_CASE("parseUuid / formatUuid round-trip, uppercase folded to canonical") {
    const auto id = parseUuid("550e8400-e29b-41d4-a716-446655440000");
    REQUIRE(id.ok());
    CHECK(formatUuid(id.value()) == "550e8400-e29b-41d4-a716-446655440000");

    const auto upper = parseUuid("550E8400-E29B-41D4-A716-446655440000");
    REQUIRE(upper.ok());
    CHECK(upper.value() == id.value());
    CHECK(formatUuid(upper.value()) == "550e8400-e29b-41d4-a716-446655440000");
}

TEST_CASE("parseUuid refuses anything but the canonical shape") {
    for (const auto* bad : {
             "",
             "550e8400e29b41d4a716446655440000",       // no hyphens
             "550e8400-e29b-41d4-a716-44665544000",    // too short
             "550e8400-e29b-41d4-a716-4466554400000",  // too long
             "550e8400-e29b-41d4-a716_446655440000",   // wrong separator
             "550e8400-e29b-41d4-a716-44665544000g",   // not hex
             "-50e8400-e29b-41d4-a716-446655440000",   // hyphen out of place
         }) {
        const auto r = parseUuid(bad);
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK(r.error().code == ErrorCode::TypeError);
    }
}

// ---- generation ----------------------------------------------------------------

TEST_CASE("generateUuidV4 sets the version and variant bits, and does not repeat") {
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        const Uuid id = generateUuidV4();
        CHECK((id.bytes[6] >> 4) == 0x4);          // version 4
        CHECK((id.bytes[8] & 0xc0) == 0x80);       // variant 10xx
        CHECK(seen.insert(formatUuid(id)).second);  // all distinct
        // The canonical form parses back to the same bytes.
        CHECK(parseUuid(formatUuid(id)).value() == id);
    }
}

// ---- Value integration ------------------------------------------------------------

TEST_CASE("Value carries UUIDs like any other type") {
    const Uuid raw = parseUuid("550e8400-e29b-41d4-a716-446655440000").value();
    const Value v = Value::uuid(raw);
    CHECK(v.type() == DataType::Uuid);
    CHECK(v.asUuid() == raw);
    CHECK(v.toText() == "550e8400-e29b-41d4-a716-446655440000");
    CHECK(Value::fromText(DataType::Uuid, v.toText()).value() == v);
    CHECK_FALSE(Value::fromText(DataType::Uuid, "not-a-uuid").ok());
}

TEST_CASE("UUID comparison: bytewise order, NULL unknown, other types refused") {
    const Value a = Value::fromText(DataType::Uuid, "00000000-0000-4000-8000-000000000001").value();
    const Value b = Value::fromText(DataType::Uuid, "ffffffff-ffff-4fff-bfff-ffffffffffff").value();
    CHECK(Value::compare(a, b).value() == Ordering::Less);
    CHECK(Value::compare(b, a).value() == Ordering::Greater);
    CHECK(Value::compare(a, a).value() == Ordering::Equal);
    CHECK(Value::compare(a, Value::null()).value() == Ordering::Unknown);
    CHECK_FALSE(Value::compare(a, Value::integer(1)).ok());
    CHECK_FALSE(Value::compare(a, Value::text("00000000-0000-4000-8000-000000000001")).ok());
}
