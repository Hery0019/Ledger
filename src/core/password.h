#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ledger {

// A user account: a name and a salted PBKDF2-HMAC-SHA256 password hash.
// This is what users.txt stores and what the catalog holds in memory; the
// plain password never persists. Accounts gate the HTTP server (ledgerd):
// whoever can read the database files directly needs no password, exactly
// like the postgres OS user.
struct UserDef {
    std::string name;         // lowercase (an identifier, folded by the lexer)
    std::string saltHex;      // 16 random bytes, lowercase hex
    std::string hashHex;      // pbkdf2Sha256(password, salt, iterations), lowercase hex
    std::uint32_t iterations = 0;
};

// Iteration count for newly (re)created users. Deliberately modest — this
// protects a personal database, not a bank — and stored per user, so it can
// be raised later without breaking existing records.
inline constexpr std::uint32_t kPasswordIterations = 10'000;

// Builds the stored record for `password`, with a fresh random salt.
UserDef makeUser(std::string name, std::string_view password);

// True if `password` matches the record. Constant-time comparison; false on
// a corrupted record (bad hex, zero iterations) rather than an error.
[[nodiscard]] bool verifyPassword(const UserDef& user, std::string_view password);

}  // namespace ledger
