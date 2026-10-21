#pragma once
#include <cstddef>
#include <cstring>

// the case-insensitive compares, for both platforms
namespace nocase {

inline int Lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

// the first n characters, ignoring case
inline int Compare(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = Lower(static_cast<unsigned char>(a[i]));
        int cb = Lower(static_cast<unsigned char>(b[i]));
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

// the whole strings, ignoring case
inline bool Equals(const char* a, const char* b) {
    size_t n = std::strlen(a);
    return n == std::strlen(b) && Compare(a, b, n) == 0;
}

// the text starts with the prefix, ignoring case
inline bool StartsWith(const char* text, const char* prefix) { return Compare(text, prefix, std::strlen(prefix)) == 0; }

}  // namespace nocase
