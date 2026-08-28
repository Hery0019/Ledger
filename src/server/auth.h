#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "semantic/catalog.h"

namespace ledger {

// HTTP Basic credentials: "Basic base64(name:password)" (RFC 7617). The name
// is folded to ASCII lowercase, like every Ledger identifier; the password is
// kept as is. nullopt for anything else (missing header, other scheme,
// broken base64, no ':').
struct Credentials {
    std::string name;
    std::string password;
};
std::optional<Credentials> parseBasicAuth(std::string_view authorization);

// Successful verifications, so that a client hammering /query does not pay
// PBKDF2 on every request. A key binds the exact stored record (salt + hash)
// to the password: ALTER USER changes the record and therefore misses; DROP
// USER is caught by the catalog lookup before the cache is consulted. Only
// successes are cached — failures stay expensive. The caller guards it with
// the same mutex as the database.
struct AuthCache {
    std::unordered_set<std::string> verified;
};

// True if the database is open (no users), or if `authorization` carries
// Basic credentials matching a stored user.
[[nodiscard]] bool authorize(const Catalog& catalog, AuthCache& cache, std::string_view authorization);

}  // namespace ledger
