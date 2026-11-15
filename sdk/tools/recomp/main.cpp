#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

#include "analysis.h"
#include "codegen.h"

namespace fs = std::filesystem;

static uint32_t ParseAddr(std::string_view s) { return uint32_t(std::stoul(std::string(s), nullptr, 0)); }

static std::optional<int64_t> Num(const toml::table* t, const char* key) {
    return t ? (*t)[key].value<int64_t>() : std::nullopt;
}

// "ret" or an address, as a number or a string
static bool ParseExit(const toml::node* n, Hook::Exit& e) {
    if (!n) return true;
    e.set = true;
    if (auto i = n->value<int64_t>()) {
        e.to = uint32_t(*i);
        return true;
    }
    auto s = n->value<std::string>();
    if (!s) return false;
    if (*s == "ret") {
        e.ret = true;
        return true;
    }
    try {
        e.to = ParseAddr(*s);
    } catch (...) {
        return false;
    }
    return true;
}

// Keys beyond [xbe], [functions], [symbols], [data] and [layout]:
//   [xbe] data_ranges = [[lo, hi], ...]  bytes in code sections that are never code, hi exclusive
//   [xbe] trace = [address, ...]  log each call of these functions (caller, arguments) and its eax
//   [[hook]] at, call, after, on_true, on_false  call a host function before or after a guest instruction;
//     on_true/on_false make it return bool and name "ret" or an instruction of the same function to jump to
//   [[function]] at, end  an entry decoded no further than end (exclusive); it also stops other walks
//   [[switch]] at, targets  the targets of an indirect jmp, replacing table detection
//   [xbe] xdk = build  includes <--xdk folder>/<build>/layout.toml
//   [xbe] named = true  functions in [symbols] and [names] are called sub_<name>, not sub_<address>, and the DLL
//     exports X86Symbols, every name's address in this image
//   [names] name = address  the game's own functions the host replaces or calls, with [xbe] named
//   [abi] name = ["esi", "stack", ...]  where a [symbols] function takes each API argument when the image does not
//     use the convention the host declares: a register, or "stack" for the next stack slot, which the callee pops
static bool LoadConfig(const fs::path& path, Config& cfg, const fs::path& xdk_dir) {
    toml::table t;
    try {
        t = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "%s: %s\n", path.string().c_str(), std::string(e.description()).c_str());
        return false;
    }
    fs::path dir = path.parent_path();
    if (auto v = t["xbe"]["xdk"].value<int64_t>()) {
        if (xdk_dir.empty()) {
            std::fprintf(stderr, "%s: [xbe] xdk = %lld needs --xdk <the SDK's xdk folder>\n", path.string().c_str(), (long long)*v);
            return false;
        }
        fs::path layout = xdk_dir / std::to_string(*v) / "layout.toml";
        if (!fs::exists(layout)) {
            std::fprintf(stderr, "%s: [xbe] xdk = %lld: %s does not exist\n", path.string().c_str(), (long long)*v,
                         layout.string().c_str());
            return false;
        }
        if (!LoadConfig(layout, cfg, xdk_dir)) return false;
    }
    auto includes = t["includes"].as_array();
    if (includes)
        for (auto& i : *includes)
            if (auto s = i.value<std::string>())
                if (!LoadConfig(dir / *s, cfg, xdk_dir)) return false;
    if (auto v = t["xbe"]["path"].value<std::string>()) cfg.xbe_path = *v;
    if (auto v = t["xbe"]["title"].value<int64_t>()) cfg.title = uint32_t(*v);
    if (auto v = t["xbe"]["entry"].value<int64_t>()) cfg.entry = uint32_t(*v);
    if (auto a = t["xbe"]["trace"].as_array())
        for (auto& i : *a) cfg.trace.insert(uint32_t(i.value_or(int64_t(0))));
    if (auto v = t["xbe"]["out"].value<std::string>()) cfg.out_dir = *v;
    if (auto v = t["xbe"]["library"].value<std::string>()) cfg.library = *v;
    if (auto v = t["xbe"]["partitions"].value<int64_t>()) cfg.partitions = uint32_t(*v);
    if (auto v = t["xbe"]["gap_fill"].value<bool>()) cfg.gap_fill = *v;
    if (auto v = t["xbe"]["pointer_seeds"].value<bool>()) cfg.pointer_seeds = *v;
    if (auto v = t["xbe"]["named"].value<bool>()) cfg.named = *v;
    if (auto a = t["xbe"]["code_sections"].as_array())
        for (auto& i : *a)
            if (auto s = i.value<std::string>()) cfg.code_sections.push_back(*s);
    if (auto a = t["xbe"]["code_prefixes"].as_array())
        for (auto& i : *a)
            if (auto s = i.value<std::string>()) cfg.code_prefixes.push_back(*s);
    if (auto a = t["xbe"]["data_ranges"].as_array())
        for (auto& i : *a)
            if (auto r = i.as_array(); r && r->size() == 2)
                cfg.data_ranges.push_back({uint32_t(r->get(0)->value<int64_t>().value_or(0)),
                                           uint32_t(r->get(1)->value<int64_t>().value_or(0))});
    if (auto f = t["functions"].as_table())
        for (auto& [k, v] : *f) cfg.functions[ParseAddr(k.str())] = v.value<std::string>().value_or("");
    for (auto [key, out] : {std::pair{"symbols", &cfg.symbols}, std::pair{"data", &cfg.data}, std::pair{"names", &cfg.names}})
        if (auto f = t[key].as_table())
            for (auto& [k, v] : *f) (*out)[std::string(k.str())] = uint32_t(v.value<int64_t>().value_or(0));
    if (auto f = t["layout"].as_table())
        for (auto& [k, v] : *f)
            if (auto i = v.value<int64_t>()) cfg.layout[std::string(k.str())] = uint32_t(*i);
    if (auto f = t["abi"].as_table())
        for (auto& [k, v] : *f) {
            std::string name(k.str());
            auto fail = [&](const std::string& why) {
                std::fprintf(stderr, "%s: [abi] %s: %s\n", path.string().c_str(), name.c_str(), why.c_str());
                return false;
            };
            auto a = v.as_array();
            if (!a) return fail("not an array of argument locations");
            if (a->size() > 16) return fail("more than 16 arguments");
            std::vector<std::string> locs;
            for (auto& x : *a) {
                std::string s = x.value<std::string>().value_or("");
                static const std::set<std::string> known{"stack", "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"};
                if (!known.count(s)) return fail("\"" + s + "\" is not stack, eax, ebx, ecx, edx, esi, edi or ebp");
                if (s != "stack" && std::count(locs.begin(), locs.end(), s)) return fail(s + " holds two arguments");
                locs.push_back(s);
            }
            cfg.abi[name] = locs;
        }
    auto bad = [&](const char* key) {
        std::fprintf(stderr, "%s: bad [[%s]] entry\n", path.string().c_str(), key);
        return false;
    };
    if (auto a = t["hook"].as_array())
        for (auto& e : *a) {
            auto h = e.as_table();
            auto at = Num(h, "at");
            auto call = h ? (*h)["call"].value<std::string>() : std::nullopt;
            Hook k;
            if (!at || !call || !ParseExit(h->get("on_true"), k.on_true) || !ParseExit(h->get("on_false"), k.on_false))
                return bad("hook");
            k.at = uint32_t(*at);
            k.call = *call;
            k.after = (*h)["after"].value_or(false);
            cfg.hooks.emplace(k.at, k);
        }
    if (auto a = t["function"].as_array())
        for (auto& e : *a) {
            auto at = Num(e.as_table(), "at");
            if (!at) return bad("function");
            cfg.functions.emplace(uint32_t(*at), "");
            cfg.function_ends[uint32_t(*at)] = uint32_t(Num(e.as_table(), "end").value_or(0));
        }
    if (auto a = t["switch"].as_array())
        for (auto& e : *a) {
            auto h = e.as_table();
            auto at = Num(h, "at");
            auto ts = h ? (*h)["targets"].as_array() : nullptr;
            if (!at || !ts || ts->empty()) return bad("switch");
            auto& v = cfg.switches[uint32_t(*at)];
            v.clear();
            for (auto& x : *ts) {
                auto i = x.value<int64_t>();
                if (!i) return bad("switch");
                v.push_back(uint32_t(*i));
            }
        }
    for (auto& [name, va] : cfg.symbols)
        if (va) cfg.functions.emplace(va, name);
    for (auto& [name, va] : cfg.names)
        if (va) cfg.functions.emplace(va, name);
    return true;
}

// overrides that analysis could not apply are config mistakes
static bool CheckOverrides(const Analysis& an) {
    bool ok = true;
    for (auto& [at, end] : an.cfg.function_ends)
        if (!an.functions.count(at) || (end && end <= at)) {
            std::fprintf(stderr, "[[function]] %08X: not decodable code or end not above it\n", at);
            ok = false;
        }
    for (auto& [at, targets] : an.cfg.switches) {
        const Insn* i = an.InsnAt(at);
        if (!i || i->flow != Flow::JmpIndirect) {
            std::fprintf(stderr, "[[switch]] %08X: not a decoded indirect jmp\n", at);
            ok = false;
        }
        for (uint32_t t : targets)
            if (!an.InsnAt(t)) {
                std::fprintf(stderr, "[[switch]] %08X: target %08X is not a decoded instruction\n", at, t);
                ok = false;
            }
    }
    return ok;
}

int main(int argc, char** argv) {
    std::vector<std::string> args;
    fs::path xdk_dir;
    bool usage = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--xdk")) args.push_back(argv[i]);
        else if (++i < argc) xdk_dir = argv[i];
        else usage = true;
    }
    if (args.empty() || usage) {
        std::fprintf(stderr, "usage: xbe_recomp <config.toml> [xbe override] [out override] [--xdk <SDK xdk folder>]\n");
        return 1;
    }
    Config cfg;
    fs::path cfg_path = args[0];
    if (!LoadConfig(cfg_path, cfg, xdk_dir)) return 1;
    if (cfg.library.empty()) {
        std::fprintf(stderr, "%s: [xbe] needs library = \"<exe>.dll\", the game code DLL the executable loads\n", args[0].c_str());
        return 1;
    }
    if (!cfg.names.empty() && !cfg.named) {
        std::fprintf(stderr, "%s: [names] needs [xbe] named = true\n", args[0].c_str());
        return 1;
    }
    for (auto& [name, va] : cfg.names)
        if (cfg.symbols.count(name)) {
            std::fprintf(stderr, "%s: %s is in both [symbols] and [names]\n", args[0].c_str(), name.c_str());
            return 1;
        }
    for (auto& [name, locs] : cfg.abi)
        if (!cfg.symbols.count(name)) {
            std::fprintf(stderr, "%s: [abi] %s is not a [symbols] name\n", args[0].c_str(), name.c_str());
            return 1;
        }
    fs::path root = cfg_path.parent_path().parent_path();  // the config sits in <game>/config
    fs::path xbe_path = args.size() > 1 ? fs::path(args[1]) : root / cfg.xbe_path;
    fs::path out_dir = args.size() > 2 ? fs::path(args[2]) : root / cfg.out_dir;

    std::ifstream f(xbe_path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", xbe_path.string().c_str());
        return 1;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    xbe::Image img;
    std::string err;
    if (!img.Parse(std::move(data), err)) {
        std::fprintf(stderr, "%s: %s\n", xbe_path.string().c_str(), err.c_str());
        return 1;
    }
    std::fprintf(stderr, "%s: title %08X entry %08X kernel thunk %08X\n", xbe_path.filename().string().c_str(),
                 img.title_id, img.entry, img.kernel_thunk);
    if ((cfg.title && cfg.title != img.title_id) || (cfg.entry && cfg.entry != img.entry)) {
        std::fprintf(stderr, "%s was written for title %08X entry %08X, not this image: delete it so build.bat writes a new one\n",
                     args[0].c_str(), cfg.title, cfg.entry);
        return 1;
    }

    Analysis an(img, cfg);
    an.Run();
    if (!CheckOverrides(an)) return 1;
    fs::create_directories(out_dir);
    CodeGen gen(an, out_dir);
    return gen.Run() ? 0 : 1;
}
