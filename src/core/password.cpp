#include "core/password.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <random>

#include "core/sha256.h"

namespace ledger {

namespace {

std::optional<int> hexNibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return std::nullopt;
}

// Lowercase hex -> bytes; nullopt on anything else (corrupted users file).
std::optional<std::string> fromHex(std::string_view hex) {
    if (hex.size() % 2) return std::nullopt;
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const auto hi = hexNibble(hex[i]);
        const auto lo = hexNibble(hex[i + 1]);
        if (!hi || !lo) return std::nullopt;
        out += static_cast<char>((*hi << 4) | *lo);
    }
    return out;
}

// 16 random salt bytes. std::random_device is the entropy source; the clock
// is mixed in because some implementations (old MinGW) made it deterministic.
std::string randomSalt() {
    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(),
                       static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()),
                       static_cast<unsigned>(std::chrono::system_clock::now().time_since_epoch().count())};
    std::mt19937 gen(seed);
    std::string salt;
    salt.reserve(16);
    for (int i = 0; i < 16; ++i) salt += static_cast<char>(gen() & 0xff);
    return salt;
}

// Not simply ==: the comparison must not leak how many leading characters
// match. Compares everything, whatever happens.
bool equalConstantTime(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^
                                                  static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

std::string hexOfBytes(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out += kHex[b >> 4];
        out += kHex[b & 0xf];
    }
    return out;
}

}  // namespace

UserDef makeUser(std::string name, std::string_view password) {
    const std::string salt = randomSalt();
    const Sha256Digest hash = pbkdf2Sha256(password, salt, kPasswordIterations);
    return UserDef{std::move(name), hexOfBytes(salt), toHex(hash), kPasswordIterations};
}

bool verifyPassword(const UserDef& user, std::string_view password) {
    const auto salt = fromHex(user.saltHex);
    if (!salt || user.iterations == 0) return false;
    const Sha256Digest hash = pbkdf2Sha256(password, *salt, user.iterations);
    return equalConstantTime(toHex(hash), user.hashHex);
}

}  // namespace ledger
