#include "doctest.h"

#include <string>

#include "core/password.h"
#include "server/auth.h"

using namespace ledger;

// ---- header parsing ------------------------------------------------------------

TEST_CASE("parseBasicAuth decodes RFC 7617 credentials") {
    // base64("alice:S3cret!") = YWxpY2U6UzNjcmV0IQ==
    const auto ok = parseBasicAuth("Basic YWxpY2U6UzNjcmV0IQ==");
    REQUIRE(ok);
    CHECK(ok->name == "alice");
    CHECK(ok->password == "S3cret!");

    // Scheme is case-insensitive; the name is folded like an identifier, the
    // password is not. base64("Alice:Pw") = QWxpY2U6UHc=
    const auto folded = parseBasicAuth("bAsIc QWxpY2U6UHc=");
    REQUIRE(folded);
    CHECK(folded->name == "alice");
    CHECK(folded->password == "Pw");

    // A password may contain ':'. base64("a:b:c") = YTpiOmM=
    const auto colons = parseBasicAuth("Basic YTpiOmM=");
    REQUIRE(colons);
    CHECK(colons->name == "a");
    CHECK(colons->password == "b:c");
}

TEST_CASE("parseBasicAuth refuses everything else") {
    CHECK_FALSE(parseBasicAuth(""));
    CHECK_FALSE(parseBasicAuth("Basic"));
    CHECK_FALSE(parseBasicAuth("Bearer abc"));
    CHECK_FALSE(parseBasicAuth("Basic !!!not-base64!!!"));
    CHECK_FALSE(parseBasicAuth("Basic YWJjZ"));         // 1 stray base64 character
    CHECK_FALSE(parseBasicAuth("Basic YWxpY2U="));      // "alice": no colon
    CHECK_FALSE(parseBasicAuth("Basic OnB3"));          // ":pw": empty name
}

// ---- authorization -------------------------------------------------------------

TEST_CASE("authorize: a database without accounts is open") {
    Catalog catalog;
    AuthCache cache;
    CHECK(authorize(catalog, cache, ""));
    CHECK(authorize(catalog, cache, "Basic garbage"));
}

TEST_CASE("authorize: with accounts, only matching credentials pass") {
    Catalog catalog;
    AuthCache cache;
    REQUIRE(catalog.addUser(makeUser("alice", "S3cret!")).ok());

    CHECK_FALSE(authorize(catalog, cache, ""));
    CHECK_FALSE(authorize(catalog, cache, "Basic YWxpY2U6d3Jvbmc="));   // alice:wrong
    CHECK_FALSE(authorize(catalog, cache, "Basic Ym9iOlMzY3JldCE="));   // bob:S3cret!
    CHECK(authorize(catalog, cache, "Basic YWxpY2U6UzNjcmV0IQ=="));     // alice:S3cret!
    // Second time comes from the cache (only successes are stored there).
    CHECK(cache.verified.size() == 1);
    CHECK(authorize(catalog, cache, "Basic YWxpY2U6UzNjcmV0IQ=="));
    CHECK(cache.verified.size() == 1);
}

TEST_CASE("authorize: the cache survives neither ALTER USER nor DROP USER") {
    Catalog catalog;
    AuthCache cache;
    REQUIRE(catalog.addUser(makeUser("bob", "bob")).ok());  // keeps the database locked throughout
    REQUIRE(catalog.addUser(makeUser("alice", "old")).ok());
    const std::string oldCreds = "Basic YWxpY2U6b2xk";  // alice:old
    REQUIRE(authorize(catalog, cache, oldCreds));

    // Password change: the cache key binds the stored salt+hash, so the old
    // password misses the cache and fails verification.
    REQUIRE(catalog.replaceUser(makeUser("alice", "new")).ok());
    CHECK_FALSE(authorize(catalog, cache, oldCreds));
    CHECK(authorize(catalog, cache, "Basic YWxpY2U6bmV3"));  // alice:new

    // Dropped user: refused before the cache is even consulted.
    REQUIRE(catalog.removeUser("alice").ok());
    CHECK_FALSE(authorize(catalog, cache, "Basic YWxpY2U6bmV3"));

    // Dropping the LAST account reopens the database: that is the documented
    // bootstrap semantic, not an oversight.
    REQUIRE(catalog.removeUser("bob").ok());
    CHECK(authorize(catalog, cache, ""));
}
