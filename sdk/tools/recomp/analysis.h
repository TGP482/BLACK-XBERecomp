#pragma once
#include <Zydis/Zydis.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "xbe/xbe.h"

struct Hook {
    struct Exit {
        bool set = false, ret = false;
        uint32_t to = 0;
    };
    uint32_t at = 0;
    std::string call;
    bool after = false;
    Exit on_true, on_false;
    bool Returns() const { return on_true.set || on_false.set; }
};

struct Config {
    std::string xbe_path;
    uint32_t title = 0, entry = 0;  // the image xbe_symbols wrote the config for, 0 = unchecked
    std::set<uint32_t> trace;       // functions whose calls and results are logged
    std::string out_dir;
    std::string library;  // the game code DLL, named in the generated exports.def
    std::vector<std::string> code_sections;
    std::vector<std::string> code_prefixes;
    uint32_t partitions = 128;
    std::map<uint32_t, std::string> functions;  // extra entry points
    // names the host binds to: functions it replaces and data it reads; 0 = not in this image
    std::map<std::string, uint32_t> symbols, data;
    // [xbe] named: the [symbols] and [names] functions take those names, not their address, so one executable binds
    // to each release of a game; [names] are the game's own functions it replaces or calls
    bool named = false;
    std::map<std::string, uint32_t> names;
    std::map<std::string, uint32_t> layout;     // XDK data layout, emitted as XDK_<name>
    std::map<std::string, std::vector<std::string>> abi;  // where a symbol takes each argument, emitted as IMAGE_ABI
    std::set<uint32_t> no_gap_fill;             // section starts to skip in gap fill
    std::vector<std::pair<uint32_t, uint32_t>> data_ranges;  // data inside code sections
    bool gap_fill = true;
    bool pointer_seeds = true;
    std::multimap<uint32_t, Hook> hooks;
    std::map<uint32_t, uint32_t> function_ends;          // declared entries, exclusive decode limit or 0
    std::map<uint32_t, std::vector<uint32_t>> switches;  // indirect jmp to its targets
};

enum class Flow : uint8_t { Normal, Jcc, Jmp, JmpIndirect, Call, CallIndirect, Ret, Stop, Invalid };

struct Insn {
    uint8_t len = 0;
    Flow flow = Flow::Invalid;
    uint32_t target = 0;  // direct branch or call target
};

struct JumpTable {
    uint32_t insn = 0;   // the jmp
    uint32_t table = 0;  // dword table
    std::vector<uint32_t> targets;
    std::vector<uint32_t> slots;  // where each target was read
};

class Analysis {
public:
    Analysis(const xbe::Image& img, const Config& cfg);
    void Run();

    bool IsCode(uint32_t va) const { return va >= lo_ && va < hi_ && code_[va - lo_]; }
    // past a section's file data, in the zeros the loader adds up to its virtual size
    bool InTail(uint32_t va) const {
        const xbe::Section* s = img.SectionAt(va);
        return s && va >= s->va + s->rsize;
    }
    bool Decode(uint32_t va, ZydisDecodedInstruction& insn, ZydisDecodedOperand* ops) const;
    const Insn* InsnAt(uint32_t va) const {
        auto it = insns_.find(va);
        return it == insns_.end() ? nullptr : &it->second;
    }

    const xbe::Image& img;
    const Config& cfg;
    uint32_t lo_ = 0, hi_ = 0;
    std::set<uint32_t> functions;
    std::unordered_map<uint32_t, JumpTable> jump_tables;  // keyed by jmp address
    std::unordered_map<uint32_t, Insn> insns_;
    std::set<uint32_t> continuations;  // where C++ catch blocks resume, reached by the CRT's jmp eax

    // body of a function: instruction addresses reachable without entering other entries
    std::vector<uint32_t> Body(uint32_t entry) const;

private:
    bool DecodeFlow(uint32_t va, Insn& out, JumpTable* jt);
    // overlap: an untrusted walk that keeps decodes overlapping earlier ones instead of failing on them
    bool Walk(uint32_t entry, bool trusted, std::vector<uint32_t>* calls, bool overlap = false);
    void SeedPointers(bool late = false);
    bool PlausibleEntry(uint32_t va) const;
    void Adopt(const std::vector<uint32_t>& calls, bool trusted);
    void GapFill();
    void EhContinuations();
    void MakeFunction(uint32_t va);
    bool ReadTable(uint32_t jmp_va, const ZydisDecodedOperand& mem, JumpTable& jt);
    bool SledTable(uint32_t jmp_va, const uint32_t* ring, int nring, JumpTable& jt);

    ZydisDecoder dec_;
    std::vector<uint8_t> code_;   // 1 = byte belongs to a code section
    std::vector<uint8_t> cover_;  // 0 unknown, 1 insn start, 2 insn body, 3 data
    const uint8_t* Bytes(uint32_t va, uint32_t& avail) const;
};
