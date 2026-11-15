#pragma once
#include <cstdint>
#include <vector>

#include "gpu/state.h"

namespace gpu {

// What the renderer uploads a console texture format as
enum class Host : uint8_t { BGRA8, BC1, BC2, BC3, R32F, R16Unorm, None };

struct FormatInfo {
    const char* name;
    uint8_t bits;      // per texel, 4 or 8 bytes per block for the DXT formats
    bool swizzled;     // Morton order in memory
    bool compressed;   // 4x4 blocks
    bool depth;
    Host host;
};

const FormatInfo& Format(uint32_t xbox_format);

// a D3DPixelContainer decoded
struct TextureDesc {
    uint32_t data = 0;  // guest virtual address of level 0
    uint32_t format = 0;
    uint32_t width = 1, height = 1, depth = 1, levels = 1;
    uint32_t pitch = 0;  // linear formats only
    bool cube = false;
    bool linear = false;
};

bool Describe(uint32_t header, TextureDesc& out);
bool DescribeSurface(const Surface& s, TextureDesc& out);
uint32_t LevelSize(const TextureDesc& d, uint32_t level);
uint32_t LevelOffset(const TextureDesc& d, uint32_t level);
uint32_t FaceSize(const TextureDesc& d);  // cube face stride
uint32_t TotalSize(const TextureDesc& d);

// converts one level (one face) from guest memory into what Host describes, rows tightly packed
void DecodeLevel(const TextureDesc& d, uint32_t level, uint32_t face, const uint8_t* guest, const uint32_t* palette,
                 std::vector<uint8_t>& out, uint32_t& row_pitch);

uint32_t LinearColorFormat(uint32_t fmt);
uint32_t LinearDepthFormat(uint32_t fmt);
void SurfaceSize(const Surface& s, uint32_t& w, uint32_t& h);

// CopyRects done in guest memory, false when the formats do not allow it
bool CopySurfaceMemory(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points);

// the title locked a resource, the next bind re-uploads it
void MarkDirty(uint32_t resource);
uint64_t DirtyEpoch(uint32_t data_va);

// vertex declarations and pixel shader constant routing, both follow the XDK's definitions
void ParseDeclaration(VertexShader& vs);
// SetPixelShaderConstant: the stages of the current shader mapped to these registers take the values
void ApplyPixelConstants(State& s, uint32_t first, uint32_t count);
uint32_t PackColor(const std::array<float, 4>& c);

}  // namespace gpu
