#include "kernel/crypto.h"

#include <cstring>
#include <utility>

namespace crypto {

namespace {

struct Sha {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t buf[64];
};
static_assert(sizeof(Sha) <= 116, "must fit the console's SHA context");

uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void Block(Sha& s, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rol(b, 30);
        b = a;
        a = t;
    }
    s.h[0] += a;
    s.h[1] += b;
    s.h[2] += c;
    s.h[3] += d;
    s.h[4] += e;
}

}  // namespace

void ShaInit(void* ctx) {
    Sha s{};
    s.h[0] = 0x67452301;
    s.h[1] = 0xEFCDAB89;
    s.h[2] = 0x98BADCFE;
    s.h[3] = 0x10325476;
    s.h[4] = 0xC3D2E1F0;
    std::memcpy(ctx, &s, sizeof(s));
}

void ShaUpdate(void* ctx, const uint8_t* data, size_t len) {
    Sha s;
    std::memcpy(&s, ctx, sizeof(s));
    for (size_t i = 0; i < len; i++) {
        s.buf[s.bytes % 64] = data[i];
        s.bytes++;
        if (s.bytes % 64 == 0) Block(s, s.buf);
    }
    std::memcpy(ctx, &s, sizeof(s));
}

void ShaFinal(void* ctx, uint8_t digest[20]) {
    Sha s;
    std::memcpy(&s, ctx, sizeof(s));
    uint64_t bits = s.bytes * 8;
    uint8_t pad = 0x80;
    ShaUpdate(&s, &pad, 1);
    uint8_t zero = 0;
    while (s.bytes % 64 != 56) ShaUpdate(&s, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - i * 8));
    ShaUpdate(&s, len, 8);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) digest[i * 4 + j] = uint8_t(s.h[i] >> (24 - j * 8));
    std::memcpy(ctx, &s, sizeof(s));
}

void Hmac(const uint8_t* key, size_t key_len, const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len,
          uint8_t digest[20]) {
    uint8_t k[64] = {};
    Sha s;
    if (key_len > 64) {
        ShaInit(&s);
        ShaUpdate(&s, key, key_len);
        ShaFinal(&s, k);
    } else {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }
    uint8_t inner[20];
    ShaInit(&s);
    ShaUpdate(&s, ipad, 64);
    if (a) ShaUpdate(&s, a, a_len);
    if (b) ShaUpdate(&s, b, b_len);
    ShaFinal(&s, inner);
    ShaInit(&s);
    ShaUpdate(&s, opad, 64);
    ShaUpdate(&s, inner, 20);
    ShaFinal(&s, digest);
}

void Rc4Key(void* ks, const uint8_t* key, size_t len) {
    auto* s = static_cast<uint8_t*>(ks);
    for (int i = 0; i < 256; i++) s[i] = uint8_t(i);
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = uint8_t(j + s[i] + key[i % len]);
        std::swap(s[i], s[j]);
    }
    s[256] = 0;  // the i and j indices live after the table
    s[257] = 0;
}

void Rc4Crypt(void* ks, uint8_t* data, size_t len) {
    auto* s = static_cast<uint8_t*>(ks);
    uint8_t i = s[256], j = s[257];
    for (size_t n = 0; n < len; n++) {
        i++;
        j = uint8_t(j + s[i]);
        std::swap(s[i], s[j]);
        data[n] ^= s[uint8_t(s[i] + s[j])];
    }
    s[256] = i;
    s[257] = j;
}

}  // namespace crypto
