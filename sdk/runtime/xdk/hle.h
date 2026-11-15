#pragma once
#include <cstring>
#include <iterator>
#include <string_view>
#include <type_traits>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

// Symbols are named in config/<image>.toml; the recompiler writes their addresses to image_symbols.h
#define X86_CAT_(a, b) a##b
#define X86_CAT(a, b) X86_CAT_(a, b)
#ifdef IMAGE_NAMED
// [xbe] named: one executable serves each release of the game, so a name's address is the loaded image's, from the
// X86Symbols its DLL exports; a name the config lacks keeps the placeholder the host gives it
namespace hle {
#define HLE_SYMBOL_NAME(name) #name,
inline constexpr std::string_view kSymbolNames[] = {IMAGE_SYMBOLS(HLE_SYMBOL_NAME)};
#undef HLE_SYMBOL_NAME
constexpr uint32_t kSymbolCount = uint32_t(std::size(kSymbolNames));
constexpr uint32_t kNotNamed = 0xFFFFFFFFu;
constexpr uint32_t kAbsent = 0xFFFF0000u;  // the recompiler's addresses for functions not in the image
inline uint32_t g_symbols[kSymbolCount] = {};

consteval uint32_t Id(std::string_view name) {
    for (uint32_t i = 0; i < kSymbolCount; i++)
        if (kSymbolNames[i] == name) return i;
    return kNotNamed;
}

struct Symbol {
    uint32_t id;
    uint32_t placeholder;
    operator uint32_t() const { return id == kNotNamed ? placeholder : g_symbols[id]; }
    bool Present(bool placeholder_present) const {
        if (id == kNotNamed) return placeholder_present;
        return g_symbols[id] && g_symbols[id] < kAbsent;
    }
};
using Va = Symbol;

inline void ResolveSymbols(const X86Symbol* table) {
    bool found[kSymbolCount] = {};
    for (const X86Symbol* s = table; s->name; s++) {
        for (uint32_t i = 0; i < kSymbolCount; i++) {
            if (kSymbolNames[i] != s->name) continue;
            g_symbols[i] = s->va;
            found[i] = true;
        }
    }
    for (uint32_t i = 0; i < kSymbolCount; i++)
        if (!found[i]) LOGE("symbols: %.*s is not in this image's config", int(kSymbolNames[i].size()), kSymbolNames[i].data());
}
}  // namespace hle
#define SYM(name) (::hle::Symbol{::hle::Id(#name), X86_CAT(0x, SYM_##name)})  // address
#define HAS(name) (SYM(name).Present(HAS_##name))
#define GUEST(name) sub_##name
#define GUEST_ORIG(name) __imp__sub_##name
#else
namespace hle {
using Va = uint32_t;
}
#define SYM(name) X86_CAT(0x, SYM_##name)                 // address
#define HAS(name) HAS_##name                              // in this image
#define GUEST(name) X86_CAT(sub_, SYM_##name)             // the function, a host override if there is one
#define GUEST_ORIG(name) X86_CAT(__imp__sub_, SYM_##name)  // the recompiled body
#endif

// Argument maps. A library built with link time code generation may take a function's arguments in registers. The
// config's [abi] says where each API argument of such a function is, image_symbols.h lists the maps as IMAGE_ABI,
// and the macros below read arguments and pop the stack by them. A name without a map keeps the declared convention.
#ifndef IMAGE_ABI
#define IMAGE_ABI(X)
#endif
namespace hle {
enum Loc : uint8_t { stack, eax, ecx, edx, ebx, esi, edi, ebp };
constexpr int kMaxArgs = 16;
struct Map {
    bool mapped = false;
    uint8_t n = 0, pops = 0;  // API arguments, stack slots the callee pops
    uint8_t loc[kMaxArgs] = {}, slot[kMaxArgs] = {};
};
constexpr Map Make(std::initializer_list<Loc> locs) {
    Map m;
    m.mapped = true;
    for (Loc l : locs) {
        m.loc[m.n] = l;
        m.slot[m.n++] = m.pops;
        if (l == stack) m.pops++;
    }
    return m;
}
struct Row {
    std::string_view name;
    Map map;
};
#define HLE_ABI_ROW(name, ...) {#name, Make({__VA_ARGS__})},
inline constexpr Row kRows[] = {IMAGE_ABI(HLE_ABI_ROW) {"", {}}};
#undef HLE_ABI_ROW
constexpr Map Find(std::string_view name, Map declared = {}) {
    for (const Row& r : kRows)
        if (r.name == name) return r.map;
    return declared;
}
// __fastcall: ecx, edx, then the stack
constexpr Map Fastcall(int stack_args) {
    Map m = Make({ecx, edx});
    for (int i = 0; i < stack_args; i++) {
        m.loc[m.n] = stack;
        m.slot[m.n++] = m.pops++;
    }
    return m;
}
// by Loc
inline constexpr uint32_t X86Context::* kReg[] = {nullptr, &X86Context::eax, &X86Context::ecx, &X86Context::edx,
                                                 &X86Context::ebx, &X86Context::esi, &X86Context::edi, &X86Context::ebp};
// API argument i at the function's entry, 0 past the map
inline uint32_t Read(const X86Context& ctx, const Map& m, int i) {
    if (i >= m.n) return 0;
    return m.loc[i] == stack ? ::kernel::R32(ctx.esp + 4 + 4 * uint32_t(m.slot[i])) : ctx.*kReg[m.loc[i]];
}
// S::map is the function's map
template <class S>
struct Args {
    X86Context& ctx;
    uint32_t operator[](int i) const { return Read(ctx, S::map, i); }
};
// calls a mapped guest function: registers loaded, stack arguments pushed, every register put back
inline uint32_t Call(X86Context& ctx, uint32_t fn, const Map& m, std::initializer_list<uint32_t> args) {
    if (args.size() != m.n) x86_trap(ctx, fn, "host call and [abi] map differ in argument count");
    uint32_t saved[8] = {0, ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
    uint32_t pushed[kMaxArgs] = {}, n = 0, i = 0;
    for (uint32_t v : args) {
        if (m.loc[i] == stack) pushed[n++] = v;
        else ctx.*kReg[m.loc[i]] = v;
        i++;
    }
    uint32_t r = ::kernel::CallGuest(ctx, fn, pushed, n);
    for (int k = 1; k < 8; k++) ctx.*kReg[k] = saved[k];
    return r;
}
}  // namespace hle

// S::map is the function's map and A its argument type: plain stack arguments unless the image maps the name
#define HLE_ABI_(S, A, text, declared, counted)                                                \
    struct S {                                                                                 \
        static constexpr ::hle::Map map = ::hle::Find(text, declared);                         \
    };                                                                                         \
    static_assert(counted, "[abi] " text " does not have the argument count of the function"); \
    using A = std::conditional_t<S::map.mapped, ::hle::Args<S>, ::kernel::Args>;

#define HLE_BODY_(fn, guest, A, pops)                    \
    static uint32_t fn(X86Context& ctx, const A& a);     \
    X86_FUNC(guest) {                                    \
        (void)base;                                      \
        A args{ctx};                                     \
        uint32_t r = fn(ctx, args);                      \
        ::kernel::Return(ctx, r, pops);                  \
    }                                                    \
    static uint32_t fn([[maybe_unused]] X86Context& ctx, [[maybe_unused]] const A& a)

// Host implementation of a guest function: the strong definition replaces the recompiled weak one.
// a[i] is API argument i.
#define HLE_STDCALL(name, nargs)                                                \
    HLE_ABI_(hle_abi_##name, hle_args_##name, #name, ::hle::Map{},              \
             !hle_abi_##name::map.mapped || hle_abi_##name::map.n == (nargs))   \
    HLE_BODY_(hle_##name, GUEST(name), hle_args_##name,                         \
              hle_abi_##name::map.mapped ? hle_abi_##name::map.pops : (nargs))

// __fastcall: first two arguments in ecx and edx, the rest on the stack
#define HLE_FASTCALL(name, stack_args)                                                                   \
    HLE_ABI_(hle_abi_##name, hle_args_##name, #name, ::hle::Fastcall(stack_args),                        \
             hle_abi_##name::map.n == 2 + (stack_args) || (hle_abi_##name::map.n == 1 && !(stack_args))) \
    HLE_BODY_(hle_##name, GUEST(name), hle_args_##name, hle_abi_##name::map.pops)

// __cdecl: the caller pops the arguments
#define HLE_CDECL(name)                                                       \
    HLE_ABI_(hle_abi_##name, hle_args_##name, #name, ::hle::Map{}, true)      \
    HLE_BODY_(hle_##name, GUEST(name), hle_args_##name, hle_abi_##name::map.mapped ? hle_abi_##name::map.pops : 0)

// Runs before the recompiled function, which then sees its arguments untouched
#define HLE_WRAP_(fn, guest, orig, A)                \
    X86_EXTERN_FUNC(orig);                           \
    static void fn(X86Context& ctx, const A& a);     \
    X86_FUNC(guest) {                                \
        fn(ctx, A{ctx});                             \
        orig(ctx, base);                             \
    }                                                \
    static void fn([[maybe_unused]] X86Context& ctx, [[maybe_unused]] const A& a)
#define HLE_WRAP(name, nargs)                                                   \
    HLE_ABI_(hle_abi_##name, hle_args_##name, #name, ::hle::Map{},              \
             !hle_abi_##name::map.mapped || hle_abi_##name::map.n == (nargs))   \
    HLE_WRAP_(hle_##name, GUEST(name), GUEST_ORIG(name), hle_args_##name)

// Calls a guest function by name with its API arguments: GUEST_CALL(name, ctx, {a, b})
#define GUEST_CALL(name, ctx, ...)                                                           \
    ([&]() -> uint32_t {                                                                     \
        static constexpr ::hle::Map map_ = ::hle::Find(#name);                               \
        if constexpr (!map_.mapped) return ::kernel::CallGuest(ctx, SYM(name), __VA_ARGS__); \
        else return ::hle::Call(ctx, SYM(name), map_, __VA_ARGS__);                          \
    }())

// float arguments
template <class A>
inline float ArgF(const A& a, int i) {
    uint32_t v = a[i];
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline float GF(uint32_t va) {
    float f;
    std::memcpy(&f, ::kernel::HostPtr(va), 4);
    return f;
}
