#include "gpu/vk_formats.h"

#include <dxgiformat.h>

#include <algorithm>
#include <cstring>

namespace gpu::vk {

namespace {

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

// D3DCMP_NEVER..ALWAYS in order, the D3D12 numbering minus one
constexpr VkCompareOp kCompare[8] = {VK_COMPARE_OP_NEVER,         VK_COMPARE_OP_LESS,
                                     VK_COMPARE_OP_EQUAL,         VK_COMPARE_OP_LESS_OR_EQUAL,
                                     VK_COMPARE_OP_GREATER,       VK_COMPARE_OP_NOT_EQUAL,
                                     VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};

uint32_t CompareIndex(uint32_t d3d) { return d3d >= 0x200 && d3d <= 0x207 ? d3d - 0x200 : 7; }

}  // namespace

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        // the blend colour, and its alpha; D3D12 had no constant alpha factor, Vulkan does
        case 0x8001: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp Compare(uint32_t v) { return kCompare[CompareIndex(v)]; }

VkCompareOp ShadowCompare(uint32_t v) {
    // texel OP r read as r OP' texel: less and greater trade places, equality stays
    static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return kCompare[swap[CompareIndex(v)]];
}

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkColorComponentFlags ColorWriteMask(uint32_t cw) {
    VkColorComponentFlags m = 0;
    if ((cw >> 16) & 1) m |= VK_COLOR_COMPONENT_R_BIT;
    if ((cw >> 8) & 1) m |= VK_COLOR_COMPONENT_G_BIT;
    if (cw & 1) m |= VK_COLOR_COMPONENT_B_BIT;
    if ((cw >> 24) & 1) m |= VK_COLOR_COMPONENT_A_BIT;
    return m;
}

VkFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Host::R32F: return VK_FORMAT_R32_SFLOAT;
        default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

bool IsBlockCompressed(VkFormat f) {
    return f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || f == VK_FORMAT_BC2_UNORM_BLOCK || f == VK_FORMAT_BC3_UNORM_BLOCK;
}

uint32_t FormatBytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: return 1;
        case VK_FORMAT_R8G8_UNORM: return 2;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK: return 16;
        default: return 4;
    }
}

uint32_t RowBytes(VkFormat f, uint32_t width) {
    return IsBlockCompressed(f) ? ((width + 3) / 4) * FormatBytes(f) : width * FormatBytes(f);
}

uint32_t RowCount(VkFormat f, uint32_t height) { return IsBlockCompressed(f) ? (height + 3) / 4 : height; }

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

SamplerDesc StageSampler(const State& s, int i, bool clamp, uint32_t aniso, bool mipped, bool shadow) {
    SamplerDesc sd;
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    sd.min = lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mag = lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mip = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && mipped && !shadow) an = aniso;
    if (an > 1) {
        // Direct3D's anisotropic filter is linear in every direction
        sd.min = sd.mag = VK_FILTER_LINEAR;
        sd.mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sd.anisotropy = std::min<uint32_t>(an, 16);
    }
    uint32_t u = t[kTssAddressU];
    uint32_t v = t[kTssAddressV];
    sd.u = Address(clamp && u == kTaddressWrap ? kTaddressClamp : u);
    sd.v = Address(clamp && v == kTaddressWrap ? kTaddressClamp : v);
    sd.w = Address(t[kTssAddressW]);
    sd.lod_bias = Bits(t[kTssMipLodBias]);
    sd.min_lod = float(t[kTssMaxMipLevel]);
    sd.max_lod = mip ? VK_LOD_CLAMP_NONE : sd.min_lod;
    Color(t[kTssBorderColor], sd.border);
    if (shadow) {
        sd.min = lin(min) || lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sd.mag = sd.min;
        sd.mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sd.anisotropy = 1;
        sd.compare = 1;
        sd.compare_op = ShadowCompare(s.rs[kRsShadowFunc]);
    }
    return sd;
}

}  // namespace gpu::vk
