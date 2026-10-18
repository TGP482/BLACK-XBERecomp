// Dedicated Vulkan backend for BLACK-XBERecomp on Linux
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "common/aspect.h"
#include "common/options.h"
#include "d3d/gpu_frame.h"
#include "d3d/occlusion.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

using TextureStageState = std::array<uint32_t, kTssCount>;

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kRingBufferSize = 64ull * 1024 * 1024; // 64MB streaming ring buffer per frame

struct TargetVk {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView srv_view = VK_NULL_HANDLE;
    VkImage fb_image = VK_NULL_HANDLE;
    VkDeviceMemory fb_memory = VK_NULL_HANDLE;
    VkImageView fb_view = VK_NULL_HANDLE;
    VkImageLayout fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFramebuffer fb_depth = VK_NULL_HANDLE;
    VkFramebuffer fb_color = VK_NULL_HANDLE;
    VkFramebuffer fb_depth_only = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, gw = 0, gh = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool depth = false;
    bool drawn = false;
    uint32_t header = 0;
    uint32_t data = 0;
    uint64_t epoch = 0;
    uint64_t sig = 0;
    uint32_t bytes = 0;
    std::vector<uint8_t> read;
};

struct TextureVk {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t format = 0;
    uint32_t w = 0, h = 0, d = 0, levels = 1, pitch = 0;
    bool linear = false;
    bool cube = false;
    uint32_t data = 0;
    uint32_t pal = 0;
    uint64_t pal_hash = 0;
    uint64_t epoch = 0;
    uint64_t sig = 0;
    uint64_t check_frame = UINT64_MAX;
};

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

static inline StagePalette ReadStagePalette(const State& s, int stage) {
    StagePalette palette{};
    if (!s.palettes[size_t(stage)]) return palette;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + 4));
    uint32_t size_code = kernel::R32(header) >> 30;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    const void* src = kernel::HostPtr(palette.va);
    if (src) {
        std::memcpy(palette.colours, src, bytes);
        palette.hash = Hash(palette.colours, sizeof(palette.colours));
    }
    return palette;
}

struct DrawTexturesVk {
    TexInfo info[4]{};
    VkImageView views[4]{};
    uint32_t levels[4]{};
    bool rt[4]{};
    float wide[4]{};
    bool blur[4]{};
};

struct UploadAlloc {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t* ptr = nullptr;
};

struct GarbageItem {
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
};

struct LateVk {
    VkBuffer rb[kFrames]{};
    VkDeviceMemory rb_mem[kFrames]{};
    void* rb_ptr[kFrames]{};
    VkImage down_img = VK_NULL_HANDLE;
    VkDeviceMemory down_mem = VK_NULL_HANDLE;
    uint32_t gw = 0, gh = 0;
    uint32_t next = 0;
    uint64_t frame[kFrames]{};
    uint64_t delivered = 0;
};

static inline VkFormat VkFormatFromDxgi(uint32_t fmt) {
    switch (fmt) {
        case 0x02: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 0x06: return VK_FORMAT_R32G32B32_SFLOAT;
        case 0x0D: return VK_FORMAT_R16G16B16A16_SNORM;
        case 0x0E: return VK_FORMAT_R16G16B16A16_SINT;
        case 0x10: return VK_FORMAT_R32G32_SFLOAT;
        case 0x1C: return VK_FORMAT_R8G8B8A8_UNORM;
        case 0x25: return VK_FORMAT_R16G16_SNORM;
        case 0x26: return VK_FORMAT_R16G16_SINT;
        case 0x29: return VK_FORMAT_R32_SFLOAT;
        case 0x2A: return VK_FORMAT_R32_UINT;
        case 0x31: return VK_FORMAT_R8G8_UNORM;
        case 0x3A: return VK_FORMAT_R16_SNORM;
        case 0x3B: return VK_FORMAT_R16_SINT;
        case 0x3D: return VK_FORMAT_R8_UNORM;
        case 0x57: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

struct PipelineKeyVk {
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;
    uint32_t topology = 0;
    uint32_t blend_enable = 0;
    uint32_t src_blend = 0;
    uint32_t dst_blend = 0;
    uint32_t blend_op = 0;
    uint32_t color_write_mask = 0;
    uint32_t z_enable = 0;
    uint32_t z_write = 0;
    uint32_t z_func = 0;
    uint32_t stencil_enable = 0;
    uint32_t stencil_func = 0;
    uint8_t stencil_mask = 0;
    uint8_t stencil_wmask = 0;
    uint8_t wireframe = 0;
    uint8_t pad0 = 0;
    uint32_t stencil_fail = 0;
    uint32_t stencil_zfail = 0;
    uint32_t stencil_pass = 0;
    uint32_t cull_mode = 0;
    uint32_t front_face = 0;
    uint32_t zbias = 0;
    uint32_t zslope = 0;
    uint32_t has_color = 0;
    uint32_t has_depth = 0;
    uint16_t regs = 0;
    uint16_t buffer_count = 0;
    std::array<uint32_t, 16> elements{};
    std::array<uint32_t, 16> strides{};

    bool operator==(const PipelineKeyVk& o) const = default;
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKeyVk& k) const {
        return Hash(&k, sizeof(k));
    }
};

static inline uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

static uint64_t MemSig(uint32_t data, uint32_t total) {
    if (!data || !total) return 0;
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    if (!p) return 0;
    if (total > 64 * 1024 * 1024) total = 64 * 1024 * 1024;
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

static bool Changed(const TargetVk& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    if (!p) return false;
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------
// Constant buffer helpers (matching register b1 / struct DrawConstants)
// ------------------------------------------------------------------------------------------------

static void SaveBmp(const char* filename, uint32_t width, uint32_t height, const uint8_t* bgra) {
    uint32_t row_size = width * 4;
    uint32_t data_size = row_size * height;
    uint32_t file_size = 54 + data_size;
    int32_t h = -int32_t(height);
    uint8_t header[54] = {
        'B', 'M',
        uint8_t(file_size), uint8_t(file_size >> 8), uint8_t(file_size >> 16), uint8_t(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0,
        40, 0, 0, 0,
        uint8_t(width), uint8_t(width >> 8), uint8_t(width >> 16), uint8_t(width >> 24),
        uint8_t(h), uint8_t(h >> 8), uint8_t(h >> 16), uint8_t(h >> 24),
        1, 0,
        32, 0,
        0, 0, 0, 0,
        uint8_t(data_size), uint8_t(data_size >> 8), uint8_t(data_size >> 16), uint8_t(data_size >> 24),
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    FILE* f = fopen(filename, "wb");
    if (f) {
        fwrite(header, 1, 54, f);
        fwrite(bgra, 1, data_size, f);
        fclose(f);
        LOGI("vulkan: captured debug frame to %s (%ux%u)", filename, width, height);
    }
}

static inline float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

static inline void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

static inline uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

static inline uint32_t Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return n + 1;
}

constexpr float kAlphaTestAlways = 7.0f;

struct GuestExtent {
    uint32_t w = 0;
    uint32_t h = 0;
};

static inline std::string VertexProgram(const VertexShader& vs, uint16_t regs, uint64_t decode) {
    const std::string clamp = "    p.w = magclamp(p.w);\n";
    const std::string screen_space = "    p.w = p.w == 0 ? 1 : magclamp(p.w);\n";
    std::string src = VertexProgramHlsl(vs.program, regs, decode);
    size_t at = src.find(clamp);
    if (at != std::string::npos) src.replace(at, clamp.size(), screen_space);
    return src;
}

static inline bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != 0x10 /* DXGI_FORMAT_R32G32_FLOAT */) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

static inline bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == 1 || t[kTssAddressV] == 1) return true;
    }
    return false;
}

static inline void FillFogConstants(const State& s, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

static inline void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / (s.zscale != 0.0f ? s.zscale : 1.0f);
    c.surf[3] = 0.0f;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

static inline void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;
constexpr float kBlurTapTexels = 7.0f / 6.0f;

static inline void FillTextureConstants(const State& s, const DrawTexturesVk& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) {
            c.texsize[i][0] = float(d.width);
            c.texsize[i][1] = float(d.height);
            if (stages.rt[i]) {
                c.texsize[i][2] = -0.5f / float(d.width);
                c.texsize[i][3] = -0.5f / float(d.height);
                if (stages.wide[i] > 0.0f) {
                    bool blur = stages.blur[i];
                    float fw = blur ? kBlurTapTexels : kGlowBoxTexels;
                    float per_texel = blur ? 1.0f : 0.5f;
                    float n = std::ceil(fw * stages.wide[i] * per_texel);
                    c.spread[i][0] = fw / (n * float(d.width));
                    c.spread[i][1] = fw / (n * float(d.height));
                    c.spread[i][2] = n;
                }
            }
        }
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

static inline void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

static inline void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

static inline void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0.0f ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

static inline void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

static inline void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

static inline void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

static inline VkBlendFactor VkBlend(uint32_t v, bool alpha) {
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
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: case 0x8003: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: case 0x8004: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

static inline VkBlendOp VkBlendOperation(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

static inline VkCompareOp VkComparisonOp(uint32_t v) {
    switch (v) {
        case 0x200: return VK_COMPARE_OP_NEVER;
        case 0x201: return VK_COMPARE_OP_LESS;
        case 0x202: return VK_COMPARE_OP_EQUAL;
        case 0x203: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 0x204: return VK_COMPARE_OP_GREATER;
        case 0x205: return VK_COMPARE_OP_NOT_EQUAL;
        case 0x206: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case 0x207: return VK_COMPARE_OP_ALWAYS;
        default: return VK_COMPARE_OP_LESS_OR_EQUAL;
    }
}

static inline VkStencilOp VkStencilOperation(uint32_t v) {
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

class VulkanBackend;
VulkanBackend* g_vulkan_backend = nullptr;

class VulkanBackend final : public Backend {
public:
    VulkanBackend() { g_vulkan_backend = this; }
    ~VulkanBackend() override {
        Destroy();
        if (g_vulkan_backend == this) g_vulkan_backend = nullptr;
    }

    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void WaitGpuFrame(uint64_t n);

private:
    bool CreateInstance();
    bool CreateDeviceAndQueue();
    bool CreateSwapchain();
    bool RecreateSwapchain();
    bool CreateRenderPasses();
    bool CreateCommandBuffers();
    bool CreateDescriptorPool();
    bool CreateRingBuffers();
    bool CreateDummyTexture();
    bool CreateBlitPipeline();
    void Destroy();
    void DestroyTargets();
    void FreeFrameGarbage(uint32_t fi);
    float DesiredScale() const;
    void ApplySettings();

    uint32_t FindMemoryType(uint32_t filter, VkMemoryPropertyFlags props);
    UploadAlloc AllocUpload(uint64_t size, uint64_t align = 256);
    TargetVk* FindTarget(const Surface& s, bool depth);
    TargetVk* EnsureTarget(const Surface& s, bool depth);
    VkImageView Feedback(TargetVk& t);
    bool DepthCopy(const State& s);
    VkImageView BindStageTexture(const State& s, int stage, TargetVk* draw_rt, TargetVk* draw_ds, DrawTexturesVk& stages);
    VkSampler GetSampler(const State& s, int stage, const DrawTexturesVk& stages, bool clamp);
    VkPipeline GetPipeline(const State& s, const Assembled& a, bool has_color, bool has_depth, float zslope_scale,
                           uint64_t vs_hash, uint64_t ps_hash, VkShaderModule vs_mod, VkShaderModule ps_mod);
    void Transition(VkCommandBuffer cmd, VkImage img, VkImageLayout old_layout, VkImageLayout new_layout,
                    VkImageAspectFlags aspect);
    bool LatchCpuWrite(TargetVk& t);
    void UploadTarget(TargetVk& t, const Surface& s);
    bool BeginTargetRenderPass(TargetVk* rt, TargetVk* ds);

    SDL_Window* window_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 1280;
    uint32_t bb_h_ = 720;
    float scale_ = 1.0f;
    bool vsync_ = true;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    std::vector<VkFramebuffer> swapchain_framebuffers_;

    VkRenderPass swapchain_render_pass_ = VK_NULL_HANDLE;
    VkRenderPass target_render_pass_ = VK_NULL_HANDLE;
    VkRenderPass target_render_pass_color_ = VK_NULL_HANDLE;
    VkRenderPass target_render_pass_depth_ = VK_NULL_HANDLE;

    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFrames> cmd_buffers_{};
    std::array<VkSemaphore, kFrames> image_available_sem_{};
    std::array<VkSemaphore, kFrames> render_finished_sem_{};
    std::array<VkFence, kFrames> frame_fences_{};
    uint32_t frame_index_ = 0;
    uint64_t presents_ = 0;
    std::array<std::atomic<uint64_t>, kFrames + 1> present_fence_{};
    std::atomic<uint64_t> presents_seen_{0};
    std::atomic<uint64_t> completed_presents_{0};
    std::array<uint64_t, kFrames> frame_present_id_{};
    std::unordered_map<uint32_t, LateVk> late_;
    std::mutex late_lock_;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;

    // Staging / Ring buffers for vertices, indices, and uniforms
    struct FrameRing {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
        uint64_t offset = 0;
    };
    std::array<FrameRing, kFrames> rings_{};
    std::array<std::vector<GarbageItem>, kFrames> frame_garbage_{};

    // Descriptor pools: per-frame for dynamic draws, plus persistent ones for blit & ImGui
    std::array<VkDescriptorPool, kFrames> frame_desc_pools_{};
    VkDescriptorPool blit_desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool imgui_desc_pool_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;

    // Cache
    std::unordered_map<uint64_t, TargetVk> targets_;
    std::unordered_map<uint64_t, TextureVk> textures_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, VkShaderModule> shader_modules_;
    std::unordered_map<PipelineKeyVk, VkPipeline, PipelineKeyHash> pipelines_;

    shaderc::Compiler compiler_;
    shaderc::CompileOptions compile_options_;

    // Screen blit pipeline & descriptors
    VkPipeline blit_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout blit_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout blit_set_layout_ = VK_NULL_HANDLE;
    VkSampler blit_sampler_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFrames> blit_sets_{};

    bool imgui_initialized_ = false;
    bool in_target_render_pass_ = false;
    TargetVk* current_rt_ = nullptr;
    TargetVk* current_ds_ = nullptr;
    TextureVk dummy_texture_{};
    TextureVk dummy_cube_{};
    TextureVk dummy_3d_{};

    double fps_ = 0.0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

// ------------------------------------------------------------------------------------------------
// Memory helpers
// ------------------------------------------------------------------------------------------------

uint32_t VulkanBackend::FindMemoryType(uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

void VulkanBackend::Transition(VkCommandBuffer cmd, VkImage img, VkImageLayout old_layout,
                               VkImageLayout new_layout, VkImageAspectFlags aspect) {
    if (old_layout == new_layout) return;

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcAccessMask = 0;
    } else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    if (new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    } else {
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// ------------------------------------------------------------------------------------------------
// Initialization & Teardown
// ------------------------------------------------------------------------------------------------

float VulkanBackend::DesiredScale() const {
    float base = app::RenderScale(guest_h_);
    uint32_t msaa = game::Opt().msaa;
    float aa_mul = (msaa >= 8) ? 2.0f : (msaa >= 4 ? 1.5f : (msaa >= 2 ? 1.25f : 1.0f));
    return std::clamp(base * aa_mul, 0.1f, 8.0f);
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    scale_ = DesiredScale();
    vsync_ = app::Get().vsync;
    window_ = static_cast<SDL_Window*>(app::Window());
    if (!window_) {
        LOGE("vulkan: window is null");
        return false;
    }

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    bb_w_ = w ? uint32_t(w) : 1280;
    bb_h_ = h ? uint32_t(h) : 720;

    compile_options_.SetSourceLanguage(shaderc_source_language_hlsl);
    compile_options_.SetHlslRegisterSetAndBinding("b0", "0", "0");
    compile_options_.SetHlslRegisterSetAndBinding("b1", "0", "1");
    for (int i = 0; i < 4; i++) {
        compile_options_.SetHlslRegisterSetAndBinding("t" + std::to_string(i), "0", std::to_string(2 + i));
        compile_options_.SetHlslRegisterSetAndBinding("s" + std::to_string(i), "0", std::to_string(6 + i));
    }

    if (!CreateInstance()) return false;
    if (!CreateDeviceAndQueue()) return false;
    if (!CreateSwapchain()) return false;
    if (!CreateRenderPasses()) return false;
    if (!CreateCommandBuffers()) return false;
    if (!CreateDescriptorPool()) return false;
    if (!CreateRingBuffers()) return false;
    if (!CreateDummyTexture()) return false;
    if (!CreateBlitPipeline()) return false;

    // Init ImGui Vulkan
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = instance_;
    init_info.PhysicalDevice = physical_device_;
    init_info.Device = device_;
    init_info.QueueFamily = queue_family_;
    init_info.Queue = graphics_queue_;
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = imgui_desc_pool_;
    init_info.RenderPass = swapchain_render_pass_;
    init_info.Subpass = 0;
    init_info.MinImageCount = 2;
    init_info.ImageCount = uint32_t(swapchain_images_.size());
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (ImGui_ImplVulkan_Init(&init_info)) {
        imgui_initialized_ = true;
    } else {
        LOGW("vulkan: ImGui Vulkan init failed");
    }

    // Begin recording first frame command buffer
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (vkBeginCommandBuffer(cmd_buffers_[0], &cbbi) != VK_SUCCESS) {
        LOGE("vulkan: initial vkBeginCommandBuffer failed");
        return false;
    }

    LOGI("vulkan: backend initialized successfully (%ux%u guest, %ux%u swapchain)", width, height, bb_w_, bb_h_);
    return true;
}

bool VulkanBackend::CreateInstance() {
    uint32_t sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (!sdl_exts) {
        LOGE("vulkan: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }

    std::vector<const char*> exts(sdl_exts, sdl_exts + sdl_ext_count);
    exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "Black";
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app_info;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        LOGE("vulkan: vkCreateInstance failed");
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        LOGE("vulkan: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool VulkanBackend::CreateDeviceAndQueue() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (!count) return false;
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Select discrete GPU if available
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical_device_ = d;
            LOGI("vulkan: selected discrete GPU: %s", props.deviceName);
            break;
        }
    }
    if (!physical_device_) {
        physical_device_ = devices[0];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        LOGI("vulkan: selected GPU: %s", props.deviceName);
    }

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf_props(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count, qf_props.data());

    bool found = false;
    for (uint32_t i = 0; i < qf_count; i++) {
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present_support);
        if ((qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
            queue_family_ = i;
            found = true;
            break;
        }
    }
    if (!found) {
        LOGE("vulkan: no suitable queue family found");
        return false;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* dev_exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkPhysicalDeviceFeatures feats{};
    feats.samplerAnisotropy = VK_TRUE;
    feats.fillModeNonSolid = VK_TRUE;
    feats.depthBiasClamp = VK_TRUE;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    dci.pEnabledFeatures = &feats;

    if (vkCreateDevice(physical_device_, &dci, nullptr, &device_) != VK_SUCCESS) {
        LOGE("vulkan: vkCreateDevice failed");
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &graphics_queue_);

    VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    std::vector<uint8_t> cache_data;
    FILE* f = fopen("vk_pipeline_cache.bin", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            cache_data.resize(sz);
            if (fread(cache_data.data(), 1, sz, f) == size_t(sz)) {
                pcci.initialDataSize = cache_data.size();
                pcci.pInitialData = cache_data.data();
            }
        }
        fclose(f);
    }
    vkCreatePipelineCache(device_, &pcci, nullptr, &pipeline_cache_);

    return true;
}

bool VulkanBackend::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps) != VK_SUCCESS) {
        LOGE("vulkan: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
        return false;
    }

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
    if (!format_count) {
        LOGE("vulkan: no surface formats found");
        return false;
    }

    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());

    VkSurfaceFormatKHR chosen_format = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_format = f;
            break;
        }
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_format = f;
        }
    }
    swapchain_format_ = chosen_format.format;

    VkExtent2D extent;
    int win_w = 0, win_h = 0;
    SDL_GetWindowSizeInPixels(window_, &win_w, &win_h);
    if (caps.currentExtent.width != 0xFFFFFFFF && caps.currentExtent.width > 0 && caps.currentExtent.height > 0) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(uint32_t(win_w ? win_w : 1280), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(uint32_t(win_h ? win_h : 720), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    bb_w_ = std::max(1u, extent.width);
    bb_h_ = std::max(1u, extent.height);

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alpha_candidates[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (auto a : alpha_candidates) {
        if (caps.supportedCompositeAlpha & a) {
            composite_alpha = a;
            break;
        }
    }

    vsync_ = app::Get().vsync;
    uint32_t pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, nullptr);
    std::vector<VkPresentModeKHR> pms(pm_count);
    if (pm_count) vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, pms.data());

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (vsync_) {
        for (auto pm : pms) {
            if (pm == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
                present_mode = pm;
                break;
            }
        }
    } else {
        bool has_mailbox = false;
        bool has_immediate = false;
        for (auto pm : pms) {
            if (pm == VK_PRESENT_MODE_MAILBOX_KHR) has_mailbox = true;
            if (pm == VK_PRESENT_MODE_IMMEDIATE_KHR) has_immediate = true;
        }
        if (has_mailbox) present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (has_immediate) present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    uint32_t image_count = (present_mode == VK_PRESENT_MODE_MAILBOX_KHR) ? 3 : (caps.minImageCount + 1);
    image_count = std::clamp(image_count, caps.minImageCount, caps.maxImageCount ? caps.maxImageCount : 3);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen_format.format;
    sci.imageColorSpace = chosen_format.colorSpace;
    sci.imageExtent = {bb_w_, bb_h_};
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = composite_alpha;
    sci.presentMode = present_mode;
    sci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_) != VK_SUCCESS) {
        LOGE("vulkan: vkCreateSwapchainKHR failed");
        return false;
    }

    uint32_t actual_count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, nullptr);
    swapchain_images_.resize(actual_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, swapchain_images_.data());

    swapchain_views_.resize(actual_count);
    for (uint32_t i = 0; i < actual_count; i++) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = swapchain_images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapchain_format_;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &vci, nullptr, &swapchain_views_[i]) != VK_SUCCESS) {
            LOGE("vulkan: vkCreateImageView failed for swapchain image %u", i);
            return false;
        }
    }

    LOGI("vulkan: created swapchain (%ux%u, format %d, count %u, vsync %d)",
         bb_w_, bb_h_, swapchain_format_, actual_count, vsync_ ? 1 : 0);
    return true;
}

bool VulkanBackend::RecreateSwapchain() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0) return false;

    vkDeviceWaitIdle(device_);
    for (auto fb : swapchain_framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    swapchain_framebuffers_.clear();
    for (auto v : swapchain_views_) vkDestroyImageView(device_, v, nullptr);
    swapchain_views_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    if (!CreateSwapchain()) return false;

    swapchain_framebuffers_.resize(swapchain_views_.size());
    for (size_t i = 0; i < swapchain_views_.size(); i++) {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = swapchain_render_pass_;
        fci.attachmentCount = 1;
        fci.pAttachments = &swapchain_views_[i];
        fci.width = bb_w_;
        fci.height = bb_h_;
        fci.layers = 1;
        if (vkCreateFramebuffer(device_, &fci, nullptr, &swapchain_framebuffers_[i]) != VK_SUCCESS) return false;
    }
    return true;
}

bool VulkanBackend::CreateRenderPasses() {
    VkAttachmentDescription color_att{};
    color_att.format = swapchain_format_;
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments = &color_att;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = 1;
    rp_ci.pDependencies = &dep;

    if (vkCreateRenderPass(device_, &rp_ci, nullptr, &swapchain_render_pass_) != VK_SUCCESS) return false;

    swapchain_framebuffers_.resize(swapchain_views_.size());
    for (size_t i = 0; i < swapchain_views_.size(); i++) {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = swapchain_render_pass_;
        fci.attachmentCount = 1;
        fci.pAttachments = &swapchain_views_[i];
        fci.width = bb_w_;
        fci.height = bb_h_;
        fci.layers = 1;
        if (vkCreateFramebuffer(device_, &fci, nullptr, &swapchain_framebuffers_[i]) != VK_SUCCESS) return false;
    }

    VkAttachmentDescription target_atts[2]{};
    target_atts[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    target_atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    target_atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    target_atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    target_atts[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    target_atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    target_atts[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
    target_atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    target_atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    target_atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    target_atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    target_atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    target_atts[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    target_atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference t_col_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference t_depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription t_subpass{};
    t_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    t_subpass.colorAttachmentCount = 1;
    t_subpass.pColorAttachments = &t_col_ref;
    t_subpass.pDepthStencilAttachment = &t_depth_ref;

    VkRenderPassCreateInfo trp_ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    trp_ci.attachmentCount = 2;
    trp_ci.pAttachments = target_atts;
    trp_ci.subpassCount = 1;
    trp_ci.pSubpasses = &t_subpass;

    if (vkCreateRenderPass(device_, &trp_ci, nullptr, &target_render_pass_) != VK_SUCCESS) return false;

    VkSubpassDescription c_subpass{};
    c_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    c_subpass.colorAttachmentCount = 1;
    c_subpass.pColorAttachments = &t_col_ref;

    VkRenderPassCreateInfo crp_ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    crp_ci.attachmentCount = 1;
    crp_ci.pAttachments = target_atts;
    crp_ci.subpassCount = 1;
    crp_ci.pSubpasses = &c_subpass;

    if (vkCreateRenderPass(device_, &crp_ci, nullptr, &target_render_pass_color_) != VK_SUCCESS) return false;

    VkAttachmentReference d_only_ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription d_subpass{};
    d_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    d_subpass.pDepthStencilAttachment = &d_only_ref;

    VkRenderPassCreateInfo drp_ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    drp_ci.attachmentCount = 1;
    drp_ci.pAttachments = &target_atts[1];
    drp_ci.subpassCount = 1;
    drp_ci.pSubpasses = &d_subpass;

    if (vkCreateRenderPass(device_, &drp_ci, nullptr, &target_render_pass_depth_) != VK_SUCCESS) return false;
    return true;
}

bool VulkanBackend::CreateCommandBuffers() {
    VkCommandPoolCreateInfo cp_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp_ci.queueFamilyIndex = queue_family_;
    cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device_, &cp_ci, nullptr, &cmd_pool_) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kFrames;
    if (vkAllocateCommandBuffers(device_, &ai, cmd_buffers_.data()) != VK_SUCCESS) return false;

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < kFrames; i++) {
        vkCreateSemaphore(device_, &sci, nullptr, &image_available_sem_[i]);
        vkCreateSemaphore(device_, &sci, nullptr, &render_finished_sem_[i]);
        vkCreateFence(device_, &fci, nullptr, &frame_fences_[i]);
    }
    return true;
}

bool VulkanBackend::CreateDescriptorPool() {
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 65536},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 65536}
    };
    for (int i = 0; i < kFrames; i++) {
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 16384;
        dpi.poolSizeCount = 3;
        dpi.pPoolSizes = pool_sizes;
        if (vkCreateDescriptorPool(device_, &dpi, nullptr, &frame_desc_pools_[i]) != VK_SUCCESS) return false;
    }

    VkDescriptorPoolSize imgui_pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}
    };
    VkDescriptorPoolCreateInfo idpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    idpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    idpi.maxSets = 64;
    idpi.poolSizeCount = 1;
    idpi.pPoolSizes = imgui_pool_sizes;
    if (vkCreateDescriptorPool(device_, &idpi, nullptr, &imgui_desc_pool_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize blit_pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(kFrames)}
    };
    VkDescriptorPoolCreateInfo bdpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    bdpi.maxSets = uint32_t(kFrames);
    bdpi.poolSizeCount = 1;
    bdpi.pPoolSizes = blit_pool_sizes;
    if (vkCreateDescriptorPool(device_, &bdpi, nullptr, &blit_desc_pool_) != VK_SUCCESS) return false;

    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    for (int i = 0; i < 4; i++) {
        bindings.push_back({uint32_t(2 + i), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        bindings.push_back({uint32_t(6 + i), VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    }

    VkDescriptorSetLayoutCreateInfo dsl_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl_ci.bindingCount = uint32_t(bindings.size());
    dsl_ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &dsl_ci, nullptr, &set_layout_) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;

    return true;
}

bool VulkanBackend::CreateRingBuffers() {
    for (int i = 0; i < kFrames; i++) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = kRingBufferSize;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bci, nullptr, &rings_[i].buffer) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, rings_[i].buffer, &req);

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device_, &mai, nullptr, &rings_[i].memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(device_, rings_[i].buffer, rings_[i].memory, 0);
        vkMapMemory(device_, rings_[i].memory, 0, kRingBufferSize, 0, (void**)&rings_[i].mapped);
        rings_[i].offset = 0;
    }
    return true;
}

bool VulkanBackend::CreateDummyTexture() {
    auto create_one = [&](TextureVk& out, VkImageType img_type, VkImageViewType view_type, uint32_t layers, VkImageCreateFlags flags) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.flags = flags;
        ici.imageType = img_type;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {1, 1, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = layers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_, &ici, nullptr, &out.image) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, out.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device_, &mai, nullptr, &out.memory);
        vkBindImageMemory(device_, out.image, out.memory, 0);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = out.image;
        vci.viewType = view_type;
        vci.format = VK_FORMAT_B8G8R8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = layers;
        vkCreateImageView(device_, &vci, nullptr, &out.view);
        out.w = 1;
        out.h = 1;
        return true;
    };

    if (!create_one(dummy_texture_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0)) return false;
    if (!create_one(dummy_cube_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) return false;
    if (!create_one(dummy_3d_, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0)) return false;

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkClearColorValue zero_col{{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};

    for (TextureVk* dt : {&dummy_texture_, &dummy_cube_, &dummy_3d_}) {
        Transition(cmd, dt->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(cmd, dt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_col, 1, &range);
        Transition(cmd, dt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);

    LOGI("vulkan: created 1x1 dummy 2D/Cube/3D textures");
    return true;
}

bool VulkanBackend::CreateBlitPipeline() {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 1;
    dlci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(device_, &dlci, nullptr, &blit_set_layout_) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(float) * 4;

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &blit_set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &blit_layout_) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 1.0f;
    if (vkCreateSampler(device_, &sci, nullptr, &blit_sampler_) != VK_SUCCESS) return false;

    for (int i = 0; i < kFrames; i++) {
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = blit_desc_pool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &blit_set_layout_;
        if (vkAllocateDescriptorSets(device_, &dsai, &blit_sets_[i]) != VK_SUCCESS) return false;
    }

    const char* vs_code = "#version 450\n"
                          "layout(location = 0) out vec2 outUV;\n"
                          "void main() {\n"
                          "    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;\n"
                          "    gl_Position = vec4(pos, 0.0, 1.0);\n"
                          "    outUV = (pos + 1.0) * 0.5;\n"
                          "}\n";

    const char* fs_code = "#version 450\n"
                          "layout(location = 0) in vec2 inUV;\n"
                          "layout(location = 0) out vec4 outColor;\n"
                          "layout(binding = 0) uniform sampler2D uTexture;\n"
                          "layout(push_constant) uniform Push { vec4 p; } pc;\n"
                          "float luma(vec3 rgb) { return dot(rgb, vec3(0.299, 0.587, 0.114)); }\n"
                          "vec3 sampleBox(vec2 uv) {\n"
                          "    int n = max(int(pc.p.z), 1);\n"
                          "    if (n <= 1) return textureLod(uTexture, uv, 0.0).rgb;\n"
                          "    vec2 px = fwidth(uv);\n"
                          "    vec3 acc = vec3(0.0);\n"
                          "    for (int y = 0; y < n; y++) {\n"
                          "        for (int x = 0; x < n; x++) {\n"
                          "            acc += textureLod(uTexture, uv + px * ((vec2(x, y) + 0.5) / float(n) - 0.5), 0.0).rgb;\n"
                          "        }\n"
                          "    }\n"
                          "    return acc / float(n * n);\n"
                          "}\n"
                          "void main() {\n"
                          "    vec3 rgbM = sampleBox(inUV);\n"
                          "    if (pc.p.w <= 0.0) { outColor = vec4(rgbM, 1.0); return; }\n"
                          "    vec2 rcpFrame = pc.p.xy;\n"
                          "    vec3 rgbNW = sampleBox(inUV + vec2(-1.0, -1.0) * rcpFrame);\n"
                          "    vec3 rgbNE = sampleBox(inUV + vec2( 1.0, -1.0) * rcpFrame);\n"
                          "    vec3 rgbSW = sampleBox(inUV + vec2(-1.0,  1.0) * rcpFrame);\n"
                          "    vec3 rgbSE = sampleBox(inUV + vec2( 1.0,  1.0) * rcpFrame);\n"
                          "    float lumaNW = luma(rgbNW), lumaNE = luma(rgbNE), lumaSW = luma(rgbSW), lumaSE = luma(rgbSE), lumaM = luma(rgbM);\n"
                          "    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));\n"
                          "    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));\n"
                          "    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125)) { outColor = vec4(rgbM, 1.0); return; }\n"
                          "    vec2 dir;\n"
                          "    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));\n"
                          "    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));\n"
                          "    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.125), 1.0 / 128.0);\n"
                          "    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
                          "    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * rcpFrame;\n"
                          "    vec3 rgbA = 0.5 * (sampleBox(inUV + dir * (1.0 / 3.0 - 0.5)) + sampleBox(inUV + dir * (2.0 / 3.0 - 0.5)));\n"
                          "    vec3 rgbB = rgbA * 0.5 + 0.25 * (sampleBox(inUV + dir * -0.5) + sampleBox(inUV + dir * 0.5));\n"
                          "    float lumaB = luma(rgbB);\n"
                          "    outColor = vec4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);\n"
                          "}\n";

    auto vs_res = compiler_.CompileGlslToSpv(vs_code, shaderc_vertex_shader, "main");
    if (vs_res.GetCompilationStatus() != shaderc_compilation_status_success) {
        LOGE("vulkan: blit VS compile error: %s", vs_res.GetErrorMessage().c_str());
        return false;
    }

    auto fs_res = compiler_.CompileGlslToSpv(fs_code, shaderc_fragment_shader, "main");
    if (fs_res.GetCompilationStatus() != shaderc_compilation_status_success) {
        LOGE("vulkan: blit FS compile error: %s", fs_res.GetErrorMessage().c_str());
        return false;
    }

    VkShaderModule vs_mod = VK_NULL_HANDLE;
    VkShaderModule fs_mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};

    smci.codeSize = (vs_res.end() - vs_res.begin()) * sizeof(uint32_t);
    smci.pCode = vs_res.begin();
    vkCreateShaderModule(device_, &smci, nullptr, &vs_mod);

    smci.codeSize = (fs_res.end() - fs_res.begin()) * sizeof(uint32_t);
    smci.pCode = fs_res.begin();
    vkCreateShaderModule(device_, &smci, nullptr, &fs_mod);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_mod;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_mod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dys{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dys.dynamicStateCount = 2;
    dys.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = nullptr;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dys;
    pci.layout = blit_layout_;
    pci.renderPass = swapchain_render_pass_;
    pci.subpass = 0;

    VkResult res = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &blit_pipeline_);
    vkDestroyShaderModule(device_, vs_mod, nullptr);
    vkDestroyShaderModule(device_, fs_mod, nullptr);

    return res == VK_SUCCESS;
}

void VulkanBackend::FreeFrameGarbage(uint32_t fi) {
    for (auto& g : frame_garbage_[fi]) {
        if (g.fb) vkDestroyFramebuffer(device_, g.fb, nullptr);
        if (g.view) vkDestroyImageView(device_, g.view, nullptr);
        if (g.image) vkDestroyImage(device_, g.image, nullptr);
        if (g.buffer) vkDestroyBuffer(device_, g.buffer, nullptr);
        if (g.memory) vkFreeMemory(device_, g.memory, nullptr);
    }
    frame_garbage_[fi].clear();
}

UploadAlloc VulkanBackend::AllocUpload(uint64_t size, uint64_t align) {
    if (!size) return {};
    auto& ring = rings_[frame_index_];
    uint64_t aligned = (ring.offset + (align - 1)) & ~(align - 1);
    if (aligned + size <= kRingBufferSize) {
        ring.offset = aligned + size;
        return {ring.buffer, aligned, ring.mapped + aligned};
    }

    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bci, nullptr, &buf) != VK_SUCCESS) return {};

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buf, nullptr);
        return {};
    }
    vkBindBufferMemory(device_, buf, mem, 0);
    void* ptr = nullptr;
    vkMapMemory(device_, mem, 0, size, 0, &ptr);
    frame_garbage_[frame_index_].push_back({VK_NULL_HANDLE, buf, mem, VK_NULL_HANDLE, VK_NULL_HANDLE});
    return {buf, 0, static_cast<uint8_t*>(ptr)};
}

void VulkanBackend::Destroy() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        for (uint32_t i = 0; i < kFrames; i++) {
            FreeFrameGarbage(i);
        }
        if (imgui_initialized_) {
            ImGui_ImplVulkan_Shutdown();
            imgui_initialized_ = false;
        }
        if (blit_pipeline_) { vkDestroyPipeline(device_, blit_pipeline_, nullptr); blit_pipeline_ = VK_NULL_HANDLE; }
        if (blit_layout_) { vkDestroyPipelineLayout(device_, blit_layout_, nullptr); blit_layout_ = VK_NULL_HANDLE; }
        if (blit_set_layout_) { vkDestroyDescriptorSetLayout(device_, blit_set_layout_, nullptr); blit_set_layout_ = VK_NULL_HANDLE; }
        if (blit_sampler_) { vkDestroySampler(device_, blit_sampler_, nullptr); blit_sampler_ = VK_NULL_HANDLE; }

        for (auto& [k, p] : pipelines_) vkDestroyPipeline(device_, p, nullptr);
        pipelines_.clear();
        for (auto& [k, m] : shader_modules_) vkDestroyShaderModule(device_, m, nullptr);
        shader_modules_.clear();
        for (auto& [k, s] : samplers_) vkDestroySampler(device_, s, nullptr);
        samplers_.clear();
        for (auto& [k, t] : textures_) {
            if (t.view && t.memory) vkDestroyImageView(device_, t.view, nullptr);
            if (t.image && t.memory) vkDestroyImage(device_, t.image, nullptr);
            if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        }
        textures_.clear();
        for (TextureVk* dt : {&dummy_texture_, &dummy_cube_, &dummy_3d_}) {
            if (dt->view) { vkDestroyImageView(device_, dt->view, nullptr); dt->view = VK_NULL_HANDLE; }
            if (dt->image) { vkDestroyImage(device_, dt->image, nullptr); dt->image = VK_NULL_HANDLE; }
            if (dt->memory) { vkFreeMemory(device_, dt->memory, nullptr); dt->memory = VK_NULL_HANDLE; }
        }
        DestroyTargets();
        for (auto& r : rings_) {
            if (r.mapped) vkUnmapMemory(device_, r.memory);
            if (r.buffer) vkDestroyBuffer(device_, r.buffer, nullptr);
            if (r.memory) vkFreeMemory(device_, r.memory, nullptr);
            r = {};
        }
        for (auto fb : swapchain_framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
        swapchain_framebuffers_.clear();
        for (auto v : swapchain_views_) vkDestroyImageView(device_, v, nullptr);
        swapchain_views_.clear();
        if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }

        if (pipeline_layout_) { vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
        if (set_layout_) { vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr); set_layout_ = VK_NULL_HANDLE; }

        for (int i = 0; i < kFrames; i++) {
            if (frame_desc_pools_[i]) { vkDestroyDescriptorPool(device_, frame_desc_pools_[i], nullptr); frame_desc_pools_[i] = VK_NULL_HANDLE; }
        }
        if (blit_desc_pool_) { vkDestroyDescriptorPool(device_, blit_desc_pool_, nullptr); blit_desc_pool_ = VK_NULL_HANDLE; }
        if (imgui_desc_pool_) { vkDestroyDescriptorPool(device_, imgui_desc_pool_, nullptr); imgui_desc_pool_ = VK_NULL_HANDLE; }

        for (int i = 0; i < kFrames; i++) {
            if (image_available_sem_[i]) vkDestroySemaphore(device_, image_available_sem_[i], nullptr);
            if (render_finished_sem_[i]) vkDestroySemaphore(device_, render_finished_sem_[i], nullptr);
            if (frame_fences_[i]) vkDestroyFence(device_, frame_fences_[i], nullptr);
        }
        if (cmd_pool_) { vkDestroyCommandPool(device_, cmd_pool_, nullptr); cmd_pool_ = VK_NULL_HANDLE; }
        if (swapchain_render_pass_) { vkDestroyRenderPass(device_, swapchain_render_pass_, nullptr); swapchain_render_pass_ = VK_NULL_HANDLE; }
        if (target_render_pass_) { vkDestroyRenderPass(device_, target_render_pass_, nullptr); target_render_pass_ = VK_NULL_HANDLE; }
        if (target_render_pass_color_) { vkDestroyRenderPass(device_, target_render_pass_color_, nullptr); target_render_pass_color_ = VK_NULL_HANDLE; }
        if (target_render_pass_depth_) { vkDestroyRenderPass(device_, target_render_pass_depth_, nullptr); target_render_pass_depth_ = VK_NULL_HANDLE; }
        for (auto& [k, l] : late_) {
            for (int i = 0; i < kFrames; i++) {
                if (l.rb_ptr[i]) {
                    vkUnmapMemory(device_, l.rb_mem[i]);
                    l.rb_ptr[i] = nullptr;
                }
                if (l.rb[i]) vkDestroyBuffer(device_, l.rb[i], nullptr);
                if (l.rb_mem[i]) vkFreeMemory(device_, l.rb_mem[i], nullptr);
            }
            if (l.down_img) vkDestroyImage(device_, l.down_img, nullptr);
            if (l.down_mem) vkFreeMemory(device_, l.down_mem, nullptr);
        }
        late_.clear();

        if (pipeline_cache_) {
            size_t sz = 0;
            if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, nullptr) == VK_SUCCESS && sz > 0) {
                std::vector<uint8_t> data(sz);
                if (vkGetPipelineCacheData(device_, pipeline_cache_, &sz, data.data()) == VK_SUCCESS) {
                    FILE* fc = fopen("vk_pipeline_cache.bin", "wb");
                    if (fc) {
                        fwrite(data.data(), 1, data.size(), fc);
                        fclose(fc);
                    }
                }
            }
            vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
            pipeline_cache_ = VK_NULL_HANDLE;
        }

        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

void VulkanBackend::DestroyTargets() {
    if (!device_) return;
    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd_buffers_[frame_index_]);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }
    for (auto& [k, t] : targets_) {
        if (t.fb_depth) vkDestroyFramebuffer(device_, t.fb_depth, nullptr);
        if (t.fb_color) vkDestroyFramebuffer(device_, t.fb_color, nullptr);
        if (t.fb_depth_only) vkDestroyFramebuffer(device_, t.fb_depth_only, nullptr);
        if (t.srv_view && t.srv_view != t.view) vkDestroyImageView(device_, t.srv_view, nullptr);
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.image) vkDestroyImage(device_, t.image, nullptr);
        if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        if (t.fb_view) vkDestroyImageView(device_, t.fb_view, nullptr);
        if (t.fb_image) vkDestroyImage(device_, t.fb_image, nullptr);
        if (t.fb_memory) vkFreeMemory(device_, t.fb_memory, nullptr);
    }
    targets_.clear();
}

void VulkanBackend::ApplySettings() {
    float want = DesiredScale();
    if (std::fabs(want - scale_) > 1e-4f) {
        vkDeviceWaitIdle(device_);
        DestroyTargets();
        scale_ = want;
        LOGI("vulkan: render scale updated to %.3f (%ux%u scene)",
             scale_, uint32_t(std::lround(guest_w_ * scale_)), uint32_t(std::lround(guest_h_ * scale_)));
    }
}

// ------------------------------------------------------------------------------------------------
// Targets & Textures
// ------------------------------------------------------------------------------------------------

TargetVk* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.data) return nullptr;
    uint32_t vdata = mem::VirtualFromPhysical(s.data);
    uint64_t key = (uint64_t(vdata) << 1) | (depth ? 1 : 0);
    auto it = targets_.find(key);
    return it == targets_.end() ? nullptr : &it->second;
}

TargetVk* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header && !s.data) return nullptr;
    uint32_t vdata = mem::VirtualFromPhysical(s.data);
    uint64_t key = (uint64_t(vdata) << 1) | (depth ? 1 : 0);
    auto it = targets_.find(key);
    uint32_t gw = 0, gh = 0;
    SurfaceSize(s, gw, gh);
    if (!gw || !gh) {
        gw = guest_w_;
        gh = guest_h_;
    }
    uint32_t w = std::max(1u, uint32_t(std::lround(gw * scale_)));
    uint32_t h = std::max(1u, uint32_t(std::lround(gh * scale_)));

    if (it != targets_.end()) {
        if (it->second.gw == gw && it->second.gh == gh && it->second.w == w && it->second.h == h) {
            return &it->second;
        }
        if (in_target_render_pass_ && (current_rt_ == &it->second || current_ds_ == &it->second)) {
            vkCmdEndRenderPass(cmd_buffers_[frame_index_]);
            in_target_render_pass_ = false;
            current_rt_ = nullptr;
            current_ds_ = nullptr;
        }
        auto& old_t = it->second;
        if (old_t.fb_depth) frame_garbage_[frame_index_].push_back({VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, old_t.fb_depth});
        if (old_t.fb_color) frame_garbage_[frame_index_].push_back({VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, old_t.fb_color});
        if (old_t.fb_depth_only) frame_garbage_[frame_index_].push_back({VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, old_t.fb_depth_only});
        if (old_t.srv_view && old_t.srv_view != old_t.view) frame_garbage_[frame_index_].push_back({VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, old_t.srv_view, VK_NULL_HANDLE});
        frame_garbage_[frame_index_].push_back({old_t.image, VK_NULL_HANDLE, old_t.memory, old_t.view, VK_NULL_HANDLE});
        if (old_t.fb_image) {
            frame_garbage_[frame_index_].push_back({old_t.fb_image, VK_NULL_HANDLE, old_t.fb_memory, old_t.fb_view, VK_NULL_HANDLE});
        }
    }

    TargetVk t{};
    t.gw = gw;
    t.gh = gh;
    t.w = w;
    t.h = h;
    t.depth = depth;
    t.data = vdata;
    t.format = depth ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_B8G8R8A8_UNORM;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = t.format;
    ici.extent = {t.w, t.h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &ici, nullptr, &t.image) != VK_SUCCESS) return nullptr;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, t.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &mai, nullptr, &t.memory);
    vkBindImageMemory(device_, t.image, t.memory, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = t.format;
    vci.subresourceRange.aspectMask = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &vci, nullptr, &t.view);

    if (depth) {
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCreateImageView(device_, &vci, nullptr, &t.srv_view);
    } else {
        t.srv_view = t.view;
    }

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];
    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    if (depth) {
        Transition(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        VkClearDepthStencilValue clear_ds{1.0f, 0};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_ds, 1, &range);
        Transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        t.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    } else {
        Transition(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        VkClearColorValue clear_col{{0.0f, 0.0f, 0.0f, 0.0f}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_col, 1, &range);
        Transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    t.bytes = depth ? (gw * gh * 4) : (GuestPitch(gw) * gh);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);

    targets_[key] = t;
    return &targets_[key];
}

VkImageView VulkanBackend::Feedback(TargetVk& t) {
    if (t.depth) return VK_NULL_HANDLE;
    if (!t.fb_image) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = t.format;
        ici.extent = {t.w, t.h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ici, nullptr, &t.fb_image) != VK_SUCCESS) return VK_NULL_HANDLE;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, t.fb_image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device_, &mai, nullptr, &t.fb_memory);
        vkBindImageMemory(device_, t.fb_image, t.fb_memory, 0);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = t.fb_image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = t.format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        vkCreateImageView(device_, &vci, nullptr, &t.fb_view);
        t.fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];
    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    Transition(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    Transition(cmd, t.fb_image, t.fb_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent = {t.w, t.h, 1};
    vkCmdCopyImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    Transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    Transition(cmd, t.fb_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    t.fb_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t.fb_view;
}

bool VulkanBackend::DepthCopy(const State& s) {
    if (!s.rt.data || !s.textures[0]) return false;
    auto dit = targets_.find((uint64_t(mem::VirtualFromPhysical(s.rt.data)) << 1) | 1);
    if (dit == targets_.end()) return false;
    TextureDesc td{};
    if (!Describe(s.textures[0], td) || !td.data) return false;
    auto sit = targets_.find((uint64_t(td.data) << 1) | 1);
    if (sit == targets_.end() || sit == dit) return false;
    TargetVk& src = sit->second;
    TargetVk& dst = dit->second;
    if (src.w != dst.w || src.h != dst.h) return false;

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];
    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    Transition(cmd, src.image, src.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect);
    Transition(cmd, dst.image, dst.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = aspect;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = aspect;
    region.dstSubresource.layerCount = 1;
    region.extent = {src.w, src.h, 1};
    vkCmdCopyImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    Transition(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, aspect);
    Transition(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, aspect);
    src.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    dst.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    dst.drawn = true;
    return true;
}

bool VulkanBackend::LatchCpuWrite(TargetVk& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

void VulkanBackend::UploadTarget(TargetVk& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d)) {
        d.data = s.data ? mem::VirtualFromPhysical(s.data) : t.data;
        d.width = t.gw;
        d.height = t.gh;
        d.format = 0x12; // A8R8G8B8
        d.linear = true;
        d.pitch = GuestPitch(t.gw);
    }
    if (Format(d.format).depth) return;

    std::vector<uint8_t> decoded;
    uint32_t row_pitch = 0;
    const uint8_t* guest_ptr = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    if (!guest_ptr) return;

    DecodeLevel(d, 0, 0, guest_ptr, nullptr, decoded, row_pitch);
    if (decoded.empty()) return;

    UploadAlloc up = AllocUpload(decoded.size(), 256);
    if (!up.ptr) return;
    std::memcpy(up.ptr, decoded.data(), decoded.size());

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];
    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    if (d.width == t.w && d.height == t.h) {
        Transition(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy copy_region{};
        copy_region.bufferOffset = up.offset;
        copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.imageSubresource.layerCount = 1;
        copy_region.imageExtent = {t.w, t.h, 1};
        vkCmdCopyBufferToImage(cmd, up.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
        Transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    } else {
        VkImage tmp_img = VK_NULL_HANDLE;
        VkDeviceMemory tmp_mem = VK_NULL_HANDLE;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {d.width, d.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ici, nullptr, &tmp_img) == VK_SUCCESS) {
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device_, tmp_img, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(device_, &mai, nullptr, &tmp_mem);
            vkBindImageMemory(device_, tmp_img, tmp_mem, 0);
            frame_garbage_[frame_index_].push_back({tmp_img, VK_NULL_HANDLE, tmp_mem, VK_NULL_HANDLE, VK_NULL_HANDLE});

            Transition(cmd, tmp_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy_region{};
            copy_region.bufferOffset = up.offset;
            copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy_region.imageSubresource.layerCount = 1;
            copy_region.imageExtent = {d.width, d.height, 1};
            vkCmdCopyBufferToImage(cmd, up.buffer, tmp_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

            Transition(cmd, tmp_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            Transition(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(d.width), int32_t(d.height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {int32_t(t.w), int32_t(t.h), 1};
            vkCmdBlitImage(cmd, tmp_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            Transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    t.drawn = true;
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

bool VulkanBackend::BeginTargetRenderPass(TargetVk* rt, TargetVk* ds) {
    if (!rt && !ds) return false;
    VkCommandBuffer cmd = cmd_buffers_[frame_index_];

    if (in_target_render_pass_ && current_rt_ == rt && current_ds_ == ds) {
        return true;
    }

    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, Surface{0, rt->data, 0, rt->bytes});
        Transition(cmd, rt->image, rt->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        rt->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        rt->drawn = true;
    }
    if (ds) {
        Transition(cmd, ds->image, ds->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        ds->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ds->drawn = true;
    }

    bool has_depth = (ds != nullptr && ds->view != VK_NULL_HANDLE);
    VkRenderPass rp = rt ? (has_depth ? target_render_pass_ : target_render_pass_color_) : target_render_pass_depth_;
    VkFramebuffer& fb = rt ? (has_depth ? rt->fb_depth : rt->fb_color) : ds->fb_depth_only;

    if (!fb) {
        VkImageView atts[2]{};
        uint32_t att_count = 0;
        if (rt) atts[att_count++] = rt->view;
        if (has_depth) atts[att_count++] = ds->view;

        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rp;
        fci.attachmentCount = att_count;
        fci.pAttachments = atts;
        fci.width = rt ? rt->w : ds->w;
        fci.height = rt ? rt->h : ds->h;
        fci.layers = 1;
        vkCreateFramebuffer(device_, &fci, nullptr, &fb);
    }

    if (fb) {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rp;
        bi.framebuffer = fb;
        bi.renderArea.extent = {rt ? rt->w : ds->w, rt ? rt->h : ds->h};
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        in_target_render_pass_ = true;
        current_rt_ = rt;
        current_ds_ = ds;
        return true;
    }
    return false;
}

VkImageView VulkanBackend::BindStageTexture(const State& s, int stage, TargetVk* draw_rt, TargetVk* draw_ds, DrawTexturesVk& stages) {
    uint32_t header = s.textures[stage];
    if (!header) return VK_NULL_HANDLE;
    TextureDesc d;
    if (!Describe(header, d)) return VK_NULL_HANDLE;

    const FormatInfo& f = Format(d.format);
    stages.info[stage].kind = d.cube ? kTexCube : (d.depth > 1 ? kTex3D : kTex2D);
    stages.info[stage].rect = !d.cube && d.depth == 1 && d.linear;
    stages.info[stage].shadow = (f.depth && !d.cube && d.depth == 1) ? ((d.format == 0x2C || d.format == 0x30) ? 2 : 1) : 0;
    stages.levels[stage] = d.levels;

    uint64_t tgt_key = (uint64_t(d.data) << 1) | (f.depth ? 1 : 0);
    auto tit = targets_.find(tgt_key);
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !f.compressed) {
        TargetVk& tgt = tit->second;
        if (&tgt == draw_ds) return VK_NULL_HANDLE;
        if (!tgt.depth && LatchCpuWrite(tgt)) {
            Surface su{};
            su.header = header;
            su.data = tgt.data;
            UploadTarget(tgt, su);
        }
        constexpr float kUpscaledMin = 1.01f;
        float k = float(tgt.w) / float(std::max(1u, tgt.gw));
        bool blur = draw_rt && tgt.gw < guest_w_ && draw_rt->gw < guest_w_;
        if (!tgt.depth && k > kUpscaledMin && draw_rt && draw_rt != &tgt &&
            (blur || tgt.gw > draw_rt->gw || tgt.gh > draw_rt->gh)) {
            stages.wide[stage] = k;
            stages.blur[stage] = blur;
            stages.info[stage].flags |= kTexWide;
        }
        if (&tgt == draw_rt) {
            return Feedback(tgt);
        }
        if (in_target_render_pass_) {
            vkCmdEndRenderPass(cmd_buffers_[frame_index_]);
            in_target_render_pass_ = false;
            current_rt_ = nullptr;
            current_ds_ = nullptr;
        }
        if (tgt.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            Transition(cmd_buffers_[frame_index_], tgt.image, tgt.layout,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       tgt.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT);
            tgt.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        stages.rt[stage] = true;
        return tgt.srv_view;
    }

    StagePalette palette;
    if (d.format == 0x0B) {
        palette = ReadStagePalette(s, stage);
        if (!palette.va) return VK_NULL_HANDLE;
    }

    uint64_t key = (uint64_t(palette.va) << 32) | d.data;
    TextureVk& tx = textures_[key];
    uint64_t epoch = DirtyEpoch(d.data);
    uint64_t sig = tx.sig;
    if (tx.view && tx.check_frame != presents_) {
        sig = MemSig(d.data, TotalSize(d));
        tx.check_frame = presents_;
    }

    bool valid = tx.view && tx.format == d.format && tx.w == d.width && tx.h == d.height &&
                 tx.d == d.depth && tx.levels == d.levels && tx.pitch == d.pitch &&
                 tx.linear == d.linear && tx.cube == d.cube && tx.pal == palette.va &&
                 tx.pal_hash == palette.hash && tx.epoch == epoch && tx.sig == sig;
    if (valid) return tx.view;

    if (tx.view && tx.memory) {
        frame_garbage_[frame_index_].push_back({tx.image, VK_NULL_HANDLE, tx.memory, tx.view, VK_NULL_HANDLE});
    }
    tx = {};

    const uint8_t* src = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    if (!src) return VK_NULL_HANDLE;

    VkFormat vk_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    if (f.host == Host::BC1) vk_fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    else if (f.host == Host::BC2) vk_fmt = VK_FORMAT_BC2_UNORM_BLOCK;
    else if (f.host == Host::BC3) vk_fmt = VK_FORMAT_BC3_UNORM_BLOCK;
    else if (f.host == Host::R32F) vk_fmt = VK_FORMAT_R32_SFLOAT;

    uint32_t faces = d.cube ? 6 : 1;
    uint32_t levels = std::max(1u, d.levels);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = d.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.imageType = (d.depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = vk_fmt;
    ici.extent = {std::max(1u, d.width), std::max(1u, d.height), std::max(1u, d.depth)};
    ici.mipLevels = levels;
    ici.arrayLayers = faces;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &ici, nullptr, &tx.image) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, tx.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &tx.memory) != VK_SUCCESS) {
        vkDestroyImage(device_, tx.image, nullptr);
        tx.image = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    vkBindImageMemory(device_, tx.image, tx.memory, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = tx.image;
    vci.viewType = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : ((d.depth > 1) ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D);
    vci.format = vk_fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = levels;
    vci.subresourceRange.layerCount = faces;
    vkCreateImageView(device_, &vci, nullptr, &tx.view);

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];
    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    Transition(cmd, tx.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    std::vector<uint8_t> decoded;
    for (uint32_t face = 0; face < faces; face++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            uint32_t row_pitch = 0;
            DecodeLevel(d, lv, face, src, palette.va ? palette.colours : nullptr, decoded, row_pitch);
            if (decoded.empty()) continue;

            UploadAlloc up = AllocUpload(decoded.size(), 256);
            if (!up.ptr) continue;
            std::memcpy(up.ptr, decoded.data(), decoded.size());

            uint32_t mw = std::max(1u, d.width >> lv);
            uint32_t mh = std::max(1u, d.height >> lv);
            uint32_t md = std::max(1u, d.depth >> lv);

            VkBufferImageCopy copy_region{};
            copy_region.bufferOffset = up.offset;
            copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy_region.imageSubresource.mipLevel = lv;
            copy_region.imageSubresource.baseArrayLayer = face;
            copy_region.imageSubresource.layerCount = 1;
            copy_region.imageExtent = {mw, mh, md};
            vkCmdCopyBufferToImage(cmd, up.buffer, tx.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
        }
    }

    Transition(cmd, tx.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    tx.format = d.format;
    tx.w = d.width;
    tx.h = d.height;
    tx.d = d.depth;
    tx.levels = d.levels;
    tx.pitch = d.pitch;
    tx.linear = d.linear;
    tx.cube = d.cube;
    tx.data = d.data;
    tx.pal = palette.va;
    tx.pal_hash = palette.hash;
    tx.epoch = epoch;
    tx.sig = MemSig(d.data, TotalSize(d));
    tx.check_frame = presents_;
    return tx.view;
}

VkSampler VulkanBackend::GetSampler(const State& s, int stage, const DrawTexturesVk& stages, bool clamp) {
    const auto& t = s.tss[stage];
    uint32_t u = t[kTssAddressU];
    uint32_t v = t[kTssAddressV];
    uint32_t w = t[kTssAddressW];
    if (clamp && !stages.info[stage].rect && stages.info[stage].kind == kTex2D) {
        if (!u || u == 1) u = 3;
        if (!v || v == 1) v = 3;
    }

    uint32_t min_f = t[kTssMinFilter];
    uint32_t mag_f = t[kTssMagFilter];
    uint32_t mip_f = t[kTssMipFilter];
    bool anisotropic = (min_f == 3 || mag_f == 3);
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1u) : 1u;
    uint32_t max_mip = t[kTssMaxMipLevel];
    uint32_t bias_bits = t[kTssMipLodBias];
    uint32_t border = t[kTssBorderColor];
    uint32_t levels = stages.levels[stage];
    bool is_rt = stages.rt[stage];
    bool is_shadow = stages.info[stage].shadow != 0;

    uint32_t aniso_setting = app::Get().aniso;
    bool mipped = mip_f != 0 && levels > 1;
    if (aniso_setting && min_f >= 2 && mipped && !is_shadow && !is_rt) {
        an = aniso_setting;
    }

    uint32_t key_data[] = {
        u, v, w, min_f, mag_f, mip_f, an, max_mip, bias_bits, border, levels,
        uint32_t(is_rt), uint32_t(is_shadow)
    };
    uint64_t key = Hash(key_data, sizeof(key_data));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;

    auto addr_mode = [](uint32_t m) {
        switch (m) {
            case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case 3:
            case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    };

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    if (is_shadow) {
        sci.magFilter = (mag_f >= 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.minFilter = (min_f >= 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sci.compareEnable = VK_TRUE;
        sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        sci.maxLod = 0.0f;
    } else if (is_rt) {
        sci.magFilter = (mag_f == 1) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sci.minFilter = (min_f == 1) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
    } else {
        sci.magFilter = (mag_f >= 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.minFilter = (min_f >= 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = (mip_f >= 2) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = addr_mode(u);
        sci.addressModeV = addr_mode(v);
        sci.addressModeW = addr_mode(w);
        sci.mipLodBias = Bits(bias_bits);
        if (an > 1) {
            sci.anisotropyEnable = VK_TRUE;
            sci.maxAnisotropy = float(std::min(an, 16u));
        }
        sci.minLod = float(max_mip);
        sci.maxLod = (mip_f == 0 || levels <= 1) ? float(max_mip) : 16.0f;
        sci.borderColor = (border & 0x00FFFFFF) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE :
                          ((border >> 24) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    }

    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(device_, &sci, nullptr, &sampler);
    samplers_[key] = sampler;
    return sampler;
}

// ------------------------------------------------------------------------------------------------
// Pipeline creation
// ------------------------------------------------------------------------------------------------

VkPipeline VulkanBackend::GetPipeline(const State& s, const Assembled& a, bool has_color, bool has_depth, float zslope_scale,
                                     uint64_t vs_hash, uint64_t ps_hash, VkShaderModule vs_mod, VkShaderModule ps_mod) {
    PipelineKeyVk key{};
    key.vs_hash = vs_hash;
    key.ps_hash = ps_hash;
    key.topology = (uint32_t(a.topology) << 1) | (a.strip ? 1 : 0);
    key.blend_enable = (has_color && SimpleState(s, kMBlendEnable) != 0) ? 1 : 0;
    if (key.blend_enable) {
        key.src_blend = SimpleState(s, kMSrcBlend);
        key.dst_blend = SimpleState(s, kMDestBlend);
        key.blend_op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    key.color_write_mask = has_color ? (((cw >> 16) & 1) | (((cw >> 8) & 1) << 1) | ((cw & 1) << 2) | (((cw >> 24) & 1) << 3)) : 0;

    key.z_enable = (has_depth && s.rs[kRsZEnable]) ? 1 : 0;
    key.z_write = (key.z_enable && SimpleState(s, kMZWriteEnable)) ? 1 : 0;
    key.z_func = SimpleState(s, kMZFunc);
    key.stencil_enable = (has_depth && s.rs[kRsStencilEnable]) ? 1 : 0;
    if (key.stencil_enable) {
        key.stencil_func = SimpleState(s, kMStencilFunc);
        key.stencil_fail = s.rs[kRsStencilFail];
        key.stencil_zfail = SimpleState(s, kMStencilZFail);
        key.stencil_pass = SimpleState(s, kMStencilPass);
        key.stencil_mask = uint8_t(SimpleState(s, kMStencilMask) & 0xFF);
        key.stencil_wmask = uint8_t(SimpleState(s, kMStencilWriteMask) & 0xFF);
    }

    key.cull_mode = (a.topology == Topology::Triangles) ? s.rs[kRsCullMode] : 0;
    key.front_face = s.rs[kRsFrontFace];
    key.wireframe = (a.topology == Topology::Triangles && s.rs[kRsFillMode] == 0x1B01) ? 1 : 0;
    if (has_depth && a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        float bias = std::round(Bits(SimpleState(s, kMDepthOffset)) * 16777215.0f / s.zscale);
        float slope = Bits(SimpleState(s, kMSlopeScale)) * zslope_scale;
        std::memcpy(&key.zbias, &bias, 4);
        std::memcpy(&key.zslope, &slope, 4);
    }
    key.has_color = has_color ? 1 : 0;
    key.has_depth = has_depth ? 1 : 0;

    key.regs = a.regs;
    key.buffer_count = uint16_t(a.buffer_count);
    for (uint32_t b = 0; b < a.buffer_count; b++) {
        key.strides[b] = a.buffers[b].stride;
    }
    for (uint32_t r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const auto& el = a.elements[r];
            key.elements[r] = el.format | (uint32_t(el.buffer) << 8) | (uint32_t(el.offset) << 16);
        }
    }

    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_mod;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps_mod;
    stages[1].pName = "main";

    std::vector<VkVertexInputBindingDescription> bind_descs;
    bind_descs.reserve(a.buffer_count);
    for (uint32_t b = 0; b < a.buffer_count; b++) {
        bind_descs.push_back({b, a.buffers[b].stride, VK_VERTEX_INPUT_RATE_VERTEX});
    }

    std::vector<VkVertexInputAttributeDescription> attr_descs;
    uint32_t loc = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1)) continue;
        const auto& el = a.elements[r];
        attr_descs.push_back({loc++, el.buffer, VkFormatFromDxgi(el.format), el.offset});
    }

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(bind_descs.size());
    vi.pVertexBindingDescriptions = bind_descs.empty() ? nullptr : bind_descs.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attr_descs.size());
    vi.pVertexAttributeDescriptions = attr_descs.empty() ? nullptr : attr_descs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    if (a.topology == Topology::Points) {
        ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    } else if (a.topology == Topology::Lines) {
        ia.topology = a.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    } else {
        ia.topology = a.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }

    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = key.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    if (key.cull_mode != 0 && a.topology == Topology::Triangles) {
        rs.cullMode = VK_CULL_MODE_BACK_BIT;
        rs.frontFace = (key.cull_mode == 0x900) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    } else {
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    }
    if (key.zbias || key.zslope) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = Bits(key.zbias);
        rs.depthBiasSlopeFactor = Bits(key.zslope);
    }
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = key.z_enable ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = key.z_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VkComparisonOp(key.z_func);
    ds.stencilTestEnable = key.stencil_enable ? VK_TRUE : VK_FALSE;
    if (key.stencil_enable) {
        VkStencilOpState sop{};
        sop.failOp = VkStencilOperation(key.stencil_fail);
        sop.passOp = VkStencilOperation(key.stencil_pass);
        sop.depthFailOp = VkStencilOperation(key.stencil_zfail);
        sop.compareOp = VkComparisonOp(key.stencil_func);
        sop.compareMask = key.stencil_mask;
        sop.writeMask = key.stencil_wmask;
        sop.reference = 0;
        ds.front = sop;
        ds.back = sop;
    }

    VkPipelineColorBlendAttachmentState cba{};
    VkColorComponentFlags mask = 0;
    if (key.color_write_mask & 1) mask |= VK_COLOR_COMPONENT_R_BIT;
    if (key.color_write_mask & 2) mask |= VK_COLOR_COMPONENT_G_BIT;
    if (key.color_write_mask & 4) mask |= VK_COLOR_COMPONENT_B_BIT;
    if (key.color_write_mask & 8) mask |= VK_COLOR_COMPONENT_A_BIT;
    cba.colorWriteMask = mask;
    cba.blendEnable = key.blend_enable ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = VkBlend(key.src_blend, false);
    cba.dstColorBlendFactor = VkBlend(key.dst_blend, false);
    cba.colorBlendOp = VkBlendOperation(key.blend_op);
    cba.srcAlphaBlendFactor = VkBlend(key.src_blend, true);
    cba.dstAlphaBlendFactor = VkBlend(key.dst_blend, true);
    cba.alphaBlendOp = VkBlendOperation(key.blend_op);

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = has_color ? 1 : 0;
    cb.pAttachments = has_color ? &cba : nullptr;

    VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    VkPipelineDynamicStateCreateInfo dys{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dys.dynamicStateCount = 4;
    dys.pDynamicStates = dyn_states;

    VkRenderPass rp = has_color ? (has_depth ? target_render_pass_ : target_render_pass_color_) : target_render_pass_depth_;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = has_depth ? &ds : nullptr;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dys;
    pci.layout = pipeline_layout_;
    pci.renderPass = rp;
    pci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pci, nullptr, &pipeline) != VK_SUCCESS) {
        LOGE("vulkan: vkCreateGraphicsPipelines failed");
        return VK_NULL_HANDLE;
    }
    pipelines_[key] = pipeline;
    return pipeline;
}

// ------------------------------------------------------------------------------------------------
// Backend Methods
// ------------------------------------------------------------------------------------------------

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                         float z, uint32_t stencil) {
    static uint64_t s_clear_count = 0;
    if (++s_clear_count <= 10 || s_clear_count % 300 == 0) {
        LOGI("vulkan: Clear #%lu rt=%08X ds=%08X flags=%X col=%08X", s_clear_count, s.rt.data, s.zs.data, flags, color);
    }
    if (flags & kClearOcclusion) {
        return; // occlusion clear
    }
    TargetVk* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    TargetVk* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    if (!rt && !ds) return;

    if (!BeginTargetRenderPass(rt, ds)) return;

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];
    VkClearAttachment clear_atts[2]{};
    uint32_t att_count = 0;
    if (rt && (flags & kClearTarget)) {
        clear_atts[att_count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear_atts[att_count].colorAttachment = 0;
        float a = ((color >> 24) & 0xFF) / 255.0f;
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float g = ((color >> 8) & 0xFF) / 255.0f;
        float b = (color & 0xFF) / 255.0f;
        clear_atts[att_count].clearValue.color = {{r, g, b, a}};
        att_count++;
    }
    if (ds && (flags & (kClearZ | kClearStencil))) {
        clear_atts[att_count].aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                           ((flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        clear_atts[att_count].clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil};
        att_count++;
    }

    uint32_t tw = rt ? rt->w : ds->w;
    uint32_t th = rt ? rt->h : ds->h;
    uint32_t gw = rt ? rt->gw : ds->gw;
    uint32_t gh = rt ? rt->gh : ds->gh;
    if (!gw) gw = guest_w_;
    if (!gh) gh = guest_h_;
    float sc = float(tw) / float(gw);

    std::vector<VkClearRect> clear_rects;
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * 16, 16);
            int32_t x0 = std::clamp<int32_t>(int32_t(r[0] * sc), 0, tw);
            int32_t y0 = std::clamp<int32_t>(int32_t(r[1] * sc), 0, th);
            int32_t x1 = std::clamp<int32_t>(int32_t(r[2] * sc), 0, tw);
            int32_t y1 = std::clamp<int32_t>(int32_t(r[3] * sc), 0, th);
            if (x1 > x0 && y1 > y0) {
                clear_rects.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
            }
        }
    } else {
        const auto& v = s.viewport;
        int32_t x0 = std::clamp<int32_t>(int32_t(v.x * sc), 0, tw);
        int32_t y0 = std::clamp<int32_t>(int32_t(v.y * sc), 0, th);
        int32_t x1 = std::clamp<int32_t>(int32_t((v.x + v.w) * sc), 0, tw);
        int32_t y1 = std::clamp<int32_t>(int32_t((v.y + v.h) * sc), 0, th);
        if (x1 > x0 && y1 > y0) {
            clear_rects.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
        }
    }
    if (clear_rects.empty()) {
        clear_rects.push_back({{{0, 0}, {tw, th}}, 0, 1});
    }

    vkCmdClearAttachments(cmd, att_count, clear_atts, uint32_t(clear_rects.size()), clear_rects.data());
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (!count || (!s.rt.data && !s.zs.data)) return;
    if (DepthCopy(s)) return;

    Assembled a;
    if (!Assemble(s, prim, start, count, indices, a)) return;

    TargetVk* rt = EnsureTarget(s.rt, false);
    TargetVk* ds = nullptr;
    if (s.zs.data && (s.rs[kRsZEnable] || s.rs[kRsStencilEnable])) {
        ds = EnsureTarget(s.zs, true);
        if (ds && rt && (ds->w != rt->w || ds->h != rt->h)) {
            ds = nullptr;
        }
    }
    if (!rt && !ds) return;

    DrawTexturesVk stages{};
    VkImageView stage_views[4]{};
    for (int i = 0; i < 4; i++) {
        stage_views[i] = BindStageTexture(s, i, rt, ds, stages);
    }

    GuestExtent extent{rt ? rt->gw : ds->gw, rt ? rt->gh : ds->gh};

    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    PixelKey pk = MakePixelKey(s, stages.info);
    std::string ps_hlsl = PixelHlsl(pk);
    std::string vs_hlsl = prog ? VertexProgram(*s.vs, a.regs, a.decode) :
                                 FixedVertexHlsl(a.regs, a.pretransformed, a.decode, MakeFixedKey(s));

    uint64_t vs_hash = Hash(vs_hlsl.data(), vs_hlsl.size());
    uint64_t ps_hash = Hash(ps_hlsl.data(), ps_hlsl.size());

    VkShaderModule vs_mod = VK_NULL_HANDLE;
    VkShaderModule ps_mod = VK_NULL_HANDLE;

    auto it_vs = shader_modules_.find(vs_hash);
    if (it_vs != shader_modules_.end()) {
        vs_mod = it_vs->second;
    } else {
        auto res = compiler_.CompileGlslToSpv(vs_hlsl, shaderc_vertex_shader, "main", compile_options_);
        if (res.GetCompilationStatus() == shaderc_compilation_status_success) {
            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = (res.end() - res.begin()) * sizeof(uint32_t);
            smci.pCode = res.begin();
            vkCreateShaderModule(device_, &smci, nullptr, &vs_mod);
            shader_modules_[vs_hash] = vs_mod;
        } else {
            LOGE("vulkan: VS compile error: %s", res.GetErrorMessage().c_str());
        }
    }

    auto it_ps = shader_modules_.find(ps_hash);
    if (it_ps != shader_modules_.end()) {
        ps_mod = it_ps->second;
    } else {
        auto res = compiler_.CompileGlslToSpv(ps_hlsl, shaderc_fragment_shader, "main", compile_options_);
        if (res.GetCompilationStatus() == shaderc_compilation_status_success) {
            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = (res.end() - res.begin()) * sizeof(uint32_t);
            smci.pCode = res.begin();
            vkCreateShaderModule(device_, &smci, nullptr, &ps_mod);
            shader_modules_[ps_hash] = ps_mod;
        } else {
            LOGE("vulkan: PS compile error: %s", res.GetErrorMessage().c_str());
        }
    }

    if (!vs_mod || !ps_mod) return;

    bool has_color = (rt != nullptr && rt->view != VK_NULL_HANDLE);
    bool has_depth = (ds != nullptr && ds->view != VK_NULL_HANDLE);
    float zslope_scale = ds ? float(ds->w) / float(std::max(1u, ds->gw)) : 1.0f;
    VkPipeline pipeline = GetPipeline(s, a, has_color, has_depth, zslope_scale, vs_hash, ps_hash, vs_mod, ps_mod);
    if (!pipeline) return;

    std::array<VkBuffer, 16> v_buffers{};
    std::array<VkDeviceSize, 16> v_offsets{};
    for (uint32_t b = 0; b < a.buffer_count; b++) {
        const auto& buf = a.buffers[b];
        if (buf.src && buf.bytes) {
            UploadAlloc up = AllocUpload(buf.bytes, 16);
            if (!up.ptr) return;
            std::memcpy(up.ptr, buf.src, buf.bytes);
            v_buffers[b] = up.buffer;
            v_offsets[b] = up.offset;
        }
    }

    UploadAlloc ib_up{};
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        if (a.index_src && ibytes) {
            ib_up = AllocUpload(ibytes, 4);
            if (!ib_up.ptr) return;
            std::memcpy(ib_up.ptr, a.index_src, ibytes);
        }
    }

    UploadAlloc vc_up = AllocUpload(sizeof(s.vsc), 256);
    UploadAlloc dc_up = AllocUpload(sizeof(DrawConstants), 256);
    if (!vc_up.ptr || !dc_up.ptr) return;

    std::memcpy(vc_up.ptr, s.vsc.data(), sizeof(s.vsc));

    DrawConstants dc{};
    FillSurfaceConstants(s, extent, dc);
    FillFogConstants(s, dc);
    FillCombinerConstants(s, dc);
    FillTextureConstants(s, stages, dc);

    if (!prog && !a.pretransformed) {
        FixedKey fk = MakeFixedKey(s);
        FillTransformConstants(s, extent, dc);
        FillLightConstants(s, fk, dc);
    }
    std::memcpy(dc_up.ptr, &dc, sizeof(dc));

    if (!BeginTargetRenderPass(rt, ds)) return;
    VkCommandBuffer cmd = cmd_buffers_[frame_index_];

    bool clamp = (!prog && a.pretransformed && AnyStageWraps(s)) ? UnitTexcoords(a) : false;

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = frame_desc_pools_[frame_index_];
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &dsai, &set) == VK_SUCCESS) {
        VkDescriptorBufferInfo b0_info{vc_up.buffer, vc_up.offset, sizeof(s.vsc)};
        VkDescriptorBufferInfo b1_info{dc_up.buffer, dc_up.offset, sizeof(DrawConstants)};

        std::vector<VkWriteDescriptorSet> writes = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &b0_info, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &b1_info, nullptr},
        };

        std::array<VkDescriptorImageInfo, 4> img_infos{};
        std::array<VkDescriptorImageInfo, 4> smp_infos{};
        for (int i = 0; i < 4; i++) {
            VkImageView view = stage_views[i];
            if (!view) {
                if (stages.info[i].kind == kTexCube) view = dummy_cube_.view;
                else if (stages.info[i].kind == kTex3D) view = dummy_3d_.view;
                else view = dummy_texture_.view;
            }
            img_infos[i] = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            smp_infos[i] = {GetSampler(s, i, stages, clamp), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};

            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, uint32_t(2 + i), 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &img_infos[i], nullptr, nullptr});
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, uint32_t(6 + i), 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &smp_infos[i], nullptr, nullptr});
        }

        vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
    }

    float W = float(rt ? rt->w : ds->w);
    float H = float(rt ? rt->h : ds->h);
    VkViewport vp{0.0f, H, W, -H, 0.0f, 1.0f};

    float sc = W / float(extent.w);
    int32_t x0 = int32_t(s.viewport.x * sc);
    int32_t y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc);
    int32_t y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    x0 = std::clamp(x0, 0, int32_t(W));
    y0 = std::clamp(y0, 0, int32_t(H));
    x1 = std::clamp(x1, x0, int32_t(W));
    y1 = std::clamp(y1, y0, int32_t(H));

    VkRect2D sc_rect{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    if (sc_rect.extent.width == 0 || sc_rect.extent.height == 0) {
        return;
    }

    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc_rect);

    uint32_t bc = SimpleState(s, kMBlendColor);
    float blend_factor[4] = {
        float((bc >> 16) & 0xFF) / 255.0f,
        float((bc >> 8) & 0xFF) / 255.0f,
        float(bc & 0xFF) / 255.0f,
        float((bc >> 24) & 0xFF) / 255.0f,
    };
    vkCmdSetBlendConstants(cmd, blend_factor);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    if (a.buffer_count > 0) {
        vkCmdBindVertexBuffers(cmd, 0, a.buffer_count, v_buffers.data(), v_offsets.data());
    }

    if (a.index_count) {
        vkCmdBindIndexBuffer(cmd, ib_up.buffer, ib_up.offset, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(cmd, a.draw_count, 1, 0, 0);
    }
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points) {
    TargetVk* src_t = FindTarget(src, false);
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopySurfaceMemory %08X to %08X failed", src.data, dst.data);
        } else if (TargetVk* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }

    TargetVk* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    if (LatchCpuWrite(*src_t)) {
        UploadTarget(*src_t, src);
    }

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];
    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    Transition(cmd, src_t->image, src_t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    Transition(cmd, dst_t->image, dst_t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    float ss = float(src_t->w) / float(std::max(1u, src_t->gw));
    float ds = float(dst_t->w) / float(std::max(1u, dst_t->gw));

    std::vector<VkImageCopy> copies;
    for (uint32_t i = 0; i < n; i++) {
        int32_t sx0 = 0, sy0 = 0, sx1 = int32_t(std::min(src_t->w, dst_t->w)), sy1 = int32_t(std::min(src_t->h, dst_t->h));
        int32_t dx = 0, dy = 0;
        if (listed) {
            const auto* r = static_cast<const int32_t*>(kernel::HostPtr(src_rects + i * 16));
            if (!r) continue;
            sx0 = std::clamp(int32_t(std::lround(r[0] * ss)), 0, int32_t(src_t->w));
            sy0 = std::clamp(int32_t(std::lround(r[1] * ss)), 0, int32_t(src_t->h));
            sx1 = std::clamp(int32_t(std::lround(r[2] * ss)), sx0, int32_t(src_t->w));
            sy1 = std::clamp(int32_t(std::lround(r[3] * ss)), sy0, int32_t(src_t->h));
            dx = std::clamp(int32_t(std::lround(r[0] * ds)), 0, int32_t(dst_t->w));
            dy = std::clamp(int32_t(std::lround(r[1] * ds)), 0, int32_t(dst_t->h));
            if (dst_points) {
                const auto* p = static_cast<const int32_t*>(kernel::HostPtr(dst_points + i * 8));
                if (p) {
                    dx = std::clamp(int32_t(std::lround(p[0] * ds)), 0, int32_t(dst_t->w));
                    dy = std::clamp(int32_t(std::lround(p[1] * ds)), 0, int32_t(dst_t->h));
                }
            }
        }
        if (std::fabs(ss - ds) > 1e-4f) {
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {sx0, sy0, 0};
            blit.srcOffsets[1] = {sx1, sy1, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {dx, dy, 0};
            int32_t bw = int32_t(std::lround((sx1 - sx0) * ds / ss));
            int32_t bh = int32_t(std::lround((sy1 - sy0) * ds / ss));
            blit.dstOffsets[1] = {std::min(dx + bw, int32_t(dst_t->w)), std::min(dy + bh, int32_t(dst_t->h)), 1};
            if (blit.dstOffsets[1].x > dx && blit.dstOffsets[1].y > dy) {
                vkCmdBlitImage(cmd, src_t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            }
        } else {
            uint32_t cw = uint32_t(std::min(sx1 - sx0, int32_t(dst_t->w) - dx));
            uint32_t ch = uint32_t(std::min(sy1 - sy0, int32_t(dst_t->h) - dy));
            if (!cw || !ch) continue;
            VkImageCopy region{};
            region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.srcSubresource.layerCount = 1;
            region.srcOffset = {sx0, sy0, 0};
            region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.dstSubresource.layerCount = 1;
            region.dstOffset = {dx, dy, 0};
            region.extent = {cw, ch, 1};
            copies.push_back(region);
        }
    }

    if (!copies.empty()) {
        vkCmdCopyImage(cmd, src_t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    }

    Transition(cmd, src_t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    Transition(cmd, dst_t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    src_t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dst_t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dst_t->drawn = true;
}

void VulkanBackend::Present(const Surface& front) {
    int win_w = 0, win_h = 0;
    SDL_GetWindowSizeInPixels(window_, &win_w, &win_h);
    bool vsync_now = app::Get().vsync;
    if ((win_w > 0 && win_h > 0 && (uint32_t(win_w) != bb_w_ || uint32_t(win_h) != bb_h_)) ||
        vsync_now != vsync_) {
        RecreateSwapchain();
    }

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];

    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    TargetVk* front_t = EnsureTarget(front, false);
    if (front_t) {
        bool cpu_changed = LatchCpuWrite(*front_t);
        if (!front_t->drawn || cpu_changed) {
            UploadTarget(*front_t, front);
        }
    }

    uint32_t image_index = 0;
    VkResult res = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_sem_[frame_index_],
                                         VK_NULL_HANDLE, &image_index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        return;
    }

    if (front_t && front_t->image) {
        Transition(cmd, front_t->image, front_t->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        front_t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo blit_img_info{};
        blit_img_info.sampler = blit_sampler_;
        blit_img_info.imageView = front_t->view;
        blit_img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet blit_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        blit_write.dstSet = blit_sets_[frame_index_];
        blit_write.dstBinding = 0;
        blit_write.descriptorCount = 1;
        blit_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        blit_write.pImageInfo = &blit_img_info;
        vkUpdateDescriptorSets(device_, 1, &blit_write, 0, nullptr);
    }

    VkClearValue clear_color{{{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = swapchain_render_pass_;
    bi.framebuffer = swapchain_framebuffers_[image_index];
    bi.renderArea.extent = {bb_w_, bb_h_};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear_color;

    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

    if (front_t && front_t->image && blit_pipeline_) {
        float aspect = game::Aspect();
        float w = float(bb_w_);
        float h = float(bb_h_);
        float vw = std::min(w, h * aspect);
        float vh = vw / aspect;
        float x0 = (w - vw) * 0.5f;
        float y0 = (h - vh) * 0.5f;

        VkViewport blit_vp{x0, y0, vw, vh, 0.0f, 1.0f};
        VkRect2D blit_sc{{0, 0}, {bb_w_, bb_h_}};
        vkCmdSetViewport(cmd, 0, 1, &blit_vp);
        vkCmdSetScissor(cmd, 0, 1, &blit_sc);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blit_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blit_layout_, 0, 1,
                                &blit_sets_[frame_index_], 0, nullptr);

        int taps = std::clamp(int(std::ceil(float(front_t->h) / std::max(1.0f, vh) - 0.25f)), 1, 4);
        float pc[4] = {
            1.0f / float(front_t->w),
            1.0f / float(front_t->h),
            float(taps),
            (game::Opt().smaa || game::Opt().msaa > 0) ? 1.0f : 0.0f
        };
        vkCmdPushConstants(cmd, blit_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4, pc);

        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    if (imgui_initialized_) {
        std::lock_guard lock(app::UiLock());
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (app::kGame.ui) app::kGame.ui(fps_);
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &image_available_sem_[frame_index_];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_finished_sem_[frame_index_];

    vkResetFences(device_, 1, &frame_fences_[frame_index_]);
    vkQueueSubmit(graphics_queue_, 1, &si, frame_fences_[frame_index_]);

    present_fence_[presents_ % (kFrames + 1)].store(presents_);
    presents_seen_.store(presents_);
    frame_present_id_[frame_index_] = presents_ + 1;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_sem_[frame_index_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index;

    res = vkQueuePresentKHR(graphics_queue_, &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain();
    }

    presents_++;
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = double(fps_n_) / el;
        fps_n_ = 0;
        fps_t_ = now;
        game::g_fps = float(fps_);
    }

    frame_index_ = (frame_index_ + 1) % kFrames;

    vkWaitForFences(device_, 1, &frame_fences_[frame_index_], VK_TRUE, UINT64_MAX);
    completed_presents_.store(std::max(completed_presents_.load(), frame_present_id_[frame_index_]));
    FreeFrameGarbage(frame_index_);
    vkResetDescriptorPool(device_, frame_desc_pools_[frame_index_], 0);
    rings_[frame_index_].offset = 0;

    ApplySettings();

    VkCommandBuffer next_cmd = cmd_buffers_[frame_index_];
    vkResetCommandBuffer(next_cmd, 0);
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(next_cmd, &cbbi);
}

void VulkanBackend::Sync(uint32_t data, uint32_t) {
    uint32_t vdata = mem::VirtualFromPhysical(data);
    uint64_t key = (uint64_t(vdata) << 1);
    auto it = targets_.find(key);
    if (it == targets_.end() || !it->second.drawn) return;
    TargetVk& t = it->second;

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];
    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    VkImage read_img = t.image;
    VkImage tmp_down_img = VK_NULL_HANDLE;
    VkDeviceMemory tmp_down_mem = VK_NULL_HANDLE;

    if (t.w != t.gw || t.h != t.gh) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = {t.gw, t.gh, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ici, nullptr, &tmp_down_img) == VK_SUCCESS) {
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device_, tmp_down_img, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(device_, &mai, nullptr, &tmp_down_mem);
            vkBindImageMemory(device_, tmp_down_img, tmp_down_mem, 0);

            Transition(cmd, tmp_down_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            Transition(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(t.w), int32_t(t.h), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {int32_t(t.gw), int32_t(t.gh), 1};
            vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp_down_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            Transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            Transition(cmd, tmp_down_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            read_img = tmp_down_img;
        }
    } else {
        Transition(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    VkBuffer read_buf = VK_NULL_HANDLE;
    VkDeviceMemory read_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size_t(t.gw) * t.gh * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(device_, &bci, nullptr, &read_buf);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, read_buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &mai, nullptr, &read_mem);
    vkBindBufferMemory(device_, read_buf, read_mem, 0);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {t.gw, t.gh, 1};
    vkCmdCopyImageToBuffer(cmd, read_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_buf, 1, &region);

    if (read_img == t.image) {
        Transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);

    void* ptr = nullptr;
    vkMapMemory(device_, read_mem, 0, bci.size, 0, &ptr);
    if (ptr) {
        uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(vdata));
        if (guest) {
            uint32_t dpitch = GuestPitch(t.gw);
            const uint8_t* src_rows = static_cast<const uint8_t*>(ptr);
            std::vector<uint8_t> px(size_t(t.gw) * t.gh * 4);
            for (uint32_t y = 0; y < t.gh; y++) {
                std::memcpy(guest + size_t(y) * dpitch, src_rows + size_t(y) * t.gw * 4, size_t(t.gw) * 4);
                std::memcpy(&px[size_t(y) * t.gw * 4], src_rows + size_t(y) * t.gw * 4, size_t(t.gw) * 4);
            }
            t.read = std::move(px);
            t.drawn = false;
            t.epoch = DirtyEpoch(t.data);
            t.sig = MemSig(t.data, t.bytes);
        }
        vkUnmapMemory(device_, read_mem);
    }
    vkDestroyBuffer(device_, read_buf, nullptr);
    vkFreeMemory(device_, read_mem, nullptr);

    if (tmp_down_img != VK_NULL_HANDLE) {
        vkDestroyImage(device_, tmp_down_img, nullptr);
        vkFreeMemory(device_, tmp_down_mem, nullptr);
    }

    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &cbbi);
}

void VulkanBackend::Flush() {
    if (device_) vkDeviceWaitIdle(device_);
}

void VulkanBackend::SetGamma(const uint8_t*) {
    // Gamma ramp stored for post processing
}

void VulkanBackend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    LateVk& l = it->second;
    uint64_t done = completed_presents_.load();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.frame[i] && l.frame[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.frame[i] > l.frame[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) return false;
    if (l.frame[best] > l.delivered && l.rb_ptr[best]) {
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        if (dst) {
            const uint8_t* p = static_cast<const uint8_t*>(l.rb_ptr[best]);
            for (uint32_t y = 0; y < l.gh; y++) {
                std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.gw * 4, l.gw * 4);
            }
        }
        l.delivered = l.frame[best];
    }
    return true;
}

void VulkanBackend::RecordLate(uint32_t data) {
    uint32_t vdata = mem::VirtualFromPhysical(data);
    uint64_t key = (uint64_t(vdata) << 1);
    auto it = targets_.find(key);
    if (it == targets_.end() || !it->second.drawn) return;
    TargetVk& t = it->second;

    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();

    std::lock_guard lock(late_lock_);
    LateVk& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        for (int i = 0; i < kFrames; i++) {
            if (l.rb_ptr[i]) {
                vkUnmapMemory(device_, l.rb_mem[i]);
                l.rb_ptr[i] = nullptr;
            }
            if (l.rb[i]) { vkDestroyBuffer(device_, l.rb[i], nullptr); l.rb[i] = VK_NULL_HANDLE; }
            if (l.rb_mem[i]) { vkFreeMemory(device_, l.rb_mem[i], nullptr); l.rb_mem[i] = VK_NULL_HANDLE; }
        }
        if (l.down_img) { vkDestroyImage(device_, l.down_img, nullptr); l.down_img = VK_NULL_HANDLE; }
        if (l.down_mem) { vkFreeMemory(device_, l.down_mem, nullptr); l.down_mem = VK_NULL_HANDLE; }
        l = LateVk{};
        l.gw = t.gw;
        l.gh = t.gh;
    }

    uint32_t slot = l.next;
    uint32_t bytes = t.gw * t.gh * 4;

    if (!l.rb[slot]) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(device_, &bci, nullptr, &l.rb[slot]) == VK_SUCCESS) {
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device_, l.rb[slot], &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(device_, &mai, nullptr, &l.rb_mem[slot]);
            vkBindBufferMemory(device_, l.rb[slot], l.rb_mem[slot], 0);
            vkMapMemory(device_, l.rb_mem[slot], 0, bytes, 0, &l.rb_ptr[slot]);
        }
    }
    if (!l.rb[slot] || !l.rb_ptr[slot]) return;

    VkCommandBuffer cmd = cmd_buffers_[frame_index_];
    if (in_target_render_pass_) {
        vkCmdEndRenderPass(cmd);
        in_target_render_pass_ = false;
        current_rt_ = nullptr;
        current_ds_ = nullptr;
    }

    VkImage src_image = t.image;
    if (t.w != t.gw || t.h != t.gh) {
        if (!l.down_img) {
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_B8G8R8A8_UNORM;
            ici.extent = {t.gw, t.gh, 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device_, &ici, nullptr, &l.down_img) == VK_SUCCESS) {
                VkMemoryRequirements req;
                vkGetImageMemoryRequirements(device_, l.down_img, &req);
                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                vkAllocateMemory(device_, &mai, nullptr, &l.down_mem);
                vkBindImageMemory(device_, l.down_img, l.down_mem, 0);
            }
        }
        if (l.down_img) {
            Transition(cmd, l.down_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            Transition(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(t.w), int32_t(t.h), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {int32_t(t.gw), int32_t(t.gh), 1};
            vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.down_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            Transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            Transition(cmd, l.down_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            src_image = l.down_img;
        }
    } else {
        Transition(cmd, t.image, t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {t.gw, t.gh, 1};
    vkCmdCopyImageToBuffer(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.rb[slot], 1, &region);

    if (src_image == t.image) {
        Transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    l.frame[slot] = presents_ + 1;
    l.next = (slot + 1) % kFrames;
}

void VulkanBackend::WaitGpuFrame(uint64_t n) {
    uint64_t target_present = present_fence_[n % (kFrames + 1)].load();
    if (target_present > completed_presents_.load()) {
        uint32_t fi = target_present % kFrames;
        vkWaitForFences(device_, 1, &frame_fences_[fi], VK_TRUE, UINT64_MAX);
        completed_presents_.store(std::max(completed_presents_.load(), target_present));
    }
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() {
    return std::make_unique<VulkanBackend>();
}

void WaitGpuFrame(uint64_t n) {
    if (g_vulkan_backend) g_vulkan_backend->WaitGpuFrame(n);
}

}  // namespace gpu
