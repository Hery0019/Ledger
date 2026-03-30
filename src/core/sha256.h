#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ledger {

// SHA-256 (FIPS 180-4), HMAC (RFC 2104) and PBKDF2 (RFC 8018), implemented
// here because password hashing is Ledger's only cryptographic need and the
// project links nothing external. One-shot APIs over byte strings.

using Sha256Digest = std::array<std::uint8_t, 32>;

Sha256Digest sha256(std::string_view data);

Sha256Digest hmacSha256(std::string_view key, std::string_view data);

// One 32-byte derived block (dkLen = hash length, so exactly one block).
// `iterations` >= 1.
Sha256Digest pbkdf2Sha256(std::string_view password, std::string_view salt, std::uint32_t iterations);

// Lowercase hex, 64 characters.
std::string toHex(const Sha256Digest& digest);

}  // namespace ledger
