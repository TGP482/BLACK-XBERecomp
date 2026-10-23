// Texture replacement and dumping, keyed exactly as the black-pc port's loader (d3d/texrepl.cpp there): FNV-1a 64 over
// {format, width, height} as three little-endian uint32, then level 0's bytes as they lie in guest memory (swizzled
// texels, or DXT blocks), then for P8 the 1024 bytes of the palette. Files: <exe>\textures\replacements\**\<16 hex>.dds
// or .png (a .dds wins), dumps to <exe>\textures\dump. Replacements made for black-pc load here unchanged.
#pragma once
#include <dxgiformat.h>

#include <cstdint>
#include <vector>

namespace gpu::texrepl {

// a replacement decoded on the CPU, ready to upload: levels tightly packed, BC rows are rows of 4x4 blocks
struct Image {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    struct Level {
        std::vector<uint8_t> data;
        uint32_t pitch = 0;  // bytes per row (of blocks for BC)
        uint32_t rows = 0;
    };
    std::vector<Level> levels;
};

bool Enabled();      // texture_replacement setting
bool DumpEnabled();  // texture_dump setting

uint64_t Key(const void* texels, uint32_t bytes, uint32_t xbox_format, uint32_t w, uint32_t h, const void* palette);
// true when a replacement exists for the key (indexes the folder on first use)
bool Has(uint64_t key);
// decodes the replacement file; false when it is missing or unreadable (logged once)
bool Load(uint64_t key, Image& out);
// true the first time a key is seen while dumping
bool DumpWanted(uint64_t key);
// level 0 as DecodeLevel produced it: DXT blocks (compressed) or BGRA8 rows
void Dump(uint64_t key, uint32_t xbox_format, const char* fmt_name, uint32_t w, uint32_t h, bool compressed,
          uint32_t block_bytes, const uint8_t* data, uint32_t pitch);

}  // namespace gpu::texrepl
