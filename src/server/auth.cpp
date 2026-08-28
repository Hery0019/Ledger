#include "server/auth.h"

#include <cstddef>

#include "core/password.h"

namespace ledger {

namespace {

int base64Value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::optional<std::string> base64Decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') text.remove_suffix(1);
    std::string out;
    out.reserve(text.size() * 3 / 4);
    unsigned bits = 0;
    unsigned have = 0;
    for (const char c : text) {
        const int v = base64Value(c);
        if (v < 0) return std::nullopt;
        bits = (bits << 6) | static_cast<unsigned>(v);
        have += 6;
        if (have >= 8) {
            have -= 8;
            out += static_cast<char>((bits >> have) & 0xff);
        }
    }
    // 6 leftover bits (1 stray character) cannot encode anything.
    if (have >= 6) return std::nullopt;
    return out;
}

char toLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::optional<Credentials> parseBasicAuth(std::string_view authorization) {
    constexpr std::string_view kScheme = "basic ";
    if (authorization.size() <= kScheme.size()) return std::nullopt;
    for (std::size_t i = 0; i < kScheme.size(); ++i) {
        if (toLowerAscii(authorization[i]) != kScheme[i]) return std::nullopt;
    }
    auto decoded = base64Decode(authorization.substr(kScheme.size()));
    if (!decoded) return std::nullopt;
    const std::size_t colon = decoded->find(':');
    if (colon == std::string::npos || colon == 0) return std::nullopt;
    Credentials creds;
    for (const char c : decoded->substr(0, colon)) creds.name += toLowerAscii(c);
    creds.password = decoded->substr(colon + 1);
    return creds;
}

bool authorize(const Catalog& catalog, AuthCache& cache, std::string_view authorization) {
    if (!catalog.hasUsers()) return true;  // an open database, like before CREATE USER
    const auto creds = parseBasicAuth(authorization);
    if (!creds) return false;
    const UserDef* user = catalog.findUser(creds->name);
    if (!user) return false;
    const std::string key =
        user->name + '\n' + user->saltHex + '\n' + user->hashHex + '\n' + creds->password;
    if (cache.verified.contains(key)) return true;
    if (!verifyPassword(*user, creds->password)) return false;
    if (cache.verified.size() >= 64) cache.verified.clear();
    cache.verified.insert(key);
    return true;
}

}  // namespace ledger
