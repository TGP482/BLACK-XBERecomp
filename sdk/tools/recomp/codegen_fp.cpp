#include <cstdarg>
#include <cstdio>

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

static int StIndex(const ZydisDecodedOperand& o) {
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER && o.reg.value >= ZYDIS_REGISTER_ST0 && o.reg.value <= ZYDIS_REGISTER_ST7)
        return int(o.reg.value - ZYDIS_REGISTER_ST0);
    return -1;
}

static int XmmIndex(const ZydisDecodedOperand& o) {
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER && o.reg.value >= ZYDIS_REGISTER_XMM0 && o.reg.value <= ZYDIS_REGISTER_XMM7)
        return int(o.reg.value - ZYDIS_REGISTER_XMM0);
    return -1;
}

static int MmIndex(const ZydisDecodedOperand& o) {
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER && o.reg.value >= ZYDIS_REGISTER_MM0 && o.reg.value <= ZYDIS_REGISTER_MM7)
        return int(o.reg.value - ZYDIS_REGISTER_MM0);
    return -1;
}

bool CodeGen::EmitX87(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
    if (!ZydisMnemonicGetString(in.mnemonic) || ZydisMnemonicGetString(in.mnemonic)[0] != 'f') return false;
    auto m = in.mnemonic;
    int vis = in.operand_count_visible;
    int mi = -1;
    for (int i = 0; i < vis; i++)
        if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) mi = i;
    std::string open = mi >= 0 ? "\t{ uint32_t ea = " + AddrExpr(ops[mi]) + "; " : "\t{ ";
    auto emit = [&](const std::string& s) { Line(open + s + " }"); };
    // value of a floating memory operand
    auto fmem = [&](const ZydisDecodedOperand& o) -> std::string {
        if (o.size == 32) return "(double)X86_MEMF32(ea)";
        if (o.size == 64) return "X86_MEMF64(ea)";
        return "x87_load80(X86_PTR(uint8_t, ea))";
    };
    auto imem = [&](const ZydisDecodedOperand& o) -> std::string {
        if (o.size == 16) return "(double)(int16_t)X86_MEM16(ea)";
        if (o.size == 64) return "(double)(int64_t)X86_MEM64(ea)";
        return "(double)(int32_t)X86_MEM32(ea)";
    };
    auto st = [&](int i) { return F("X87_ST(%d)", i); };

    struct Arith {
        ZydisMnemonic m;
        const char* op;
        bool rev, pop, integer;
    };
    static const Arith arith[] = {
        {ZYDIS_MNEMONIC_FADD, "+", false, false, false},   {ZYDIS_MNEMONIC_FADDP, "+", false, true, false},
        {ZYDIS_MNEMONIC_FIADD, "+", false, false, true},   {ZYDIS_MNEMONIC_FSUB, "-", false, false, false},
        {ZYDIS_MNEMONIC_FSUBP, "-", false, true, false},   {ZYDIS_MNEMONIC_FISUB, "-", false, false, true},
        {ZYDIS_MNEMONIC_FSUBR, "-", true, false, false},   {ZYDIS_MNEMONIC_FSUBRP, "-", true, true, false},
        {ZYDIS_MNEMONIC_FISUBR, "-", true, false, true},   {ZYDIS_MNEMONIC_FMUL, "*", false, false, false},
        {ZYDIS_MNEMONIC_FMULP, "*", false, true, false},   {ZYDIS_MNEMONIC_FIMUL, "*", false, false, true},
        {ZYDIS_MNEMONIC_FDIV, "/", false, false, false},   {ZYDIS_MNEMONIC_FDIVP, "/", false, true, false},
        {ZYDIS_MNEMONIC_FIDIV, "/", false, false, true},   {ZYDIS_MNEMONIC_FDIVR, "/", true, false, false},
        {ZYDIS_MNEMONIC_FDIVRP, "/", true, true, false},   {ZYDIS_MNEMONIC_FIDIVR, "/", true, false, true},
    };
    for (auto& a : arith) {
        if (a.m != m) continue;
        std::string dst, src;
        if (mi >= 0) {
            dst = st(0);
            src = a.integer ? imem(ops[mi]) : fmem(ops[mi]);
        } else if (vis >= 2) {
            dst = st(StIndex(ops[0]));
            src = st(StIndex(ops[1]));
        } else {
            dst = st(1);
            src = st(0);
        }
        std::string expr = a.rev ? "(s " + std::string(a.op) + " d)" : "(d " + std::string(a.op) + " s)";
        emit(F("double s = %s; double d = %s; %s = x87_prec(ctx, %s);%s", src.c_str(), dst.c_str(), dst.c_str(), expr.c_str(),
               a.pop ? " x87_pop(ctx);" : ""));
        return true;
    }

    switch (m) {
        case ZYDIS_MNEMONIC_FLD:
            if (mi >= 0) emit("x87_push(ctx, " + fmem(ops[mi]) + ");");
            else emit(F("double v = %s; x87_push(ctx, v);", st(StIndex(ops[0])).c_str()));
            return true;
        case ZYDIS_MNEMONIC_FILD:
            emit("x87_push(ctx, " + imem(ops[mi]) + ");");
            return true;
        case ZYDIS_MNEMONIC_FST:
        case ZYDIS_MNEMONIC_FSTP: {
            const char* pop = m == ZYDIS_MNEMONIC_FSTP ? " x87_pop(ctx);" : "";
            if (mi >= 0) {
                auto& o = ops[mi];
                if (o.size == 32) emit(F("X86_MEMF32(ea) = (float)X87_ST(0);%s", pop));
                else if (o.size == 64) emit(F("X86_MEMF64(ea) = X87_ST(0);%s", pop));
                else emit(F("x87_store80(X86_PTR(uint8_t, ea), X87_ST(0));%s", pop));
            } else {
                emit(F("%s = X87_ST(0);%s", st(StIndex(ops[0])).c_str(), pop));
            }
            return true;
        }
        case ZYDIS_MNEMONIC_FIST:
        case ZYDIS_MNEMONIC_FISTP:
        case ZYDIS_MNEMONIC_FISTTP: {
            const char* pop = m != ZYDIS_MNEMONIC_FIST ? " x87_pop(ctx);" : "";
            auto& o = ops[mi];
            std::string v = m == ZYDIS_MNEMONIC_FISTTP ? "std::trunc(X87_ST(0))" : "X87_ST(0)";
            // fcw 0xC00 sets rounding to truncate for the store
            std::string pre = m == ZYDIS_MNEMONIC_FISTTP ? "uint16_t cw = ctx.fcw; ctx.fcw |= 0xC00; " : "";
            std::string post = m == ZYDIS_MNEMONIC_FISTTP ? " ctx.fcw = cw;" : "";
            if (o.size == 16) emit(pre + "X86_MEM16(ea) = (uint16_t)x87_to_i16(ctx, " + v + ");" + post + pop);
            else if (o.size == 64) emit(pre + "X86_MEM64(ea) = (uint64_t)x87_to_i64(ctx, " + v + ");" + post + pop);
            else emit(pre + "X86_MEM32(ea) = (uint32_t)x87_to_i32(ctx, " + v + ");" + post + pop);
            return true;
        }
        case ZYDIS_MNEMONIC_FXCH: {
            int i = vis ? StIndex(ops[0]) : 1;
            emit(F("double t = X87_ST(0); X87_ST(0) = %s; %s = t;", st(i).c_str(), st(i).c_str()));
            return true;
        }
        case ZYDIS_MNEMONIC_FCOM:
        case ZYDIS_MNEMONIC_FCOMP:
        case ZYDIS_MNEMONIC_FUCOM:
        case ZYDIS_MNEMONIC_FUCOMP:
        case ZYDIS_MNEMONIC_FICOM:
        case ZYDIS_MNEMONIC_FICOMP: {
            bool pop = m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP || m == ZYDIS_MNEMONIC_FICOMP;
            bool integer = m == ZYDIS_MNEMONIC_FICOM || m == ZYDIS_MNEMONIC_FICOMP;
            std::string src = mi >= 0 ? (integer ? imem(ops[mi]) : fmem(ops[mi])) : vis ? st(StIndex(ops[0])) : st(1);
            emit(F("x87_compare(ctx, X87_ST(0), %s);%s", src.c_str(), pop ? " x87_pop(ctx);" : ""));
            return true;
        }
        case ZYDIS_MNEMONIC_FCOMPP:
        case ZYDIS_MNEMONIC_FUCOMPP:
            emit("x87_compare(ctx, X87_ST(0), X87_ST(1)); x87_pop(ctx); x87_pop(ctx);");
            return true;
        case ZYDIS_MNEMONIC_FCOMI:
        case ZYDIS_MNEMONIC_FCOMIP:
        case ZYDIS_MNEMONIC_FUCOMI:
        case ZYDIS_MNEMONIC_FUCOMIP: {
            bool pop = m == ZYDIS_MNEMONIC_FCOMIP || m == ZYDIS_MNEMONIC_FUCOMIP;
            emit(F("double a = X87_ST(0), b = %s; bool u = a != a || b != b; zf = u || a == b; pf = u; cf = u || a < b; of = 0; sf = 0;%s",
                   st(StIndex(ops[1])).c_str(), pop ? " x87_pop(ctx);" : ""));
            return true;
        }
        case ZYDIS_MNEMONIC_FTST:
            emit("x87_compare(ctx, X87_ST(0), 0.0);");
            return true;
        case ZYDIS_MNEMONIC_FXAM:
            emit("x87_examine(ctx);");
            return true;
        case ZYDIS_MNEMONIC_FCHS: emit("X87_ST(0) = -X87_ST(0);"); return true;
        case ZYDIS_MNEMONIC_FABS: emit("X87_ST(0) = std::fabs(X87_ST(0));"); return true;
        case ZYDIS_MNEMONIC_FSQRT: emit("X87_ST(0) = x87_prec(ctx, std::sqrt(X87_ST(0)));"); return true;
        // clearing C2 (0x400) reports the argument in range
        case ZYDIS_MNEMONIC_FSIN: emit("X87_ST(0) = std::sin(X87_ST(0)); ctx.fsw &= ~0x400;"); return true;
        case ZYDIS_MNEMONIC_FCOS: emit("X87_ST(0) = std::cos(X87_ST(0)); ctx.fsw &= ~0x400;"); return true;
        case ZYDIS_MNEMONIC_FSINCOS:
            emit("double v = X87_ST(0); X87_ST(0) = std::sin(v); x87_push(ctx, std::cos(v)); ctx.fsw &= ~0x400;");
            return true;
        case ZYDIS_MNEMONIC_FPTAN:
            emit("X87_ST(0) = std::tan(X87_ST(0)); x87_push(ctx, 1.0); ctx.fsw &= ~0x400;");
            return true;
        case ZYDIS_MNEMONIC_FPATAN:
            emit("double v = std::atan2(X87_ST(1), X87_ST(0)); x87_pop(ctx); X87_ST(0) = v;");
            return true;
        case ZYDIS_MNEMONIC_F2XM1: emit("X87_ST(0) = std::exp2(X87_ST(0)) - 1.0;"); return true;
        case ZYDIS_MNEMONIC_FYL2X:
            emit("double v = X87_ST(1) * std::log2(X87_ST(0)); x87_pop(ctx); X87_ST(0) = v;");
            return true;
        case ZYDIS_MNEMONIC_FYL2XP1:
            emit("double v = X87_ST(1) * std::log2(X87_ST(0) + 1.0); x87_pop(ctx); X87_ST(0) = v;");
            return true;
        case ZYDIS_MNEMONIC_FSCALE:
            emit("X87_ST(0) = std::ldexp(X87_ST(0), (int)std::trunc(X87_ST(1)));");
            return true;
        case ZYDIS_MNEMONIC_FRNDINT: emit("X87_ST(0) = x87_round(ctx, X87_ST(0));"); return true;
        case ZYDIS_MNEMONIC_FPREM:
            emit("X87_ST(0) = std::fmod(X87_ST(0), X87_ST(1)); ctx.fsw &= ~0x400;");
            return true;
        case ZYDIS_MNEMONIC_FPREM1:
            emit("X87_ST(0) = std::remainder(X87_ST(0), X87_ST(1)); ctx.fsw &= ~0x400;");
            return true;
        case ZYDIS_MNEMONIC_FXTRACT:
            // zero: exponent -inf (the zero divide exception is masked); infinity: exponent +inf; NaN: NaN in both
            emit("int e; double v = X87_ST(0); double mnt = std::frexp(v, &e); "
                 "X87_ST(0) = v == 0.0 ? -INFINITY : std::isfinite(v) ? (double)(e - 1) : std::fabs(v); "
                 "x87_push(ctx, std::isfinite(v) ? mnt * 2.0 : v);");
            return true;
        case ZYDIS_MNEMONIC_FLD1: emit("x87_push(ctx, 1.0);"); return true;
        case ZYDIS_MNEMONIC_FLDZ: emit("x87_push(ctx, 0.0);"); return true;
        case ZYDIS_MNEMONIC_FLDPI: emit("x87_push(ctx, 3.14159265358979323846);"); return true;
        case ZYDIS_MNEMONIC_FLDL2E: emit("x87_push(ctx, 1.44269504088896340736);"); return true;
        case ZYDIS_MNEMONIC_FLDL2T: emit("x87_push(ctx, 3.32192809488736234787);"); return true;
        case ZYDIS_MNEMONIC_FLDLN2: emit("x87_push(ctx, 0.693147180559945309417);"); return true;
        case ZYDIS_MNEMONIC_FLDLG2: emit("x87_push(ctx, 0.301029995663981195214);"); return true;
        case ZYDIS_MNEMONIC_FLDCW: emit("ctx.fcw = X86_MEM16(ea);"); return true;
        case ZYDIS_MNEMONIC_FNSTCW: emit("X86_MEM16(ea) = ctx.fcw;"); return true;
        case ZYDIS_MNEMONIC_FNSTSW:
            if (mi >= 0) emit("X86_MEM16(ea) = x87_status(ctx);");
            else emit("ctx.eax = (ctx.eax & 0xFFFF0000u) | x87_status(ctx);");
            return true;
        case ZYDIS_MNEMONIC_FNINIT: emit("ctx.fcw = 0x37F; ctx.fsw = 0; ctx.top = 0;"); return true;  // 0x37F is the power on control word
        case ZYDIS_MNEMONIC_FNCLEX: emit("ctx.fsw &= 0x7F00;"); return true;  // keeps condition codes and TOP
        case ZYDIS_MNEMONIC_FFREE: return true;
        case ZYDIS_MNEMONIC_FINCSTP: emit("ctx.top = (ctx.top + 1) & 7;"); return true;
        case ZYDIS_MNEMONIC_FDECSTP: emit("ctx.top = (ctx.top - 1) & 7;"); return true;
        case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVBE: case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVNB:
        case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNE: case ZYDIS_MNEMONIC_FCMOVNU: case ZYDIS_MNEMONIC_FCMOVU: {
            bool saved = pending_.valid;
            pending_.valid = false;
            std::string c = Cond(m);
            pending_.valid = saved;
            emit(F("if (%s) X87_ST(0) = %s;", c.c_str(), st(StIndex(ops[1])).c_str()));
            return true;
        }
        case ZYDIS_MNEMONIC_FNSTENV:
            emit("X86_MEM32(ea) = ctx.fcw; X86_MEM32(ea + 4) = x87_status(ctx); X86_MEM32(ea + 8) = 0xFFFF;");
            return true;
        case ZYDIS_MNEMONIC_FNSAVE: emit("x87_save(ctx, X86_PTR(uint8_t, ea));"); return true;
        case ZYDIS_MNEMONIC_FLDENV:
            emit("ctx.fcw = (uint16_t)X86_MEM32(ea); ctx.fsw = (uint16_t)X86_MEM32(ea + 4); ctx.top = (ctx.fsw >> 11) & 7;");
            return true;
        case ZYDIS_MNEMONIC_FRSTOR: emit("x87_restore(ctx, X86_PTR(uint8_t, ea));"); return true;
        default:
            return false;
    }
}

bool CodeGen::EmitSse(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
    auto m = in.mnemonic;
    int mi = -1;
    for (int i = 0; i < in.operand_count_visible; i++)
        if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) mi = i;
    std::string open = mi >= 0 ? "\t{ uint32_t ea = " + AddrExpr(ops[mi]) + "; " : "\t{ ";
    auto emit = [&](const std::string& s) { Line(open + s + " }"); };
    int d = XmmIndex(ops[0]);
    int s = in.operand_count_visible > 1 ? XmmIndex(ops[1]) : -1;
    // 128 bit source as an X86Xmm expression
    auto src128 = [&]() -> std::string {
        if (s >= 0) return F("ctx.xmm[%d]", s);
        return "*X86_PTR(X86Xmm, ea)";
    };
    auto srcf0 = [&]() -> std::string {
        if (s >= 0) return F("ctx.xmm[%d].f[0]", s);
        return "X86_MEMF32(ea)";
    };
    struct Bin {
        ZydisMnemonic ps, ss;
        const char* expr;
    };
    static const Bin bins[] = {
        {ZYDIS_MNEMONIC_ADDPS, ZYDIS_MNEMONIC_ADDSS, "a + b"},
        {ZYDIS_MNEMONIC_SUBPS, ZYDIS_MNEMONIC_SUBSS, "a - b"},
        {ZYDIS_MNEMONIC_MULPS, ZYDIS_MNEMONIC_MULSS, "a * b"},
        {ZYDIS_MNEMONIC_DIVPS, ZYDIS_MNEMONIC_DIVSS, "a / b"},
        {ZYDIS_MNEMONIC_MINPS, ZYDIS_MNEMONIC_MINSS, "sse_min(a, b)"},
        {ZYDIS_MNEMONIC_MAXPS, ZYDIS_MNEMONIC_MAXSS, "sse_max(a, b)"},
        {ZYDIS_MNEMONIC_SQRTPS, ZYDIS_MNEMONIC_SQRTSS, "std::sqrt(b)"},
        {ZYDIS_MNEMONIC_RCPPS, ZYDIS_MNEMONIC_RCPSS, "1.0f / b"},
        {ZYDIS_MNEMONIC_RSQRTPS, ZYDIS_MNEMONIC_RSQRTSS, "1.0f / std::sqrt(b)"},
    };
    for (auto& b : bins) {
        if (m == b.ps) {
            emit(F("X86Xmm v = %s; for (int i = 0; i < 4; i++) { float a = ctx.xmm[%d].f[i], b = v.f[i]; (void)a; ctx.xmm[%d].f[i] = %s; }",
                   src128().c_str(), d, d, b.expr));
            return true;
        }
        if (m == b.ss) {
            emit(F("float a = ctx.xmm[%d].f[0], b = %s; (void)a; ctx.xmm[%d].f[0] = %s;", d, srcf0().c_str(), d, b.expr));
            return true;
        }
    }
    switch (m) {
        case ZYDIS_MNEMONIC_MOVAPS:
        case ZYDIS_MNEMONIC_MOVUPS:
        case ZYDIS_MNEMONIC_MOVNTPS:
        case ZYDIS_MNEMONIC_MOVAPD:
        case ZYDIS_MNEMONIC_MOVUPD:
        case ZYDIS_MNEMONIC_MOVDQA:
        case ZYDIS_MNEMONIC_MOVDQU:
            if (d >= 0) emit(F("ctx.xmm[%d] = %s;", d, src128().c_str()));
            else emit(F("*X86_PTR(X86Xmm, ea) = ctx.xmm[%d];", s));
            return true;
        case ZYDIS_MNEMONIC_MOVSS:
            if (d >= 0 && s >= 0) emit(F("ctx.xmm[%d].f[0] = ctx.xmm[%d].f[0];", d, s));
            else if (d >= 0) emit(F("ctx.xmm[%d].u[0] = X86_MEM32(ea); ctx.xmm[%d].u[1] = ctx.xmm[%d].u[2] = ctx.xmm[%d].u[3] = 0;", d, d, d, d));
            else emit(F("X86_MEM32(ea) = ctx.xmm[%d].u[0];", s));
            return true;
        case ZYDIS_MNEMONIC_MOVLPS:
            if (d >= 0) emit(F("ctx.xmm[%d].q[0] = X86_MEM64(ea);", d));
            else emit(F("X86_MEM64(ea) = ctx.xmm[%d].q[0];", s));
            return true;
        case ZYDIS_MNEMONIC_MOVHPS:
            if (d >= 0) emit(F("ctx.xmm[%d].q[1] = X86_MEM64(ea);", d));
            else emit(F("X86_MEM64(ea) = ctx.xmm[%d].q[1];", s));
            return true;
        case ZYDIS_MNEMONIC_MOVLHPS: emit(F("ctx.xmm[%d].q[1] = ctx.xmm[%d].q[0];", d, s)); return true;
        case ZYDIS_MNEMONIC_MOVHLPS: emit(F("ctx.xmm[%d].q[0] = ctx.xmm[%d].q[1];", d, s)); return true;
        case ZYDIS_MNEMONIC_ANDPS:
        case ZYDIS_MNEMONIC_ANDNPS:
        case ZYDIS_MNEMONIC_ORPS:
        case ZYDIS_MNEMONIC_XORPS: {
            const char* e = m == ZYDIS_MNEMONIC_ANDPS ? "a & b" : m == ZYDIS_MNEMONIC_ANDNPS ? "~a & b" : m == ZYDIS_MNEMONIC_ORPS ? "a | b" : "a ^ b";
            emit(F("X86Xmm v = %s; for (int i = 0; i < 4; i++) { uint32_t a = ctx.xmm[%d].u[i], b = v.u[i]; ctx.xmm[%d].u[i] = %s; }",
                   src128().c_str(), d, d, e));
            return true;
        }
        case ZYDIS_MNEMONIC_CMPPS:
            emit(F("X86Xmm v = %s; for (int i = 0; i < 4; i++) ctx.xmm[%d].u[i] = sse_cmp(ctx.xmm[%d].f[i], v.f[i], %d);",
                   src128().c_str(), d, d, int(ops[2].imm.value.u)));
            return true;
        case ZYDIS_MNEMONIC_CMPSS:
            emit(F("ctx.xmm[%d].u[0] = sse_cmp(ctx.xmm[%d].f[0], %s, %d);", d, d, srcf0().c_str(), int(ops[2].imm.value.u)));
            return true;
        case ZYDIS_MNEMONIC_COMISS:
        case ZYDIS_MNEMONIC_UCOMISS:
            emit(F("float a = ctx.xmm[%d].f[0], b = %s; bool u = a != a || b != b; zf = u || a == b; pf = u; cf = u || a < b; of = 0; sf = 0;",
                   d, srcf0().c_str()));
            return true;
        case ZYDIS_MNEMONIC_CVTSI2SS:
            emit(F("ctx.xmm[%d].f[0] = (float)(int32_t)%s;", d, Read(ops[1], "ea").c_str()));
            return true;
        case ZYDIS_MNEMONIC_CVTSS2SI:
            emit(RegWrite(ops[0].reg.value, F("(uint32_t)sse_cvt(%s)", s >= 0 ? F("ctx.xmm[%d].f[0]", s).c_str() : "X86_MEMF32(ea)")));
            return true;
        case ZYDIS_MNEMONIC_CVTTSS2SI:
            emit(RegWrite(ops[0].reg.value, F("(uint32_t)sse_cvtt(%s)", s >= 0 ? F("ctx.xmm[%d].f[0]", s).c_str() : "X86_MEMF32(ea)")));
            return true;
        case ZYDIS_MNEMONIC_CVTPI2PS: {
            std::string src = MmIndex(ops[1]) >= 0 ? F("ctx.mm[%d]", MmIndex(ops[1])) : "X86_MEM64(ea)";
            emit(F("uint64_t v = %s; ctx.xmm[%d].f[0] = (float)(int32_t)(uint32_t)v; ctx.xmm[%d].f[1] = (float)(int32_t)(uint32_t)(v >> 32);",
                   src.c_str(), d, d));
            return true;
        }
        case ZYDIS_MNEMONIC_CVTPS2PI:
        case ZYDIS_MNEMONIC_CVTTPS2PI: {
            const char* fn = m == ZYDIS_MNEMONIC_CVTPS2PI ? "sse_cvt" : "sse_cvtt";
            std::string src = s >= 0 ? F("ctx.xmm[%d]", s) : "*X86_PTR(X86Xmm, ea)";
            emit(F("X86Xmm v = %s; ctx.mm[%d] = (uint64_t)(uint32_t)%s(v.f[0]) | ((uint64_t)(uint32_t)%s(v.f[1]) << 32);",
                   src.c_str(), MmIndex(ops[0]), fn, fn));
            return true;
        }
        case ZYDIS_MNEMONIC_SHUFPS: {
            int imm = int(ops[2].imm.value.u);
            emit(F("X86Xmm a = ctx.xmm[%d], b = %s; ctx.xmm[%d].u[0] = a.u[%d]; ctx.xmm[%d].u[1] = a.u[%d]; ctx.xmm[%d].u[2] = b.u[%d]; ctx.xmm[%d].u[3] = b.u[%d];",
                   d, src128().c_str(), d, imm & 3, d, (imm >> 2) & 3, d, (imm >> 4) & 3, d, (imm >> 6) & 3));
            return true;
        }
        case ZYDIS_MNEMONIC_UNPCKLPS:
            emit(F("X86Xmm a = ctx.xmm[%d], b = %s; ctx.xmm[%d].u[0] = a.u[0]; ctx.xmm[%d].u[1] = b.u[0]; ctx.xmm[%d].u[2] = a.u[1]; ctx.xmm[%d].u[3] = b.u[1];",
                   d, src128().c_str(), d, d, d, d));
            return true;
        case ZYDIS_MNEMONIC_UNPCKHPS:
            emit(F("X86Xmm a = ctx.xmm[%d], b = %s; ctx.xmm[%d].u[0] = a.u[2]; ctx.xmm[%d].u[1] = b.u[2]; ctx.xmm[%d].u[2] = a.u[3]; ctx.xmm[%d].u[3] = b.u[3];",
                   d, src128().c_str(), d, d, d, d));
            return true;
        case ZYDIS_MNEMONIC_MOVMSKPS:
            emit(RegWrite(ops[0].reg.value, F("(ctx.xmm[%d].u[0] >> 31) | (ctx.xmm[%d].u[1] >> 31) << 1 | (ctx.xmm[%d].u[2] >> 31) << 2 | (ctx.xmm[%d].u[3] >> 31) << 3", s, s, s, s)));
            return true;
        case ZYDIS_MNEMONIC_LDMXCSR: emit("ctx.mxcsr = X86_MEM32(ea);"); return true;
        case ZYDIS_MNEMONIC_STMXCSR: emit("X86_MEM32(ea) = ctx.mxcsr;"); return true;
        default:
            return false;
    }
}

bool CodeGen::EmitMmx(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
    auto m = in.mnemonic;
    int mi = -1;
    for (int i = 0; i < in.operand_count_visible; i++)
        if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) mi = i;
    std::string open = mi >= 0 ? "\t{ uint32_t ea = " + AddrExpr(ops[mi]) + "; " : "\t{ ";
    auto emit = [&](const std::string& s) { Line(open + s + " }"); };
    int d = MmIndex(ops[0]);
    int s = in.operand_count_visible > 1 ? MmIndex(ops[1]) : -1;
    if (d < 0 && s < 0 && m != ZYDIS_MNEMONIC_MOVD && m != ZYDIS_MNEMONIC_MOVQ) return false;
    std::string src = s >= 0 ? F("ctx.mm[%d]", s) : mi >= 0 ? "X86_MEM64(ea)" : "0ull";
    if (in.operand_count_visible > 1 && ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) src = F("%lluull", (unsigned long long)ops[1].imm.value.u);
    std::string D = d >= 0 ? F("ctx.mm[%d]", d) : "";

    struct Lane {
        ZydisMnemonic m;
        const char* type;
        const char* expr;
    };
    static const Lane lanes[] = {
        {ZYDIS_MNEMONIC_PADDB, "uint8_t", "a + b"},
        {ZYDIS_MNEMONIC_PADDW, "uint16_t", "a + b"},
        {ZYDIS_MNEMONIC_PADDD, "uint32_t", "a + b"},
        {ZYDIS_MNEMONIC_PADDSB, "int8_t", "sat<int8_t>((int)a + b)"},
        {ZYDIS_MNEMONIC_PADDSW, "int16_t", "sat<int16_t>((int)a + b)"},
        {ZYDIS_MNEMONIC_PADDUSB, "uint8_t", "sat<uint8_t>((int)a + b)"},
        {ZYDIS_MNEMONIC_PADDUSW, "uint16_t", "sat<uint16_t>((int)a + b)"},
        {ZYDIS_MNEMONIC_PSUBB, "uint8_t", "a - b"},
        {ZYDIS_MNEMONIC_PSUBW, "uint16_t", "a - b"},
        {ZYDIS_MNEMONIC_PSUBD, "uint32_t", "a - b"},
        {ZYDIS_MNEMONIC_PSUBSB, "int8_t", "sat<int8_t>((int)a - b)"},
        {ZYDIS_MNEMONIC_PSUBSW, "int16_t", "sat<int16_t>((int)a - b)"},
        {ZYDIS_MNEMONIC_PSUBUSB, "uint8_t", "sat<uint8_t>((int)a - b)"},
        {ZYDIS_MNEMONIC_PSUBUSW, "uint16_t", "sat<uint16_t>((int)a - b)"},
        {ZYDIS_MNEMONIC_PMULLW, "int16_t", "(int16_t)((int)a * b)"},
        {ZYDIS_MNEMONIC_PMULHW, "int16_t", "(int16_t)(((int)a * b) >> 16)"},
        {ZYDIS_MNEMONIC_PMULHUW, "uint16_t", "(uint16_t)(((uint32_t)a * b) >> 16)"},
        {ZYDIS_MNEMONIC_PCMPEQB, "uint8_t", "a == b ? 0xFF : 0"},
        {ZYDIS_MNEMONIC_PCMPEQW, "uint16_t", "a == b ? 0xFFFF : 0"},
        {ZYDIS_MNEMONIC_PCMPEQD, "uint32_t", "a == b ? 0xFFFFFFFFu : 0"},
        {ZYDIS_MNEMONIC_PCMPGTB, "int8_t", "a > b ? -1 : 0"},
        {ZYDIS_MNEMONIC_PCMPGTW, "int16_t", "a > b ? -1 : 0"},
        {ZYDIS_MNEMONIC_PCMPGTD, "int32_t", "a > b ? -1 : 0"},
        {ZYDIS_MNEMONIC_PAVGB, "uint8_t", "(a + b + 1) >> 1"},
        {ZYDIS_MNEMONIC_PAVGW, "uint16_t", "(a + b + 1) >> 1"},
        {ZYDIS_MNEMONIC_PMAXUB, "uint8_t", "a > b ? a : b"},
        {ZYDIS_MNEMONIC_PMINUB, "uint8_t", "a < b ? a : b"},
        {ZYDIS_MNEMONIC_PMAXSW, "int16_t", "a > b ? a : b"},
        {ZYDIS_MNEMONIC_PMINSW, "int16_t", "a < b ? a : b"},
    };
    for (auto& l : lanes)
        if (l.m == m) {
            emit(F("%s = mmx_map2<%s>(%s, %s, [](%s a, %s b) { return %s; });", D.c_str(), l.type, D.c_str(), src.c_str(),
                   l.type, l.type, l.expr));
            return true;
        }
    auto shift = [&](const char* type, const char* op, int bits) {
        bool arith = type[0] == 'i';
        if (arith)
            emit(F("uint64_t n = %s; if (n > %d) n = %d; %s = mmx_map2<%s>(%s, 0, [n](%s a, %s) { return (%s)(a >> n); });",
                   src.c_str(), bits - 1, bits - 1, D.c_str(), type, D.c_str(), type, type, type));
        else
            emit(F("uint64_t n = %s; %s = n >= %d ? 0 : mmx_map2<%s>(%s, 0, [n](%s a, %s) { return (%s)(a %s n); });", src.c_str(),
                   D.c_str(), bits, type, D.c_str(), type, type, type, op));
    };
    switch (m) {
        case ZYDIS_MNEMONIC_MOVQ:
            if (d >= 0) emit(F("%s = %s;", D.c_str(), src.c_str()));
            else emit(F("X86_MEM64(ea) = ctx.mm[%d];", s));
            return true;
        case ZYDIS_MNEMONIC_MOVNTQ:
            emit(F("X86_MEM64(ea) = ctx.mm[%d];", s));
            return true;
        case ZYDIS_MNEMONIC_MOVD:
            if (d >= 0) emit(F("%s = (uint32_t)(%s);", D.c_str(), Read(ops[1], "ea").c_str()));
            else emit(Write(ops[0], "ea", F("(uint32_t)ctx.mm[%d]", s)));
            return true;
        case ZYDIS_MNEMONIC_PAND: emit(F("%s &= %s;", D.c_str(), src.c_str())); return true;
        case ZYDIS_MNEMONIC_PANDN: emit(F("%s = ~%s & %s;", D.c_str(), D.c_str(), src.c_str())); return true;
        case ZYDIS_MNEMONIC_POR: emit(F("%s |= %s;", D.c_str(), src.c_str())); return true;
        case ZYDIS_MNEMONIC_PXOR: emit(F("%s ^= %s;", D.c_str(), src.c_str())); return true;
        case ZYDIS_MNEMONIC_PSLLW: shift("uint16_t", "<<", 16); return true;
        case ZYDIS_MNEMONIC_PSLLD: shift("uint32_t", "<<", 32); return true;
        case ZYDIS_MNEMONIC_PSRLW: shift("uint16_t", ">>", 16); return true;
        case ZYDIS_MNEMONIC_PSRLD: shift("uint32_t", ">>", 32); return true;
        case ZYDIS_MNEMONIC_PSRAW: shift("int16_t", ">>", 16); return true;
        case ZYDIS_MNEMONIC_PSRAD: shift("int32_t", ">>", 32); return true;
        case ZYDIS_MNEMONIC_PSLLQ: emit(F("uint64_t n = %s; %s = n >= 64 ? 0 : %s << n;", src.c_str(), D.c_str(), D.c_str())); return true;
        case ZYDIS_MNEMONIC_PSRLQ: emit(F("uint64_t n = %s; %s = n >= 64 ? 0 : %s >> n;", src.c_str(), D.c_str(), D.c_str())); return true;
        case ZYDIS_MNEMONIC_PMADDWD:
            emit(F("int16_t a[4], b[4]; uint64_t x = %s, y = %s; std::memcpy(a, &x, 8); std::memcpy(b, &y, 8); "
                   "int32_t r0 = a[0] * b[0] + a[1] * b[1], r1 = a[2] * b[2] + a[3] * b[3]; %s = (uint64_t)(uint32_t)r0 | ((uint64_t)(uint32_t)r1 << 32);",
                   D.c_str(), src.c_str(), D.c_str()));
            return true;
        case ZYDIS_MNEMONIC_PSADBW:
            emit(F("uint8_t a[8], b[8]; uint64_t x = %s, y = %s; std::memcpy(a, &x, 8); std::memcpy(b, &y, 8); uint32_t s = 0; "
                   "for (int i = 0; i < 8; i++) s += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]; %s = s;",
                   D.c_str(), src.c_str(), D.c_str()));
            return true;
        case ZYDIS_MNEMONIC_PUNPCKLBW:
        case ZYDIS_MNEMONIC_PUNPCKHBW:
        case ZYDIS_MNEMONIC_PUNPCKLWD:
        case ZYDIS_MNEMONIC_PUNPCKHWD:
        case ZYDIS_MNEMONIC_PUNPCKLDQ:
        case ZYDIS_MNEMONIC_PUNPCKHDQ: {
            int sz = (m == ZYDIS_MNEMONIC_PUNPCKLBW || m == ZYDIS_MNEMONIC_PUNPCKHBW) ? 1
                     : (m == ZYDIS_MNEMONIC_PUNPCKLWD || m == ZYDIS_MNEMONIC_PUNPCKHWD) ? 2 : 4;
            bool hi = m == ZYDIS_MNEMONIC_PUNPCKHBW || m == ZYDIS_MNEMONIC_PUNPCKHWD || m == ZYDIS_MNEMONIC_PUNPCKHDQ;
            emit(F("uint8_t a[8], b[8], r[8]; uint64_t x = %s, y = %s; std::memcpy(a, &x, 8); std::memcpy(b, &y, 8); "
                   "for (int i = 0; i < %d; i++) { std::memcpy(r + i * %d, a + %d + i * %d, %d); std::memcpy(r + i * %d + %d, b + %d + i * %d, %d); } "
                   "std::memcpy(&%s, r, 8);",
                   D.c_str(), src.c_str(), 4 / sz, 2 * sz, hi ? 4 : 0, sz, sz, 2 * sz, sz, hi ? 4 : 0, sz, sz, D.c_str()));
            return true;
        }
        case ZYDIS_MNEMONIC_PACKSSWB:
        case ZYDIS_MNEMONIC_PACKUSWB:
            emit(F("int16_t a[4], b[4]; %s r[8]; uint64_t x = %s, y = %s; std::memcpy(a, &x, 8); std::memcpy(b, &y, 8); "
                   "for (int i = 0; i < 4; i++) { r[i] = sat<%s>(a[i]); r[i + 4] = sat<%s>(b[i]); } std::memcpy(&%s, r, 8);",
                   m == ZYDIS_MNEMONIC_PACKSSWB ? "int8_t" : "uint8_t", D.c_str(), src.c_str(),
                   m == ZYDIS_MNEMONIC_PACKSSWB ? "int8_t" : "uint8_t", m == ZYDIS_MNEMONIC_PACKSSWB ? "int8_t" : "uint8_t", D.c_str()));
            return true;
        case ZYDIS_MNEMONIC_PACKSSDW:
            emit(F("int32_t a[2], b[2]; int16_t r[4]; uint64_t x = %s, y = %s; std::memcpy(a, &x, 8); std::memcpy(b, &y, 8); "
                   "r[0] = sat<int16_t>(a[0]); r[1] = sat<int16_t>(a[1]); r[2] = sat<int16_t>(b[0]); r[3] = sat<int16_t>(b[1]); std::memcpy(&%s, r, 8);",
                   D.c_str(), src.c_str(), D.c_str()));
            return true;
        case ZYDIS_MNEMONIC_PSHUFW: {
            int imm = int(ops[2].imm.value.u);
            emit(F("uint16_t a[4], r[4]; uint64_t x = %s; std::memcpy(a, &x, 8); r[0] = a[%d]; r[1] = a[%d]; r[2] = a[%d]; r[3] = a[%d]; std::memcpy(&%s, r, 8);",
                   src.c_str(), imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3, D.c_str()));
            return true;
        }
        case ZYDIS_MNEMONIC_PEXTRW:
            emit(RegWrite(ops[0].reg.value, F("(uint16_t)(ctx.mm[%d] >> (16 * (%d & 3)))", s, int(ops[2].imm.value.u))));
            return true;
        case ZYDIS_MNEMONIC_PINSRW: {
            int sh = 16 * (int(ops[2].imm.value.u) & 3);
            emit(F("%s = (%s & ~(0xFFFFull << %d)) | ((uint64_t)(uint16_t)(%s) << %d);", D.c_str(), D.c_str(), sh,
                   Read(ops[1], "ea").c_str(), sh));
            return true;
        }
        case ZYDIS_MNEMONIC_PMOVMSKB:
            emit(RegWrite(ops[0].reg.value, F("[&] { uint32_t r = 0; for (int i = 0; i < 8; i++) r |= ((ctx.mm[%d] >> (i * 8 + 7)) & 1) << i; return r; }()", s)));
            return true;
        default:
            return false;
    }
}
