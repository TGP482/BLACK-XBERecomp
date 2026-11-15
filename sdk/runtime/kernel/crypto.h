#pragma once
#include <cstddef>
#include <cstdint>

namespace crypto {

// state is kept inside the caller's context buffer (at least 92 bytes)
void ShaInit(void* ctx);
void ShaUpdate(void* ctx, const uint8_t* data, size_t len);
void ShaFinal(void* ctx, uint8_t digest[20]);
void Hmac(const uint8_t* key, size_t key_len, const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len,
          uint8_t digest[20]);

// 258 byte key struct: S[256], i, j
void Rc4Key(void* ks, const uint8_t* key, size_t len);
void Rc4Crypt(void* ks, uint8_t* data, size_t len);

}  // namespace crypto
