#pragma once
// The title's Direct3D 8 state as Vulkan takes it: blend, compare, stencil and address modes, sampler filters,
// vertex element and texture formats. Pure functions, no device.
#include <vulkan/vulkan.h>

#include <cstdint>

#include "gpu/state.h"
#include "gpu/texture.h"

namespace gpu::vk {

// D3DBLEND as a colour or an alpha factor
VkBlendFactor Blend(uint32_t d3d, bool alpha);
// D3DBLENDOP
VkBlendOp BlendOp(uint32_t d3d);
// D3DCMP_* (0x200 to 0x207), anything else always
VkCompareOp Compare(uint32_t d3d);
// the console tests texel OP reference for shadow lookups; Vulkan tests reference OP texel
VkCompareOp ShadowCompare(uint32_t d3d);
// D3DSTENCILOP
VkStencilOp StencilOp(uint32_t d3d);
// D3DTEXTUREADDRESS
VkSamplerAddressMode Address(uint32_t d3d);
// console ARGB colour write bits to Vulkan RGBA bits
VkColorComponentFlags ColorWriteMask(uint32_t d3d);

// a vertex element's DXGI format number (gpu/vertex.cpp names them) as Vulkan reads it
VkFormat VertexFormat(uint32_t dxgi);

// what a console texture format is uploaded as
VkFormat HostFormat(Host h);
// bytes per texel, or per 4x4 block of a compressed format
uint32_t FormatBytes(VkFormat f);
bool IsBlockCompressed(VkFormat f);
// a row of texels, or of blocks, in bytes; the rows of a level (block rows when compressed)
uint32_t RowBytes(VkFormat f, uint32_t width);
uint32_t RowCount(VkFormat f, uint32_t height);

// one stage's sampler as Direct3D 8 sets it; aniso the setting's forced level, 0 for the title's;
// every field is 4 bytes, so a desc hashes as its bytes; clamp: the draw's coordinates all lie in 0..1, wrap is read as clamp; mipped: the texture has levels;
// shadow: a depth texture compared against
struct SamplerDesc {
    VkFilter min = VK_FILTER_NEAREST, mag = VK_FILTER_NEAREST;
    VkSamplerMipmapMode mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode u = VK_SAMPLER_ADDRESS_MODE_REPEAT, v = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                         w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    float lod_bias = 0, min_lod = 0, max_lod = VK_LOD_CLAMP_NONE;
    uint32_t anisotropy = 1;  // 1 off
    uint32_t compare = 0;  // 1: a comparison sampler
    VkCompareOp compare_op = VK_COMPARE_OP_NEVER;
    float border[4] = {};  // RGBA
    bool operator==(const SamplerDesc&) const = default;
};
SamplerDesc StageSampler(const State& s, int stage, bool clamp, uint32_t aniso, bool mipped, bool shadow);

// D3DCOLOR (ARGB) to RGBA floats
void Color(uint32_t argb, float out[4]);

}  // namespace gpu::vk
