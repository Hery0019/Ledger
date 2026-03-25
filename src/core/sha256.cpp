#include "core/sha256.h"

#include <cstddef>
#include <cstring>

namespace ledger {

namespace {

// FIPS 180-4 section 4.2.2: first 32 bits of the fractional parts of the
// cube roots of the first 64 primes.
constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32 - n)); }

// Streaming context: the padding trailer needs the total length, and HMAC /
// PBKDF2 hash concatenations without building them.
struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint8_t block[64];
    std::size_t fill = 0;       // bytes waiting in `block`
    std::uint64_t total = 0;    // message length so far, in bytes

    void compress(const std::uint8_t* p) noexcept {
        std::uint32_t w[64];
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{p[4 * i]} << 24) | (std::uint32_t{p[4 * i + 1]} << 16) |
                   (std::uint32_t{p[4 * i + 2]} << 8) | std::uint32_t{p[4 * i + 3]};
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + kRound[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const void* data, std::size_t size) noexcept {
        const auto* p = static_cast<const std::uint8_t*>(data);
        total += size;
        if (fill) {
            const std::size_t take = size < 64 - fill ? size : 64 - fill;
            std::memcpy(block + fill, p, take);
            fill += take;
            p += take;
            size -= take;
            if (fill < 64) return;
            compress(block);
            fill = 0;
        }
        for (; size >= 64; p += 64, size -= 64) compress(p);
        std::memcpy(block, p, size);
        fill = size;
    }

    void update(std::string_view data) noexcept { update(data.data(), data.size()); }

    Sha256Digest finish() noexcept {
        const std::uint64_t bits = total * 8;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (fill != 56) update(&zero, 1);
        std::uint8_t trailer[8];
        for (std::size_t i = 0; i < 8; ++i) trailer[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        update(trailer, 8);
        Sha256Digest out;
        for (std::size_t i = 0; i < 8; ++i) {
            out[4 * i] = static_cast<std::uint8_t>(h[i] >> 24);
            out[4 * i + 1] = static_cast<std::uint8_t>(h[i] >> 16);
            out[4 * i + 2] = static_cast<std::uint8_t>(h[i] >> 8);
            out[4 * i + 3] = static_cast<std::uint8_t>(h[i]);
        }
        return out;
    }
};

// The two padded keys of HMAC: K' ^ ipad and K' ^ opad.
struct HmacKey {
    std::uint8_t inner[64];
    std::uint8_t outer[64];

    explicit HmacKey(std::string_view key) noexcept {
        std::uint8_t k[64] = {};
        if (key.size() > 64) {
            const Sha256Digest d = sha256(key);
            std::memcpy(k, d.data(), d.size());
        } else {
            std::memcpy(k, key.data(), key.size());
        }
        for (std::size_t i = 0; i < 64; ++i) {
            inner[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
            outer[i] = static_cast<std::uint8_t>(k[i] ^ 0x5c);
        }
    }

    Sha256Digest mac(const void* data, std::size_t size) const noexcept {
        Sha256 in;
        in.update(inner, 64);
        in.update(data, size);
        const Sha256Digest first = in.finish();
        Sha256 out;
        out.update(outer, 64);
        out.update(first.data(), first.size());
        return out.finish();
    }
};

}  // namespace

Sha256Digest sha256(std::string_view data) {
    Sha256 ctx;
    ctx.update(data);
    return ctx.finish();
}

Sha256Digest hmacSha256(std::string_view key, std::string_view data) {
    return HmacKey(key).mac(data.data(), data.size());
}

Sha256Digest pbkdf2Sha256(std::string_view password, std::string_view salt, std::uint32_t iterations) {
    const HmacKey key(password);
    // U1 = HMAC(P, S || INT(1)); Ui = HMAC(P, Ui-1); T = U1 ^ ... ^ Uc.
    std::string first(salt);
    first.append("\x00\x00\x00\x01", 4);
    Sha256Digest u = key.mac(first.data(), first.size());
    Sha256Digest t = u;
    for (std::uint32_t i = 1; i < iterations; ++i) {
        u = key.mac(u.data(), u.size());
        for (std::size_t j = 0; j < t.size(); ++j) t[j] = static_cast<std::uint8_t>(t[j] ^ u[j]);
    }
    return t;
}

std::string toHex(const Sha256Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 0xf];
    }
    return out;
}

}  // namespace ledger
