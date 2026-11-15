#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "codegen.h"

namespace fs = std::filesystem;

static std::string F(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string F(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

// rewriting identical files would force a full rebuild of the generated code
static void WriteIfChanged(const fs::path& p, const std::string& text) {
    std::ifstream in(p, std::ios::binary);
    if (in) {
        std::string old((std::istreambuf_iterator<char>(in)), {});
        if (old == text) return;
    }
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string CodeGen::EmitFunction(uint32_t entry) {
    entry_ = entry;
    pending_.valid = false;
    std::vector<uint32_t> body = an_.Body(entry);
    body_set_.clear();
    body_set_.insert(body.begin(), body.end());
    labels_.clear();
    // branch targets need labels, collect them before emitting so the peephole never spans one
    std::set<uint32_t> targets;
    for (uint32_t va : body) {
        const Insn* in = an_.InsnAt(va);
        if (!in) continue;
        if (in->flow == Flow::Jcc || in->flow == Flow::Jmp) targets.insert(in->target);
        if (in->flow == Flow::JmpIndirect) {
            auto jt = an_.jump_tables.find(va);
            if (jt != an_.jump_tables.end()) targets.insert(jt->second.targets.begin(), jt->second.targets.end());
        }
    }
    for (size_t i = 0; i < body.size(); i++) {
        const Insn* in = an_.InsnAt(body[i]);
        if (!in) continue;
        uint32_t next = body[i] + in->len;
        bool falls = in->flow == Flow::Normal || in->flow == Flow::Call || in->flow == Flow::CallIndirect || in->flow == Flow::Jcc;
        if (falls && (i + 1 >= body.size() || body[i + 1] != next)) targets.insert(next);
    }
    if (!an_.cfg.hooks.empty()) {
        ret_bytes_ = RetBytes(body);
        for (uint32_t va : body)
            for (auto [it, e] = an_.cfg.hooks.equal_range(va); it != e; ++it)
                for (auto* x : {&it->second.on_true, &it->second.on_false})
                    if (x->set && !x->ret) targets.insert(x->to);
    }
    body_.clear();
    if (body.empty()) Line(F("\tx86_trap(ctx, 0x%X, \"function was not decoded\");", entry));
    for (size_t i = 0; i < body.size(); i++) {
        uint32_t va = body[i];
        if ((targets.count(va) && va != entry) || (va == entry && body.front() != entry)) {
            Line(F("loc_%08X:", va));
            pending_.valid = false;
        }
        if (!an_.cfg.hooks.empty()) EmitHooks(va, false);
        const Insn* info = an_.InsnAt(va);
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!info || info->flow == Flow::Invalid || !an_.Decode(va, in, ops)) {
            Line(F("\tx86_trap(ctx, 0x%X, \"undecodable instruction\");", va));
            pending_.valid = false;
            continue;
        }
        Line("\t// " + F("%08X ", va) + Text(va, in, ops));
        uint32_t next_body = i + 1 < body.size() ? body[i + 1] : 0;
        EmitInsn(va, in, ops, next_body);
        if (!an_.cfg.hooks.empty()) EmitHooks(va, true);
        bool falls = info->flow == Flow::Normal || info->flow == Flow::Call || info->flow == Flow::CallIndirect ||
                     info->flow == Flow::Jcc;
        uint32_t next = va + info->len;
        if (falls && next_body != next) {
            if (an_.InsnAt(next) || an_.functions.count(next))
                Line("\t" + Target(next));
            else
                Line(F("\tx86_trap(ctx, 0x%X, \"fell off decoded code\");", next));
        }
    }
    // a function that registers an SEH frame (mov fs:[0], ...) may be where a C++ catch resumes: the CRT's jump to
    // the continuation throws X86Resume, caught by the innermost such frame whose stack holds the resume esp
    bool seh = false;
    for (uint32_t va : body) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (an_.Decode(va, in, ops) && in.mnemonic == ZYDIS_MNEMONIC_MOV && ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
            ops[0].mem.segment == ZYDIS_REGISTER_FS && ops[0].mem.base == ZYDIS_REGISTER_NONE &&
            ops[0].mem.index == ZYDIS_REGISTER_NONE && ops[0].mem.disp.value == 0)
            seh = true;
    }
    std::string out;
    out += F("X86_FUNC(__imp__%s) {\n", FuncName(entry).c_str());
    if (seh) out += "\tconst uint32_t x86_frame = ctx.esp;\n\ttry {\n";
    out += "\tX86_LOAD_FLAGS();\n\tuint32_t ca = 0, cb = 0;\n\t(void)ca; (void)cb;\n";
    // code below the entry (a shared tail reached by a jump) is emitted first, so enter by label
    bool entry_first = !body.empty() && body.front() == entry;
    if (!entry_first) out += F("\tgoto loc_%08X;\n", entry);
    if (labels_.count(entry) && entry_first) out += F("loc_%08X:;\n", entry);
    out += body_;
    if (seh) out += "\t} catch (const X86Resume& r) {\n\t\tif (r.esp >= x86_frame) throw;\n\t\tx86_resume(ctx, base, r);\n\t}\n";
    out += "}\n";
    // a replace hook defines the function's own name in the host: aliasing it here would define it twice
    // a name the host binds to: a replace hook, or an entry from [symbols] that the host implements. Aliasing it here
    // would define it twice on Mach-O, where an alias cannot be weak.
    auto host_bound = [this](uint32_t va) {
        for (auto [it, e] = an_.cfg.hooks.equal_range(va); it != e; ++it)
            if (!it->second.after && it->second.call == FuncName(va)) return true;
        for (auto& [name, at] : an_.cfg.symbols)
            if (at && at == va) return true;
        return false;
    };
    if (an_.cfg.trace.count(entry))
        out += F("X86_TRACED_FUNC(%s, __imp__%s, 0x%XU)\n\n", FuncName(entry).c_str(), FuncName(entry).c_str(), entry);
    else  // weak, so a host function of the same name replaces it
        out += F("X86_WEAK_FUNC(%s, __imp__%s)\n\n", FuncName(entry).c_str(), FuncName(entry).c_str());
    return out;
}

int CodeGen::RetBytes(const std::vector<uint32_t>& body) const {
    int n = -1;
    for (uint32_t va : body) {
        const Insn* i = an_.InsnAt(va);
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!i || i->flow != Flow::Ret) continue;
        if (!an_.Decode(va, in, ops)) return -1;
        int k = in.operand_count_visible && ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? int(ops[0].imm.value.u) : 0;
        if (n >= 0 && n != k) return -1;
        n = k;
    }
    return n;
}

bool CodeGen::CheckHooks(const std::vector<uint32_t>& funcs) const {
    bool ok = true;
    auto fail = [&](const Hook& h, uint32_t f, const char* why) {
        std::fprintf(stderr, "[[hook]] %s at %08X%s: %s\n", h.call.c_str(), h.at, f ? F(" in %s", FuncName(f).c_str()).c_str() : "",
                     why);
        ok = false;
    };
    std::set<uint32_t> placed;
    for (uint32_t f : funcs) {
        std::vector<uint32_t> body = an_.Body(f);
        std::set<uint32_t> in(body.begin(), body.end());
        for (uint32_t va : body)
            for (auto [it, e] = an_.cfg.hooks.equal_range(va); it != e; ++it) {
                const Hook& h = it->second;
                Flow flow = an_.InsnAt(va)->flow;
                placed.insert(va);
                if (h.after && flow != Flow::Normal && flow != Flow::Call && flow != Flow::CallIndirect)
                    fail(h, f, "after = true on a control transfer");
                for (auto* x : {&h.on_true, &h.on_false}) {
                    if (x->ret && RetBytes(body) < 0)
                        fail(h, f, "\"ret\" needs a function with one ret form, name an address instead");
                    if (x->set && !x->ret && !in.count(x->to)) fail(h, f, "jump target is not an instruction of the function");
                }
            }
    }
    std::map<std::string, bool> kind;
    for (auto& [va, h] : an_.cfg.hooks) {
        if (!placed.count(va)) fail(h, 0, "not an instruction in a recompiled function");
        bool ident = !h.call.empty() && !std::isdigit((unsigned char)h.call[0]);
        for (char c : h.call) ident &= std::isalnum((unsigned char)c) || c == '_';
        if (!ident) fail(h, 0, "call is not an identifier");
        if (auto [k, fresh] = kind.emplace(h.call, h.Returns()); !fresh && k->second != h.Returns())
            fail(h, 0, "call used both with and without on_true/on_false");
    }
    return ok;
}

// the hook sees guest flags in ctx and may change them, so they are stored before and reloaded after
void CodeGen::EmitHooks(uint32_t va, bool after) {
    for (auto [it, e] = an_.cfg.hooks.equal_range(va); it != e; ++it) {
        const Hook& h = it->second;
        if (h.after != after) continue;
        auto leave = [&](const Hook::Exit& x) {
            if (x.ret) return F("ctx.esp += %d; return;", 4 + ret_bytes_);  // return address plus the ret n bytes
            return (x.to <= va ? std::string("X86_BARRIER(); ") : std::string()) + Target(x.to);
        };
        Line("\tX86_SAVE_FLAGS();");
        std::string reload = "cf = ctx.cf; zf = ctx.zf; sf = ctx.sf; of = ctx.of; pf = ctx.pf;";
        if (!h.Returns()) {
            Line(F("\t%s(ctx, base); %s", h.call.c_str(), reload.c_str()));
        } else {
            std::string s = F("\t{ bool hook = %s(ctx, base); %s", h.call.c_str(), reload.c_str());
            if (h.on_true.set) s += " if (hook) { " + leave(h.on_true) + " }";
            if (h.on_false.set) s += (h.on_true.set ? " else { " : " if (!hook) { ") + leave(h.on_false) + " }";
            Line(s + " }");
        }
        pending_.valid = false;
    }
}

bool CodeGen::Run() {
    std::vector<uint32_t> funcs(an_.functions.begin(), an_.functions.end());
    std::sort(funcs.begin(), funcs.end());
    if (!an_.cfg.hooks.empty() && !CheckHooks(funcs)) return false;

    // symbols the host binds to by name; one missing from this image gets a placeholder address whose
    // recompiled body traps, so the host override still links
    std::string syms = "#pragma once\n";
    syms += F("#define IMAGE_XBE \"%s\"\n", fs::path(an_.cfg.xbe_path).filename().string().c_str());
    std::vector<uint32_t> absent;
    std::map<std::string, uint32_t> functions = an_.cfg.symbols;
    functions.insert(an_.cfg.names.begin(), an_.cfg.names.end());
    std::string ids;   // [xbe] named: every name, for xdk/hle.h
    std::string rows;  // and its address in this image, exported as X86Symbols
    for (auto& [name, va] : functions) {
        uint32_t a = va ? va : 0xFFFF0000u + uint32_t(absent.size());
        if (!va) absent.push_back(a);
        syms += F("#define SYM_%s %08X\n#define HAS_%s %d\n", name.c_str(), a, name.c_str(), va ? 1 : 0);
        if (!an_.cfg.named) continue;
        auto [at, fresh] = named_.emplace(a, name);
        if (!fresh) {
            std::fprintf(stderr, "[xbe] named: %s and %s are one function, %08X\n", at->second.c_str(), name.c_str(), a);
            return false;
        }
        ids += " \\\n\tX(" + name + ")";
        rows += F("\t{\"%s\", 0x%Xu},\n", name.c_str(), a);
    }
    for (auto& [name, va] : an_.cfg.data) {
        syms += F("#define SYM_%s %08X\n#define HAS_%s %d\n", name.c_str(), va, name.c_str(), va ? 1 : 0);
        ids += " \\\n\tX(" + name + ")";
        rows += F("\t{\"%s\", 0x%Xu},\n", name.c_str(), va);
    }
    if (an_.cfg.named) syms += "#define IMAGE_NAMED 1\n#define IMAGE_SYMBOLS(X)" + ids + "\n";
    for (auto& [name, v] : an_.cfg.layout) syms += F("#define XDK_%s 0x%X\n", name.c_str(), v);
    // argument maps, one X(name, location...) row each; xdk/hle.h makes the table
    if (!an_.cfg.abi.empty()) {
        syms += "#define IMAGE_ABI(X)";
        for (auto& [name, locs] : an_.cfg.abi) {
            syms += " \\\n\tX(" + name;
            for (auto& l : locs) syms += ", " + l;
            syms += ")";
        }
        syms += "\n";
    }
    WriteIfChanged(out_ / "image_symbols.h", syms);

    // shared declarations
    std::string shared = "#pragma once\n#include \"cpu/x86_ops.h\"\n\n";
    for (uint32_t f : funcs) shared += F("X86_EXTERN_FUNC(%s);\n", FuncName(f).c_str());
    for (uint32_t f : absent) shared += F("X86_EXTERN_FUNC(%s);\n", FuncName(f).c_str());
    std::map<std::string, bool> hooks;
    for (auto& [va, h] : an_.cfg.hooks) hooks.emplace(h.call, h.Returns());
    for (auto& [name, returns] : hooks)
        shared += F("X86_HOST_API %s %s(X86Context& ctx, uint8_t* base);\n", returns ? "bool" : "void", name.c_str());
    WriteIfChanged(out_ / "x86_recomp_shared.h", shared);

    // the executable exports the hooks under their C++ names (app/game_code.cpp includes this)
    std::string hook_exports = "#pragma once\n";
    for (auto& [name, returns] : hooks) {
        const char* result = returns ? "_N" : "X";
        hook_exports += F("#pragma comment(linker, \"/EXPORT:?%s@@YA%sAEAUX86Context@@PEAE@Z\")\n", name.c_str(), result);
    }
    WriteIfChanged(out_ / "hook_exports.h", hook_exports);

    // partition by instruction count so the files compile in similar time
    size_t total = 0;
    std::vector<size_t> weight(funcs.size());
    for (size_t i = 0; i < funcs.size(); i++) {
        weight[i] = an_.Body(funcs[i]).size() + 8;
        total += weight[i];
    }
    uint32_t parts = std::max<uint32_t>(1, an_.cfg.partitions);
    size_t per = total / parts + 1;
    size_t fi = 0;
    for (uint32_t p = 0; p < parts; p++) {
        std::string text = "#include \"x86_recomp_shared.h\"\n\n";
        size_t acc = 0;
        while (fi < funcs.size() && (acc < per || p == parts - 1)) {
            text += EmitFunction(funcs[fi]);
            acc += weight[fi];
            fi++;
        }
        WriteIfChanged(out_ / F("x86_recomp.%u.cpp", p), text);
    }

    std::string map = "#include \"x86_recomp_shared.h\"\n\n";
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    for (auto& s : an_.img.sections) {
        bool code = false;
        for (auto& n : an_.cfg.code_sections) code |= s.name == n;
        for (auto& pfx : an_.cfg.code_prefixes) code |= s.name.rfind(pfx, 0) == 0;
        if (!code) continue;
        lo = std::min(lo, s.va);
        hi = std::max(hi, s.va + s.rsize);
    }
    map += F("const uint32_t X86CodeBase = 0x%Xu;\nconst uint32_t X86CodeEnd = 0x%Xu;\n", lo, hi);
    map += F("const uint32_t X86ImageBase = 0x%Xu;\nconst uint32_t X86ImageEnd = 0x%Xu;\n", an_.img.base,
             an_.img.base + an_.img.image_size);
    map += F("const uint32_t X86EntryPoint = 0x%Xu;\nconst uint32_t X86KernelThunk = 0x%Xu;\n\n", an_.img.entry,
             an_.img.kernel_thunk);
    map += "const X86FuncMapping X86FuncMappings[] = {\n";
    for (uint32_t f : funcs) map += F("\t{0x%Xu, %s},\n", f, FuncName(f).c_str());
    map += "\t{0, nullptr},\n};\n";
    map += "\nconst uint32_t X86Continuations[] = {\n";
    for (uint32_t c : an_.continuations) map += F("\t0x%Xu,\n", c);
    map += "\t0,\n};\n";
    if (an_.cfg.named) map += "\nconst X86Symbol X86Symbols[] = {\n" + rows + "\t{nullptr, 0},\n};\n";
    // a replace hook defines these names in the host, so the image does not alias them
    // a name the host binds to: a replace hook, or an entry from [symbols] that the host implements. Aliasing it here
    // would define it twice on Mach-O, where an alias cannot be weak.
    auto host_bound = [this](uint32_t va) {
        for (auto [it, e] = an_.cfg.hooks.equal_range(va); it != e; ++it)
            if (!it->second.after && it->second.call == FuncName(va)) return true;
        for (auto& [name, at] : an_.cfg.symbols)
            if (at && at == va) return true;
        return false;
    };
    for (uint32_t f : absent) {
        map += F("\nX86_FUNC(__imp__%s) { x86_trap(ctx, 0x%Xu, \"not in this image\"); }\n", FuncName(f).c_str(), f);
        map += F("X86_WEAK_FUNC(%s, __imp__%s)\n", FuncName(f).c_str(), FuncName(f).c_str());
    }
    WriteIfChanged(out_ / "x86_func_mapping.cpp", map);

    // the game code DLL exports its tables and each recompiled body (one name a function: PE allows 65535); the
    // executable's import library also takes a function's own name, for the calls it makes where it has no override
    std::string library = F("LIBRARY %s\nEXPORTS\n", an_.cfg.library.c_str());
    std::string exports;
    std::string aliases;
    for (const char* table : {"X86FuncMappings", "X86Continuations", "X86CodeBase", "X86CodeEnd", "X86EntryPoint"})
        exports += F("    %s DATA\n", table);
    if (an_.cfg.named) exports += "    X86Symbols DATA\n";
    for (const std::vector<uint32_t>* list : {&funcs, &absent}) {
        for (uint32_t f : *list) {
            std::string name = FuncName(f);
            exports += F("    __imp__%s\n", name.c_str());
            aliases += F("    %s == __imp__%s\n", name.c_str(), name.c_str());
        }
    }
    WriteIfChanged(out_ / "exports.def", library + exports);
    WriteIfChanged(out_ / "imports.def", library + exports + aliases);

    std::string stats = F("functions %zu\ninstructions %zu\n", funcs.size(), insn_count_);
    for (auto& [k, v] : unimpl_) stats += F("unimplemented %s %zu\n", k.c_str(), v);
    WriteIfChanged(out_ / "x86_recomp_stats.txt", stats);
    std::fprintf(stderr, "%s", stats.c_str());
    return true;
}
