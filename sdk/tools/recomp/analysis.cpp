#include "analysis.h"

#include <algorithm>
#include <cstdio>
#include <deque>

Analysis::Analysis(const xbe::Image& image, const Config& config) : img(image), cfg(config) {
    ZydisDecoderInit(&dec_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    auto is_code = [&](const xbe::Section& s) {
        for (auto& n : cfg.code_sections)
            if (s.name == n) return true;
        for (auto& p : cfg.code_prefixes)
            if (s.name.rfind(p, 0) == 0) return true;
        return false;
    };
    lo_ = 0xFFFFFFFF;
    for (auto& s : img.sections)
        if (is_code(s)) {
            lo_ = std::min(lo_, s.va);
            hi_ = std::max(hi_, s.va + s.rsize);
        }
    code_.assign(hi_ - lo_, 0);
    cover_.assign(hi_ - lo_, 0);
    for (auto& s : img.sections)
        if (is_code(s))
            for (uint32_t i = 0; i < s.rsize; i++) code_[s.va - lo_ + i] = 1;
    for (auto& r : cfg.data_ranges)
        for (uint32_t a = r.first; a < r.second; a++)
            if (a >= lo_ && a < hi_) cover_[a - lo_] = 3;
}

const uint8_t* Analysis::Bytes(uint32_t va, uint32_t& avail) const {
    int64_t o = img.Offset(va);
    if (o < 0) return nullptr;
    const xbe::Section* s = img.SectionAt(va);
    uint32_t end = s ? s->va + s->rsize : va;
    avail = end > va ? end - va : 0;
    return img.file.data() + o;
}

bool Analysis::Decode(uint32_t va, ZydisDecodedInstruction& insn, ZydisDecodedOperand* ops) const {
    uint32_t avail = 0;
    const uint8_t* p = Bytes(va, avail);
    if (!p || !avail) return false;
    // an image keeps a section's trailing zeros out of its file data, and an instruction's last bytes may be among them
    uint8_t padded[15] = {};
    const xbe::Section* s = img.SectionAt(va);
    uint32_t room = std::min<uint32_t>(s->va + s->vsize - va, sizeof(padded));
    if (avail < room) {
        std::copy_n(p, avail, padded);
        p = padded;
        avail = room;
    }
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec_, p, std::min<uint32_t>(avail, 15), &insn, ops));
}

bool Analysis::ReadTable(uint32_t jmp_va, const ZydisDecodedOperand& mem, JumpTable& jt) {
    jt.insn = jmp_va;
    jt.table = uint32_t(mem.mem.disp.value);
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t t;
        if (!img.Read32(jt.table + i * 4, t)) break;
        if (!IsCode(t)) {
            if (i == 0 && jt.table == uint32_t(mem.mem.disp.value)) {
                // index 0 unused, its slot overlaps code: the table proper starts one entry later
                jt.table += 4;
                i = uint32_t(-1);
                continue;
            }
            break;
        }
        if (i && jt.table + i * 4 >= lo_ && jt.table + i * 4 < hi_ && cover_[jt.table + i * 4 - lo_] == 1) break;
        jt.targets.push_back(t);
        jt.slots.push_back(jt.table + i * 4);
    }
    // "neg idx; jmp [idx*4 + table]" indexes a table that ends at its base
    uint32_t avail = 0;
    const uint8_t* p = Bytes(jmp_va - 2, avail);
    bool neg = p && avail >= 2 && p[0] == 0xF7 && (p[1] & 0xF8) == 0xD8 && ZYDIS_REGISTER_EAX + (p[1] & 7) == mem.mem.index;
    if (neg || jt.targets.empty()) {
        for (uint32_t i = 1; i < 64; i++) {
            uint32_t t;
            if (!img.Read32(uint32_t(mem.mem.disp.value) - i * 4, t) || !IsCode(t) || (t > jmp_va ? t - jmp_va : jmp_va - t) > 0x1000) break;
            jt.targets.push_back(t);
            jt.slots.push_back(uint32_t(mem.mem.disp.value) - i * 4);
        }
    }
    return !jt.targets.empty();
}

static bool IsCond(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_JLE:
        case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_JNLE:
        case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_JNZ:
        case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_JZ:
        case ZYDIS_MNEMONIC_JCXZ: case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_LOOP:
        case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE:
            return true;
        default:
            return false;
    }
}

bool Analysis::DecodeFlow(uint32_t va, Insn& out, JumpTable* jt) {
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!Decode(va, in, ops)) return false;
    out.len = in.length;
    out.flow = Flow::Normal;
    out.target = 0;
    auto m = in.mnemonic;
    if (m == ZYDIS_MNEMONIC_CALL) {
        if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            out.flow = Flow::Call;
            out.target = uint32_t(va + in.length + ops[0].imm.value.s);
        } else {
            out.flow = Flow::CallIndirect;
        }
    } else if (m == ZYDIS_MNEMONIC_JMP) {
        if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            out.flow = Flow::Jmp;
            out.target = uint32_t(va + in.length + ops[0].imm.value.s);
        } else {
            out.flow = Flow::JmpIndirect;
            if (jt && ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].mem.base == ZYDIS_REGISTER_NONE &&
                ops[0].mem.index != ZYDIS_REGISTER_NONE && ops[0].mem.scale == 4 && ops[0].mem.disp.has_displacement)
                ReadTable(va, ops[0], *jt);
        }
    } else if (IsCond(m)) {
        out.flow = Flow::Jcc;
        out.target = uint32_t(va + in.length + ops[0].imm.value.s);
    } else if (m == ZYDIS_MNEMONIC_RET) {
        out.flow = Flow::Ret;
    } else if (m == ZYDIS_MNEMONIC_IRETD || m == ZYDIS_MNEMONIC_IRET || m == ZYDIS_MNEMONIC_INT3 ||
               m == ZYDIS_MNEMONIC_HLT || m == ZYDIS_MNEMONIC_UD2 || m == ZYDIS_MNEMONIC_UD0 ||
               m == ZYDIS_MNEMONIC_UD1 || m == ZYDIS_MNEMONIC_INTO || m == ZYDIS_MNEMONIC_BOUND) {
        out.flow = Flow::Stop;
    } else if (in.meta.category == ZYDIS_CATEGORY_RET) {
        out.flow = Flow::Stop;  // far returns
    } else if (m == ZYDIS_MNEMONIC_JMP || in.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
        out.flow = Flow::Stop;  // far jumps
    }
    return true;
}

// "and r, mask; neg r; add r, end; jmp r": a jump back into a run of one byte instructions ending at end, as the
// MSVC memcpy tail copies its last dwords with a sled of movsd
bool Analysis::SledTable(uint32_t jmp_va, const uint32_t* ring, int nring, JumpTable& jt) {
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!Decode(jmp_va, in, ops) || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
    ZydisRegister r = ops[0].reg.value;
    // add right before the jump, then neg and and further back past instructions that leave r alone
    const ZydisMnemonic want[3] = {ZYDIS_MNEMONIC_ADD, ZYDIS_MNEMONIC_NEG, ZYDIS_MNEMONIC_AND};
    uint32_t imm[3] = {};
    int k = 0;
    for (int back = 2; back <= std::min(nring, 8) && k < 3; back++) {
        if (!Decode(ring[(nring - back) & 7], in, ops)) return false;
        bool writes_r = ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                        ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, ops[0].reg.value) == r;
        if (in.mnemonic == want[k] && writes_r && (k == 1 || ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)) {
            imm[k] = k == 1 ? 0 : uint32_t(ops[1].imm.value.u);
            k++;
        } else if (writes_r || k == 0) {
            return false;
        }
    }
    if (k < 3) return false;
    if (imm[2] > 255) return false;
    jt.insn = jmp_va;
    for (uint32_t i = 0; i <= imm[2]; i++)
        if (IsCode(imm[0] - i)) jt.targets.push_back(imm[0] - i);
    return !jt.targets.empty();
}

bool Analysis::Walk(uint32_t entry, bool trusted, std::vector<uint32_t>* calls, bool overlap) {
    struct Pending {
        uint32_t va;
        Insn insn;
    };
    std::vector<Pending> local;
    std::unordered_map<uint32_t, uint8_t> seen;
    std::vector<JumpTable> tables;
    std::vector<uint32_t> found_calls;
    std::vector<uint32_t> work{entry};
    uint32_t stop = 0;
    if (auto e = cfg.function_ends.find(entry); e != cfg.function_ends.end()) stop = e->second;
    auto conflict = [&](uint32_t va, uint32_t len) {
        for (uint32_t i = 0; i < len; i++) {
            uint32_t a = va + i;
            if (i && InTail(a)) continue;
            if (!IsCode(a)) return true;
            uint8_t c = cover_[a - lo_];
            if (c == 3 || (c == 1 && i) || (c == 2)) return true;
        }
        return false;
    };
    while (!work.empty()) {
        uint32_t va = work.back();
        work.pop_back();
        uint32_t ring[8] = {};
        int nring = 0;
        for (;;) {
            if (insns_.count(va) || seen.count(va)) break;
            if ((va != entry && cfg.function_ends.count(va)) || (stop && va >= stop)) break;
            if (!IsCode(va)) {
                if (trusted) break;
                return false;
            }
            if (cover_[va - lo_] != 0 && ((!trusted && !overlap) || cover_[va - lo_] == 3)) {
                if (trusted) break;
                return false;
            }
            Insn insn;
            JumpTable jt;
            if (!DecodeFlow(va, insn, &jt)) {
                if (!trusted) return false;
                insn.len = 1;
                insn.flow = Flow::Invalid;
                local.push_back({va, insn});
                seen[va] = 1;
                break;
            }
            if (stop && va + insn.len > stop) break;
            if (conflict(va, insn.len)) {
                // real control flow wins over a guessed entry decoded across these bytes: both decodes are kept
                bool data = false;
                for (uint32_t i = 0; i < insn.len; i++)
                    data |= !(i && InTail(va + i)) && (!IsCode(va + i) || cover_[va + i - lo_] == 3);
                if (!trusted && (!overlap || data)) return false;
                if (data) break;
            }
            local.push_back({va, insn});
            seen[va] = 1;
            ring[nring++ & 7] = va;
            if (insn.flow == Flow::Call) {
                if (insn.target == va + insn.len) {
                    // call $+5 pushes eip only
                } else if (IsCode(insn.target)) {
                    found_calls.push_back(insn.target);
                } else if (!trusted) {
                    return false;
                }
            } else if (insn.flow == Flow::Jcc) {
                if (!IsCode(insn.target) && !trusted) return false;
                work.push_back(insn.target);
            } else if (insn.flow == Flow::Jmp) {
                if (!IsCode(insn.target) && !trusted) return false;
                work.push_back(insn.target);
                break;
            } else if (insn.flow == Flow::JmpIndirect) {
                if (auto s = cfg.switches.find(va); s != cfg.switches.end()) {
                    jt = JumpTable{va, 0, s->second, {}};
                    for (uint32_t t : jt.targets) work.push_back(t);
                    tables.push_back(jt);
                } else if (jt.targets.empty() && SledTable(va, ring, nring, jt)) {
                    for (uint32_t t : jt.targets) work.push_back(t);
                    tables.push_back(jt);
                } else if (!jt.targets.empty()) {
                    // bound the table from a preceding "cmp idx, n"
                    ZydisDecodedInstruction ji;
                    ZydisDecodedOperand jo[ZYDIS_MAX_OPERAND_COUNT];
                    Decode(va, ji, jo);
                    ZydisRegister idx = jo[0].mem.index;
                    uint32_t bound = 0;
                    uint32_t byte_table = 0;
                    ZydisRegister byte_idx = ZYDIS_REGISTER_NONE;
                    for (int k = 1; k < 4 && k < nring; k++) {
                        uint32_t pa = ring[(nring - 1 - k) & 7];
                        ZydisDecodedInstruction pi;
                        ZydisDecodedOperand po[ZYDIS_MAX_OPERAND_COUNT];
                        if (!Decode(pa, pi, po)) break;
                        if (pi.mnemonic == ZYDIS_MNEMONIC_MOVZX && po[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                            ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, po[0].reg.value) == idx &&
                            po[1].type == ZYDIS_OPERAND_TYPE_MEMORY && po[1].size == 8 &&
                            po[1].mem.base != ZYDIS_REGISTER_NONE && po[1].mem.disp.has_displacement) {
                            byte_table = uint32_t(po[1].mem.disp.value);
                            byte_idx = po[1].mem.base;
                        }
                        if (pi.mnemonic == ZYDIS_MNEMONIC_CMP && po[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                            po[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                            ZydisRegister r = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, po[0].reg.value);
                            if (r == idx || r == byte_idx) bound = uint32_t(po[1].imm.value.u) + 1;
                            break;
                        }
                    }
                    if (bound && bound < 4096) {
                        uint32_t n = bound;
                        if (byte_table) {
                            uint32_t mx = 0;
                            for (uint32_t i = 0; i < bound; i++) {
                                int64_t o = img.Offset(byte_table + i);
                                if (o >= 0) mx = std::max<uint32_t>(mx, img.file[size_t(o)]);
                                if (byte_table + i >= lo_ && byte_table + i < hi_) cover_[byte_table + i - lo_] = 3;
                            }
                            n = mx + 1;
                        }
                        jt.targets.clear();
                        jt.slots.clear();
                        uint32_t base = uint32_t(ji.operand_count ? jo[0].mem.disp.value : jt.table);
                        for (uint32_t i = 0; i < n; i++) {
                            uint32_t t;
                            if (!img.Read32(base + i * 4, t)) break;
                            jt.targets.push_back(t);
                            jt.slots.push_back(base + i * 4);
                        }
                    }
                    if (!bound && byte_table) {
                        // unbounded index table: it runs while its bytes index the dword table
                        for (uint32_t a = byte_table; a >= lo_ && a < hi_ && a - byte_table < 4096 && cover_[a - lo_] == 0; a++) {
                            int64_t o = img.Offset(a);
                            if (o < 0 || img.file[size_t(o)] >= jt.targets.size()) break;
                            cover_[a - lo_] = 3;
                        }
                    }
                    for (uint32_t i = 0; i < jt.targets.size(); i++) {
                        uint32_t a = jt.slots[i];
                        if (a >= lo_ && a + 4 <= hi_)
                            for (int b = 0; b < 4; b++) cover_[a + b - lo_] = 3;
                        if (IsCode(jt.targets[i])) work.push_back(jt.targets[i]);
                    }
                    tables.push_back(jt);
                }
                break;
            } else if (insn.flow == Flow::Ret || insn.flow == Flow::Stop) {
                break;
            }
            va += insn.len;
            if (functions.count(va) && va != entry) break;  // fall into the next function
        }
    }
    for (auto& p : local) {
        insns_[p.va] = p.insn;
        if (p.va >= lo_ && p.va < hi_) {
            cover_[p.va - lo_] = 1;
            for (uint32_t i = 1; i < p.insn.len && p.va + i < hi_; i++) cover_[p.va + i - lo_] = 2;
        }
    }
    for (auto& t : tables) jump_tables[t.insn] = t;
    if (calls) calls->insert(calls->end(), found_calls.begin(), found_calls.end());
    return true;
}

// an untrusted entry must follow padding, a return or an unconditional jump
bool Analysis::PlausibleEntry(uint32_t va) const {
    const xbe::Section* sec = img.SectionAt(va);
    if (sec && sec->va == va) return true;
    auto byte = [&](uint32_t a) -> int {
        int64_t o = img.Offset(a);
        return o < 0 ? -1 : img.file[size_t(o)];
    };
    if (va >= lo_ && va < hi_ && cover_[va - lo_] == 1) {
        // inside decoded code: only right after a jmp, ret or padding
        int pb = byte(va - 1);
        if ((pb == 0xCC || pb == 0x90) && va > lo_ && cover_[va - 1 - lo_] == 0) return true;
        for (uint32_t n = 1; n <= 15 && va >= lo_ + n; n++) {
            auto it = insns_.find(va - n);
            if (it == insns_.end() || it->second.len != n) continue;
            Flow f = it->second.flow;
            if (f == Flow::Jmp || f == Flow::JmpIndirect || f == Flow::Ret || f == Flow::Stop) return true;
            int b = byte(va - n);
            return b == 0xCC || b == 0x90;
        }
        return false;
    }
    if (va > lo_ && cover_[va - 1 - lo_] == 3) return true;  // right after a jump table
    int p1 = byte(va - 1);
    if (p1 == 0xCC || p1 == 0x90 || p1 == 0xC3) return true;  // int3 or nop padding, ret
    if (byte(va - 3) == 0xC2 && byte(va - 1) == 0) return true;  // ret n, n below 256
    if (va >= lo_ + 5 && byte(va - 5) == 0xE9 && cover_[va - 5 - lo_] == 1) return true;  // jmp rel32
    if (va >= lo_ + 2 && byte(va - 2) == 0xEB && cover_[va - 2 - lo_] == 1) return true;  // jmp rel8
    if (byte(va - 6) == 0xFF && byte(va - 5) == 0x25) return true;  // jmp [import]
    if (byte(va - 2) == 0xFF && (byte(va - 1) & 0xF8) == 0xE0) return true;  // jmp reg
    for (uint32_t n = 1; n <= 15 && va >= lo_ + n; n++) {
        auto it = insns_.find(va - n);
        if (it == insns_.end() || it->second.len != n) continue;
        Flow f = it->second.flow;
        if (f == Flow::Jmp || f == Flow::JmpIndirect || f == Flow::Ret) return true;
    }
    return false;
}

void Analysis::Adopt(const std::vector<uint32_t>& calls, bool trusted) {
    std::deque<std::pair<uint32_t, bool>> q;
    for (uint32_t c : calls) q.push_back({c, trusted});
    while (!q.empty()) {
        auto [f, tr] = q.front();
        q.pop_front();
        if (functions.count(f)) continue;
        if (!IsCode(f)) continue;
        uint8_t cov = cover_[f - lo_];
        if (cov == 2 || cov == 3) continue;  // lands inside an instruction or data
        std::vector<uint32_t> more;
        if (cov == 1) {
            functions.insert(f);
            continue;
        }
        if (!Walk(f, tr, &more)) continue;
        functions.insert(f);
        for (uint32_t m : more) q.push_back({m, tr});
    }
}

void Analysis::SeedPointers(bool late) {
    std::vector<uint32_t> cands;
    std::set<uint32_t> vtable;  // runs of code pointers, entries even without padding before them
    for (auto& s : img.sections) {
        bool code = false;
        for (auto& n : cfg.code_sections) code |= s.name == n;
        for (auto& p : cfg.code_prefixes) code |= s.name.rfind(p, 0) == 0;
        uint32_t start = s.raw, end = s.raw + s.rsize;
        for (uint32_t o = start; o + 4 <= end; o += (code ? 1 : 4)) {
            uint32_t v = img.U32(o);
            if (!IsCode(v)) continue;
            if (code) continue;  // pointers inside code sections come only from jump tables
            cands.push_back(v);
            if ((o >= start + 4 && IsCode(img.U32(o - 4))) || (o + 8 <= end && IsCode(img.U32(o + 4)))) vtable.insert(v);
        }
    }
    std::sort(cands.begin(), cands.end());
    cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
    size_t added = 0, rejected = 0;
    // late: seeds rejected before the code above them was decoded, then decoded as the tail of the function that
    // jumps to them. A conditional jump target is a label inside a function.
    std::set<uint32_t> labels;
    if (late)
        for (auto& [va, insn] : insns_)
            if (insn.flow == Flow::Jcc) labels.insert(insn.target);
    for (uint32_t c : cands) {
        if (functions.count(c)) continue;
        uint8_t cov = cover_[c - lo_];
        if (late) {
            if (cov == 1 && PlausibleEntry(c) && !labels.count(c)) {
                functions.insert(c);
                added++;
            }
            continue;
        }
        bool vt = vtable.count(c) != 0;
        if (cov == 1) {
            if (PlausibleEntry(c) || (vt && insns_.count(c))) {
                functions.insert(c);
                added++;
            } else {
                rejected++;
            }
            continue;
        }
        if (cov != 0 || !PlausibleEntry(c)) {
            rejected++;
            continue;
        }
        std::vector<uint32_t> calls;
        if (Walk(c, false, &calls)) {
            functions.insert(c);
            added++;
            Adopt(calls, false);
        } else {
            rejected++;
        }
    }
    if (late) std::fprintf(stderr, "late pointer seeds: %zu\n", added);
    else std::fprintf(stderr, "pointer seeds: %zu accepted, %zu rejected\n", added, rejected);
}

void Analysis::GapFill() {
    size_t added = 0;
    for (uint32_t va = lo_; va < hi_;) {
        if (!code_[va - lo_] || cover_[va - lo_] != 0) {
            va++;
            continue;
        }
        int64_t o = img.Offset(va);
        uint8_t b = o >= 0 ? img.file[size_t(o)] : 0;
        if (b == 0xCC || b == 0x90 || b == 0x00 || !PlausibleEntry(va)) {
            va++;
            continue;
        }
        std::vector<uint32_t> calls;
        if (Walk(va, false, &calls)) {
            functions.insert(va);
            added++;
            Adopt(calls, false);
        } else {
            va++;
        }
    }
    std::fprintf(stderr, "gap fill: %zu functions\n", added);
}

void Analysis::Run() {
    std::deque<uint32_t> q;
    q.push_back(img.entry);
    for (auto& f : cfg.functions) q.push_back(f.first);
    auto drain = [&] {
        while (!q.empty()) {
            uint32_t f = q.front();
            q.pop_front();
            if (functions.count(f) || !IsCode(f)) continue;
            functions.insert(f);
            std::vector<uint32_t> calls;
            Walk(f, true, &calls);
            q.insert(q.end(), calls.begin(), calls.end());
        }
    };
    drain();
    std::fprintf(stderr, "direct: %zu functions, %zu instructions\n", functions.size(), insns_.size());
    if (cfg.pointer_seeds) SeedPointers();
    if (cfg.gap_fill) GapFill();
    // immediates that point at code are callbacks: make sure they are entries. A pointer seed or gap fill guess
    // decoded across one's body first does not disqualify it.
    size_t imm = 0;
    std::vector<uint32_t> extra;
    for (auto& [va, insn] : insns_) {
        if (insn.flow != Flow::Normal) continue;
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!Decode(va, in, ops)) continue;
        if (in.mnemonic != ZYDIS_MNEMONIC_PUSH && in.mnemonic != ZYDIS_MNEMONIC_MOV) continue;
        for (int i = 0; i < in.operand_count_visible; i++)
            if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                uint32_t v = uint32_t(ops[i].imm.value.u);
                if (IsCode(v) && !functions.count(v) && PlausibleEntry(v)) extra.push_back(v);
            }
    }
    std::sort(extra.begin(), extra.end());
    extra.erase(std::unique(extra.begin(), extra.end()), extra.end());
    for (uint32_t v : extra) {
        if (functions.count(v)) continue;
        uint8_t cov = cover_[v - lo_];
        if (cov == 1) {
            functions.insert(v);
            imm++;
        } else if (cov == 0) {
            std::vector<uint32_t> calls;
            if (Walk(v, false, &calls, true)) {
                functions.insert(v);
                imm++;
                Adopt(calls, false);
            }
        }
    }
    EhContinuations();
    // every direct call from decoded code names a function
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<uint32_t> targets;
        for (auto& [va, insn] : insns_)
            if (insn.flow == Flow::Call && IsCode(insn.target) && !functions.count(insn.target)) targets.push_back(insn.target);
        for (uint32_t t : targets) {
            if (functions.count(t)) continue;
            uint8_t cov = cover_[t - lo_];
            if (cov == 2 || cov == 3) continue;
            std::vector<uint32_t> calls;
            if (cov == 0) Walk(t, true, &calls);
            functions.insert(t);
            changed = true;
        }
    }
    if (cfg.pointer_seeds) SeedPointers(true);
    size_t bytes = 0, overlaps = 0;
    for (auto& [va, insn] : insns_) {
        bytes += insn.len;
        for (uint32_t i = 1; i < insn.len; i++)
            if (insns_.count(va + i)) overlaps++;
    }
    std::fprintf(stderr, "overlapping decodes: %zu\n", overlaps);
    std::fprintf(stderr, "total: %zu functions, %zu instructions, %zu code bytes of %u, %zu jump tables, %zu callback entries\n",
                 functions.size(), insns_.size(), bytes, hi_ - lo_, jump_tables.size(), imm);
}

void Analysis::MakeFunction(uint32_t va) {
    if (functions.count(va) || !IsCode(va)) return;
    uint8_t cov = cover_[va - lo_];
    if (cov == 2 || cov == 3) return;
    std::vector<uint32_t> calls;
    if (cov == 0) Walk(va, true, &calls);
    functions.insert(va);
}

// MSVC C++ EH: a frame's handler thunk is "mov eax, FuncInfo; jmp __CxxFrameHandler". Its catch blocks and unwind
// actions are called through pointers, and a catch block returns in eax where its function resumes.
void Analysis::EhContinuations() {
    std::vector<uint32_t> thunks(functions.begin(), functions.end());
    size_t catches = 0;
    for (uint32_t f : thunks) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!Decode(f, in, ops) || in.mnemonic != ZYDIS_MNEMONIC_MOV || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            ops[0].reg.value != ZYDIS_REGISTER_EAX || ops[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
            continue;
        uint32_t info = uint32_t(ops[1].imm.value.u), magic = 0, max_state = 0, unwind = 0, tries = 0, map = 0;
        if (!Decode(f + in.length, in, ops) || in.mnemonic != ZYDIS_MNEMONIC_JMP) continue;
        // FuncInfo: magic, maxState, pUnwindMap, nTryBlocks, pTryBlockMap
        if (!img.Read32(info, magic) || magic < 0x19930520 || magic > 0x19930522) continue;
        img.Read32(info + 4, max_state);
        img.Read32(info + 8, unwind);
        img.Read32(info + 12, tries);
        img.Read32(info + 16, map);
        // unwind map entries are 8 bytes, the action at +4
        for (uint32_t i = 0; i < max_state && i < 4096; i++) {
            uint32_t action = 0;
            if (img.Read32(unwind + i * 8 + 4, action) && action) MakeFunction(action);
        }
        // try blocks are 20 bytes, nCatches at +12, handler array at +16; handlers are 16 bytes, the address at +12
        for (uint32_t t = 0; t < tries && t < 1024; t++) {
            uint32_t count = 0, handlers = 0;
            img.Read32(map + t * 20 + 12, count);
            img.Read32(map + t * 20 + 16, handlers);
            for (uint32_t h = 0; h < count && h < 64; h++) {
                uint32_t block = 0;
                if (!img.Read32(handlers + h * 16 + 12, block) || !IsCode(block)) continue;
                MakeFunction(block);
                // the block's rets are "mov eax, continuation; ret"
                std::vector<uint32_t> body = Body(block);
                for (size_t k = 1; k < body.size(); k++) {
                    const Insn* r = InsnAt(body[k]);
                    if (!r || r->flow != Flow::Ret || !Decode(body[k - 1], in, ops) || body[k - 1] + in.length != body[k]) continue;
                    if (in.mnemonic == ZYDIS_MNEMONIC_MOV && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                        ops[0].reg.value == ZYDIS_REGISTER_EAX && ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                        IsCode(uint32_t(ops[1].imm.value.u))) {
                        uint32_t c = uint32_t(ops[1].imm.value.u);
                        MakeFunction(c);
                        if (functions.count(c)) continuations.insert(c);
                    }
                }
                catches++;
            }
        }
    }
    std::fprintf(stderr, "c++ eh: %zu catch blocks, %zu continuations\n", catches, continuations.size());
}

std::vector<uint32_t> Analysis::Body(uint32_t entry) const {
    std::vector<uint32_t> out;
    std::set<uint32_t> seen;
    std::vector<uint32_t> work{entry};
    uint32_t stop = 0;
    if (auto e = cfg.function_ends.find(entry); e != cfg.function_ends.end()) stop = e->second;
    while (!work.empty()) {
        uint32_t va = work.back();
        work.pop_back();
        if (seen.count(va)) continue;
        if (va != entry && functions.count(va)) continue;
        auto it = insns_.find(va);
        if (it == insns_.end()) continue;
        if (stop && va + it->second.len > stop) continue;
        seen.insert(va);
        out.push_back(va);
        const Insn& in = it->second;
        uint32_t next = va + in.len;
        switch (in.flow) {
            case Flow::Normal:
            case Flow::Call:
            case Flow::CallIndirect:
                work.push_back(next);
                break;
            case Flow::Jcc:
                work.push_back(next);
                work.push_back(in.target);
                break;
            case Flow::Jmp:
                work.push_back(in.target);
                break;
            case Flow::JmpIndirect: {
                auto jt = jump_tables.find(va);
                if (jt != jump_tables.end())
                    for (uint32_t t : jt->second.targets) work.push_back(t);
                break;
            }
            default:
                break;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
