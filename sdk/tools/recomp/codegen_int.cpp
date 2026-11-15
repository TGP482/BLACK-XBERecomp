#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "codegen.h"

static std::string F(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string F(const char* f, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

static bool FindMem(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, int& idx) {
    for (int i = 0; i < in.operand_count_visible; i++)
        if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            idx = i;
            return true;
        }
    idx = -1;
    return false;
}

static std::string Szp(unsigned w) {
    return F("zf = r == 0; sf = (uint32_t)(r >> %u) & 1; pf = X86_PARITY(r);", w - 1);
}

static bool IsCmov(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_CMOVB: case ZYDIS_MNEMONIC_CMOVBE: case ZYDIS_MNEMONIC_CMOVL: case ZYDIS_MNEMONIC_CMOVLE:
        case ZYDIS_MNEMONIC_CMOVNB: case ZYDIS_MNEMONIC_CMOVNBE: case ZYDIS_MNEMONIC_CMOVNL: case ZYDIS_MNEMONIC_CMOVNLE:
        case ZYDIS_MNEMONIC_CMOVNO: case ZYDIS_MNEMONIC_CMOVNP: case ZYDIS_MNEMONIC_CMOVNS: case ZYDIS_MNEMONIC_CMOVNZ:
        case ZYDIS_MNEMONIC_CMOVO: case ZYDIS_MNEMONIC_CMOVP: case ZYDIS_MNEMONIC_CMOVS: case ZYDIS_MNEMONIC_CMOVZ:
            return true;
        default:
            return false;
    }
}

static bool IsSetcc(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_SETLE:
        case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_SETNLE:
        case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_SETNP: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_SETNZ:
        case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_SETZ:
            return true;
        default:
            return false;
    }
}

bool CodeGen::EmitInteger(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
    auto m = in.mnemonic;
    int mi;
    bool mem = FindMem(in, ops, mi);
    std::string ea = mem ? "ea" : "";
    std::string open = mem ? "\t{ uint32_t ea = " + AddrExpr(ops[mi]) + "; " : "\t{ ";
    bool lock = (in.attributes & ZYDIS_ATTRIB_HAS_LOCK) && mem && mi == 0;
    unsigned W = ops[0].size == 8 || ops[0].size == 16 ? ops[0].size : 32;
    std::string T = CType(W), S = CType(W, true);
    auto rd = [&](int i) { return "(" + T + ")(" + Read(ops[i], ea) + ")"; };
    auto wr = [&](int i, const std::string& v) { return Write(ops[i], ea, v); };
    auto emit = [&](const std::string& s) { Line(open + s + " }"); };

    // read modify write through an atomic compare exchange loop when locked
    auto rmw = [&](const std::string& pre, const std::string& expr, const std::string& flags) {
        if (lock) {
            emit(F("%s* p = X86_PTR(%s, ea); %s a = __atomic_load_n(p, __ATOMIC_SEQ_CST); %s %s r; "
                   "do { r = (%s)(%s); } while (!__atomic_compare_exchange_n(p, &a, r, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); %s",
                   T.c_str(), T.c_str(), T.c_str(), pre.c_str(), T.c_str(), T.c_str(), expr.c_str(), flags.c_str()));
        } else {
            emit(F("%s a = %s; %s %s r = (%s)(%s); %s %s", T.c_str(), rd(0).c_str(), pre.c_str(), T.c_str(), T.c_str(),
                   expr.c_str(), wr(0, "r").c_str(), flags.c_str()));
        }
    };

    switch (m) {
        case ZYDIS_MNEMONIC_NOP:
        case ZYDIS_MNEMONIC_PAUSE:
        case ZYDIS_MNEMONIC_PREFETCHNTA:
        case ZYDIS_MNEMONIC_PREFETCHT0:
        case ZYDIS_MNEMONIC_PREFETCHT1:
        case ZYDIS_MNEMONIC_PREFETCHT2:
        case ZYDIS_MNEMONIC_PREFETCHW:
        case ZYDIS_MNEMONIC_FNOP:
        case ZYDIS_MNEMONIC_EMMS:
        case ZYDIS_MNEMONIC_FWAIT:
            return true;
        case ZYDIS_MNEMONIC_SFENCE:
        case ZYDIS_MNEMONIC_LFENCE:
        case ZYDIS_MNEMONIC_MFENCE:
            Line("\t__atomic_thread_fence(__ATOMIC_SEQ_CST);");
            return true;

        case ZYDIS_MNEMONIC_MOV: {
            auto special = [](const ZydisDecodedOperand& o) {
                return o.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                       ((o.reg.value >= ZYDIS_REGISTER_ES && o.reg.value <= ZYDIS_REGISTER_GS) ||
                        (o.reg.value >= ZYDIS_REGISTER_CR0 && o.reg.value <= ZYDIS_REGISTER_DR15));
            };
            if (special(ops[0]) || (special(ops[1]) && ops[1].reg.value >= ZYDIS_REGISTER_CR0)) {
                Line(F("\tx86_priv(ctx, 0x%X, \"%s\");", va, ZydisMnemonicGetString(m)));
                return true;
            }
            emit(wr(0, Read(ops[1], ea)));
            return true;
        }
        case ZYDIS_MNEMONIC_MOVZX:
            emit(wr(0, "(" + CType(ops[1].size) + ")(" + Read(ops[1], ea) + ")"));
            return true;
        case ZYDIS_MNEMONIC_MOVSX:
            emit(wr(0, "(" + T + ")(" + CType(ops[1].size, true) + ")(" + Read(ops[1], ea) + ")"));
            return true;
        case ZYDIS_MNEMONIC_LEA:
            Line("\t" + RegWrite(ops[0].reg.value, AddrExpr(ops[1], false)));
            return true;
        case ZYDIS_MNEMONIC_XCHG:
            if (mem) {
                int r = mi == 0 ? 1 : 0;
                emit(F("%s old = __atomic_exchange_n(X86_PTR(%s, ea), (%s)(%s), __ATOMIC_SEQ_CST); %s", T.c_str(), T.c_str(),
                       T.c_str(), Read(ops[r], ea).c_str(), RegWrite(ops[r].reg.value, "old").c_str()));
            } else {
                emit(F("%s t = %s; %s %s", T.c_str(), rd(0).c_str(), wr(0, Read(ops[1], ea)).c_str(), wr(1, "t").c_str()));
            }
            return true;
        case ZYDIS_MNEMONIC_BSWAP:
            emit(wr(0, "__builtin_bswap32(" + Read(ops[0], ea) + ")"));
            return true;

        case ZYDIS_MNEMONIC_ADD:
            rmw(F("%s b = %s;", T.c_str(), rd(1).c_str()), "a + b",
                F("cf = r < a; of = (uint32_t)(((a ^ r) & (b ^ r)) >> %u) & 1; %s", W - 1, Szp(W).c_str()));
            return true;
        case ZYDIS_MNEMONIC_OR:
            rmw(F("%s b = %s;", T.c_str(), rd(1).c_str()), "a | b", "cf = 0; of = 0; " + Szp(W));
            return true;
        case ZYDIS_MNEMONIC_AND:
            rmw(F("%s b = %s;", T.c_str(), rd(1).c_str()), "a & b", "cf = 0; of = 0; " + Szp(W));
            return true;
        case ZYDIS_MNEMONIC_XOR:
            rmw(F("%s b = %s;", T.c_str(), rd(1).c_str()), "a ^ b", "cf = 0; of = 0; " + Szp(W));
            return true;
        case ZYDIS_MNEMONIC_SUB:
            rmw(F("%s b = %s;", T.c_str(), rd(1).c_str()), "a - b",
                F("cf = a < b; of = (uint32_t)(((a ^ b) & (a ^ r)) >> %u) & 1; %s", W - 1, Szp(W).c_str()));
            return true;
        case ZYDIS_MNEMONIC_ADC:
            rmw(F("%s b = %s; uint32_t c0 = cf;", T.c_str(), rd(1).c_str()), "a + b + c0",
                F("cf = ((uint64_t)a + b + c0) >> %u & 1; of = (uint32_t)(((a ^ r) & (b ^ r)) >> %u) & 1; %s", W, W - 1,
                  Szp(W).c_str()));
            return true;
        case ZYDIS_MNEMONIC_SBB:
            rmw(F("%s b = %s; uint32_t c0 = cf;", T.c_str(), rd(1).c_str()), "a - b - c0",
                F("cf = (uint64_t)a < (uint64_t)b + c0; of = (uint32_t)(((a ^ b) & (a ^ r)) >> %u) & 1; %s", W - 1,
                  Szp(W).c_str()));
            return true;
        case ZYDIS_MNEMONIC_CMP:
            emit(F("%s a = %s, b = %s; %s r = (%s)(a - b); ca = a; cb = b; cf = a < b; of = (uint32_t)(((a ^ b) & (a ^ r)) >> %u) & 1; %s",
                   T.c_str(), rd(0).c_str(), rd(1).c_str(), T.c_str(), T.c_str(), W - 1, Szp(W).c_str()));
            pending_ = {true, false, W};
            return true;
        case ZYDIS_MNEMONIC_TEST:
            emit(F("%s a = %s, b = %s; %s r = (%s)(a & b); ca = a; cb = b; cf = 0; of = 0; %s", T.c_str(), rd(0).c_str(),
                   rd(1).c_str(), T.c_str(), T.c_str(), Szp(W).c_str()));
            pending_ = {true, true, W};
            return true;
        case ZYDIS_MNEMONIC_INC:
            rmw("", "a + 1", F("of = r == (%s)(1u << %u); %s", T.c_str(), W - 1, Szp(W).c_str()));
            return true;
        case ZYDIS_MNEMONIC_DEC:
            rmw("", "a - 1", F("of = a == (%s)(1u << %u); %s", T.c_str(), W - 1, Szp(W).c_str()));
            return true;
        case ZYDIS_MNEMONIC_NEG:
            rmw("", "0 - a", F("cf = a != 0; of = a == (%s)(1u << %u); %s", T.c_str(), W - 1, Szp(W).c_str()));
            return true;
        case ZYDIS_MNEMONIC_NOT:
            rmw("", "~a", "");
            return true;

        case ZYDIS_MNEMONIC_SHL:
            emit(F("%s a = %s; uint32_t c = (uint32_t)(%s) & 31; if (c) { %s r = (%s)((uint32_t)a << c); "
                   "cf = c <= %u ? ((uint32_t)a >> (%u - c)) & 1 : 0; of = ((uint32_t)(r >> %u) & 1) ^ cf; %s %s }",
                   T.c_str(), rd(0).c_str(), Read(ops[1], ea).c_str(), T.c_str(), T.c_str(), W, W, W - 1,
                   Szp(W).c_str(), wr(0, "r").c_str()));
            return true;
        case ZYDIS_MNEMONIC_SHR:
            emit(F("%s a = %s; uint32_t c = (uint32_t)(%s) & 31; if (c) { %s r = (%s)((uint32_t)a >> c); "
                   "cf = c <= %u ? ((uint32_t)a >> (c - 1)) & 1 : 0; of = (uint32_t)(a >> %u) & 1; %s %s }",
                   T.c_str(), rd(0).c_str(), Read(ops[1], ea).c_str(), T.c_str(), T.c_str(), W, W - 1, Szp(W).c_str(),
                   wr(0, "r").c_str()));
            return true;
        case ZYDIS_MNEMONIC_SAR:
            emit(F("%s a = %s; uint32_t c = (uint32_t)(%s) & 31; if (c) { %s r = (%s)((int32_t)(%s)a >> c); "
                   "cf = (uint32_t)((int32_t)(%s)a >> (c - 1)) & 1; of = 0; %s %s }",
                   T.c_str(), rd(0).c_str(), Read(ops[1], ea).c_str(), T.c_str(), T.c_str(), S.c_str(), S.c_str(),
                   Szp(W).c_str(), wr(0, "r").c_str()));
            return true;
        case ZYDIS_MNEMONIC_ROL:
            emit(F("%s a = %s; uint32_t c = (uint32_t)(%s) & 31; if (c) { uint32_t n = c %% %u; "
                   "%s r = n ? (%s)(((uint32_t)a << n) | ((uint32_t)a >> (%u - n))) : a; cf = r & 1; "
                   "of = ((uint32_t)(r >> %u) & 1) ^ cf; %s }",
                   T.c_str(), rd(0).c_str(), Read(ops[1], ea).c_str(), W, T.c_str(), T.c_str(), W, W - 1,
                   wr(0, "r").c_str()));
            return true;
        case ZYDIS_MNEMONIC_ROR:
            emit(F("%s a = %s; uint32_t c = (uint32_t)(%s) & 31; if (c) { uint32_t n = c %% %u; "
                   "%s r = n ? (%s)(((uint32_t)a >> n) | ((uint32_t)a << (%u - n))) : a; cf = (uint32_t)(r >> %u) & 1; "
                   "of = ((uint32_t)(r >> %u) ^ (uint32_t)(r >> %u)) & 1; %s }",
                   T.c_str(), rd(0).c_str(), Read(ops[1], ea).c_str(), W, T.c_str(), T.c_str(), W, W - 1, W - 1, W - 2,
                   wr(0, "r").c_str()));
            return true;
        case ZYDIS_MNEMONIC_RCL:
            // OF is defined for a count of 1 only: MSB of the result xor the new CF
            emit(F("%s a = %s; uint32_t c = (uint32_t)(%s) & 31, n = c %% %u; for (; n; n--) { uint32_t hi = (uint32_t)(a >> %u) & 1; "
                   "a = (%s)(((uint32_t)a << 1) | cf); cf = hi; } if (c == 1) of = ((uint32_t)(a >> %u) & 1) ^ cf; %s",
                   T.c_str(), rd(0).c_str(), Read(ops[1], ea).c_str(), W + 1, W - 1, T.c_str(), W - 1, wr(0, "a").c_str()));
            return true;
        case ZYDIS_MNEMONIC_RCR:
            // OF for a count of 1: MSB of the operand xor the old CF, taken before the rotate
            emit(F("%s a = %s; uint32_t c = (uint32_t)(%s) & 31, n = c %% %u; if (c == 1) of = ((uint32_t)(a >> %u) & 1) ^ cf; "
                   "for (; n; n--) { uint32_t lo = a & 1; a = (%s)(((uint32_t)a >> 1) | (cf << %u)); cf = lo; } %s",
                   T.c_str(), rd(0).c_str(), Read(ops[1], ea).c_str(), W + 1, W - 1, T.c_str(), W - 1, wr(0, "a").c_str()));
            return true;
        case ZYDIS_MNEMONIC_SHLD:
            if (W != 32) return false;
            emit(F("uint32_t a = %s, b = %s; uint32_t c = (uint32_t)(%s) & 31; if (c) { uint32_t r = (uint32_t)((((uint64_t)a << 32) | b) << c >> 32); "
                   "cf = (a >> (32 - c)) & 1; of = ((r ^ a) >> 31) & 1; %s %s }",
                   Read(ops[0], ea).c_str(), Read(ops[1], ea).c_str(), Read(ops[2], ea).c_str(), Szp(32).c_str(),
                   wr(0, "r").c_str()));
            return true;
        case ZYDIS_MNEMONIC_SHRD:
            if (W != 32) return false;
            emit(F("uint32_t a = %s, b = %s; uint32_t c = (uint32_t)(%s) & 31; if (c) { uint32_t r = (uint32_t)((((uint64_t)b << 32) | a) >> c); "
                   "cf = (a >> (c - 1)) & 1; of = ((r ^ a) >> 31) & 1; %s %s }",
                   Read(ops[0], ea).c_str(), Read(ops[1], ea).c_str(), Read(ops[2], ea).c_str(), Szp(32).c_str(),
                   wr(0, "r").c_str()));
            return true;

        case ZYDIS_MNEMONIC_MUL:
        case ZYDIS_MNEMONIC_IMUL: {
            bool sgn = m == ZYDIS_MNEMONIC_IMUL;
            int vis = 0;
            for (int i = 0; i < in.operand_count_visible; i++)
                if (ops[i].visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT) vis++;
            if (vis == 1) {
                unsigned w = ops[0].size;
                if (w == 8) {
                    if (sgn)
                        emit(F("int16_t p = (int16_t)((int8_t)ctx.eax * (int8_t)%s); ctx.eax = (ctx.eax & 0xFFFF0000u) | (uint16_t)p; cf = of = p != (int8_t)p;",
                               Read(ops[0], ea).c_str()));
                    else
                        emit(F("uint16_t p = (uint16_t)((uint8_t)ctx.eax * (uint8_t)%s); ctx.eax = (ctx.eax & 0xFFFF0000u) | p; cf = of = (p >> 8) != 0;",
                               Read(ops[0], ea).c_str()));
                } else if (w == 16) {
                    if (sgn)
                        emit(F("int32_t p = (int32_t)(int16_t)ctx.eax * (int16_t)%s; ctx.eax = (ctx.eax & 0xFFFF0000u) | (uint16_t)p; "
                               "ctx.edx = (ctx.edx & 0xFFFF0000u) | (uint16_t)(p >> 16); cf = of = p != (int16_t)p;",
                               Read(ops[0], ea).c_str()));
                    else
                        emit(F("uint32_t p = (uint32_t)(uint16_t)ctx.eax * (uint16_t)%s; ctx.eax = (ctx.eax & 0xFFFF0000u) | (uint16_t)p; "
                               "ctx.edx = (ctx.edx & 0xFFFF0000u) | (uint16_t)(p >> 16); cf = of = (p >> 16) != 0;",
                               Read(ops[0], ea).c_str()));
                } else {
                    if (sgn)
                        emit(F("int64_t p = (int64_t)(int32_t)ctx.eax * (int32_t)%s; ctx.eax = (uint32_t)p; ctx.edx = (uint32_t)((uint64_t)p >> 32); cf = of = p != (int32_t)p;",
                               Read(ops[0], ea).c_str()));
                    else
                        emit(F("uint64_t p = (uint64_t)ctx.eax * (uint32_t)%s; ctx.eax = (uint32_t)p; ctx.edx = (uint32_t)(p >> 32); cf = of = ctx.edx != 0;",
                               Read(ops[0], ea).c_str()));
                }
                return true;
            }
            std::string a = vis == 3 ? Read(ops[1], ea) : Read(ops[0], ea);
            std::string b = vis == 3 ? Read(ops[2], ea) : Read(ops[1], ea);
            emit(F("int64_t p = (int64_t)(%s)(%s) * (int64_t)(%s)(%s); %s r = (%s)p; cf = of = p != (%s)r; %s", S.c_str(),
                   a.c_str(), S.c_str(), b.c_str(), T.c_str(), T.c_str(), S.c_str(), wr(0, "r").c_str()));
            return true;
        }
        case ZYDIS_MNEMONIC_DIV:
        case ZYDIS_MNEMONIC_IDIV: {
            bool sgn = m == ZYDIS_MNEMONIC_IDIV;
            unsigned w = ops[0].size;
            std::string trap = F("x86_trap(ctx, 0x%X, \"divide error\");", va);
            if (w == 8) {
                if (sgn)
                    emit(F("int16_t n = (int16_t)ctx.eax; int8_t d = (int8_t)%s; if (!d) %s int16_t q = n / d; if (q != (int8_t)q) %s "
                           "ctx.eax = (ctx.eax & 0xFFFF0000u) | (uint8_t)q | ((uint32_t)(uint8_t)(n %% d) << 8);",
                           Read(ops[0], ea).c_str(), trap.c_str(), trap.c_str()));
                else
                    emit(F("uint16_t n = (uint16_t)ctx.eax; uint8_t d = (uint8_t)%s; if (!d) %s uint16_t q = n / d; if (q > 0xFF) %s "
                           "ctx.eax = (ctx.eax & 0xFFFF0000u) | (uint8_t)q | ((uint32_t)(uint8_t)(n %% d) << 8);",
                           Read(ops[0], ea).c_str(), trap.c_str(), trap.c_str()));
            } else if (w == 16) {
                if (sgn)
                    emit(F("int32_t n = (int32_t)(((ctx.edx & 0xFFFFu) << 16) | (ctx.eax & 0xFFFFu)); int16_t d = (int16_t)%s; if (!d) %s "
                           "int32_t q = n / d; if (q != (int16_t)q) %s ctx.eax = (ctx.eax & 0xFFFF0000u) | (uint16_t)q; ctx.edx = (ctx.edx & 0xFFFF0000u) | (uint16_t)(n %% d);",
                           Read(ops[0], ea).c_str(), trap.c_str(), trap.c_str()));
                else
                    emit(F("uint32_t n = ((ctx.edx & 0xFFFFu) << 16) | (ctx.eax & 0xFFFFu); uint16_t d = (uint16_t)%s; if (!d) %s "
                           "uint32_t q = n / d; if (q > 0xFFFF) %s ctx.eax = (ctx.eax & 0xFFFF0000u) | (uint16_t)q; ctx.edx = (ctx.edx & 0xFFFF0000u) | (uint16_t)(n %% d);",
                           Read(ops[0], ea).c_str(), trap.c_str(), trap.c_str()));
            } else {
                if (sgn)
                    emit(F("int64_t n = (int64_t)(((uint64_t)ctx.edx << 32) | ctx.eax); int32_t d = (int32_t)%s; if (!d) %s "
                           "if (n == INT64_MIN && d == -1) %s int64_t q = n / d; if (q != (int32_t)q) %s ctx.eax = (uint32_t)q; ctx.edx = (uint32_t)(n %% d);",
                           Read(ops[0], ea).c_str(), trap.c_str(), trap.c_str(), trap.c_str()));
                else
                    emit(F("uint64_t n = ((uint64_t)ctx.edx << 32) | ctx.eax; uint32_t d = (uint32_t)%s; if (!d) %s "
                           "uint64_t q = n / d; if (q > 0xFFFFFFFFull) %s ctx.eax = (uint32_t)q; ctx.edx = (uint32_t)(n %% d);",
                           Read(ops[0], ea).c_str(), trap.c_str(), trap.c_str()));
            }
            return true;
        }

        case ZYDIS_MNEMONIC_XADD:
            if (lock)
                emit(F("%s b = %s; %s a = __atomic_fetch_add(X86_PTR(%s, ea), b, __ATOMIC_SEQ_CST); %s r = (%s)(a + b); %s "
                       "cf = r < a; of = (uint32_t)(((a ^ r) & (b ^ r)) >> %u) & 1; %s",
                       T.c_str(), rd(1).c_str(), T.c_str(), T.c_str(), T.c_str(), T.c_str(), wr(1, "a").c_str(), W - 1,
                       Szp(W).c_str()));
            else
                emit(F("%s a = %s, b = %s; %s r = (%s)(a + b); %s %s cf = r < a; of = (uint32_t)(((a ^ r) & (b ^ r)) >> %u) & 1; %s",
                       T.c_str(), rd(0).c_str(), rd(1).c_str(), T.c_str(), T.c_str(), wr(1, "a").c_str(), wr(0, "r").c_str(),
                       W - 1, Szp(W).c_str()));
            return true;
        case ZYDIS_MNEMONIC_CMPXCHG: {
            std::string acc = W == 8 ? "(uint8_t)ctx.eax" : W == 16 ? "(uint16_t)ctx.eax" : "ctx.eax";
            ZydisRegister accr = W == 8 ? ZYDIS_REGISTER_AL : W == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_EAX;
            std::string flags = F("%s r = (%s)(x - a); cf = x < a; of = (uint32_t)(((x ^ a) & (x ^ r)) >> %u) & 1; %s",
                                  T.c_str(), T.c_str(), W - 1, Szp(W).c_str());
            if (mem)
                emit(F("%s x = %s; %s a = x; bool ok = __atomic_compare_exchange_n(X86_PTR(%s, ea), &a, (%s)(%s), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); "
                       "if (!ok) { %s } %s",
                       T.c_str(), acc.c_str(), T.c_str(), T.c_str(), T.c_str(), Read(ops[1], ea).c_str(),
                       RegWrite(accr, "a").c_str(), flags.c_str()));
            else
                emit(F("%s x = %s; %s a = %s; if (x == a) { %s } else { %s } %s", T.c_str(), acc.c_str(), T.c_str(),
                       rd(0).c_str(), wr(0, Read(ops[1], ea)).c_str(), RegWrite(accr, "a").c_str(), flags.c_str()));
            return true;
        }
        case ZYDIS_MNEMONIC_CMPXCHG8B:
            emit("uint64_t x = ((uint64_t)ctx.edx << 32) | ctx.eax; uint64_t n = ((uint64_t)ctx.ecx << 32) | ctx.ebx; "
                 "bool ok = __atomic_compare_exchange_n(X86_PTR(uint64_t, ea), &x, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); "
                 "zf = ok; if (!ok) { ctx.eax = (uint32_t)x; ctx.edx = (uint32_t)(x >> 32); }");
            return true;

        case ZYDIS_MNEMONIC_BT:
        case ZYDIS_MNEMONIC_BTS:
        case ZYDIS_MNEMONIC_BTR:
        case ZYDIS_MNEMONIC_BTC: {
            const char* op = m == ZYDIS_MNEMONIC_BTS ? "a | bit" : m == ZYDIS_MNEMONIC_BTR ? "a & ~bit" : "a ^ bit";
            if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                std::string s = F("%s a = %s; uint32_t n = (uint32_t)(%s) & %u; %s bit = (%s)(1u << n); cf = (a >> n) & 1;",
                                  T.c_str(), rd(0).c_str(), Read(ops[1], ea).c_str(), W - 1, T.c_str(), T.c_str());
                if (m != ZYDIS_MNEMONIC_BT) s += " " + wr(0, op);
                emit(s);
                return true;
            }
            std::string addr, n;
            if (ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                addr = F("ea + (uint32_t)(((int32_t)%s >> %u) * %u)", RegRead(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, ops[1].reg.value)).c_str(),
                         W == 16 ? 4 : 5, W / 8);
                n = F("((uint32_t)%s & %u)", Read(ops[1], ea).c_str(), W - 1);
            } else {
                addr = "ea";
                n = F("((uint32_t)%s & %u)", Read(ops[1], ea).c_str(), W - 1);
            }
            std::string s = F("uint32_t a2 = %s; uint32_t n = %s; %s bit = (%s)(1u << n); ", addr.c_str(), n.c_str(),
                              T.c_str(), T.c_str());
            if (m == ZYDIS_MNEMONIC_BT) {
                s += F("cf = (%s >> n) & 1;", Mem(W, "a2").c_str());
            } else if (in.attributes & ZYDIS_ATTRIB_HAS_LOCK) {
                std::string ats = m == ZYDIS_MNEMONIC_BTS   ? std::string("__atomic_fetch_or(p, bit, __ATOMIC_SEQ_CST)")
                                  : m == ZYDIS_MNEMONIC_BTR ? F("__atomic_fetch_and(p, (%s)~bit, __ATOMIC_SEQ_CST)", T.c_str())
                                                            : std::string("__atomic_fetch_xor(p, bit, __ATOMIC_SEQ_CST)");
                s += F("%s* p = X86_PTR(%s, a2); %s a = %s; cf = (a >> n) & 1;", T.c_str(), T.c_str(), T.c_str(), ats.c_str());
            } else {
                s += F("%s a = %s; cf = (a >> n) & 1; %s = (%s)(%s);", T.c_str(), Mem(W, "a2").c_str(), Mem(W, "a2").c_str(),
                       T.c_str(), op);
            }
            emit(s);
            return true;
        }
        case ZYDIS_MNEMONIC_BSF:
            emit(F("uint32_t s = (%s)%s; zf = s == 0; if (s) { %s }", T.c_str(), Read(ops[1], ea).c_str(),
                   wr(0, "__builtin_ctz(s)").c_str()));
            return true;
        case ZYDIS_MNEMONIC_BSR:
            emit(F("uint32_t s = (%s)%s; zf = s == 0; if (s) { %s }", T.c_str(), Read(ops[1], ea).c_str(),
                   wr(0, "31 - __builtin_clz(s)").c_str()));
            return true;

        case ZYDIS_MNEMONIC_CBW:
            Line("\tctx.eax = (ctx.eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)ctx.eax;");
            return true;
        case ZYDIS_MNEMONIC_CWDE:
            Line("\tctx.eax = (uint32_t)(int32_t)(int16_t)ctx.eax;");
            return true;
        case ZYDIS_MNEMONIC_CWD:
            Line("\tctx.edx = (ctx.edx & 0xFFFF0000u) | ((ctx.eax & 0x8000u) ? 0xFFFFu : 0u);");
            return true;
        case ZYDIS_MNEMONIC_CDQ:
            Line("\tctx.edx = (uint32_t)((int32_t)ctx.eax >> 31);");
            return true;
        case ZYDIS_MNEMONIC_LAHF:
            Line("\tctx.eax = (ctx.eax & 0xFFFF00FFu) | ((sf << 7 | zf << 6 | pf << 2 | 2u | cf) << 8);");
            return true;
        case ZYDIS_MNEMONIC_SAHF:
            Line("\t{ uint32_t h = (ctx.eax >> 8) & 0xFF; sf = (h >> 7) & 1; zf = (h >> 6) & 1; pf = (h >> 2) & 1; cf = h & 1; }");
            return true;
        case ZYDIS_MNEMONIC_CLC: Line("\tcf = 0;"); return true;
        case ZYDIS_MNEMONIC_STC: Line("\tcf = 1;"); return true;
        case ZYDIS_MNEMONIC_CMC: Line("\tcf ^= 1;"); return true;
        case ZYDIS_MNEMONIC_CLD: Line("\tctx.df = 0;"); return true;
        case ZYDIS_MNEMONIC_STD: Line("\tctx.df = 1;"); return true;

        case ZYDIS_MNEMONIC_PUSH:
            if (ops[0].size == 16 && ops[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                emit(F("uint16_t v = (uint16_t)(%s); ctx.esp -= 2; X86_MEM16(ctx.esp) = v;", Read(ops[0], ea).c_str()));
            } else {
                emit(F("uint32_t v = (uint32_t)(%s); X86_PUSH32(v);", Read(ops[0], ea).c_str()));
            }
            return true;
        case ZYDIS_MNEMONIC_POP:
            if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[0].reg.value >= ZYDIS_REGISTER_ES && ops[0].reg.value <= ZYDIS_REGISTER_GS) {
                Line("\tctx.esp += 4;");
                return true;
            }
            if (mem)
                Line(F("\t{ uint32_t v = X86_MEM32(ctx.esp); ctx.esp += 4; uint32_t ea = %s; %s }", AddrExpr(ops[0]).c_str(),
                       Write(ops[0], "ea", "v").c_str()));
            else if (ops[0].size == 16)
                Line(F("\t{ uint16_t v = X86_MEM16(ctx.esp); ctx.esp += 2; %s }", RegWrite(ops[0].reg.value, "v").c_str()));
            else
                Line(F("\t{ uint32_t v = X86_MEM32(ctx.esp); ctx.esp += 4; %s }", RegWrite(ops[0].reg.value, "v").c_str()));
            return true;
        case ZYDIS_MNEMONIC_PUSHAD:
            Line("\t{ uint32_t s = ctx.esp; X86_PUSH32(ctx.eax); X86_PUSH32(ctx.ecx); X86_PUSH32(ctx.edx); X86_PUSH32(ctx.ebx); "
                 "X86_PUSH32(s); X86_PUSH32(ctx.ebp); X86_PUSH32(ctx.esi); X86_PUSH32(ctx.edi); }");
            return true;
        case ZYDIS_MNEMONIC_POPAD:
            Line("\t{ uint32_t s = ctx.esp; ctx.edi = X86_MEM32(s); ctx.esi = X86_MEM32(s + 4); ctx.ebp = X86_MEM32(s + 8); "
                 "ctx.ebx = X86_MEM32(s + 16); ctx.edx = X86_MEM32(s + 20); ctx.ecx = X86_MEM32(s + 24); ctx.eax = X86_MEM32(s + 28); ctx.esp = s + 32; }");
            return true;
        case ZYDIS_MNEMONIC_PUSHA:  // 16 bit: the low halves, the upper halves stay
            Line("\t{ uint32_t s = ctx.esp; X86_MEM16(s - 2) = (uint16_t)ctx.eax; X86_MEM16(s - 4) = (uint16_t)ctx.ecx; "
                 "X86_MEM16(s - 6) = (uint16_t)ctx.edx; X86_MEM16(s - 8) = (uint16_t)ctx.ebx; X86_MEM16(s - 10) = (uint16_t)s; "
                 "X86_MEM16(s - 12) = (uint16_t)ctx.ebp; X86_MEM16(s - 14) = (uint16_t)ctx.esi; X86_MEM16(s - 16) = (uint16_t)ctx.edi; "
                 "ctx.esp = s - 16; }");
            return true;
        case ZYDIS_MNEMONIC_POPA:
            Line("\t{ uint32_t s = ctx.esp; ctx.edi = (ctx.edi & 0xFFFF0000u) | X86_MEM16(s); ctx.esi = (ctx.esi & 0xFFFF0000u) | X86_MEM16(s + 2); "
                 "ctx.ebp = (ctx.ebp & 0xFFFF0000u) | X86_MEM16(s + 4); ctx.ebx = (ctx.ebx & 0xFFFF0000u) | X86_MEM16(s + 8); "
                 "ctx.edx = (ctx.edx & 0xFFFF0000u) | X86_MEM16(s + 10); ctx.ecx = (ctx.ecx & 0xFFFF0000u) | X86_MEM16(s + 12); "
                 "ctx.eax = (ctx.eax & 0xFFFF0000u) | X86_MEM16(s + 14); ctx.esp = s + 16; }");
            return true;
        case ZYDIS_MNEMONIC_PUSHFD:  // bit 1 always reads 1, IF (0x200) always set, AC and ID kept in xflags
            Line("\tX86_PUSH32(cf | 2u | pf << 2 | zf << 6 | sf << 7 | 0x200u | ctx.df << 10 | of << 11 | ctx.xflags);");
            return true;
        case ZYDIS_MNEMONIC_POPFD:
            Line("\t{ uint32_t f = X86_MEM32(ctx.esp); ctx.esp += 4; cf = f & 1; pf = (f >> 2) & 1; zf = (f >> 6) & 1; sf = (f >> 7) & 1; "
                 "ctx.df = (f >> 10) & 1; of = (f >> 11) & 1; ctx.xflags = f & 0x240000u; }");
            return true;
        case ZYDIS_MNEMONIC_LEAVE:
            Line("\tctx.esp = ctx.ebp; ctx.ebp = X86_MEM32(ctx.esp); ctx.esp += 4;");
            return true;
        case ZYDIS_MNEMONIC_ENTER:
            if (ops[1].imm.value.u != 0) return false;
            Line(F("\tX86_PUSH32(ctx.ebp); ctx.ebp = ctx.esp; ctx.esp -= %uu;", uint32_t(ops[0].imm.value.u)));
            return true;
        case ZYDIS_MNEMONIC_XLAT:
            Line("\tctx.eax = (ctx.eax & 0xFFFFFF00u) | X86_MEM8(ctx.ebx + (uint8_t)ctx.eax);");
            return true;

        case ZYDIS_MNEMONIC_RDTSC:
            Line("\t{ uint64_t t = x86_rdtsc(); ctx.eax = (uint32_t)t; ctx.edx = (uint32_t)(t >> 32); }");
            return true;
        case ZYDIS_MNEMONIC_CPUID:
            Line("\tx86_cpuid(ctx);");
            return true;
        case ZYDIS_MNEMONIC_INT3:
        case ZYDIS_MNEMONIC_UD2:
        case ZYDIS_MNEMONIC_HLT:
            Line(F("\tx86_trap(ctx, 0x%X, \"%s\");", va, ZydisMnemonicGetString(m)));
            return true;
        case ZYDIS_MNEMONIC_INT:
        case ZYDIS_MNEMONIC_IN:
        case ZYDIS_MNEMONIC_OUT:
        case ZYDIS_MNEMONIC_CLI:
        case ZYDIS_MNEMONIC_STI:
        case ZYDIS_MNEMONIC_WBINVD:
        case ZYDIS_MNEMONIC_INVD:
        case ZYDIS_MNEMONIC_SGDT:
        case ZYDIS_MNEMONIC_SIDT:
        case ZYDIS_MNEMONIC_LGDT:
        case ZYDIS_MNEMONIC_LIDT:
        case ZYDIS_MNEMONIC_RDMSR:
        case ZYDIS_MNEMONIC_WRMSR:
        case ZYDIS_MNEMONIC_INVLPG:
            if (m == ZYDIS_MNEMONIC_IN) Line("\tctx.eax = 0;");
            Line(F("\tx86_priv(ctx, 0x%X, \"%s\");", va, ZydisMnemonicGetString(m)));
            return true;
        default:
            break;
    }
    if (IsSetcc(m)) {
        emit(wr(0, "(" + Cond(m) + ") ? 1u : 0u"));
        return true;
    }
    if (IsCmov(m)) {
        emit(F("if (%s) { %s }", Cond(m).c_str(), wr(0, Read(ops[1], ea)).c_str()));
        return true;
    }
    return false;
}

bool CodeGen::EmitString(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
    if (in.meta.category != ZYDIS_CATEGORY_STRINGOP) return false;
    auto m = in.mnemonic;
    unsigned w = 0;
    const char* kind = nullptr;
    switch (m) {
        case ZYDIS_MNEMONIC_MOVSB: w = 1; kind = "movs"; break;
        case ZYDIS_MNEMONIC_MOVSW: w = 2; kind = "movs"; break;
        case ZYDIS_MNEMONIC_MOVSD: w = 4; kind = "movs"; break;
        case ZYDIS_MNEMONIC_STOSB: w = 1; kind = "stos"; break;
        case ZYDIS_MNEMONIC_STOSW: w = 2; kind = "stos"; break;
        case ZYDIS_MNEMONIC_STOSD: w = 4; kind = "stos"; break;
        case ZYDIS_MNEMONIC_LODSB: w = 1; kind = "lods"; break;
        case ZYDIS_MNEMONIC_LODSW: w = 2; kind = "lods"; break;
        case ZYDIS_MNEMONIC_LODSD: w = 4; kind = "lods"; break;
        case ZYDIS_MNEMONIC_CMPSB: w = 1; kind = "cmps"; break;
        case ZYDIS_MNEMONIC_CMPSW: w = 2; kind = "cmps"; break;
        case ZYDIS_MNEMONIC_CMPSD: w = 4; kind = "cmps"; break;
        case ZYDIS_MNEMONIC_SCASB: w = 1; kind = "scas"; break;
        case ZYDIS_MNEMONIC_SCASW: w = 2; kind = "scas"; break;
        case ZYDIS_MNEMONIC_SCASD: w = 4; kind = "scas"; break;
        default: return false;
    }
    bool rep = in.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
    bool repe = in.attributes & ZYDIS_ATTRIB_HAS_REPE;
    bool repne = in.attributes & ZYDIS_ATTRIB_HAS_REPNE;
    std::string T = CType(w * 8);
    std::string M = w == 1 ? "X86_MEM8" : w == 2 ? "X86_MEM16" : "X86_MEM32";
    std::string step = F("int32_t d = ctx.df ? -%u : %u;", w, w);
    std::string acc = w == 1 ? "(uint8_t)ctx.eax" : w == 2 ? "(uint16_t)ctx.eax" : "ctx.eax";
    std::string cmpflags = F("%s r = (%s)(a - b); cf = a < b; of = (uint32_t)(((a ^ b) & (a ^ r)) >> %u) & 1; "
                             "zf = r == 0; sf = (uint32_t)(r >> %u) & 1; pf = X86_PARITY(r);",
                             T.c_str(), T.c_str(), w * 8 - 1, w * 8 - 1);
    std::string s;
    if (!std::strcmp(kind, "movs")) {
        if (rep) s = F("x86_rep_movs(ctx, base, %u);", w);
        else s = F("%s %s(ctx.edi) = %s(ctx.esi); ctx.esi += d; ctx.edi += d;", step.c_str(), M.c_str(), M.c_str());
    } else if (!std::strcmp(kind, "stos")) {
        if (rep) s = F("x86_rep_stos(ctx, base, %u);", w);
        else s = F("%s %s(ctx.edi) = (%s)ctx.eax; ctx.edi += d;", step.c_str(), M.c_str(), T.c_str());
    } else if (!std::strcmp(kind, "lods")) {
        std::string wr = w == 1 ? "ctx.eax = (ctx.eax & 0xFFFFFF00u) | v;" : w == 2 ? "ctx.eax = (ctx.eax & 0xFFFF0000u) | v;" : "ctx.eax = v;";
        std::string body = F("%s v = %s(ctx.esi); %s ctx.esi += d;", T.c_str(), M.c_str(), wr.c_str());
        s = rep ? step + " while (ctx.ecx) { " + body + " ctx.ecx--; }" : step + " " + body;
    } else if (!std::strcmp(kind, "cmps")) {
        std::string body = F("%s a = %s(ctx.esi), b = %s(ctx.edi); ctx.esi += d; ctx.edi += d; %s", T.c_str(), M.c_str(),
                             M.c_str(), cmpflags.c_str());
        if (rep) s = step + " while (ctx.ecx) { " + body + " ctx.ecx--; if (" + (repne ? "zf" : "!zf") + ") break; }";
        else s = step + " " + body;
    } else {
        std::string body = F("%s a = %s, b = %s(ctx.edi); ctx.edi += d; %s", T.c_str(), acc.c_str(), M.c_str(), cmpflags.c_str());
        if (rep) s = step + " while (ctx.ecx) { " + body + " ctx.ecx--; if (" + (repne ? "zf" : "!zf") + ") break; }";
        else s = step + " " + body;
    }
    (void)repe;
    Line("\t{ " + s + " }");
    return true;
}
