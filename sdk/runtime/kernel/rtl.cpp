// Kernel run time library exports: memory, counted strings, integer helpers, interlocked lists
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>

#include "kernel/exports.h"

namespace kernel {

namespace {

std::mutex g_slist;

uint16_t Upper16(uint16_t c) { return c < 0x80 ? uint16_t(std::toupper(c)) : c; }
uint16_t Lower16(uint16_t c) { return c < 0x80 ? uint16_t(std::tolower(c)) : c; }

// STRING and UNICODE_STRING: USHORT Length, USHORT MaximumLength, buffer; lengths in bytes
uint16_t Len(uint32_t s) { return R16(s); }
uint16_t Max(uint32_t s) { return R16(s + 2); }
uint32_t Buf(uint32_t s) { return R32(s + 4); }

void Copy(uint32_t d, uint32_t s, bool wide, uint16_t (*map)(uint16_t)) {
    if (!s) {
        W16(d, 0);
        return;
    }
    uint16_t n = std::min(Len(s), Max(d));
    if (wide) n &= ~1;
    for (uint16_t i = 0; i < n; i += wide ? 2 : 1) {
        if (wide) W16(Buf(d) + i, map(R16(Buf(s) + i)));
        else G<uint8_t>(Buf(d) + i) = uint8_t(map(G<uint8_t>(Buf(s) + i)));
    }
    W16(d, n);
    if (n + (wide ? 2 : 1) <= Max(d)) {
        if (wide) W16(Buf(d) + n, 0);
        else G<uint8_t>(Buf(d) + n) = 0;
    }
}

uint16_t Same(uint16_t c) { return c; }

int CompareWide(uint32_t s1, uint32_t s2, bool ci) {
    uint16_t n1 = Len(s1) / 2, n2 = Len(s2) / 2;
    for (uint16_t i = 0; i < std::min(n1, n2); i++) {
        uint16_t c1 = R16(Buf(s1) + i * 2u), c2 = R16(Buf(s2) + i * 2u);
        if (ci) c1 = Upper16(c1), c2 = Upper16(c2);
        if (c1 != c2) return int(c1) - int(c2);
    }
    return int(n1) - int(n2);
}

uint32_t Append(uint32_t d, uint32_t src, uint16_t bytes) {
    if (Len(d) + bytes > Max(d)) return kBufferTooSmall;
    std::memmove(HostPtr(Buf(d) + Len(d)), HostPtr(src), bytes);
    W16(d, uint16_t(Len(d) + bytes));
    return kSuccess;
}

// digits of a guest string after optional blanks, sign and a 0x/0o/0b prefix when base is 0
template <class At>
uint32_t ParseInt(At at, uint32_t n, uint32_t base, uint32_t out) {
    uint32_t i = 0;
    while (i < n && at(i) <= ' ') i++;
    bool neg = false;
    if (i < n && (at(i) == '-' || at(i) == '+')) neg = at(i++) == '-';
    if (!base) {
        base = 10;
        if (i + 1 < n && at(i) == '0') {
            char p = char(std::tolower(at(i + 1)));
            base = p == 'x' ? 16 : p == 'o' ? 8 : p == 'b' ? 2 : 10;
            if (base != 10) i += 2;
        }
    } else if (base != 2 && base != 8 && base != 10 && base != 16) {
        return kInvalidParameter;
    }
    uint32_t v = 0;
    for (; i < n; i++) {
        int c = std::tolower(at(i)), d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
        if (d >= int(base)) break;
        v = v * base + uint32_t(d);
    }
    W32(out, neg ? 0u - v : v);
    return kSuccess;
}

std::string Digits(uint32_t v, uint32_t base) {
    if (!base) base = 10;
    std::string s;
    do s.insert(s.begin(), "0123456789ABCDEF"[v % base]);
    while (v /= base);
    return s;
}

}  // namespace

// memory

KERNEL_EXPORT(320, RtlZeroMemory, 2) {
    std::memset(HostPtr(a[0]), 0, a[1]);
    return 0;
}
KERNEL_EXPORT(284, RtlFillMemory, 3) {
    std::memset(HostPtr(a[0]), int(a[2] & 0xFF), a[1]);
    return 0;
}
KERNEL_EXPORT(285, RtlFillMemoryUlong, 3) {
    for (uint32_t o = 0; o + 4 <= a[1]; o += 4) W32(a[0] + o, a[2]);
    return 0;
}
KERNEL_EXPORT(298, RtlMoveMemory, 3) {
    std::memmove(HostPtr(a[0]), HostPtr(a[1]), a[2]);
    return 0;
}
KERNEL_EXPORT(268, RtlCompareMemory, 3) {
    uint32_t n = 0;
    while (n < a[2] && G<uint8_t>(a[0] + n) == G<uint8_t>(a[1] + n)) n++;
    return n;
}

KERNEL_FASTCALL(307, RtlUlongByteSwap) { return __builtin_bswap32(ctx.ecx); }
KERNEL_FASTCALL(318, RtlUshortByteSwap) { return __builtin_bswap16(uint16_t(ctx.ecx)); }

// 64 bit helpers, results in edx:eax

KERNEL_EXPORT(281, RtlExtendedIntegerMultiply, 3) {
    int64_t r = int64_t(uint64_t(a[0]) | uint64_t(a[1]) << 32) * int32_t(a[2]);
    ctx.edx = uint32_t(uint64_t(r) >> 32);
    return uint32_t(r);
}

KERNEL_EXPORT(282, RtlExtendedLargeIntegerDivide, 4) {
    uint64_t n = uint64_t(a[0]) | uint64_t(a[1]) << 32;
    uint32_t d = a[2];
    uint64_t q = d ? n / d : 0;
    if (a[3]) W32(a[3], d ? uint32_t(n % d) : 0);
    ctx.edx = uint32_t(q >> 32);
    return uint32_t(q);
}

// high 64 bits of |dividend| * magic, shifted, with the dividend's sign
KERNEL_EXPORT(283, RtlExtendedMagicDivide, 5) {
    int64_t n = int64_t(uint64_t(a[0]) | uint64_t(a[1]) << 32);
    uint64_t m = uint64_t(a[2]) | uint64_t(a[3]) << 32;
    uint64_t u = n < 0 ? 0 - uint64_t(n) : uint64_t(n);
    uint64_t q = uint64_t((unsigned __int128)u * m >> 64) >> (a[4] & 0xFF);
    if (n < 0) q = 0 - q;
    ctx.edx = uint32_t(q >> 32);
    return uint32_t(q);
}

// characters and counted strings

KERNEL_EXPORT(296, RtlLowerChar, 1) { return uint32_t(std::tolower(int(a[0] & 0xFF))) & 0xFF; }
KERNEL_EXPORT(275, RtlDowncaseUnicodeChar, 1) { return Lower16(uint16_t(a[0])); }

KERNEL_EXPORT(272, RtlCopyString, 2) {
    Copy(a[0], a[1], false, Same);
    return 0;
}
KERNEL_EXPORT(273, RtlCopyUnicodeString, 2) {
    Copy(a[0], a[1], true, Same);
    return 0;
}
KERNEL_EXPORT(317, RtlUpperString, 2) {
    Copy(a[0], a[1], false, [](uint16_t c) { return uint16_t(std::toupper(c)); });
    return 0;
}

static uint32_t CaseWide(uint32_t d, uint32_t s, bool alloc, uint16_t (*map)(uint16_t)) {
    if (alloc) {
        W32(d + 4, mem::AllocSystem(Len(s) + 2u));
        W16(d + 2, uint16_t(Len(s) + 2));
    } else if (Max(d) < Len(s)) {
        return kBufferTooSmall;
    }
    Copy(d, s, true, map);
    return kSuccess;
}
KERNEL_EXPORT(314, RtlUpcaseUnicodeString, 3) { return CaseWide(a[0], a[1], a[2] & 0xFF, Upper16); }
KERNEL_EXPORT(276, RtlDowncaseUnicodeString, 3) { return CaseWide(a[0], a[1], a[2] & 0xFF, Lower16); }

KERNEL_EXPORT(274, RtlCreateUnicodeString, 2) {
    uint32_t n = 0;
    while (R16(a[1] + n)) n += 2;
    uint32_t buf = mem::AllocSystem(n + 2);
    std::memcpy(HostPtr(buf), HostPtr(a[1]), n + 2);
    W16(a[0], uint16_t(n));
    W16(a[0] + 2, uint16_t(n + 2));
    W32(a[0] + 4, buf);
    return 1;
}

KERNEL_EXPORT(287, RtlFreeUnicodeString, 1) {
    if (uint32_t buf = Buf(a[0])) mem::FreeSystem(buf);
    W32(a[0], 0);
    W32(a[0] + 4, 0);
    return 0;
}

KERNEL_EXPORT(271, RtlCompareUnicodeString, 3) { return uint32_t(CompareWide(a[0], a[1], a[2] & 0xFF)); }
KERNEL_EXPORT(280, RtlEqualUnicodeString, 3) { return CompareWide(a[0], a[1], a[2] & 0xFF) == 0; }

KERNEL_EXPORT(261, RtlAppendStringToString, 2) { return Append(a[0], Buf(a[1]), Len(a[1])); }
KERNEL_EXPORT(262, RtlAppendUnicodeStringToString, 2) { return Append(a[0], Buf(a[1]), Len(a[1])); }
KERNEL_EXPORT(263, RtlAppendUnicodeToString, 2) {
    if (!a[1]) return kSuccess;
    uint32_t n = 0;
    while (R16(a[1] + n)) n += 2;
    uint32_t st = Append(a[0], a[1], uint16_t(n));
    if (st == kSuccess && Len(a[0]) + 2 <= Max(a[0])) W16(Buf(a[0]) + Len(a[0]), 0);
    return st;
}

// the console code page is Latin 1
KERNEL_EXPORT(299, RtlMultiByteToUnicodeN, 5) {
    uint32_t n = std::min(a[1] / 2, a[4]);
    for (uint32_t i = 0; i < n; i++) W16(a[0] + i * 2, G<uint8_t>(a[3] + i));
    if (a[2]) W32(a[2], n * 2);
    return kSuccess;
}
KERNEL_EXPORT(310, RtlUnicodeToMultiByteN, 5) {
    uint32_t n = std::min(a[1], a[4] / 2);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = R16(a[3] + i * 2);
        G<uint8_t>(a[0] + i) = c < 0x100 ? uint8_t(c) : '?';
    }
    if (a[2]) W32(a[2], n);
    return kSuccess;
}
KERNEL_EXPORT(315, RtlUpcaseUnicodeToMultiByteN, 5) {
    uint32_t n = std::min(a[1], a[4] / 2);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = Upper16(R16(a[3] + i * 2));
        G<uint8_t>(a[0] + i) = c < 0x100 ? uint8_t(c) : '?';
    }
    if (a[2]) W32(a[2], n);
    return kSuccess;
}
KERNEL_EXPORT(300, RtlMultiByteToUnicodeSize, 3) {
    W32(a[0], a[2] * 2);
    return kSuccess;
}
KERNEL_EXPORT(311, RtlUnicodeToMultiByteSize, 3) {
    W32(a[0], a[2] / 2);
    return kSuccess;
}

KERNEL_EXPORT(267, RtlCharToInteger, 3) {
    uint32_t s = a[0];
    return ParseInt([&](uint32_t i) { return G<uint8_t>(s + i); }, uint32_t(std::strlen(static_cast<char*>(HostPtr(s)))), a[1], a[2]);
}
KERNEL_EXPORT(309, RtlUnicodeStringToInteger, 3) {
    uint32_t b = Buf(a[0]);
    return ParseInt([&](uint32_t i) { return uint8_t(std::min<uint16_t>(R16(b + i * 2), 0xFF)); }, Len(a[0]) / 2u, a[1], a[2]);
}
KERNEL_EXPORT(292, RtlIntegerToChar, 4) {
    std::string s = Digits(a[0], a[1]);
    if (s.size() > a[2]) return 0x80000005;  // buffer overflow
    std::memcpy(HostPtr(a[3]), s.data(), s.size());
    if (s.size() < a[2]) G<uint8_t>(a[3] + uint32_t(s.size())) = 0;
    return kSuccess;
}
KERNEL_EXPORT(293, RtlIntegerToUnicodeString, 3) {
    std::string s = Digits(a[0], a[1]);
    uint32_t d = a[2];
    if (s.size() * 2 > Max(d)) return kBufferTooSmall;
    for (size_t i = 0; i < s.size(); i++) W16(Buf(d) + uint32_t(i) * 2, uint8_t(s[i]));
    W16(d, uint16_t(s.size() * 2));
    if (s.size() * 2 + 2 <= Max(d)) W16(Buf(d) + uint32_t(s.size()) * 2, 0);
    return kSuccess;
}

// no frame walking for the guest: empty traces
KERNEL_EXPORT(288, RtlGetCallersAddress, 2) {
    if (a[0]) W32(a[0], 0);
    if (a[1]) W32(a[1], 0);
    return 0;
}
KERNEL_EXPORT(266, RtlCaptureStackBackTrace, 4) {
    if (a[3]) W32(a[3], 0);
    return 0;
}
KERNEL_EXPORT(319, RtlWalkFrameChain, 3) { return 0; }

// interlocked, fastcall

KERNEL_FASTCALL(53, InterlockedIncrement) { return uint32_t(__atomic_add_fetch(&G<int32_t>(ctx.ecx), 1, __ATOMIC_SEQ_CST)); }
KERNEL_FASTCALL(52, InterlockedDecrement) { return uint32_t(__atomic_sub_fetch(&G<int32_t>(ctx.ecx), 1, __ATOMIC_SEQ_CST)); }
KERNEL_FASTCALL(54, InterlockedExchange) { return __atomic_exchange_n(&G<uint32_t>(ctx.ecx), ctx.edx, __ATOMIC_SEQ_CST); }
KERNEL_FASTCALL(55, InterlockedExchangeAdd) { return __atomic_fetch_add(&G<uint32_t>(ctx.ecx), ctx.edx, __ATOMIC_SEQ_CST); }

// destination in ecx, exchange in edx, comparand on the stack
static void InterlockedCompareExchangeEntry(X86Context& ctx, uint8_t*) {
    uint32_t expected = R32(ctx.esp + 4);
    __atomic_compare_exchange_n(&G<uint32_t>(ctx.ecx), &expected, ctx.edx, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    Return(ctx, expected, 1);
}
static const bool kreg_InterlockedCompareExchange =
    (RegisterExport(51, "InterlockedCompareExchange", InterlockedCompareExchangeEntry), true);

// SLIST_HEADER: first entry, depth, sequence
KERNEL_FASTCALL(58, InterlockedPushEntrySList) {
    std::lock_guard<std::mutex> lock(g_slist);
    uint32_t head = ctx.ecx, entry = ctx.edx, first = R32(head);
    W32(entry, first);
    W32(head, entry);
    W16(head + 4, uint16_t(R16(head + 4) + 1));
    W16(head + 6, uint16_t(R16(head + 6) + 1));
    return first;
}
KERNEL_FASTCALL(57, InterlockedPopEntrySList) {
    std::lock_guard<std::mutex> lock(g_slist);
    uint32_t head = ctx.ecx, first = R32(head);
    if (first) {
        W32(head, R32(first));
        W16(head + 4, uint16_t(R16(head + 4) - 1));
        W16(head + 6, uint16_t(R16(head + 6) + 1));
    }
    return first;
}
KERNEL_FASTCALL(56, InterlockedFlushSList) {
    std::lock_guard<std::mutex> lock(g_slist);
    uint32_t head = ctx.ecx, first = R32(head);
    W32(head, 0);
    W16(head + 4, 0);
    return first;
}

}  // namespace kernel
