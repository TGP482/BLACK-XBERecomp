#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Everything the title handed to the XDK Direct3D API, as the API defines it. The renderer translates
// this to D3D12; nothing here models the console's GPU.
namespace gpu {

// D3DRS indices of XDK 4831, 5659 and 5849, which number them alike, taken from their own tables
enum : uint32_t {
    kRsDeferredFirst = 92,
    kRsFogEnable = 92,
    kRsFogTableMode = 93,
    kRsFogStart = 94,
    kRsFogEnd = 95,
    kRsFogDensity = 96,
    kRsRangeFogEnable = 97,
    kRsWrap0 = 98,
    kRsLighting = 102,
    kRsSpecularEnable = 103,
    kRsLocalViewer = 104,
    kRsColorVertex = 105,
    kRsSpecularMaterialSource = 110,
    kRsDiffuseMaterialSource = 111,
    kRsAmbientMaterialSource = 112,
    kRsEmissiveMaterialSource = 113,
    kRsAmbient = 115,
    kRsPointSize = 116,
    kRsPointSizeMin = 117,
    kRsPointSpriteEnable = 118,
    kRsPointScaleEnable = 119,
    kRsPointScaleA = 120,
    kRsPointScaleB = 121,
    kRsPointScaleC = 122,
    kRsPointSizeMax = 123,
    kRsPresentationInterval = 127,
    kRsComplexFirst = 136,
    kRsPsTextureModes = 136,
    kRsVertexBlend = 137,
    kRsFogColor = 138,
    kRsFillMode = 139,
    kRsBackFillMode = 140,
    kRsTwoSidedLighting = 141,
    kRsNormalizeNormals = 142,
    kRsZEnable = 143,
    kRsStencilEnable = 144,
    kRsStencilFail = 145,
    kRsFrontFace = 146,
    kRsCullMode = 147,
    kRsTextureFactor = 148,
    kRsZBias = 149,
    kRsLogicOp = 150,
    kRsEdgeAntiAlias = 151,
    kRsMultiSampleAntiAlias = 152,
    kRsMultiSampleMask = 153,
    kRsMultiSampleMode = 154,
    kRsMultiSampleRenderTargetMode = 155,
    kRsShadowFunc = 156,
    kRsLineWidth = 157,
    kRsSampleAlpha = 158,
    kRsDxt1NoiseEnable = 159,
    kRsYuvEnable = 160,
    kRsOcclusionCullEnable = 161,
    kRsStencilCullEnable = 162,
    kRsRopZCmpAlwaysRead = 163,
    kRsRopZRead = 164,
    kRsDoNotCullUncompressed = 165,
    kRsMax = 166,
};

// Simple render states reach the API as (method, value) pairs; the method is the key the XDK's own
// D3DSIMPLERENDERSTATEENCODE table assigns to each state.
enum : uint32_t {
    kMAlphaTestEnable = 0x300,
    kMBlendEnable = 0x304,
    kMDitherEnable = 0x310,
    kMAlphaFunc = 0x33C,
    kMAlphaRef = 0x340,
    kMSrcBlend = 0x344,
    kMDestBlend = 0x348,
    kMBlendColor = 0x34C,
    kMBlendOp = 0x350,
    kMZFunc = 0x354,
    kMColorWriteEnable = 0x358,
    kMZWriteEnable = 0x35C,
    kMStencilWriteMask = 0x360,
    kMStencilFunc = 0x364,
    kMStencilRef = 0x368,
    kMStencilMask = 0x36C,
    kMStencilZFail = 0x374,
    kMStencilPass = 0x378,
    kMShadeMode = 0x37C,
    kMSolidOffsetEnable = 0x338,
    kMSlopeScale = 0x384,
    kMDepthOffset = 0x388,
    kMPsAlphaInputs = 0x260,     // 8
    kMPsFinalAbcd = 0x288,
    kMPsFinalEfgh = 0x28C,
    kMPsConstant0 = 0xA60,       // 8
    kMPsConstant1 = 0xA80,       // 8
    kMPsAlphaOutputs = 0xAA0,    // 8
    kMPsRgbInputs = 0xAC0,       // 8
    kMPsCompareMode = 0x17F8,
    kMPsFinalConstant0 = 0x1E20,
    kMPsFinalConstant1 = 0x1E24,
    kMPsRgbOutputs = 0x1E40,     // 8
    kMPsCombinerCount = 0x1E60,
    kMPsDotMapping = 0x1E74,
    kMPsInputTexture = 0x1E78,
    kMDepthClipControl = 0x1D78,
    // visibility tests, sent as raw commands rather than render states
    kMClearReport = 0x17C8,
    kMCountEnable = 0x17CC,
    kMGetReport = 0x17D0,
};

// texture stage state indices
enum : uint32_t {
    kTssAddressU = 0, kTssAddressV, kTssAddressW, kTssMagFilter, kTssMinFilter, kTssMipFilter, kTssMipLodBias,
    kTssMaxMipLevel, kTssMaxAnisotropy, kTssColorKeyOp, kTssColorSign, kTssAlphaKill, kTssColorOp, kTssColorArg0,
    kTssColorArg1, kTssColorArg2, kTssAlphaOp, kTssAlphaArg0, kTssAlphaArg1, kTssAlphaArg2, kTssResultArg,
    kTssTextureTransformFlags, kTssBumpEnvMat00, kTssBumpEnvMat01, kTssBumpEnvMat11, kTssBumpEnvMat10,
    kTssBumpEnvLScale, kTssBumpEnvLOffset, kTssTexCoordIndex, kTssBorderColor, kTssColorKeyColor, kTssCount = 32
};

struct VertexShader {
    uint32_t handle = 0;       // guest handle, record address | 1
    std::vector<uint32_t> decl;
    std::vector<uint32_t> program;  // 4 dwords per instruction
    uint32_t program_type = 0;      // 0x2078 vertex, 0x7378 state, 0x7778 read/write
    uint64_t hash = 0;
    // stream layout parsed from the declaration: register -> stream, offset, type
    struct Input {
        int stream = -1;
        uint32_t offset = 0;
        uint32_t type = 0;  // D3DVSDT
    };
    std::array<Input, 16> inputs{};
    std::array<uint32_t, 16> stream_stride{};  // bytes consumed per stream by the declaration
    std::vector<std::pair<uint32_t, std::array<float, 4>>> constants;  // D3DVSD_CONST tokens
};

struct PixelShader {
    uint32_t handle = 0;
    uint32_t def[0xF0 / 4] = {};  // D3DPIXELSHADERDEF
};

struct Viewport {
    uint32_t x = 0, y = 0, w = 640, h = 480;
    float min_z = 0, max_z = 1;
};

struct Surface {
    uint32_t header = 0;  // guest D3DSurface
    uint32_t data = 0, format = 0, size = 0;
};

struct State {
    std::array<uint32_t, 0x800> simple{};  // simple render states indexed by method / 4
    std::array<uint32_t, kRsMax> rs{};     // deferred and complex render states by D3DRS
    std::array<std::array<uint32_t, kTssCount>, 4> tss{};
    std::array<uint32_t, 4> textures{};    // guest D3DBaseTexture pointers
    std::array<uint32_t, 4> palettes{};    // guest D3DPalette pointers, the colours of the stage's P8 texture
    struct Stream {
        uint32_t vb = 0;  // guest D3DVertexBuffer
        uint32_t stride = 0;
    };
    std::array<Stream, 16> streams{};
    uint32_t index_buffer = 0, index_data = 0, base_vertex = 0;
    uint32_t vs_handle = 0;  // programmable when bit 0 set, otherwise an FVF
    std::shared_ptr<VertexShader> vs;
    std::shared_ptr<PixelShader> ps;
    std::array<std::array<float, 4>, 192> vsc{};  // hardware order: D3D register + 96
    std::array<std::array<float, 4>, 16> psc{};
    Viewport viewport;
    Surface rt, zs;
    uint32_t scissor_count = 0;
    std::array<uint32_t, 4 * 8> scissors{};
    // fixed function
    std::array<std::array<float, 16>, 512> transforms{};  // by D3DTRANSFORMSTATETYPE
    std::array<float, 17> material{};
    std::array<std::array<uint32_t, 26>, 8> lights{};  // D3DLIGHT8
    uint32_t light_enable = 0;                         // bit per light
    float ss_offset_x = 0, ss_offset_y = 0;
    float zscale = 16777215.0f;  // depth buffer units per 1.0
};

}  // namespace gpu
