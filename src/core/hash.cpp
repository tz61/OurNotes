#include "core/hash.h"

#include <cstring>
#include <fstream>
#include <random>
#include <vector>

#include "core/util.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace on {

static inline uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
static inline uint32_t rotr(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }

// --- MD5 (RFC 1321) ---------------------------------------------------------------

Md5::Md5() : state_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476} {}

void Md5::transform(const uint8_t block[64])
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t M[16];
    for (int i = 0; i < 16; ++i)
        M[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) | ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl(a + f + K[i] + M[g], R[i]);
        a = tmp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::update(const void* data, size_t len)
{
    auto p = (const uint8_t*)data;
    size_t have = count_ % 64;
    count_ += len;
    if (have) {
        size_t need = 64 - have;
        if (len < need) {
            std::memcpy(buffer_ + have, p, len);
            return;
        }
        std::memcpy(buffer_ + have, p, need);
        transform(buffer_);
        p += need;
        len -= need;
    }
    while (len >= 64) {
        transform(p);
        p += 64;
        len -= 64;
    }
    std::memcpy(buffer_, p, len);
}

std::string Md5::hexDigest()
{
    uint64_t bits = count_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (count_ % 64 != 56)
        update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i)
        len[i] = (uint8_t)(bits >> (8 * i));
    update(len, 8);
    std::string raw(16, '\0');
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            raw[i * 4 + j] = (char)(state_[i] >> (8 * j));
    return toHex(raw);
}

// --- SHA-256 (FIPS 180-4) -----------------------------------------------------------

Sha256::Sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}
{
}

void Sha256::transform(const uint8_t block[64])
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, size_t len)
{
    auto p = (const uint8_t*)data;
    size_t have = count_ % 64;
    count_ += len;
    if (have) {
        size_t need = 64 - have;
        if (len < need) {
            std::memcpy(buffer_ + have, p, len);
            return;
        }
        std::memcpy(buffer_ + have, p, need);
        transform(buffer_);
        p += need;
        len -= need;
    }
    while (len >= 64) {
        transform(p);
        p += 64;
        len -= 64;
    }
    std::memcpy(buffer_, p, len);
}

std::string Sha256::digest()
{
    uint64_t bits = count_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (count_ % 64 != 56)
        update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i)
        len[i] = (uint8_t)(bits >> (8 * (7 - i)));
    update(len, 8);
    std::string raw(32, '\0');
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            raw[i * 4 + j] = (char)(state_[i] >> (8 * (3 - j)));
    return raw;
}

std::string Sha256::hexDigest() { return toHex(digest()); }

// --- helpers ------------------------------------------------------------------------

std::string toHex(std::string_view raw)
{
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

std::string md5Hex(std::string_view data)
{
    Md5 m;
    m.update(data.data(), data.size());
    return m.hexDigest();
}

std::string md5File(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error("cannot read " + pathString(path));
    Md5 m;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        auto n = in.gcount();
        if (n > 0)
            m.update(buf.data(), (size_t)n);
    }
    return m.hexDigest();
}

std::string sha256Hex(std::string_view data)
{
    Sha256 s;
    s.update(data.data(), data.size());
    return s.hexDigest();
}

std::string hmacSha256(std::string_view key, std::string_view msg)
{
    std::string k(key);
    if (k.size() > 64) {
        Sha256 s;
        s.update(k.data(), k.size());
        k = s.digest();
    }
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (int i = 0; i < 64; ++i) {
        ipad[i] = (char)(k[i] ^ 0x36);
        opad[i] = (char)(k[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad.data(), 64);
    inner.update(msg.data(), msg.size());
    auto ih = inner.digest();
    Sha256 outer;
    outer.update(opad.data(), 64);
    outer.update(ih.data(), ih.size());
    return outer.digest();
}

std::string pbkdf2Sha256(std::string_view password, std::string_view salt, int iterations, size_t len)
{
    // HMAC with the password as its key, the padded key hashed once: each iteration starts
    // from copies of those two hashes (as hmacSha256 does, without hashing the pads again,
    // which was half the work of each of the 120k iterations of a login)
    std::string k(password);
    if (k.size() > 64) {
        Sha256 s;
        s.update(k.data(), k.size());
        k = s.digest();
    }
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (int i = 0; i < 64; ++i) {
        ipad[i] = (char)(k[i] ^ 0x36);
        opad[i] = (char)(k[i] ^ 0x5c);
    }
    Sha256 innerKeyed, outerKeyed;
    innerKeyed.update(ipad.data(), 64);
    outerKeyed.update(opad.data(), 64);
    auto hmac = [&](std::string_view msg) {
        Sha256 inner = innerKeyed;
        inner.update(msg.data(), msg.size());
        auto ih = inner.digest();
        Sha256 outer = outerKeyed;
        outer.update(ih.data(), ih.size());
        return outer.digest();
    };

    std::string out;
    for (uint32_t block = 1; out.size() < len; ++block) {
        std::string s(salt);
        s += (char)(block >> 24);
        s += (char)(block >> 16);
        s += (char)(block >> 8);
        s += (char)block;
        std::string u = hmac(s);
        std::string t = u;
        for (int i = 1; i < iterations; ++i) {
            u = hmac(u);
            for (size_t j = 0; j < t.size(); ++j)
                t[j] ^= u[j];
        }
        out += t;
    }
    out.resize(len);
    return out;
}

std::string randomBytes(size_t n)
{
    std::string out(n, '\0');
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, (PUCHAR)out.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return out;
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom && urandom.read(out.data(), (std::streamsize)n))
        return out;
#endif
    std::random_device rd;
    for (auto& c : out)
        c = (char)(rd() & 0xFF);
    return out;
}

std::string randomToken(size_t bytes) { return toHex(randomBytes(bytes)); }

bool constantTimeEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace on
