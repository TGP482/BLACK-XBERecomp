#include "d3d/texture_pack.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "app/app.h"
#include "core/log.h"

namespace texture_pack {
namespace {
const std::filesystem::path& Directory() {
    static const auto path = app::ExeDir() / "textures";
    return path;
}

uint64_t Fingerprint(const gpu::TextureDesc& d, const uint8_t* pixels, const uint32_t* palette) {
    uint64_t hash = 14695981039346656037ull;
    auto add = [&](uint8_t b) { hash = (hash ^ b) * 1099511628211ull; };
    for (uint32_t v : {d.width, d.height, d.format})
        for (uint32_t shift = 0; shift < 32; shift += 8) add(uint8_t(v >> shift));
    for (uint32_t i = 0; i < gpu::LevelSize(d, 0); i++) add(pixels[i]);
    if (d.format == 0x0b)
        for (uint32_t i = 0; i < 1024; i++) add(reinterpret_cast<const uint8_t*>(palette)[i]);
    return hash;
}
}  // namespace

bool Enabled() {
    static const bool enabled = [] {
        std::error_code ec;
        return std::filesystem::is_directory(Directory(), ec);
    }();
    return enabled;
}

bool Load(const gpu::TextureDesc& original, const uint8_t* pixels, const uint32_t* palette, gpu::TextureDesc& replacement,
          std::vector<uint8_t>& blocks) {
    if (!Enabled() || original.cube || original.depth != 1 || original.linear ||
        (original.format != 0x0c && original.format != 0x0e && original.format != 0x0f &&
         original.format != 0x0b && original.format != 0x3a) || (original.format == 0x0b && !palette)) return false;
    const auto key = Fingerprint(original, pixels, palette);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.dds", static_cast<unsigned long long>(key));
    std::ifstream file(Directory() / name, std::ios::binary | std::ios::ate);
    if (!file) return false;
    auto length = file.tellg();
    if (length < 128 || length > (256 << 20)) return false;
    file.seekg(0);
    std::array<uint32_t, 32> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), sizeof(header))) return false;
    // BC3 mip chains for surfaces, or lossless single-level BGRA8 for font edges.
    if (header[0] != 0x20534444 || header[1] != 124 || header[19] != 32 ||
        (header[28] & 0x20fe00u)) return false;
    bool bgra = header[20] == 0x41 && header[21] == 0 && header[22] == 32 &&
        header[23] == 0x00ff0000 && header[24] == 0x0000ff00 &&
        header[25] == 0x000000ff && header[26] == 0xff000000;
    if (!bgra && (header[20] != 4 || header[21] != 0x35545844)) return false;
    replacement = {};
    replacement.width = header[4];
    replacement.height = header[3];
    replacement.levels = std::max(1u, header[7]);
    replacement.format = bgra ? 0x12 : 0x0f;
    replacement.linear = bgra;
    replacement.pitch = bgra ? replacement.width * 4 : 0;
    if (bgra && (replacement.levels != 1 || header[5] != replacement.pitch)) return false;
    if (!replacement.width || !replacement.height || replacement.width > 16384 || replacement.height > 16384 ||
        replacement.levels > 15) return false;
    uint32_t max_levels = 1;
    for (uint32_t size = std::max(replacement.width, replacement.height); size > 1; size >>= 1) max_levels++;
    uint32_t bytes = gpu::TotalSize(replacement);
    if (replacement.levels > max_levels || uint64_t(length) != 128ull + bytes) return false;
    blocks.resize(bytes);
    if (!file.read(reinterpret_cast<char*>(blocks.data()), bytes)) return false;
    LOGR("textures: %s %ux%u -> %ux%u (%u mips)", name, original.width, original.height,
         replacement.width, replacement.height, replacement.levels);
    return true;
}
}  // namespace texture_pack
