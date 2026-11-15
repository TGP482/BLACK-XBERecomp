#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace xbe {

// the header XORs the entry point and kernel thunk address with these
constexpr uint32_t kRetailEntryKey = 0xA8FC57AB;
constexpr uint32_t kDebugEntryKey = 0x94859D4B;
constexpr uint32_t kRetailThunkKey = 0x5B6D40B6;
constexpr uint32_t kDebugThunkKey = 0xEFB1F152;

enum SectionFlags : uint32_t {
    kWritable = 1,
    kPreload = 2,
    kExecutable = 4,
    kInsertedFile = 8,
};

struct Section {
    std::string name;
    uint32_t flags = 0;
    uint32_t va = 0, vsize = 0;
    uint32_t raw = 0, rsize = 0;
};

struct Library {
    std::string name;
    uint16_t major = 0, minor = 0, build = 0, flags = 0;
};

struct Tls {
    uint32_t data_start = 0, data_end = 0, index_addr = 0, callbacks = 0, zero_fill = 0, characteristics = 0;
};

struct Image {
    std::vector<uint8_t> file;
    uint32_t base = 0, header_size = 0, image_size = 0;
    uint32_t entry = 0, kernel_thunk = 0, tls_addr = 0;
    uint32_t stack_commit = 0, heap_reserve = 0, heap_commit = 0;
    uint32_t cert = 0, title_id = 0, init_flags = 0;
    bool debug = false;
    std::vector<Section> sections;
    std::vector<Library> libraries;
    Tls tls;

    uint32_t U32(uint32_t off) const {
        uint32_t v;
        std::memcpy(&v, file.data() + off, 4);
        return v;
    }

    // file offset for a virtual address, -1 if unmapped or in the zero filled tail
    int64_t Offset(uint32_t va) const {
        if (va >= base && va < base + header_size) return va - base;
        for (auto& s : sections)
            if (va >= s.va && va < s.va + s.rsize) return int64_t(s.raw) + (va - s.va);
        return -1;
    }

    const Section* SectionAt(uint32_t va) const {
        for (auto& s : sections)
            if (va >= s.va && va < s.va + s.vsize) return &s;
        return nullptr;
    }

    bool Read32(uint32_t va, uint32_t& out) const {
        int64_t o = Offset(va);
        if (o < 0 || o + 4 > int64_t(file.size())) return false;
        out = U32(uint32_t(o));
        return true;
    }

    std::string CStr(uint32_t va) const {
        int64_t o = Offset(va);
        if (o < 0) return {};
        std::string s;
        while (size_t(o) < file.size() && file[size_t(o)]) s += char(file[size_t(o++)]);
        return s;
    }

    bool Parse(std::vector<uint8_t> data, std::string& error) {
        file = std::move(data);
        if (file.size() < 0x180 || std::memcmp(file.data(), "XBEH", 4)) {
            error = "not an XBE";
            return false;
        }
        base = U32(0x104);
        header_size = U32(0x108);
        image_size = U32(0x10C);
        cert = U32(0x118);
        uint32_t nsec = U32(0x11C), sec_addr = U32(0x120);
        init_flags = U32(0x124);
        uint32_t ep = U32(0x128), kt = U32(0x158);
        tls_addr = U32(0x12C);
        stack_commit = U32(0x130);
        heap_reserve = U32(0x134);
        heap_commit = U32(0x138);
        auto inside = [&](uint32_t v) { return v >= base && v < base + image_size; };
        if (inside(ep ^ kRetailEntryKey) && inside(kt ^ kRetailThunkKey)) {
            entry = ep ^ kRetailEntryKey;
            kernel_thunk = kt ^ kRetailThunkKey;
        } else if (inside(ep ^ kDebugEntryKey) && inside(kt ^ kDebugThunkKey)) {
            entry = ep ^ kDebugEntryKey;
            kernel_thunk = kt ^ kDebugThunkKey;
            debug = true;
        } else {
            error = "entry point key not recognised";
            return false;
        }
        for (uint32_t i = 0; i < nsec; i++) {
            uint32_t o = sec_addr - base + i * 56;
            Section s;
            s.flags = U32(o);
            s.va = U32(o + 4);
            s.vsize = U32(o + 8);
            s.raw = U32(o + 12);
            s.rsize = U32(o + 16);
            uint32_t name = U32(o + 20);
            if (name - base < header_size)
                s.name = reinterpret_cast<const char*>(file.data() + (name - base));
            sections.push_back(s);
        }
        uint32_t nlib = U32(0x160), lib_addr = U32(0x164);
        for (uint32_t i = 0; i < nlib && lib_addr; i++) {
            uint32_t o = lib_addr - base + i * 16;
            Library l;
            char n[9] = {};
            std::memcpy(n, file.data() + o, 8);
            l.name = n;
            std::memcpy(&l.major, file.data() + o + 8, 8);  // major, minor, build and flags, four uint16 in order
            libraries.push_back(l);
        }
        if (cert - base + 12 <= file.size()) title_id = U32(cert - base + 8);  // certificate +8 is the title id
        if (tls_addr) {
            int64_t o = Offset(tls_addr);
            if (o >= 0) {
                tls.data_start = U32(uint32_t(o));
                tls.data_end = U32(uint32_t(o) + 4);
                tls.index_addr = U32(uint32_t(o) + 8);
                tls.callbacks = U32(uint32_t(o) + 12);
                tls.zero_fill = U32(uint32_t(o) + 16);
                tls.characteristics = U32(uint32_t(o) + 20);
            }
        }
        return true;
    }
};

}  // namespace xbe
