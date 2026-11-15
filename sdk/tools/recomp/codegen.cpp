#include "codegen.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstdarg>

namespace fs = std::filesystem;

static std::string Fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string Fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string CodeGen::Hex(uint32_t v) const { return Fmt("0x%Xu", v); }
std::string CodeGen::FuncName(uint32_t va) const {
    auto it = named_.find(va);
    return it != named_.end() ? "sub_" + it->second : Fmt("sub_%08X", va);
}

std::string CodeGen::CType(unsigned bits, bool s) const {
    switch (bits) {
        case 8: return s ? "int8_t" : "uint8_t";
        case 16: return s ? "int16_t" : "uint16_t";
        case 64: return s ? "int64_t" : "uint64_t";
        default: return s ? "int32_t" : "uint32_t";
    }
}

static const char* Reg32Name(ZydisRegister r) {
    switch (r) {
        case ZYDIS_REGISTER_EAX: case ZYDIS_REGISTER_AX: case ZYDIS_REGISTER_AL: case ZYDIS_REGISTER_AH: return "ctx.eax";
        case ZYDIS_REGISTER_ECX: case ZYDIS_REGISTER_CX: case ZYDIS_REGISTER_CL: case ZYDIS_REGISTER_CH: return "ctx.ecx";
        case ZYDIS_REGISTER_EDX: case ZYDIS_REGISTER_DX: case ZYDIS_REGISTER_DL: case ZYDIS_REGISTER_DH: return "ctx.edx";
        case ZYDIS_REGISTER_EBX: case ZYDIS_REGISTER_BX: case ZYDIS_REGISTER_BL: case ZYDIS_REGISTER_BH: return "ctx.ebx";
        case ZYDIS_REGISTER_ESP: case ZYDIS_REGISTER_SP: return "ctx.esp";
        case ZYDIS_REGISTER_EBP: case ZYDIS_REGISTER_BP: return "ctx.ebp";
        case ZYDIS_REGISTER_ESI: case ZYDIS_REGISTER_SI: return "ctx.esi";
        case ZYDIS_REGISTER_EDI: case ZYDIS_REGISTER_DI: return "ctx.edi";
        default: return nullptr;
    }
}

static bool IsHigh8(ZydisRegister r) {
    return r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH || r == ZYDIS_REGISTER_DH || r == ZYDIS_REGISTER_BH;
}

static int RegClassIndex(ZydisRegister r, ZydisRegister first) { return int(r - first); }

std::string CodeGen::RegRead(ZydisRegister r) const {
    if (const char* n = Reg32Name(r)) {
        unsigned w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r);
        if (w == 32) return n;
        if (w == 16) return Fmt("((uint16_t)%s)", n);
        if (IsHigh8(r)) return Fmt("((uint8_t)(%s >> 8))", n);
        return Fmt("((uint8_t)%s)", n);
    }
    if (r >= ZYDIS_REGISTER_ST0 && r <= ZYDIS_REGISTER_ST7) return Fmt("X87_ST(%d)", RegClassIndex(r, ZYDIS_REGISTER_ST0));
    if (r >= ZYDIS_REGISTER_MM0 && r <= ZYDIS_REGISTER_MM7) return Fmt("ctx.mm[%d]", RegClassIndex(r, ZYDIS_REGISTER_MM0));
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM7) return Fmt("ctx.xmm[%d]", RegClassIndex(r, ZYDIS_REGISTER_XMM0));
    return "0u";
}

std::string CodeGen::RegWrite(ZydisRegister r, const std::string& v) const {
    if (const char* n = Reg32Name(r)) {
        unsigned w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r);
        if (w == 32) return Fmt("%s = (uint32_t)(%s);", n, v.c_str());
        if (w == 16) return Fmt("%s = (%s & 0xFFFF0000u) | (uint16_t)(%s);", n, n, v.c_str());
        if (IsHigh8(r)) return Fmt("%s = (%s & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);", n, n, v.c_str());
        return Fmt("%s = (%s & 0xFFFFFF00u) | (uint8_t)(%s);", n, n, v.c_str());
    }
    if (r >= ZYDIS_REGISTER_ST0 && r <= ZYDIS_REGISTER_ST7)
        return Fmt("X87_ST(%d) = %s;", RegClassIndex(r, ZYDIS_REGISTER_ST0), v.c_str());
    if (r >= ZYDIS_REGISTER_MM0 && r <= ZYDIS_REGISTER_MM7)
        return Fmt("ctx.mm[%d] = %s;", RegClassIndex(r, ZYDIS_REGISTER_MM0), v.c_str());
    return Fmt("/* write to segment or unknown register */ (void)(%s);", v.c_str());
}

std::string CodeGen::AddrExpr(const ZydisDecodedOperand& o, bool seg) const {
    std::string e;
    if (o.mem.base != ZYDIS_REGISTER_NONE) {
        const char* n = Reg32Name(o.mem.base);
        e = n ? n : "0u";
    }
    if (o.mem.index != ZYDIS_REGISTER_NONE) {
        const char* n = Reg32Name(o.mem.index);
        if (!e.empty()) e += " + ";
        e += n ? n : "0u";
        if (o.mem.scale > 1) e += Fmt(" * %uu", o.mem.scale);
    }
    uint32_t disp = uint32_t(o.mem.disp.value);
    if (o.mem.disp.has_displacement && disp) {
        if (!e.empty()) e += " + ";
        e += Hex(disp);
    }
    if (e.empty()) e = "0u";
    if (seg && o.mem.segment == ZYDIS_REGISTER_FS) e = "ctx.fs_base + " + e;
    return "(uint32_t)(" + e + ")";
}

std::string CodeGen::Mem(unsigned bits, const std::string& ea) const {
    switch (bits) {
        case 8: return "X86_MEM8(" + ea + ")";
        case 16: return "X86_MEM16(" + ea + ")";
        case 64: return "X86_MEM64(" + ea + ")";
        default: return "X86_MEM32(" + ea + ")";
    }
}

std::string CodeGen::Read(const ZydisDecodedOperand& o, const std::string& ea) const {
    switch (o.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: return RegRead(o.reg.value);
        case ZYDIS_OPERAND_TYPE_MEMORY: return Mem(o.size, ea);
        case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
            uint64_t v = o.imm.value.u;
            return Hex(uint32_t(v));
        }
        default: return "0u";
    }
}

std::string CodeGen::Write(const ZydisDecodedOperand& o, const std::string& ea, const std::string& v) const {
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) return RegWrite(o.reg.value, v);
    if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) return Mem(o.size, ea) + " = (" + CType(o.size) + ")(" + v + ");";
    return "/* bad write */";
}

std::string CodeGen::Text(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) const {
    static ZydisFormatter fmt = [] {
        ZydisFormatter f;
        ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
        return f;
    }();
    char buf[256];
    ZydisFormatterFormatInstruction(&fmt, &in, ops, in.operand_count_visible, buf, sizeof(buf), va, nullptr);
    for (char* p = buf; *p; p++)
        if (*p == '*' && p[1] == '/') *p = 'x';
    return buf;
}

void CodeGen::Unimpl(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
    std::string t = Text(va, in, ops);
    unimpl_[ZydisMnemonicGetString(in.mnemonic)]++;
    std::string esc;
    for (char c : t) esc += c == '"' ? '\'' : c;
    Line(Fmt("\tx86_trap(ctx, 0x%X, \"unimplemented: %s\");", va, esc.c_str()));
}

std::string CodeGen::Target(uint32_t t) {
    if (body_set_.count(t) && t != entry_) {
        labels_.insert(t);
        return Fmt("goto loc_%08X;", t);
    }
    if (t == entry_) {
        labels_.insert(t);
        return Fmt("goto loc_%08X;", t);
    }
    if (an_.functions.count(t)) return Fmt("{ X86_SAVE_FLAGS(); %s(ctx, base); return; }", FuncName(t).c_str());
    return Fmt("{ X86_SAVE_FLAGS(); x86_icall(ctx, base, 0x%Xu); return; }", t);
}

std::string CodeGen::Cond(ZydisMnemonic m) {
    if (pending_.valid) {
        unsigned w = pending_.bits;
        std::string U = "(" + CType(w) + ")", S = "(" + CType(w, true) + ")";
        std::string a = U + "ca", b = U + "cb", sa = S + "ca", sb = S + "cb";
        if (pending_.is_test) {
            std::string r = "(" + U + "(ca & cb))", sr = "(" + S + "(ca & cb))";
            switch (m) {
                case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: return r + " == 0";
                case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: return r + " != 0";
                case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS:
                case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return sr + " < 0";
                case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS:
                case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return sr + " >= 0";
                case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return sr + " <= 0";
                case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return sr + " > 0";
                case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: return r + " == 0";
                case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: return r + " != 0";
                default: break;
            }
        } else {
            switch (m) {
                case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: return a + " == " + b;
                case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: return a + " != " + b;
                case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB: return a + " < " + b;
                case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_CMOVNB: return a + " >= " + b;
                case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: return a + " <= " + b;
                case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: return a + " > " + b;
                case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return sa + " < " + sb;
                case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return sa + " >= " + sb;
                case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return sa + " <= " + sb;
                case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return sa + " > " + sb;
                case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS: return "(" + S + "(" + a + " - " + b + ")) < 0";
                case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS: return "(" + S + "(" + a + " - " + b + ")) >= 0";
                default: break;
            }
        }
    }
    switch (m) {
        case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_CMOVO: return "of";
        case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_CMOVNO: return "!of";
        case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB: case ZYDIS_MNEMONIC_FCMOVB: return "cf";
        case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_CMOVNB: case ZYDIS_MNEMONIC_FCMOVNB: return "!cf";
        case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ: case ZYDIS_MNEMONIC_FCMOVE: return "zf";
        case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ: case ZYDIS_MNEMONIC_CMOVNZ: case ZYDIS_MNEMONIC_FCMOVNE: return "!zf";
        case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_CMOVBE: case ZYDIS_MNEMONIC_FCMOVBE: return "(cf | zf)";
        case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_CMOVNBE: case ZYDIS_MNEMONIC_FCMOVNBE: return "!(cf | zf)";
        case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS: return "sf";
        case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_CMOVNS: return "!sf";
        case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_CMOVP: case ZYDIS_MNEMONIC_FCMOVU: return "pf";
        case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_SETNP: case ZYDIS_MNEMONIC_CMOVNP: case ZYDIS_MNEMONIC_FCMOVNU: return "!pf";
        case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL: return "(sf != of)";
        case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_CMOVNL: return "(sf == of)";
        case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE: case ZYDIS_MNEMONIC_CMOVLE: return "(zf | (sf != of))";
        case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE: case ZYDIS_MNEMONIC_CMOVNLE: return "(!zf && sf == of)";
        case ZYDIS_MNEMONIC_JCXZ: return "(uint16_t)ctx.ecx == 0";
        case ZYDIS_MNEMONIC_JECXZ: return "ctx.ecx == 0";
        default: return "false /* unknown condition */";
    }
}

void CodeGen::EmitInsn(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, uint32_t next_body) {
    insn_count_++;
    bool keep_pending = false;
    auto m = in.mnemonic;
    uint32_t next = va + in.length;

    // control flow first
    switch (m) {
        case ZYDIS_MNEMONIC_CALL: {
            if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uint32_t t = uint32_t(next + ops[0].imm.value.s);
                if (t == next) {  // call $+5 only pushes eip
                    Line(Fmt("\tX86_PUSH32(0x%Xu);", next));
                } else if (an_.functions.count(t)) {
                    Line(Fmt("\tX86_PUSH32(0x%Xu); %s(ctx, base);", next, FuncName(t).c_str()));
                } else {
                    Line(Fmt("\tX86_PUSH32(0x%Xu); x86_icall(ctx, base, 0x%Xu);", next, t));
                }
            } else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].mem.base == ZYDIS_REGISTER_NONE &&
                       ops[0].mem.index == ZYDIS_REGISTER_NONE && ops[0].mem.segment != ZYDIS_REGISTER_FS &&
                       uint32_t(ops[0].mem.disp.value) >= an_.img.kernel_thunk &&
                       uint32_t(ops[0].mem.disp.value) < an_.img.kernel_thunk + 0x400) {
                uint32_t slot = uint32_t(ops[0].mem.disp.value), ord = 0;
                an_.img.Read32(slot, ord);
                Line(Fmt("\tX86_PUSH32(0x%Xu); x86_kernel(ctx, base, %u);", next, ord & 0x7FFFFFFF));
            } else {
                std::string ea = ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? AddrExpr(ops[0]) : "";
                Line(Fmt("\t{ uint32_t t = %s; X86_PUSH32(0x%Xu); x86_icall(ctx, base, t); }", Read(ops[0], ea).c_str(), next));
            }
            pending_.valid = false;
            return;
        }
        case ZYDIS_MNEMONIC_JMP: {
            if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uint32_t t = uint32_t(next + ops[0].imm.value.s);
                // backward jumps get a compiler barrier so spin loops reread memory other threads write
                if (t <= va) Line("\tX86_BARRIER();");
                Line("\t" + Target(t));
            } else {
                auto jt = an_.jump_tables.find(va);
                std::string ea = ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? AddrExpr(ops[0]) : "";
                if (jt != an_.jump_tables.end()) {
                    Line(Fmt("\tswitch (%s) {", Read(ops[0], ea).c_str()));
                    std::set<uint32_t> done;
                    for (uint32_t t : jt->second.targets) {
                        if (!done.insert(t).second) continue;
                        Line(Fmt("\tcase 0x%Xu: %s", t, Target(t).c_str()));
                    }
                    Line(Fmt("\tdefault: { X86_SAVE_FLAGS(); x86_icall(ctx, base, %s); return; }", Read(ops[0], ea).c_str()));
                    Line("\t}");
                } else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].mem.base == ZYDIS_REGISTER_NONE &&
                           ops[0].mem.index == ZYDIS_REGISTER_NONE &&
                           uint32_t(ops[0].mem.disp.value) >= an_.img.kernel_thunk &&
                           uint32_t(ops[0].mem.disp.value) < an_.img.kernel_thunk + 0x400) {
                    uint32_t ord = 0;
                    an_.img.Read32(uint32_t(ops[0].mem.disp.value), ord);
                    Line(Fmt("\tx86_kernel(ctx, base, %u); return;", ord & 0x7FFFFFFF));
                } else {
                    Line(Fmt("\t{ uint32_t t = %s; X86_SAVE_FLAGS(); x86_icall(ctx, base, t); return; }", Read(ops[0], ea).c_str()));
                }
            }
            pending_.valid = false;
            return;
        }
        case ZYDIS_MNEMONIC_RET: {
            uint32_t n = in.operand_count_visible && ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? uint32_t(ops[0].imm.value.u) : 0;
            Line(Fmt("\tctx.esp += %u; return;", 4 + n));
            pending_.valid = false;
            return;
        }
        case ZYDIS_MNEMONIC_LOOP:
        case ZYDIS_MNEMONIC_LOOPE:
        case ZYDIS_MNEMONIC_LOOPNE: {
            uint32_t t = uint32_t(next + ops[0].imm.value.s);
            const char* extra = m == ZYDIS_MNEMONIC_LOOPE ? " && zf" : m == ZYDIS_MNEMONIC_LOOPNE ? " && !zf" : "";
            Line(Fmt("\tif (--ctx.ecx != 0%s) { X86_BARRIER(); %s }", extra, Target(t).c_str()));
            pending_.valid = false;
            return;
        }
        default:
            break;
    }
    if (in.meta.category == ZYDIS_CATEGORY_COND_BR) {
        uint32_t t = uint32_t(next + ops[0].imm.value.s);
        std::string c = Cond(m);
        Line(Fmt("\tif (%s) { %s%s }", c.c_str(), t <= va ? "X86_BARRIER(); " : "", Target(t).c_str()));
        pending_.valid = false;
        return;
    }

    bool ok = EmitInteger(va, in, ops) || EmitString(va, in, ops) || EmitX87(va, in, ops) || EmitSse(va, in, ops) ||
              EmitMmx(va, in, ops);
    if (!ok) Unimpl(va, in, ops);
    if (m == ZYDIS_MNEMONIC_CMP || m == ZYDIS_MNEMONIC_TEST) keep_pending = true;
    else if (in.cpu_flags && !(in.cpu_flags->modified | in.cpu_flags->set_0 | in.cpu_flags->set_1 | in.cpu_flags->undefined))
        keep_pending = pending_.valid;
    if (!keep_pending) pending_.valid = false;
}
