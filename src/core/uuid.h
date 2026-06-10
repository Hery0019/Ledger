#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/result.h"

namespace ledger {

// A 128-bit UUID (RFC 4122), stored as raw bytes. Ordered bytewise, which is
// also the lexicographic order of the canonical text form (fixed hyphens,
// lowercase hex), so text dumps sort like the engine does.
struct Uuid {
    std::array<std::uint8_t, 16> bytes{};
    auto operator<=>(const Uuid&) const = default;
};

// Canonical form: 8-4-4-4-12 lowercase hex, e.g.
// "550e8400-e29b-41d4-a716-446655440000".
std::string formatUuid(const Uuid& id);

// Accepts exactly the canonical shape (hyphens included); hex digits may be
// either case. TypeError otherwise.
Result<Uuid> parseUuid(std::string_view text);

// A fresh version-4 (random) UUID.
Uuid generateUuidV4();

}  // namespace ledger
