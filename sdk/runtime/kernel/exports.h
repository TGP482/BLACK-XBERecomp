#pragma once
#include "kernel/kernel.h"

// stdcall export: the body returns eax, the wrapper pops the arguments
#define KERNEL_EXPORT(ord, name, nargs)                                                                   \
    static uint32_t kimpl_##name(X86Context& ctx, const ::kernel::Args& a);                              \
    static void kentry_##name(X86Context& ctx, uint8_t*) {                                               \
        ::kernel::Args args{ctx};                                                                        \
        uint32_t r = kimpl_##name(ctx, args);                                                            \
        ::kernel::Return(ctx, r, nargs);                                                                 \
    }                                                                                                    \
    static const bool kreg_##name = (::kernel::RegisterExport(ord, #name, kentry_##name), true);         \
    static uint32_t kimpl_##name([[maybe_unused]] X86Context& ctx, [[maybe_unused]] const ::kernel::Args& a)

// fastcall export: arguments in ecx and edx, no stack arguments
#define KERNEL_FASTCALL(ord, name)                                                                        \
    static uint32_t kimpl_##name(X86Context& ctx);                                                       \
    static void kentry_##name(X86Context& ctx, uint8_t*) {                                               \
        uint32_t r = kimpl_##name(ctx);                                                                  \
        ::kernel::Return(ctx, r, 0);                                                                     \
    }                                                                                                    \
    static const bool kreg_##name = (::kernel::RegisterExport(ord, #name, kentry_##name), true);         \
    static uint32_t kimpl_##name([[maybe_unused]] X86Context& ctx)

// kernel variable exported by address
#define KERNEL_DATA(ord, name, size) \
    static const uint32_t kdata_##name = ::kernel::RegisterData(ord, #name, size)
