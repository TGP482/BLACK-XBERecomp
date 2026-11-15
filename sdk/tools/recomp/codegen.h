#pragma once
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "analysis.h"

class CodeGen {
public:
    CodeGen(const Analysis& an, std::filesystem::path out) : an_(an), out_(std::move(out)) {}
    bool Run();

private:
    struct Op {
        const ZydisDecodedOperand* o = nullptr;
        std::string ea;  // name of the address temp when memory
    };

    std::string EmitFunction(uint32_t entry);
    bool CheckHooks(const std::vector<uint32_t>& funcs) const;
    int RetBytes(const std::vector<uint32_t>& body) const;  // the one ret n of a body, or -1
    void EmitHooks(uint32_t va, bool after);
    void EmitInsn(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, uint32_t next_body);

    // operand helpers
    std::string RegRead(ZydisRegister r) const;
    std::string RegWrite(ZydisRegister r, const std::string& v) const;
    std::string AddrExpr(const ZydisDecodedOperand& o, bool with_segment = true) const;
    std::string Read(const ZydisDecodedOperand& o, const std::string& ea) const;
    std::string Write(const ZydisDecodedOperand& o, const std::string& ea, const std::string& v) const;
    std::string Mem(unsigned bits, const std::string& ea) const;
    std::string CType(unsigned bits, bool is_signed = false) const;

    std::string Cond(ZydisMnemonic m);
    std::string Target(uint32_t t);  // goto or tail call
    std::string Hex(uint32_t v) const;
    std::string FuncName(uint32_t va) const;
    void Line(const std::string& s) { body_ += s; body_ += '\n'; }
    void Unimpl(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    std::string Text(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) const;

    // instruction groups
    bool EmitInteger(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    bool EmitString(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    bool EmitX87(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    bool EmitSse(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);
    bool EmitMmx(uint32_t va, const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops);

    const Analysis& an_;
    std::filesystem::path out_;
    std::string body_;
    uint32_t entry_ = 0;
    std::set<uint32_t> body_set_;
    std::set<uint32_t> labels_;
    // cmp/test peephole
    struct Pending {
        bool valid = false;
        bool is_test = false;
        unsigned bits = 32;
    } pending_;
    std::map<uint32_t, std::string> named_;  // [xbe] named: the functions named for their symbol
    std::map<std::string, size_t> unimpl_;
    size_t insn_count_ = 0;
    int ret_bytes_ = -1;
};
