#pragma once
#include <cstdint>

union X86Xmm {
    float f[4];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
};

struct alignas(64) X86Context {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t cf, zf, sf, of, pf;  // only live across tail calls, functions keep flags in locals
    uint32_t df;
    uint32_t xflags;  // AC and ID, kept so feature probes see them toggle
    uint32_t fs_base;
    uint32_t top;  // x87 stack top
    uint16_t fcw, fsw;
    uint32_t mxcsr;
    double st[8];
    uint64_t mm[8];
    X86Xmm xmm[8];
    // runtime owned
    uint32_t thread_handle;
    uint32_t last_icall;
    void* host;
};

using X86Func = void(X86Context& __restrict ctx, uint8_t* base);

struct X86FuncMapping {
    uint32_t guest;
    X86Func* host;
};

// [xbe] named: a [symbols], [names] or [data] name and its address in this image
struct X86Symbol {
    const char* name;
    uint32_t va;
};

// produced by the recompiler, exported by the game code DLL
extern "C" const X86FuncMapping X86FuncMappings[];
extern "C" const X86Symbol X86Symbols[];  // [xbe] named, ends with a null name
extern "C" const uint32_t X86Continuations[];  // zero terminated
extern "C" const uint32_t X86CodeBase, X86CodeEnd, X86ImageBase, X86ImageEnd, X86EntryPoint, X86KernelThunk;
