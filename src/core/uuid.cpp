#include "core/uuid.h"

#include <chrono>
#include <cstddef>
#include <random>

namespace ledger {

namespace {

constexpr char kHex[] = "0123456789abcdef";

// Positions of the hyphens in the canonical form.
constexpr bool isHyphenPos(std::size_t i) noexcept { return i == 8 || i == 13 || i == 18 || i == 23; }

int hexNibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// One generator per thread, seeded once. std::random_device is the entropy
// source; the clock is mixed in because some implementations (old MinGW)
// made it deterministic. Uniqueness matters here, not secrecy.
std::mt19937_64& rng() {
    thread_local std::mt19937_64 gen = [] {
        std::random_device rd;
        std::seed_seq seed{rd(), rd(), rd(), rd(),
                           static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()),
                           static_cast<unsigned>(std::chrono::system_clock::now().time_since_epoch().count())};
        return std::mt19937_64(seed);
    }();
    return gen;
}

}  // namespace

std::string formatUuid(const Uuid& id) {
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += kHex[id.bytes[i] >> 4];
        out += kHex[id.bytes[i] & 0xf];
    }
    return out;
}

Result<Uuid> parseUuid(std::string_view text) {
    const auto invalid = [&] {
        return makeError(ErrorCode::TypeError, "invalid UUID literal: '" + std::string(text) +
                                                   "' (expected 8-4-4-4-12 hex digits)");
    };
    if (text.size() != 36) return invalid();
    Uuid id;
    std::size_t byte = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (isHyphenPos(i)) {
            if (text[i] != '-') return invalid();
            ++i;
            continue;
        }
        const int hi = hexNibble(text[i]);
        const int lo = hexNibble(text[i + 1]);
        if (hi < 0 || lo < 0) return invalid();
        id.bytes[byte++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
    }
    return id;
}

Uuid generateUuidV4() {
    Uuid id;
    std::uint64_t r = 0;
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
        if (i % 8 == 0) r = rng()();
        id.bytes[i] = static_cast<std::uint8_t>(r >> ((i % 8) * 8));
    }
    // RFC 4122: version 4 in the high nibble of byte 6, variant 10xx in the
    // two high bits of byte 8.
    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0f) | 0x40);
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3f) | 0x80);
    return id;
}

}  // namespace ledger
