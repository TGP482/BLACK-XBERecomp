#include <Zydis/Zydis.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xbe/xbe.h"

namespace fs = std::filesystem;

namespace {

// Signatures and fingerprints

constexpr uint64_t kMul = 0x100000001B3ull;  // FNV 64 bit prime, the rolling hash base

// operand values taken as image addresses; one range for every title so masks agree
bool IsAddr(uint64_t v) { return v >= 0x10000 && v < 0x4000000; }

// fingerprint constants below kSmall or from kNegative up are frame and counter noise
constexpr uint32_t kSmall = 0x40;
constexpr uint32_t kNegative = 0xFFFFFF00;
// a match by constants needs this many in common, this score, and this lead over every rival
constexpr size_t kMinConstants = 3;
constexpr double kAccept = 0.6;
constexpr double kLead = 0.2;
const char* const kRegs[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
constexpr int kRegCount = int(std::size(kRegs));

uint64_t Pow(uint32_t n) {
    static std::vector<uint64_t> p{1};
    while (p.size() <= n) p.push_back(p.back() * kMul);
    return p[n];
}

std::string Hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", (unsigned long long)v);
    return b;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

struct Sig {
    uint32_t len = 0;
    uint64_t hash = 0;
    std::vector<Sig> calls;  // callees, two levels deep
    bool leaf = true;        // callees not recorded
    bool deep = false;       // lookalikes exist, always check callees' callees
    std::string sec;         // library section, candidates elsewhere are skipped
    uint32_t count = 1;
    uint32_t nth = 0;
    uint32_t xtlid = 0;      // .XTLID export id, used when the bytes match nothing
    std::vector<uint32_t> k;    // constants, sorted
    std::vector<uint32_t> ret;  // stack bytes each ret pops
    uint8_t regs = 0;           // registers read before written, bit per kRegs
    bool shape = false;         // ret and regs are known
};

std::string Regs(uint8_t m) {
    std::string r;
    for (int i = 0; i < kRegCount; i++)
        if (m >> i & 1) r += (r.empty() ? "" : " ") + std::string(kRegs[i]);
    return r;
}

// share of the constants two functions have in common, repeats counted
double Similar(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t i = 0;
    size_t j = 0;
    size_t both = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) {
            i++;
        } else if (b[j] < a[i]) {
            j++;
        } else {
            both++;
            i++;
            j++;
        }
    }
    return both < kMinConstants ? 0 : double(both) / double(a.size() + b.size() - both);
}

struct DataRef {
    std::string in;
    uint32_t insn = 0;
    bool imm = false;
    uint32_t xtlid = 0;  // .XTLID export id, used when no function has it
    std::vector<uint32_t> deref = {};  // dwords read in turn at these offsets from the operand: data behind a table
};

// Sweep and hashing

constexpr uint16_t kMasked = 0x100;  // above every byte value
constexpr size_t kMaxInsnLen = 15;   // longest x86 instruction
constexpr uint32_t kAlign = 16;      // function alignment the linker pads to

// XBE with a masked linear sweep of its code sections
struct Image {
    struct Code {
        std::string name;
        uint32_t va = 0;
        std::vector<uint8_t> b;      // section bytes
        std::vector<uint16_t> m;     // byte, or 0x100 where an address or rel32 target sits
        std::vector<uint8_t> start;  // 1 where the sweep decoded an instruction start
        std::vector<uint64_t> h;     // prefix hashes of m, h[i] covers m[0, i)
    };
    xbe::Image x;
    std::vector<Code> code;
    std::vector<uint32_t> calls;  // rel32 call targets, sorted and unique after Sweep
    std::set<uint32_t> labels;  // targets of conditional and short jumps, inside functions
    ZydisDecoder dec;

    Image() { ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32); }

    bool Load(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::string err;
        if (f && x.Parse(std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}), err)) return true;
        std::fprintf(stderr, "%s: %s\n", p.string().c_str(), f ? err.c_str() : "cannot open");
        return false;
    }

    const Code* CodeAt(uint32_t va) const {
        for (auto& c : code)
            if (va >= c.va && va - c.va < c.b.size()) return &c;
        return nullptr;
    }

    bool DecodeAt(const Code& c, size_t o, ZydisDecodedInstruction& in) const {
        ZydisDecoderContext ctx;
        size_t avail = std::min<size_t>(kMaxInsnLen, c.b.size() - o);
        return ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&dec, &ctx, c.b.data() + o, avail, &in));
    }

    bool Decode(uint32_t va, ZydisDecodedInstruction& in) const {
        auto c = CodeAt(va);
        if (!c) return false;
        return DecodeAt(*c, va - c->va, in);
    }

    static uint32_t Target(uint32_t va, const ZydisDecodedInstruction& in) {
        return uint32_t(va + in.length + in.raw.imm[0].value.s);
    }
    static bool Rel32(const ZydisDecodedInstruction& in) {
        return in.raw.imm[0].is_relative && in.raw.imm[0].size == 32;
    }

    // a return, int3 or hlt: the path goes no further
    static bool Stops(const ZydisDecodedInstruction& in) {
        return in.meta.category == ZYDIS_CATEGORY_RET || in.mnemonic == ZYDIS_MNEMONIC_INT3 ||
               in.mnemonic == ZYDIS_MNEMONIC_HLT;
    }

    // decodes one instruction into the sweep, returns its length
    uint32_t SweepInstruction(Code& c, size_t o) {
        ZydisDecodedInstruction in;
        c.start[o] = 1;
        // zero fill between library data and code would swallow the next function's first bytes
        if (!c.b[o] || !DecodeAt(c, o, in)) {
            c.m[o] = c.b[o];
            return 1;
        }
        for (uint32_t i = 0; i < in.length; i++) {
            c.m[o + i] = c.b[o + i];
            if (i) c.start[o + i] = 0;
        }
        MaskAddresses(c, o, in);
        NoteBranch(c, o, in);
        return in.length;
    }

    void MaskAddresses(Code& c, size_t o, const ZydisDecodedInstruction& in) {
        auto mask = [&](uint32_t off) {
            for (uint32_t i = 0; i < 4; i++) c.m[o + off + i] = kMasked;
        };
        if (in.raw.disp.size == 32 && IsAddr(uint32_t(in.raw.disp.value))) mask(in.raw.disp.offset);
        for (auto& im : in.raw.imm)
            if (im.size == 32 && (im.is_relative || IsAddr(uint32_t(im.value.u)))) mask(im.offset);
    }

    // records rel32 call targets and conditional or short jump targets
    void NoteBranch(const Code& c, size_t o, const ZydisDecodedInstruction& in) {
        uint32_t va = c.va + uint32_t(o);
        if (in.meta.category == ZYDIS_CATEGORY_CALL && Rel32(in)) {
            calls.push_back(Target(va, in));
            return;
        }
        bool cond = in.meta.category == ZYDIS_CATEGORY_COND_BR;
        if (in.raw.imm[0].is_relative && (cond || in.raw.imm[0].size == 8)) labels.insert(Target(va, in));
    }

    // sweeps the named sections, then restarts decoding at call targets the sweep ran past
    void Sweep(const std::vector<std::string>& names) {
        for (auto& s : x.sections) {
            if (std::find(names.begin(), names.end(), s.name) == names.end()) continue;
            code.push_back(SweepSection(s));
        }
        FollowCalls();
        std::sort(calls.begin(), calls.end());
        calls.erase(std::unique(calls.begin(), calls.end()), calls.end());
        HashMasks();
    }

    Code SweepSection(const xbe::Section& s) {
        Code c;
        c.name = s.name;
        c.va = s.va;
        uint32_t n = std::min(s.vsize, s.rsize);
        c.b.assign(x.file.begin() + s.raw, x.file.begin() + s.raw + n);
        c.m.resize(n);
        c.start.resize(n);
        for (size_t o = 0; o < n;) o += SweepInstruction(c, o);
        return c;
    }

    void FollowCalls() {
        for (uint32_t va : std::vector<uint32_t>(calls)) {
            auto c = const_cast<Code*>(CodeAt(va));
            if (!c || c->start[va - c->va]) continue;
            size_t o = va - c->va;
            // stops once decoding falls back in step with an instruction start the sweep already has
            do o += SweepInstruction(*c, o);
            while (o < c->b.size() && !c->start[o]);
        }
    }

    // prefix hashes of the masked bytes
    void HashMasks() {
        for (auto& c : code) {
            c.h.resize(c.m.size() + 1);
            // + 1 so zero bytes still change the hash
            for (size_t i = 0; i < c.m.size(); i++) c.h[i + 1] = c.h[i] * kMul + c.m[i] + 1;
        }
    }

    bool IsInstructionStart(uint32_t va) const {
        auto c = CodeAt(va);
        return c && c->start[va - c->va];
    }

    std::optional<uint64_t> Hash(uint32_t va, uint32_t len) const {
        auto c = CodeAt(va);
        if (!c || !len || va - c->va + uint64_t(len) > c->m.size()) return {};
        size_t a = va - c->va;
        return c->h[a + len] - c->h[a] * Pow(len);
    }

    // function extent: reachable code from va, branches followed while below bound
    uint32_t Extent(uint32_t va, uint32_t bound) const {
        std::vector<uint32_t> work{va};
        std::set<uint32_t> seen;
        uint32_t end = va;
        while (!work.empty()) {
            uint32_t p = work.back();
            work.pop_back();
            ZydisDecodedInstruction in;
            while (p >= va && p < bound && seen.insert(p).second && Decode(p, in)) {
                uint32_t q = p + in.length;
                end = std::max(end, q);
                auto cat = in.meta.category;
                if (Stops(in)) break;
                if ((cat == ZYDIS_CATEGORY_UNCOND_BR || cat == ZYDIS_CATEGORY_COND_BR) && in.raw.imm[0].is_relative)
                    work.push_back(Target(p, in));
                if (cat == ZYDIS_CATEGORY_UNCOND_BR) break;
                p = q;
            }
        }
        return end - va;
    }

    template <class F>
    void Walk(uint32_t va, uint32_t len, F f) const {
        ZydisDecodedInstruction in;
        uint32_t i = 0;
        for (uint32_t p = va; p < va + len && Decode(p, in); p += in.length) {
            f(p, i, in);
            i++;
        }
    }

    // rel32 call and jump targets leaving [va, va + len), in order
    std::vector<uint32_t> ExternalTargets(uint32_t va, uint32_t len) const {
        std::vector<uint32_t> r;
        Walk(va, len, [&](uint32_t p, uint32_t, const ZydisDecodedInstruction& in) {
            if (!Rel32(in)) return;
            uint32_t t = Target(p, in);
            if (t < va || t >= va + len) r.push_back(t);
        });
        return r;
    }

    // constants, ret sizes and entry registers of the code reachable inside [va, va + len)
    void Trace(uint32_t va, uint32_t len, Sig& s) const {
        s.shape = true;
        auto c = CodeAt(va);
        std::map<uint32_t, int> seen;  // registers written on every path to an instruction
        std::vector<std::pair<uint32_t, int>> work{{va, 0}};
        while (c && !work.empty()) {
            auto [p, wr] = work.back();
            work.pop_back();
            ZydisDecodedInstruction in;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            while (p >= va && p - va < len) {
                auto it = seen.find(p);
                bool first = it == seen.end();
                // every register written on all earlier paths here is written on this one too: nothing new
                if (!first && !(it->second & ~wr)) break;
                if (!first) wr &= it->second;
                seen[p] = wr;
                size_t o = p - c->va;
                size_t avail = std::min<size_t>(kMaxInsnLen, c->b.size() - o);
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, c->b.data() + o, avail, &in, ops))) break;
                auto cat = in.meta.category;
                int rd = 0;
                int wt = 0;
                bool stack = OperandRegisters(in, ops, rd, wt);
                // clearing a register and saving one on the stack do not read it
                if (ReadsNothing(in, ops)) rd = 0;
                s.regs |= uint8_t(rd & ~wr);
                wr |= wt;
                if (cat == ZYDIS_CATEGORY_CALL) wr |= 7;  // eax ecx edx
                bool rel = in.raw.imm[0].is_relative;
                if (first && !stack && !rel) AddConstants(*c, in, s);
                if (cat == ZYDIS_CATEGORY_RET) AddRet(in, s);
                if (Stops(in)) break;
                if (cat == ZYDIS_CATEGORY_COND_BR && rel) work.push_back({Target(p, in), wr});
                if (cat == ZYDIS_CATEGORY_UNCOND_BR) {
                    if (!rel) break;
                    p = Target(p, in);
                } else {
                    p += in.length;
                }
            }
        }
        std::sort(s.k.begin(), s.k.end());
        std::sort(s.ret.begin(), s.ret.end());
    }

    // bit per kRegs, 0 for esp and registers outside eax to edi
    static int RegBit(ZydisRegister r) {
        r = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
        bool general = r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI && r != ZYDIS_REGISTER_ESP;
        return general ? 1 << (r - ZYDIS_REGISTER_EAX) : 0;
    }

    // registers the operands read and write; true when the instruction uses the stack
    static bool OperandRegisters(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, int& rd, int& wt) {
        // pushed arguments, stack slots and the frame size change with inlining and the callee's convention
        bool stack = ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[0].reg.value == ZYDIS_REGISTER_ESP;
        for (uint32_t i = 0; i < in.operand_count; i++) {
            auto& op = ops[i];
            if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                if (op.actions & ZYDIS_OPERAND_ACTION_MASK_READ) rd |= RegBit(op.reg.value);
                if (op.actions & ZYDIS_OPERAND_ACTION_WRITE) wt |= RegBit(op.reg.value);
            } else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                rd |= RegBit(op.mem.base) | RegBit(op.mem.index);
                stack |= op.mem.base == ZYDIS_REGISTER_ESP;
            }
        }
        return stack;
    }

    static bool ReadsNothing(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
        bool same = in.operand_count_visible == 2 && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[0].reg.value == ops[1].reg.value;
        bool clear = in.mnemonic == ZYDIS_MNEMONIC_XOR || in.mnemonic == ZYDIS_MNEMONIC_SUB ||
                     in.mnemonic == ZYDIS_MNEMONIC_SBB;
        return (same && clear) || (in.mnemonic == ZYDIS_MNEMONIC_PUSH && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER);
    }

    void AddConstants(const Code& c, const ZydisDecodedInstruction& in, Sig& s) const {
        if (in.raw.disp.size) AddConstant(c, uint64_t(in.raw.disp.value), s);
        for (auto& im : in.raw.imm)
            if (im.size) AddConstant(c, im.value.u, s);
    }

    // a library section does not point into .text, so an address there is a constant such as a GPU command
    void AddConstant(const Code& c, uint64_t v64, Sig& s) const {
        uint32_t v = uint32_t(v64);
        auto at = IsAddr(v) ? x.SectionAt(v) : nullptr;
        bool text_in_library = c.name != ".text" && at && at->name == ".text";
        if (v >= kSmall && v < kNegative && (!IsAddr(v) || text_in_library)) s.k.push_back(v);
    }

    static void AddRet(const ZydisDecodedInstruction& in, Sig& s) {
        uint32_t n = in.raw.imm[0].size ? uint32_t(in.raw.imm[0].value.u) : 0;
        if (std::find(s.ret.begin(), s.ret.end(), n) == s.ret.end()) s.ret.push_back(n);
    }

    // likely function starts: call targets, and code after padding, a return or a jump that is aligned or
    // pointed at and that no conditional or short jump leads to
    std::vector<uint32_t> Starts() const {
        std::vector<uint32_t> r;
        for (uint32_t va : calls)
            if (IsInstructionStart(va)) r.push_back(va);
        for (auto& s : x.sections) {
            uint32_t n = std::min(s.vsize, s.rsize);
            for (uint32_t o = 0; o + 4 <= n; o++) {
                uint32_t v = x.U32(s.raw + o);
                if (StartsAfterEnd(v)) r.push_back(v);
            }
        }
        for (auto& c : code)
            for (uint32_t va = (c.va + kAlign - 1) & ~(kAlign - 1); va - c.va < c.b.size(); va += kAlign)
                if (StartsAfterEnd(va)) r.push_back(va);
        std::sort(r.begin(), r.end());
        r.erase(std::unique(r.begin(), r.end()), r.end());
        return r;
    }

    // a boundary right after a return, jump or padding that no label points at, or a section start
    bool StartsAfterEnd(uint32_t va) const {
        auto c = CodeAt(va);
        if (!c) return false;
        size_t o = va - c->va;
        if (!o) return true;
        if (!c->start[o] || labels.count(va)) return false;
        size_t p = o;
        do p--;
        while (p && !c->start[p]);
        ZydisDecodedInstruction in;
        if (!Decode(c->va + uint32_t(p), in) || p + in.length != o) return false;
        return in.meta.category == ZYDIS_CATEGORY_RET || in.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
               in.mnemonic == ZYDIS_MNEMONIC_INT3 || in.mnemonic == ZYDIS_MNEMONIC_NOP;
    }

    // deep also checks the callees' own callees
    bool CallsMatch(uint32_t va, const Sig& s, bool deep) const {
        auto out = ExternalTargets(va, s.len);
        if (out.size() != s.calls.size()) return false;
        for (size_t i = 0; i < out.size(); i++) {
            auto& c = s.calls[i];
            if (!c.len) continue;
            if (Hash(out[i], c.len) != c.hash) return false;
            if (deep && !c.leaf && !CallsMatch(out[i], c, false)) return false;
        }
        return true;
    }

    // every boundary whose masked bytes and callees match, per signature
    std::vector<std::vector<uint32_t>> Find(const std::vector<const Sig*>& sigs, bool refine = true) const {
        std::map<uint32_t, std::unordered_map<uint64_t, std::vector<size_t>>> by_len;
        for (size_t i = 0; i < sigs.size(); i++)
            if (sigs[i]->len) by_len[sigs[i]->len][sigs[i]->hash].push_back(i);
        std::vector<std::vector<uint32_t>> r(sigs.size());
        for (auto& c : code)
            for (size_t o = 0; o < c.m.size(); o++)
                if (c.start[o]) FindAt(c, o, by_len, sigs, r);
        if (refine) KeepDeepMatches(sigs, r);
        return r;
    }

    void FindAt(const Code& c, size_t o,
                const std::map<uint32_t, std::unordered_map<uint64_t, std::vector<size_t>>>& by_len,
                const std::vector<const Sig*>& sigs, std::vector<std::vector<uint32_t>>& r) const {
        uint32_t va = c.va + uint32_t(o);
        for (auto& [len, idx] : by_len) {
            // by_len is sorted ascending, so every later length runs past the end too
            if (o + len > c.m.size()) break;
            auto it = idx.find(c.h[o + len] - c.h[o] * Pow(len));
            if (it == idx.end()) continue;
            for (size_t i : it->second) {
                bool in_section = sigs[i]->sec.empty() || sigs[i]->sec == c.name;
                if (in_section && CallsMatch(va, *sigs[i], false)) r[i].push_back(va);
            }
        }
    }

    // several hits or known lookalikes: keep the hits whose callees' callees match too
    void KeepDeepMatches(const std::vector<const Sig*>& sigs, std::vector<std::vector<uint32_t>>& r) const {
        for (size_t i = 0; i < sigs.size(); i++) {
            if (r[i].size() <= 1 && !sigs[i]->deep) continue;
            std::vector<uint32_t> deep;
            for (uint32_t va : r[i])
                if (CallsMatch(va, *sigs[i], true)) deep.push_back(va);
            // when no hit passes, the shallow hits stay unless the signature is deep
            if (!deep.empty() || sigs[i]->deep) r[i] = deep;
        }
    }

    std::optional<uint32_t> Operand(uint32_t va, uint32_t len, const DataRef& d) const {
        std::optional<uint32_t> r;
        Walk(va, len, [&](uint32_t, uint32_t i, const ZydisDecodedInstruction& in) {
            if (i != d.insn) return;
            if (d.imm) {
                for (auto& im : in.raw.imm)
                    if (im.size == 32 && !im.is_relative) r = uint32_t(im.value.u);
            } else if (in.raw.disp.size == 32) {
                r = uint32_t(in.raw.disp.value);
            }
        });
        return r;
    }

    // .XTLID: export id and address pairs
    std::vector<std::pair<uint32_t, uint32_t>> Exports() const {
        constexpr uint32_t kEntryBytes = 8;    // id then address
        constexpr uint32_t kAddressOffset = 4;
        std::vector<std::pair<uint32_t, uint32_t>> r;
        for (auto& s : x.sections)
            for (uint32_t o = 0; s.name == ".XTLID" && o + kEntryBytes <= std::min(s.vsize, s.rsize); o += kEntryBytes)
                r.push_back({x.U32(s.raw + o), x.U32(s.raw + o + kAddressOffset)});
        return r;
    }
};

// Signature file

struct SigFile {
    std::map<std::string, Sig> symbols;
    std::map<std::string, Sig> anchors;
    std::map<std::string, DataRef> data;
    std::set<std::string> names;  // every [symbols] name, with or without a signature
};

// callee: [len, hash] or [len, hash, [callees]]
void ReadCalls(const toml::array& a, Sig& s) {
    for (auto& e : a) {
        auto c = e.as_array();
        if (!c || c->size() < 2) continue;
        Sig k;
        k.len = uint32_t(c->get(0)->value_or(int64_t(0)));
        k.hash = std::stoull(c->get(1)->value_or(std::string("0")), nullptr, 0);
        if (c->size() > 2 && c->get(2)->as_array()) {
            k.leaf = false;
            ReadCalls(*c->get(2)->as_array(), k);
        }
        s.calls.push_back(k);
    }
}

Sig ReadSig(const toml::table& t) {
    Sig s;
    s.len = uint32_t(t["len"].value_or(int64_t(0)));
    s.hash = std::stoull(t["hash"].value_or(std::string("0")), nullptr, 0);
    s.count = uint32_t(t["count"].value_or(int64_t(1)));
    s.nth = uint32_t(t["nth"].value_or(int64_t(0)));
    s.leaf = false;
    s.deep = t["deep"].value_or(false);
    s.sec = t["sec"].value_or(std::string());
    s.xtlid = uint32_t(t["xtlid"].value_or(int64_t(0)));
    if (auto a = t["calls"].as_array()) ReadCalls(*a, s);
    if (auto a = t["k"].as_array())
        for (auto& e : *a) s.k.push_back(uint32_t(e.value_or(int64_t(0))));
    if (auto a = t["ret"].as_array())
        for (auto& e : *a) s.ret.push_back(uint32_t(e.value_or(int64_t(0))));
    else if (auto v = t["ret"].value<int64_t>())
        s.ret.push_back(uint32_t(*v));
    if (auto a = t["regs"].as_array()) {
        s.shape = true;
        for (auto& e : *a)
            for (int i = 0; i < kRegCount; i++)
                if (e.value_or(std::string()) == kRegs[i]) s.regs |= uint8_t(1 << i);
    }
    return s;
}

std::string WriteCalls(const Sig& s) {
    std::string r = "[";
    for (size_t i = 0; i < s.calls.size(); i++) {
        auto& c = s.calls[i];
        r += (i ? ", [" : "[") + Hex(c.len) + ", \"" + Hex(c.hash) + "\"";
        if (!c.leaf) r += ", " + WriteCalls(c);
        r += "]";
    }
    return r + "]";
}

std::string WriteSig(const Sig& s) {
    if (!s.len) return s.xtlid ? "{xtlid = " + Hex(s.xtlid) + "}" : "{}";
    std::string r = "{sec = \"" + s.sec + "\", len = " + Hex(s.len) + ", hash = \"" + Hex(s.hash) + "\"";
    if (s.xtlid) r += ", xtlid = " + Hex(s.xtlid);
    if (s.count > 1) r += ", count = " + std::to_string(s.count) + ", nth = " + std::to_string(s.nth);
    if (s.deep) r += ", deep = true";
    if (!s.calls.empty()) r += ", calls = " + WriteCalls(s);
    auto list = [&](const char* key, const std::vector<uint32_t>& v, bool hex) {
        if (v.empty()) return;
        r += std::string(", ") + key + " = " + (hex || v.size() > 1 ? "[" : "");
        for (size_t i = 0; i < v.size(); i++) r += (i ? ", " : "") + (hex ? Hex(v[i]) : std::to_string(v[i]));
        r += hex || v.size() > 1 ? "]" : "";
    };
    list("k", s.k, true);
    list("ret", s.ret, false);
    if (s.shape) {
        r += ", regs = [";
        int n = 0;
        for (int i = 0; i < kRegCount; i++)
            if (s.regs >> i & 1) r += std::string(n++ ? ", \"" : "\"") + kRegs[i] + "\"";
        r += "]";
    }
    return r + "}";
}

// the parsed file, or nothing after printing why it does not parse
std::optional<toml::table> ParseToml(const fs::path& p) {
    try {
        return toml::parse_file(p.string());
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "%s: %s\n", p.string().c_str(), std::string(e.description()).c_str());
        return std::nullopt;
    }
}

bool LoadSigs(const fs::path& p, SigFile& f) {
    std::optional<toml::table> parsed = ParseToml(p);
    if (!parsed) return false;
    toml::table& t = *parsed;
    for (auto [key, out] : {std::pair{"symbols", &f.symbols}, std::pair{"anchors", &f.anchors}})
        if (auto s = t[key].as_table())
            for (auto& [k, v] : *s) {
                (*out)[std::string(k.str())] = v.as_table() ? ReadSig(*v.as_table()) : Sig{};
                if (out == &f.symbols) f.names.insert(std::string(k.str()));
            }
    if (auto s = t["data"].as_table())
        for (auto& [k, v] : *s) {
            DataRef d;
            if (auto e = v.as_table()) {
                d.in = (*e)["in"].value_or(std::string());
                d.insn = uint32_t((*e)["insn"].value_or(int64_t(0)));
                d.imm = (*e)["op"].value_or(std::string()) == "imm";
                d.xtlid = uint32_t((*e)["xtlid"].value_or(int64_t(0)));
                if (auto p = (*e)["deref"].as_array())
                    for (auto& o : *p) d.deref.push_back(uint32_t(o.value_or(int64_t(0))));
            }
            f.data[std::string(k.str())] = d;
        }
    return true;
}

// Sign mode

// recompiler config, read when signing
struct Config {
    fs::path root;
    std::string xbe;
    std::vector<std::string> code_sections;
    std::map<std::string, uint32_t> symbols;
    std::map<std::string, uint32_t> data;
    std::map<std::string, uint32_t> xtlid;  // export ids of functions no signing title links
    std::map<uint32_t, std::string> functions;
};

bool LoadConfig(const fs::path& p, Config& cfg) {
    std::optional<toml::table> parsed = ParseToml(p);
    if (!parsed) return false;
    toml::table& t = *parsed;
    if (auto a = t["includes"].as_array())
        for (auto& i : *a)
            if (auto s = i.value<std::string>(); s && !LoadConfig(p.parent_path() / *s, cfg)) return false;
    if (auto v = t["xbe"]["path"].value<std::string>()) cfg.xbe = *v;
    if (auto a = t["xbe"]["code_sections"].as_array())
        for (auto& i : *a) cfg.code_sections.push_back(i.value_or(std::string()));
    for (auto [key, out] :
         {std::pair{"symbols", &cfg.symbols}, std::pair{"data", &cfg.data}, std::pair{"xtlid", &cfg.xtlid}})
        if (auto s = t[key].as_table())
            for (auto& [k, v] : *s) (*out)[std::string(k.str())] = uint32_t(v.value_or(int64_t(0)));
    if (auto s = t["functions"].as_table())
        for (auto& [k, v] : *s)
            cfg.functions[uint32_t(std::stoul(std::string(k.str()), nullptr, 0))] = v.value_or(std::string());
    return true;
}

std::string Plain(std::string n) {
    if (auto i = n.find("::"); i != std::string::npos) n = n.substr(i + 2);
    return n;
}

// function length from va, up to the next known start or the end of its section
uint32_t ExtentBefore(const Image& img, const Image::Code& c, uint32_t va, const std::vector<uint32_t>& starts) {
    auto next = std::upper_bound(starts.begin(), starts.end(), va);
    uint32_t bound = next == starts.end() ? ~0u : *next;
    return img.Extent(va, uint32_t(std::min<uint64_t>(bound, c.va + c.b.size())));
}

// one signing title: its config, swept image and sorted known function starts
struct Title {
    fs::path cp;
    Config cfg;
    Image img;
    std::vector<uint32_t> known;
};

constexpr int kCallDepth = 2;  // callee levels a signature records

Sig SweepSig(const Image& img, const std::vector<uint32_t>& known, uint32_t va, int depth) {
    Sig s;
    auto c = img.CodeAt(va);
    if (!c || !img.IsInstructionStart(va)) return s;
    s.len = ExtentBefore(img, *c, va, known);
    s.hash = img.Hash(va, s.len).value_or(0);
    s.leaf = depth == kCallDepth;
    if (!s.leaf)
        for (uint32_t t : img.ExternalTargets(va, s.len)) s.calls.push_back(SweepSig(img, known, t, depth + 1));
    return s;
}

Sig MakeSig(const Title& t, uint32_t va) {
    if (!t.img.IsInstructionStart(va))
        std::fprintf(stderr, "%s: %s is not on the linear sweep\n", t.cp.filename().string().c_str(), Hex(va).c_str());
    Sig s = SweepSig(t.img, t.known, va, 0);
    if (s.len) s.sec = t.img.CodeAt(va)->name;
    return s;
}

// identical functions keep their place in link order
void RankSigs(const Title& t, std::map<std::string, Sig>& sigs, const std::map<std::string, uint32_t>& at) {
    const Image& img = t.img;
    std::vector<const Sig*> v;
    std::vector<std::string> n;
    for (auto& [name, s] : sigs) {
        if (!at.count(name) || !s.len) continue;
        v.push_back(&s);
        n.push_back(name);
    }
    auto hits = img.Find(v, false);
    for (size_t i = 0; i < v.size(); i++) {
        auto& s = sigs[n[i]];
        s.deep = hits[i].size() > 1;
        if (s.deep)
            std::erase_if(hits[i], [&](uint32_t va) { return !img.CallsMatch(va, s, true); });
        auto it = std::find(hits[i].begin(), hits[i].end(), at.at(n[i]));
        if (it == hits[i].end()) {
            std::fprintf(stderr, "%s: %s does not match itself\n", t.cp.filename().string().c_str(), n[i].c_str());
            s = {};
            continue;
        }
        s.count = uint32_t(hits[i].size());
        s.nth = uint32_t(it - hits[i].begin());
    }
}

// signs the title's named functions no earlier title signed; returns their addresses
std::map<std::string, uint32_t> SignSymbols(const Title& t, const std::set<std::string>& skip, SigFile& sf,
                                            std::map<std::string, std::string>& origin) {
    std::map<std::string, uint32_t> fresh;
    for (auto& [n, id] : t.cfg.xtlid) {
        if (skip.count(n)) continue;
        sf.names.insert(n);
        sf.symbols[n].xtlid = id;
    }
    for (auto& [n, va] : t.cfg.symbols) {
        if (skip.count(n)) continue;
        sf.names.insert(n);
        for (auto& [id, eva] : t.img.Exports())
            if (id && va && eva == va && !sf.symbols[n].xtlid) sf.symbols[n].xtlid = id;
        if (!va || sf.symbols[n].len) continue;
        // MakeSig returns a fresh signature: carry the export id found above over to it
        uint32_t id = sf.symbols[n].xtlid;
        sf.symbols[n] = MakeSig(t, va);
        sf.symbols[n].xtlid = id;
        if (sf.symbols[n].len) t.img.Trace(va, sf.symbols[n].len, sf.symbols[n]);
        fresh[n] = va;
        origin[n] = t.cp.filename().string();
    }
    return fresh;
}

// data: the first unique signed function using the address, then a named library function, then any
// function called outside .text (game code there would not carry over)
std::vector<std::pair<std::string, uint32_t>> DataCandidates(const Title& t,
                                                             const std::map<std::string, uint32_t>& fresh,
                                                             const SigFile& sf, const std::string& dn) {
    std::vector<std::pair<std::string, uint32_t>> order;
    for (auto& [n, va] : fresh)
        if (sf.symbols.at(n).count == 1) order.push_back({n, va});
    for (auto& [va, n] : t.cfg.functions) order.push_back({Plain(n), va});
    for (uint32_t va : t.img.calls)
        if (t.img.CodeAt(va) && t.img.CodeAt(va)->name != ".text") order.push_back({dn + "_ref", va});
    return order;
}

// the first instruction of function n in [va, va + len) with dva as displacement or immediate
std::optional<DataRef> FindRef(const Image& img, const std::string& n, uint32_t va, uint32_t len, uint32_t dva) {
    std::optional<DataRef> ref;
    img.Walk(va, len, [&](uint32_t, uint32_t i, const ZydisDecodedInstruction& in) {
        if (ref) return;
        if (in.raw.disp.size == 32 && uint32_t(in.raw.disp.value) == dva) ref = DataRef{n, i, false};
        for (auto& im : in.raw.imm)
            if (!ref && im.size == 32 && !im.is_relative && uint32_t(im.value.u) == dva)
                ref = DataRef{n, i, true};
    });
    return ref;
}

constexpr uint32_t kTableSpan = 0x40;  // bytes of a table searched at each level

// offsets of the dwords read in turn from a, one or two levels, that lead to dva. A table or record lies inside one
// section, so a read past the section's end is a neighbor's data; records in .text are game code.
std::optional<std::vector<uint32_t>> TablePath(const xbe::Image& x, uint32_t a, uint32_t dva) {
    for (uint32_t i = 0; i < kTableSpan; i += 4) {
        uint32_t p = 0;
        if (x.SectionAt(a + i) != x.SectionAt(a) || !x.Read32(a + i, p)) continue;
        if (p == dva) return std::vector<uint32_t>{i};
        const xbe::Section* s = x.SectionAt(p);
        if (!s || s->name == ".text") continue;
        for (uint32_t j = 0; j < kTableSpan; j += 4) {
            uint32_t q = 0;
            if (x.SectionAt(p + j) == s && x.Read32(p + j, q) && q == dva) return std::vector<uint32_t>{i, j};
        }
    }
    return std::nullopt;
}

// the first instruction of function n in [va, va + len) holding the address of a table dva is read from
std::optional<DataRef> FindTableRef(const Image& img, const std::string& n, uint32_t va, uint32_t len, uint32_t dva) {
    std::optional<DataRef> ref;
    img.Walk(va, len, [&](uint32_t, uint32_t i, const ZydisDecodedInstruction& in) {
        auto check = [&](uint32_t a, bool imm) {
            if (ref || !IsAddr(a)) return;
            if (auto path = TablePath(img.x, a, dva)) ref = DataRef{n, i, imm, 0, *path};
        };
        if (in.raw.disp.size == 32) check(uint32_t(in.raw.disp.value), false);
        for (auto& im : in.raw.imm)
            if (im.size == 32 && !im.is_relative) check(uint32_t(im.value.u), true);
    });
    return ref;
}

// the anchor this title signed at va, empty for none
std::string AnchorAt(const SigFile& sf, const std::map<std::string, uint32_t>& anchor_at, uint32_t va) {
    for (auto& [n, a] : anchor_at)
        if (a == va && sf.anchors.count(n)) return n;
    return {};
}

// signs n at va as an anchor; false and dropped when identical functions exist
bool AddAnchor(const Title& t, SigFile& sf, std::map<std::string, uint32_t>& anchor_at, const std::string& n,
               uint32_t va) {
    sf.anchors[n] = MakeSig(t, va);
    anchor_at[n] = va;
    RankSigs(t, sf.anchors, anchor_at);
    if (sf.anchors[n].count == 1) return true;
    sf.anchors.erase(n);
    return false;
}

// points each config data address at an instruction of a signed function or anchor, or at its .XTLID id
void SignData(const Title& t, const std::set<std::string>& skip, const std::map<std::string, uint32_t>& fresh,
              SigFile& sf) {
    std::map<std::string, uint32_t> anchor_at;
    // the first candidate whose code finds dva sets the data's recipe
    auto sign = [&](const std::string& dn, uint32_t dva, decltype(&FindRef) find) {
        for (auto& [n, va] : DataCandidates(t, fresh, sf, dn)) {
            bool is_sym = fresh.count(n) && fresh.at(n) == va;
            if (!is_sym && sf.names.count(n)) continue;
            uint32_t len = is_sym ? sf.symbols[n].len : MakeSig(t, va).len;
            std::optional<DataRef> ref = find(t.img, n, va, len, dva);
            if (!ref) continue;
            // a function this title already anchored for other data serves this one too
            std::string made = AnchorAt(sf, anchor_at, va);
            if (!is_sym && !made.empty()) ref->in = made;
            else if (!is_sym && (sf.anchors.count(n) || !AddAnchor(t, sf, anchor_at, n, va))) continue;
            sf.data[dn] = *ref;
            return true;
        }
        return false;
    };
    for (auto& [dn, dva] : t.cfg.data) {
        if (skip.count(dn) || !sf.data[dn].in.empty() || sf.data[dn].xtlid || !dva) continue;
        if (!sign(dn, dva, FindRef)) sign(dn, dva, FindTableRef);
        for (auto& [id, va] : t.img.Exports())
            if (va == dva) sf.data[dn].xtlid = id;
        if (sf.data[dn].in.empty() && !sf.data[dn].xtlid)
            std::fprintf(stderr, "%s: no function uses %s\n", t.cp.filename().string().c_str(), dn.c_str());
    }
}

// signs one title's functions and data; false when its config or image does not load
bool SignTitle(const fs::path& cp, const std::set<std::string>& skip, SigFile& sf,
               std::map<std::string, std::string>& origin) {
    Title t;
    t.cp = cp;
    Config& cfg = t.cfg;
    if (!LoadConfig(cp, cfg)) return false;
    cfg.root = cp.parent_path().parent_path();
    Image& img = t.img;
    if (!img.Load(cfg.root / cfg.xbe)) return false;
    std::vector<uint32_t>& known = t.known;
    for (auto& [n, va] : cfg.symbols)
        if (va) known.push_back(va);
    for (auto& [va, n] : cfg.functions) known.push_back(va);
    img.Sweep(cfg.code_sections);
    known.insert(known.end(), img.calls.begin(), img.calls.end());
    std::sort(known.begin(), known.end());
    std::map<std::string, uint32_t> fresh = SignSymbols(t, skip, sf, origin);
    RankSigs(t, sf.symbols, fresh);
    SignData(t, skip, fresh, sf);
    return true;
}

std::string WriteData(const DataRef& d) {
    std::string e;
    if (!d.in.empty()) {
        const char* op = d.imm ? "imm" : "disp";
        e = std::string("in = \"") + d.in + "\", insn = " + std::to_string(d.insn) + ", op = \"" + op + "\"";
        for (size_t i = 0; i < d.deref.size(); i++) e += (i ? ", " : ", deref = [") + Hex(d.deref[i]);
        if (!d.deref.empty()) e += "]";
    }
    if (d.xtlid) e += (e.empty() ? "" : ", ") + std::string("xtlid = ") + Hex(d.xtlid);
    return "{" + e + "}";
}

std::string SigFileText(SigFile& sf) {
    std::string s =
        "# XDK library signatures read by xbe_symbols, written by xbe_symbols in sign mode. No library bytes stored.\n"
        "# [symbols] name = {sec, len, hash, calls, deep, count, nth}: hash of len bytes from the function start in\n"
        "# section sec, image addresses and rel32 targets masked; calls = [len, hash, calls] of each rel32 call or\n"
        "# jump target outside it, in order, two levels deep; deep = lookalikes exist, so the callees' callees must\n"
        "# match too; count and nth pick one of identical functions by link order; xtlid = its .XTLID export id,\n"
        "# used when the bytes match nothing. {} = no signature, stays 0.\n"
        "# k = its constants, sorted: immediates and displacements of instructions that do not use the stack, when\n"
        "# not an image address, a branch target or below 0x40; an address inside .text counts in a library section,\n"
        "# which holds no such pointer. ret = stack bytes its rets pop, regs = registers it reads before it writes\n"
        "# them. A function the bytes do not find is matched by k; ret and regs then show a changed convention.\n"
        "# [anchors] functions only used to find data. [data] in = function, insn = instruction index, op = operand,\n"
        "# deref = offsets of the dwords then read in turn, for data behind a table, xtlid = the id .XTLID lists the\n"
        "# address under, used when in finds nothing.\n";
    s += "\n[symbols]\n";
    for (auto& n : sf.names) s += n + " = " + WriteSig(sf.symbols[n]) + "\n";
    s += "\n[anchors]\n";
    for (auto& [n, sig] : sf.anchors) s += n + " = " + WriteSig(sig) + "\n";
    s += "\n[data]\n";
    for (auto& [n, d] : sf.data) s += n + " = " + WriteData(d) + "\n";
    return s;
}

void PrintSigned(const fs::path& out, const std::vector<fs::path>& configs, SigFile& sf,
                 const std::map<std::string, std::string>& origin) {
    size_t signed_count = 0;
    for (auto& n : sf.names) signed_count += sf.symbols[n].len != 0;
    std::fprintf(stderr, "%s: %zu of %zu functions signed, %zu anchors, %zu data\n", out.string().c_str(),
                 signed_count, sf.names.size(), sf.anchors.size(), sf.data.size());
    for (auto& [n, o] : origin)
        if (o != configs[0].filename().string() && sf.symbols[n].len)
            std::fprintf(stderr, "  %s from %s\n", n.c_str(), o.c_str());
}

// signatures of every named function of the given titles; the first title that has a name wins
int Sign(const fs::path& out, const std::vector<fs::path>& configs, const std::set<std::string>& skip) {
    SigFile sf;
    std::map<std::string, std::string> origin;
    for (auto& cp : configs)
        if (!SignTitle(cp, skip, sf, origin)) return 1;
    std::ofstream(out, std::ios::binary) << SigFileText(sf);
    PrintSigned(out, configs, sf, origin);
    return 0;
}

// Scan mode

std::vector<std::string> GuessCode(const xbe::Image& x) {
    static const std::set<std::string> data = {".rdata", ".data", ".data1", ".XTLID", "DOLBY", "XON_RD", "dsstdfx"};
    std::vector<std::string> r;
    for (auto& s : x.sections) {
        const auto& n = s.name;
        if (!(s.flags & xbe::kExecutable) || data.count(n) || n.rfind("$$", 0) == 0 ||
            (n.size() > 4 && n.compare(n.size() - 4, 4, "DATA") == 0))
            continue;
        if (std::find(r.begin(), r.end(), n) == r.end()) r.push_back(n);
    }
    return r;
}

std::string Table(const std::map<std::string, uint32_t>& m) {
    std::vector<std::string> keys;
    size_t w = 0;
    for (auto& [k, v] : m) {
        keys.push_back(k);
        w = std::max(w, k.size());
    }
    std::sort(keys.begin(), keys.end(), [](auto& a, auto& b) { return Lower(a) < Lower(b); });
    std::string s;
    for (auto& k : keys) {
        char b[16];
        std::snprintf(b, sizeof b, m.at(k) ? "0x%06X" : "0", m.at(k));
        s += k + std::string(w - k.size() + 1, ' ') + "= " + b + "\n";
    }
    return s;
}

void PrintLibraries(const fs::path& xbe_path, const xbe::Image& x) {
    std::fprintf(stderr, "%s: title %08X\n  libraries:", xbe_path.filename().string().c_str(), x.title_id);
    for (auto& l : x.libraries) std::fprintf(stderr, " %s %u.%u.%u", l.name.c_str(), l.major, l.minor, l.build);
    std::fprintf(stderr, "\n");
}

// the D3D8 or XAPILIB build picks the signature set first, then any library's build
std::vector<uint16_t> WantedBuilds(const xbe::Image& x) {
    std::vector<uint16_t> want;
    for (const char* pick : {"D3D8", "D3D8LTCG", "XAPILIB"})
        for (auto& l : x.libraries)
            if (l.name == pick) want.push_back(l.build);
    for (auto& l : x.libraries) want.push_back(l.build);
    return want;
}

// the signature folder for the image's library builds, empty after listing the available ones
fs::path SignatureDir(const xbe::Image& x, const fs::path& xdk, uint16_t& build) {
    for (auto b : WantedBuilds(x)) {
        if (!fs::exists(xdk / std::to_string(b) / "signatures.toml")) continue;
        build = b;
        return xdk / std::to_string(b);
    }
    std::fprintf(stderr, "no signatures for these library builds; available:");
    std::error_code ec;
    for (auto& e : fs::directory_iterator(xdk, ec))
        if (fs::exists(e.path() / "signatures.toml"))
            std::fprintf(stderr, " %s", e.path().filename().string().c_str());
    std::fprintf(stderr, "\n");
    return {};
}

// unique hits, and the nth of identical functions; several hits for one name are reported
void ResolveHits(const std::vector<const Sig*>& v, const std::vector<std::string>& n,
                 const std::vector<std::vector<uint32_t>>& hits, std::map<std::string, uint32_t>& found,
                 std::vector<std::string>& report) {
    for (size_t i = 0; i < v.size(); i++)
        if (hits[i].size() == 1 && v[i]->count == 1) found[n[i]] = hits[i][0];
    for (size_t i = 0; i < v.size(); i++) {
        auto& h = hits[i];
        auto& s = *v[i];
        if (s.count == 1 && h.size() == 1) continue;
        if (s.count > 1 && h.size() == s.count) {
            found[n[i]] = h[s.nth];
        } else if (h.size() > 1) {
            std::string line = "  ambiguous " + n[i] + ":";
            for (uint32_t a : h) line += " " + Hex(a);
            report.push_back(line);
        }
    }
}

// one address claimed by several names: keep the one whose callees' callees also match
void DropSharedClaims(const Image& img, const std::vector<const Sig*>& v, const std::vector<std::string>& n,
                      std::map<std::string, uint32_t>& found, std::vector<std::string>& report) {
    std::map<uint32_t, std::vector<size_t>> claims;
    for (size_t i = 0; i < v.size(); i++)
        if (found.count(n[i])) claims[found[n[i]]].push_back(i);
    for (auto& [va, who] : claims) {
        if (who.size() < 2) continue;
        std::vector<size_t> ok;
        std::string line = "  " + Hex(va) + " claimed by";
        for (size_t i : who) {
            line += " " + n[i];
            if (img.CallsMatch(va, *v[i], true)) ok.push_back(i);
        }
        for (size_t i : who)
            if (ok.size() != 1 || ok[0] != i) found.erase(n[i]);
        report.push_back(line + (ok.size() == 1 ? ", kept " + n[ok[0]] : ", dropped all"));
    }
}

std::map<std::string, uint32_t> FindByBytes(const Image& img, SigFile& sf, std::vector<std::string>& report) {
    std::vector<const Sig*> v;
    std::vector<std::string> n;
    for (auto* m : {&sf.symbols, &sf.anchors})
        for (auto& [k, s] : *m) {
            if (!s.len) continue;
            v.push_back(&s);
            n.push_back(k);
        }
    auto hits = img.Find(v);
    std::map<std::string, uint32_t> found;
    ResolveHits(v, n, hits, found, report);
    DropSharedClaims(img, v, n, found, report);
    return found;
}

// every [symbols] name: its address found by bytes, else by .XTLID, else 0
std::map<std::string, uint32_t> NameSymbols(const Image& img, SigFile& sf,
                                            const std::map<std::string, uint32_t>& found,
                                            std::vector<std::string>& report) {
    std::map<std::string, uint32_t> symbols;
    for (auto& k : sf.names) symbols[k] = sf.symbols.count(k) && found.count(k) ? found.at(k) : 0;
    for (auto& [k, va] : symbols)
        for (auto& [id, eva] : img.Exports())
            if (!va && id && sf.symbols.count(k) && sf.symbols[k].xtlid == id) {
                va = eva;
                report.push_back("  " + k + " found by .XTLID " + Hex(id));
            }
    return symbols;
}

// every known start, sorted, as a bound for function extents
std::vector<uint32_t> Bounds(const Image& img, const std::map<std::string, uint32_t>& symbols) {
    std::vector<uint32_t> bounds(img.calls);
    for (auto& [k, va] : symbols)
        if (va) bounds.push_back(va);
    std::sort(bounds.begin(), bounds.end());
    return bounds;
}

// the ranges found functions cover, start to end
std::map<uint32_t, uint32_t> Claimed(const SigFile& sf, const std::map<std::string, uint32_t>& found,
                                     const std::map<std::string, uint32_t>& symbols) {
    std::map<uint32_t, uint32_t> claimed;
    for (auto& [k, va] : found) claimed[va] = va + (sf.symbols.count(k) ? sf.symbols.at(k) : sf.anchors.at(k)).len;
    for (auto& [k, va] : symbols)
        if (va) claimed.insert({va, va + 1});
    return claimed;
}

Sig ShapeAt(const Image& img, const std::vector<uint32_t>& bounds, uint32_t va) {
    Sig s;
    auto c = img.CodeAt(va);
    if (c) img.Trace(va, ExtentBefore(img, *c, va, bounds), s);
    return s;
}

// best and runner up score; who is a start index in by_name and a name index in by_start
struct Rank {
    double best = 0;
    double next = 0;
    size_t who = 0;
    void Add(double v, size_t w) {
        if (v > best) {
            next = best;
            best = v;
            who = w;
        } else {
            next = std::max(next, v);
        }
    }
};

// symbols with enough constants to be matched by them
std::vector<std::string> ConstantNamed(const SigFile& sf) {
    std::vector<std::string> named;
    for (auto& [k, s] : sf.symbols)
        if (s.k.size() >= kMinConstants) named.push_back(k);
    return named;
}

// unclaimed starts per section of a missing symbol with enough constants
std::map<std::string, std::vector<std::pair<uint32_t, Sig>>> UnclaimedStarts(
    const Image& img, const SigFile& sf, const std::map<std::string, uint32_t>& symbols,
    const std::vector<uint32_t>& bounds, const std::map<uint32_t, uint32_t>& claimed) {
    std::map<std::string, std::vector<std::pair<uint32_t, Sig>>> starts;
    // symbols holds every sf.symbols name, so at() never throws
    for (auto& [k, s] : sf.symbols)
        if (s.k.size() >= kMinConstants && !symbols.at(k)) starts[s.sec];
    for (uint32_t va : img.Starts()) {
        auto c = img.CodeAt(va);
        auto it = claimed.upper_bound(va);
        bool unclaimed = it == claimed.begin() || va >= std::prev(it)->second;
        if (starts.count(c->name) && unclaimed) starts[c->name].push_back({va, ShapeAt(img, bounds, va)});
    }
    return starts;
}

void ScoreStarts(SigFile& sf, const std::vector<std::string>& named,
                 const std::map<std::string, std::vector<std::pair<uint32_t, Sig>>>& starts,
                 std::vector<Rank>& by_name, std::map<uint32_t, Rank>& by_start) {
    for (size_t i = 0; i < named.size(); i++) {
        auto& s = sf.symbols[named[i]];
        if (!starts.count(s.sec)) continue;
        auto& c = starts.at(s.sec);
        for (size_t j = 0; j < c.size(); j++) {
            double score = Similar(s.k, c[j].second.k);
            by_name[i].Add(score, j);
            by_start[c[j].first].Add(score, i);
        }
    }
}

// symbols found, but not by their bytes
std::set<std::string> Loose(const std::map<std::string, uint32_t>& symbols,
                            const std::map<std::string, uint32_t>& found) {
    std::set<std::string> loose;
    for (auto& [k, va] : symbols)
        if (va && !found.count(k)) loose.insert(k);
    return loose;
}

// addresses of the missing symbols whose constants match one unclaimed start
std::map<std::string, uint32_t> FindByConstants(const Image& img, SigFile& sf,
                                                const std::map<std::string, uint32_t>& symbols,
                                                const std::vector<uint32_t>& bounds,
                                                const std::map<uint32_t, uint32_t>& claimed,
                                                std::vector<std::string>& report) {
    std::vector<std::string> named = ConstantNamed(sf);
    auto starts = UnclaimedStarts(img, sf, symbols, bounds, claimed);
    std::vector<Rank> by_name(named.size());
    std::map<uint32_t, Rank> by_start;
    ScoreStarts(sf, named, starts, by_name, by_start);
    std::map<std::string, uint32_t> matches;
    for (size_t i = 0; i < named.size(); i++) {
        auto& r = by_name[i];
        if (symbols.at(named[i]) || r.best < kAccept || r.best - r.next < kLead) continue;
        uint32_t va = starts[sf.symbols[named[i]].sec][r.who].first;
        // accepted only when the start's own best match is this name, by kLead too
        auto& rival = by_start[va];
        if (rival.who != i || rival.best - rival.next < kLead) continue;
        matches[named[i]] = va;
        char b[16];
        std::snprintf(b, sizeof b, "%.2f", r.best);
        report.push_back("  " + named[i] + " found by constants (" + b + ") at " + Hex(va));
    }
    return matches;
}

std::string Convention(const Sig& s) {
    std::string r = s.regs ? "reads " + Regs(s.regs) : "reads no register";
    for (size_t i = 0; i < s.ret.size(); i++) r += (i ? " or " : ", pops ") + std::to_string(s.ret[i]);
    return r + (s.ret.empty() ? ", leaves by jump" : "");
}

constexpr uint32_t kSlotBytes = 4;  // one stack argument

// stack slots plus entry registers of the reference, when it has one ret size
size_t ArgCount(const Sig& s) {
    if (s.ret.size() != 1) return 0;
    size_t args = s.ret[0] / kSlotBytes;  // when the reference tells
    for (int i = 0; i < kRegCount; i++) args += s.regs >> i & 1;
    return args;
}

// a different ret size or other entry registers than the reference: arguments moved
std::map<std::string, std::string> CheckConventions(const Image& img, const std::vector<uint32_t>& bounds,
                                                    SigFile& sf, const std::map<std::string, uint32_t>& symbols,
                                                    const std::set<std::string>& loose,
                                                    std::vector<std::string>& report) {
    std::map<std::string, std::string> abi;
    for (auto& k : loose) {
        auto& s = sf.symbols[k];
        Sig c = ShapeAt(img, bounds, symbols.at(k));
        // a function that leaves by jump does not tell its stack bytes
        bool same_ret = c.ret == s.ret || c.ret.empty() || s.ret.empty();
        bool same_convention = c.regs == s.regs && same_ret;
        if (!s.shape || same_convention) continue;
        size_t args = ArgCount(s);
        abi[k] = "[\"?\"";
        for (size_t i = 1; i < args; i++) abi[k] += ", \"?\"";
        abi[k] += "]  # " + Convention(c);
        if (abi.size() == 1)
            report.push_back("  convention differs from the reference, prove each argument order by hand and write it in [abi]:");
        report.push_back("    " + k + " at " + Hex(symbols.at(k)) + " " + Convention(c) +
                         "; reference " + Convention(s));
    }
    return abi;
}

Sig* SigNamed(SigFile& sf, const std::string& name) {
    if (sf.symbols.count(name)) return &sf.symbols[name];
    if (sf.anchors.count(name)) return &sf.anchors[name];
    return nullptr;
}

// the dword at each offset in turn from a; 0 once one is unmapped
uint32_t Follow(const xbe::Image& x, uint32_t a, const std::vector<uint32_t>& deref) {
    for (uint32_t off : deref)
        if (!a || !x.Read32(a + off, a)) return 0;
    return a;
}

std::map<std::string, uint32_t> ResolveData(const Image& img, SigFile& sf,
                                            const std::map<std::string, uint32_t>& found,
                                            const std::set<std::string>& loose, std::vector<std::string>& report) {
    std::map<std::string, uint32_t> data;
    for (auto& [k, d] : sf.data) {
        data[k] = 0;
        Sig* sig = SigNamed(sf, d.in);
        if (sig && found.count(d.in)) {
            uint32_t operand = img.Operand(found.at(d.in), sig->len, d).value_or(0);
            data[k] = Follow(img.x, operand, d.deref);
        }
        for (auto& [id, va] : img.Exports())
            if (!data[k] && d.xtlid && id == d.xtlid) data[k] = va;
        // an instruction index only holds in a function the bytes found
        if (!data[k] && loose.count(d.in))
            report.push_back("  " + k + " not read from " + d.in + ", which the bytes did not find");
    }
    return data;
}

void PrintScan(const xbe::Image& x, const std::map<std::string, uint32_t>& symbols,
               const std::map<std::string, uint32_t>& data, const std::vector<std::string>& report) {
    size_t functions_found = 0;
    size_t data_found = 0;
    std::string miss;
    for (auto& [k, va] : symbols) {
        functions_found += va != 0;
        if (!va) miss += " " + k;
    }
    for (auto& [k, va] : data) {
        data_found += va != 0;
        if (!va) miss += " " + k;
    }
    std::fprintf(stderr, "  found %zu of %zu functions, %zu of %zu data\n", functions_found, symbols.size(),
                 data_found, data.size());
    for (auto& r : report) std::fprintf(stderr, "%s\n", r.c_str());
    if (!miss.empty())
        std::fprintf(stderr, "  missing (0, the host traps if the game calls one):%s\n", miss.c_str());
    for (auto& l : x.libraries)
        if (l.name.size() > 4 && l.name.compare(l.name.size() - 4, 4, "LTCG") == 0)
            std::fprintf(stderr, "  %s is link time code generated: check the functions found by constants and the missing ones by hand\n", l.name.c_str());
}

uint32_t Partitions(const Image& img) {
    constexpr uint32_t kFirstParts = 8;
    constexpr uint32_t kMaxParts = 256;
    constexpr uint64_t kPartBytes = 40000;
    // a partition may grow to 3 / 2 of kPartBytes before the count doubles
    constexpr uint32_t kSlackNum = 3;
    constexpr uint32_t kSlackDen = 2;
    uint64_t code = 0;
    for (auto& c : img.code) code += c.b.size();
    // doubled from 8 until a partition holds about 60 KB of code or less
    uint32_t parts = kFirstParts;
    while (parts < kMaxParts && code / kPartBytes > parts * kSlackNum / kSlackDen) parts *= 2;
    return parts;
}

std::string XbeTable(const fs::path& xbe_path, const xbe::Image& x, uint16_t build, const std::string& name,
                     uint32_t parts, const std::vector<std::string>& sections) {
    std::string s = "# Recompiler input for " + xbe_path.filename().string() + ", written by xbe_symbols\n\n[xbe]\n";
    s += "path = \"assets/" + xbe_path.filename().string() + "\"\n";
    s += "title = " + Hex(x.title_id) + "\nentry = " + Hex(x.entry) + "\nxdk = " + std::to_string(build) + "\n";
    s += "out = \"generated/" + name + "\"\n";
    s += "partitions = " + std::to_string(parts) + "\n";
    s += "code_sections = [";
    for (size_t i = 0; i < sections.size(); i++) s += (i ? ", \"" : "\"") + sections[i] + "\"";
    s += "]\n";
    return s;
}

std::string AbiTable(const std::map<std::string, std::string>& abi) {
    std::string s =
        "\n# argument locations in API order for functions a link time code generated library compiled with another\n"
        "# convention: a register (eax ebx ecx edx esi edi ebp) or \"stack\" for the next stack slot, which the callee pops\n"
        "[abi]\n";
    for (auto& [k, e] : abi) s += k + " = " + e + "\n";
    return s;
}

int Scan(const fs::path& xbe_path, const fs::path& xdk, const fs::path& out, const std::string& name,
         const std::string& library) {
    Image img;
    if (!img.Load(xbe_path)) return 1;
    auto& x = img.x;
    PrintLibraries(xbe_path, x);

    uint16_t build = 0;
    fs::path dir = SignatureDir(x, xdk, build);
    if (dir.empty()) return 1;
    SigFile sf;
    if (!LoadSigs(dir / "signatures.toml", sf)) return 1;
    std::fprintf(stderr, "  using %s\n", dir.string().c_str());

    auto sections = GuessCode(x);
    img.Sweep(sections);
    std::vector<std::string> report;
    std::map<std::string, uint32_t> found = FindByBytes(img, sf, report);
    std::map<std::string, uint32_t> symbols = NameSymbols(img, sf, found, report);

    // functions the bytes did not find: the unclaimed start whose constants are closest
    std::map<uint32_t, uint32_t> claimed = Claimed(sf, found, symbols);
    std::vector<uint32_t> bounds = Bounds(img, symbols);
    std::set<std::string> loose = Loose(symbols, found);  // found, but not by their bytes
    for (auto& [k, va] : FindByConstants(img, sf, symbols, bounds, claimed, report)) {
        symbols[k] = va;
        loose.insert(k);
    }
    std::map<std::string, std::string> abi = CheckConventions(img, bounds, sf, symbols, loose, report);

    std::map<std::string, uint32_t> data = ResolveData(img, sf, found, loose, report);
    PrintScan(x, symbols, data, report);

    std::string s = XbeTable(xbe_path, x, build, name, Partitions(img), sections);
    s += "library = \"" + library + "\"  # <exe>.dll, which the executable delay loads\n";
    s += "\n# extra entry points\n[functions]\n";
    s += "\n# functions the host replaces or calls by name, 0 where the image has none\n[symbols]\n" + Table(symbols);
    s += "\n[data]\n" + Table(data);
    s += AbiTable(abi);
    if (out.empty()) std::fwrite(s.data(), 1, s.size(), stdout);
    else std::ofstream(out, std::ios::binary) << s;
    return 0;
}

}  // namespace

// Command line

int main(int argc, char** argv) {
    std::vector<std::string> a(argv + 1, argv + argc);
    auto opt = [&](const char* k) {
        auto it = std::find(a.begin(), a.end(), k);
        if (it == a.end() || it + 1 == a.end()) return std::string();
        std::string v = *(it + 1);
        a.erase(it, it + 2);
        return v;
    };
    std::string out = opt("--out");
    std::string name = opt("--name");
    std::string library = opt("--library");
    std::string skip = opt("--skip");
    if (!a.empty() && a[0] == "--sign" && a.size() >= 2 && !out.empty()) {
        std::set<std::string> sk;
        for (size_t p = 0; !skip.empty() && p != std::string::npos;) {
            size_t q = skip.find(',', p);
            sk.insert(skip.substr(p, q == std::string::npos ? q : q - p));
            p = q == std::string::npos ? q : q + 1;
        }
        return Sign(out, std::vector<fs::path>(a.begin() + 1, a.end()), sk);
    }
    if (a.size() != 2 || library.empty()) {
        std::fprintf(stderr,
                     "usage: xbe_symbols <game.xbe> <sdk>/xdk --library <exe>.dll [--out config/game.toml] [--name game]\n"
                     "       xbe_symbols --sign <config.toml>... --out signatures.toml [--skip name,...]\n");
        return 1;
    }
    return Scan(a[0], a[1], out, name.empty() ? "game" : name, library);
}
