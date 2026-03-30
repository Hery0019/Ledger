#include "doctest.h"

#include <string>

#include "core/password.h"
#include "core/sha256.h"

using namespace ledger;

// ---- SHA-256 (FIPS 180-4 / NIST examples) ------------------------------------

TEST_CASE("sha256 matches the standard test vectors") {
    CHECK(toHex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toHex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // One million 'a': exercises the multi-block and length-padding paths.
    CHECK(toHex(sha256(std::string(1'000'000, 'a'))) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // 64 bytes exactly: the padding needs a whole extra block.
    CHECK(toHex(sha256(std::string(64, 'x'))) ==
          toHex(sha256(std::string(32, 'x') + std::string(32, 'x'))));
}

// ---- HMAC-SHA256 (RFC 4231) ----------------------------------------------------

TEST_CASE("hmacSha256 matches RFC 4231") {
    CHECK(toHex(hmacSha256(std::string(20, '\x0b'), "Hi There")) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(toHex(hmacSha256("Jefe", "what do ya want for nothing?")) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // Key longer than a block: hashed first (RFC 4231 test case 6).
    CHECK(toHex(hmacSha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// ---- PBKDF2-HMAC-SHA256 --------------------------------------------------------

TEST_CASE("pbkdf2Sha256 matches the published vectors") {
    CHECK(toHex(pbkdf2Sha256("password", "salt", 1)) ==
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(toHex(pbkdf2Sha256("password", "salt", 2)) ==
          "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    CHECK(toHex(pbkdf2Sha256("password", "salt", 4096)) ==
          "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}

// ---- user records --------------------------------------------------------------

TEST_CASE("makeUser / verifyPassword round-trip") {
    const UserDef u = makeUser("alice", "s3cret");
    CHECK(u.name == "alice");
    CHECK(u.saltHex.size() == 32);   // 16 bytes
    CHECK(u.hashHex.size() == 64);   // 32 bytes
    CHECK(u.iterations == kPasswordIterations);
    CHECK(verifyPassword(u, "s3cret"));
    CHECK_FALSE(verifyPassword(u, "S3cret"));
    CHECK_FALSE(verifyPassword(u, ""));
}

TEST_CASE("two records for the same password differ (fresh salt)") {
    const UserDef a = makeUser("a", "same");
    const UserDef b = makeUser("b", "same");
    CHECK(a.saltHex != b.saltHex);
    CHECK(a.hashHex != b.hashHex);
    CHECK(verifyPassword(a, "same"));
    CHECK(verifyPassword(b, "same"));
}

TEST_CASE("verifyPassword refuses corrupted records instead of failing loudly") {
    UserDef u = makeUser("alice", "pw");
    UserDef badSalt = u;
    badSalt.saltHex = "not-hex!";
    CHECK_FALSE(verifyPassword(badSalt, "pw"));
    UserDef badIter = u;
    badIter.iterations = 0;
    CHECK_FALSE(verifyPassword(badIter, "pw"));
    UserDef badHash = u;
    badHash.hashHex[0] = badHash.hashHex[0] == '0' ? '1' : '0';
    CHECK_FALSE(verifyPassword(badHash, "pw"));
}
