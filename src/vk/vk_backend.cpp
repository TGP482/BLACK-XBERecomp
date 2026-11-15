// Frames, targets, textures and draws: the D3D12 renderer's core, on Vulkan
#include "vk/vk_backend.h"
#include <vk_mem_alloc.h>  // DIAG(heap): VmaBudget
#if defined(__APPLE__)
#include <mach/mach.h>  // DIAG(memory): resident size and footprint
#endif
#include <sys/stat.h>
#include <unistd.h>

#include <dxgiformat.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "app/app.h"
#include "common/options.h"
#include "core/diag.h"
#include "core/log.h"
#include "d3d/occlusion.h"
#include "gpu/texturedumper.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu::vk {

void DiagReadbackTick();  // vk_readback.cpp: the read-back summary, written even when it is zero

Perf g_perf;

namespace {

PFN_vkCmdPushDescriptorSetKHR g_push = nullptr;

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// The game's 2D layer (menus, text, HUD) leaves oPos.w at 0: screen space. Clamped to 2^-64 its clip space
// coordinates are near 1e-20 and AMD drops parts of those draws; w = 1 draws the same, as XYZRHW does
std::string VertexProgram(const VertexShader& vs, uint16_t regs, uint64_t decode) {
    const std::string clamp = "    p.w = magclamp(p.w);\n";
    const std::string screen_space = "    p.w = p.w == 0 ? 1 : magclamp(p.w);\n";
    std::string src = VertexProgramHlsl(vs.program, regs, decode);
    size_t at = src.find(clamp);
    if (at != std::string::npos) src.replace(at, clamp.size(), screen_space);
    return src;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text. Upscaled, the far edge's last half
// texel is drawn too and wrap blends in the opposite edge (thin lines on HUD sprites); clamp takes the edge texel.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
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

constexpr uint32_t kTaddressWrap = 1;

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    return false;
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }


// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    return false;
}


// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}


TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// a pack image's DXGI format as this renderer uploads it, undefined for one it has no use for
VkFormat PackFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: return HostFormat(Host::BGRA8);
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_BC1_UNORM: return HostFormat(Host::BC1);
        case DXGI_FORMAT_BC2_UNORM: return HostFormat(Host::BC2);
        case DXGI_FORMAT_BC3_UNORM: return HostFormat(Host::BC3);
        default: return VK_FORMAT_UNDEFINED;
    }
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;
    constexpr uint32_t kPaletteCommon = 0;
    constexpr uint32_t kPaletteData = 4;
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

DrawShaders ClassifyShaders(const State& s, const Assembled& a) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgram(*s.vs, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    k.wmask = ColorWriteMask(SimpleState(s, kMColorWriteEnable));
}

constexpr float kDepth24Max = 16777215.0f;

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    constexpr uint32_t kCullCw = 0x900;  // D3DCULL_CW
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

// alpha test off: compare always, in D3D12's comparison numbering minus 1 like the enabled test
constexpr float kAlphaTestAlways = 7.0f;

float CompareIndex(uint32_t d3d) { return float(d3d >= 0x200 && d3d <= 0x207 ? d3d - 0x200 : 7); }

void FillFogConstants(const State& s, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? CompareIndex(SimpleState(s, kMAlphaFunc)) : kAlphaTestAlways;
    c.misc[2] = CompareIndex(s.rs[kRsShadowFunc]);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] + x[r * 4 + 3] * y[12 + col];
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w;
    float sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1;
    float oy = 1 - (2.0f * v.y + v.h) / size.h;
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

// D3DLIGHT8 as floats
constexpr int kLightFloats = 26;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

VkPrimitiveTopology PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (a.topology == Topology::Lines) return a.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    return a.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkPrimitiveTopology TopologyClass(uint32_t topo) {
    switch (topo) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;
constexpr float kUpscaledMin = 1.01f;
constexpr float kGlowBoxTexels = 3.0f;
constexpr float kBlurTapTexels = 7.0f / 6;

// copies a decoded level's rows into upload memory: rows past the source's are zero
void CopyLevelRows(uint8_t* out, uint32_t rows, uint32_t slices, uint32_t row_size, const std::vector<uint8_t>& px,
                   uint32_t pitch) {
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * row_size;
            if (y < src_rows) {
                size_t n = std::min<size_t>(pitch, row_size);
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], n);
                if (n < row_size) std::memset(dst + n, 0, row_size - n);
            } else {
                std::memset(dst, 0, row_size);
            }
        }
    }
}

struct GuestRect {
    int32_t left, top, right, bottom;
};
struct GuestPoint {
    int32_t x, y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * 16;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)), int32_t(kernel::R32(r + 12))};
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * 8;
    return {int32_t(kernel::R32(p)), int32_t(kernel::R32(p + 4))};
}

struct DrawnRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, dx = 0, dy = 0;
};

DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r{float(g.left), float(g.top), float(g.right), float(g.bottom), float(g.left), float(g.top)};
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(float(uint32_t(v)) * sc); }

constexpr float kTapsSlack = 1e-3f;

// ---------------------------------------------------------------- diagnostics

// DIAG(heap+churn): counters for memory pressure and cache turnover
uint64_t g_diag_evicted_textures = 0;
uint64_t g_diag_evicted_targets = 0;

// DIAG(all): XBE_DIAG opens the temporary lines below; the lookup happens once
bool DiagOn() {
    static const bool on = [] { return std::getenv("XBE_DIAG") != nullptr; }();
    return on;
}

// a counter as a delta since the last line: XBE_PERF's own log resets g_perf, so a smaller reading starts afresh
uint64_t Since(uint64_t v, uint64_t& last) {
    uint64_t d = v >= last ? v - last : v;
    last = v;
    return d;
}

double Since(double v, double& last) {
    double d = v >= last ? v - last : v;
    last = v;
    return d;
}

// ---------------------------------------------------------------- pipeline cache file

// a file past this is a driver writing something else: it is not read back in
constexpr size_t kCacheFileMax = 64u << 20;

// where the cache lives between runs: BLACK_PIPELINE_CACHE names the file, else it sits with the shader cache
const std::string& PipelineCachePath() {
    static const std::string path = [] {
        if (const char* env = std::getenv("BLACK_PIPELINE_CACHE"); env && *env) return std::string(env);
        const char* home = std::getenv("HOME");
        if (!home || !*home) return std::string("/tmp/black-pipeline.bin");
        std::string base = std::string(home) + "/Library/Caches";
        mkdir(base.c_str(), 0755);
        std::string dir = base + "/black";
        mkdir(dir.c_str(), 0755);
        return dir + "/pipeline.bin";
    }();
    return path;
}

bool ReadCacheFile(std::vector<uint8_t>& out) {
    const std::string& path = PipelineCachePath();
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || st.st_size < int64_t(sizeof(VkPipelineCacheHeaderVersionOne))) return false;
    if (size_t(st.st_size) > kCacheFileMax) return false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.resize(size_t(st.st_size));
    size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (n != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

// a cache from another GPU or driver is data the driver may trust: the header has to name this device
bool CacheBelongsToDevice(const std::vector<uint8_t>& data, const Caps& caps) {
    if (data.size() < sizeof(VkPipelineCacheHeaderVersionOne)) return false;
    VkPipelineCacheHeaderVersionOne h{};
    std::memcpy(&h, data.data(), sizeof(h));
    if (h.headerSize != sizeof(VkPipelineCacheHeaderVersionOne)) return false;
    if (h.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) return false;
    if (h.vendorID != caps.vendor_id || h.deviceID != caps.device_id) return false;
    return std::memcmp(h.pipelineCacheUUID, caps.cache_uuid, VK_UUID_SIZE) == 0;
}

struct DiagTick {
    PerfClock::time_point start = PerfClock::now();
    Perf prev;
    uint64_t frames = 0;
    uint64_t pso_keys = 0;  // pipelines EnsurePipeline made since the last line
    double pso_ms = 0;      // what vkCreateGraphicsPipelines spent on them
} g_diag;

// DIAG(upload pressure): is the texture cache growing without bound, and are the 16 MB upload rings overflowing?
// DIAG(draw churn): are pipelines created mid level, and how often? once a second, per frame averages
void DiagLine(VmaAllocator_T* vma, VkPhysicalDevice phys, size_t live_textures, size_t live_targets, size_t live_psos,
              size_t live_pass_psos) {
    DiagTick& d = g_diag;
    d.frames++;
    double el = PerfMs(d.start);
    if (el < 1000.0) return;
    Perf& p = g_perf;
    double n = double(d.frames);
    uint64_t upload = Since(p.upload, d.prev.upload), overflow = Since(p.overflow, d.prev.overflow);
    uint64_t texups = Since(p.texups, d.prev.texups), tex_bytes = Since(p.texup_bytes, d.prev.texup_bytes);
    uint64_t newtex = Since(p.newtex, d.prev.newtex), newtargets = Since(p.targets, d.prev.targets);
    uint64_t draws = Since(p.draws, d.prev.draws), raw = Since(p.raw, d.prev.raw);
    uint64_t made = Since(p.psos, d.prev.psos), shaders = Since(p.shaders, d.prev.shaders);
    double tex_ms = Since(p.texup, d.prev.texup);
    uint64_t keys = d.pso_keys;
    LOGR("diag: %.1f fps | upload %.2f MB/frame, ring overflow %.2f/frame (ring %.0f MB) | texture uploads "
         "%.1f/frame %.2f MB %.2f ms CPU | new textures %.1f, new targets %.1f per frame | cache holds %zu "
         "textures, %zu targets",
         n * 1000.0 / el, double(upload) / n / 1048576.0, double(overflow) / n, double(kUploadSize) / 1048576.0,
         double(texups) / n, double(tex_bytes) / n / 1048576.0, tex_ms / n, double(newtex) / n,
         double(newtargets) / n, live_textures, live_targets);
    LOGR("diag: draws %.1f/frame (%.1f raw) | new PsoKeys %llu/s, pipelines made %llu/s in %.2f ms, shaders %llu/s | "
         "pipelines live %zu, pass pipelines live %zu",
         double(draws) / n, double(raw) / n, (unsigned long long)keys, (unsigned long long)made, d.pso_ms,
         (unsigned long long)shaders, live_psos, live_pass_psos);
    DiagReadbackTick();  // vk_readback.cpp: keeps the read-back line alive when it is zero
    // DIAG(memory): resident and physical footprint, the numbers an 8 GB machine runs out of
    mach_task_basic_info_data_t basic{};
    mach_msg_type_number_t bn = MACH_TASK_BASIC_INFO_COUNT;
    task_vm_info_data_t vm{};
    mach_msg_type_number_t vn = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&basic), &bn) == KERN_SUCCESS &&
        task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &vn) == KERN_SUCCESS)
    {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        std::vector<VmaBudget> budgets(mp.memoryHeapCount);
        double used = 0, budget = 0;
        if (vma) {
            vmaGetHeapBudgets(vma, budgets.data());
            for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
                if (!(mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
                used += double(budgets[i].usage) / 1048576.0;
                budget += double(budgets[i].budget) / 1048576.0;
            }
        }
        LOGR("diag: memory %.0f MB resident, %.0f MB footprint | heap %.0f/%.0f MB | evicted %llu textures, %llu targets",
             double(basic.resident_size) / 1048576.0, double(vm.phys_footprint) / 1048576.0, used, budget,
             (unsigned long long)g_diag_evicted_textures, (unsigned long long)g_diag_evicted_targets);
        g_diag_evicted_textures = 0;
        g_diag_evicted_targets = 0;
    }
    // prev keeps the last reading: the next line needs the delta from it, not from the start of the run
    d.start = PerfClock::now();
    d.frames = 0;
    d.pso_keys = 0;
    d.pso_ms = 0.0;
}

}  // namespace

// ---------------------------------------------------------------- shared helpers

uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

// ---------------------------------------------------------------- set up

VulkanBackend::~VulkanBackend() {
    if (!ctx_.dev) return;
    vkDeviceWaitIdle(ctx_.dev);
    ShutdownUi();
    for (auto& [k, p] : psos_)
        if (p) vkDestroyPipeline(ctx_.dev, p, nullptr);
    for (auto& [k, p] : pass_psos_)
        if (p) vkDestroyPipeline(ctx_.dev, p, nullptr);
    for (auto& [k, m] : modules_)
        if (m) vkDestroyShaderModule(ctx_.dev, m, nullptr);
    for (auto& [k, s] : samplers_) vkDestroySampler(ctx_.dev, s, nullptr);
    for (Frame& f : frames_) {
        for (VkSampler s : f.samplers) vkDestroySampler(ctx_.dev, s, nullptr);
        f.images.clear();
        f.buffers.clear();
        f.upload.reset();
        if (f.pool) vkDestroyCommandPool(ctx_.dev, f.pool, nullptr);
    }
    targets_.clear();
    textures_.clear();
    late_.clear();
    for (auto& d : dummy_) d.reset();
    lut_.reset();
    out_frame_.reset();
    out_panel_.reset();
    for (auto& r : smaa_) r.reset();
    for (auto& r : smaa_lut_) r.reset();
    for (auto& r : ao_) r.reset();
    if (cache_) vkDestroyPipelineCache(ctx_.dev, cache_, nullptr);
    if (layout_) vkDestroyPipelineLayout(ctx_.dev, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(ctx_.dev, set_layout_, nullptr);
    swap_.reset();
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    std::string why;
    if (!CompilerAvailable(why)) {
        LOGE("vulkan: no shader compiler: %s", why.c_str());
        return false;
    }
    std::vector<const char*> exts;
    if (!headless_) exts = app::VulkanInstanceExtensions();
    if (!ctx_.Init(exts.data(), uint32_t(exts.size()), !headless_)) return false;
    if (!headless_) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!app::CreateVulkanSurface(ctx_.instance, reinterpret_cast<uint64_t*>(&surface))) {
            LOGE("vulkan: no window surface");
            return false;
        }
        swap_ = std::make_unique<Swapchain>();
        if (!swap_->Init(ctx_, surface)) return false;
    }
    g_push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(ctx_.dev, "vkCmdPushDescriptorSet"));
    if (!g_push) g_push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(ctx_.dev, "vkCmdPushDescriptorSetKHR"));
    if (!g_push) {
        LOGE("vulkan: no push descriptors");
        return false;
    }
    CreateLayouts();
    if (!CreateFrames()) return false;
    Begin();
    CreateDummies();
    lut_ = MakeImage({.format = kBgra, .w = 256, .h = 1});
    if (!lut_) return false;
    Barrier(*lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    LoadSettings();
    LOGI("vulkan: %ux%u guest, render scale %.3f", width, height, scale_);
    return true;
}

// the D3D12 root signature as one push descriptor set: b0 b1, t0 to t3, s0 to s3
void VulkanBackend::CreateLayouts() {
    VkDescriptorSetLayoutBinding b[kBindingCount] = {};
    for (uint32_t i = 0; i < kBindingCount; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        b[i].descriptorType = i < kBindingTextures   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : i < kBindingSamplers ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                     : VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = kBindingCount;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(ctx_.dev, &li, nullptr, &set_layout_);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    vkCreatePipelineLayout(ctx_.dev, &pl, nullptr, &layout_);
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    // the previous run's pipelines: MoltenVK compiles HLSL-derived MSL to Metal once, and the file is what makes
    // the second launch skip it
    std::vector<uint8_t> initial;
    bool have_file = ReadCacheFile(initial);
    if (have_file && !CacheBelongsToDevice(initial, ctx_.caps)) {
        initial.clear();
        have_file = false;
        if (DiagOn()) LOGR("pipeline cache: stale file for another device %04x:%04x, ignored", ctx_.caps.vendor_id,
                           ctx_.caps.device_id);
    }
    if (have_file) {
        ci.initialDataSize = initial.size();
        ci.pInitialData = initial.data();
    }
    VkResult r = vkCreatePipelineCache(ctx_.dev, &ci, nullptr, &cache_);
    if (r != VK_SUCCESS && have_file) {
        // a driver that rejects the file still has to run: the cache is made empty instead
        LOGE("pipeline cache: the file was refused (%d), starting empty", int(r));
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        have_file = false;
        r = vkCreatePipelineCache(ctx_.dev, &ci, nullptr, &cache_);
    }
    cache_bytes_ = have_file ? initial.size() : 0;
    if (DiagOn()) {
        if (have_file)
            LOGR("pipeline cache: loaded %zu bytes from %s", cache_bytes_, PipelineCachePath().c_str());
        else
            LOGR("pipeline cache: loaded 0 bytes, empty (%s)", PipelineCachePath().c_str());
    }
}

bool VulkanBackend::CreateFrames() {
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = ctx_.family;
        if (vkCreateCommandPool(ctx_.dev, &pi, nullptr, &f.pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx_.dev, &ai, &f.cb);
        f.upload = MakeBuffer(kUploadSize,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              Memory::Upload);
        if (!f.upload) return false;
    }
    return true;
}

// null views for stages without a texture: a black transparent texel of each kind
void VulkanBackend::CreateDummies() {
    dummy_[kTex2D] = MakeImage({.format = kBgra});
    dummy_[kTex3D] = MakeImage({.format = kBgra, .depth = 2});
    dummy_[kTexCube] = MakeImage({.format = kBgra, .layers = 6, .cube = true});
    dummy_[kTexNone] = dummy_[kTex2D];
    VkClearColorValue black{};
    for (int i = 1; i < 4; i++) {
        Image& d = *dummy_[i];
        Barrier(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d.desc.layers};
        vkCmdClearColorImage(Cb(), d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
        Barrier(d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

// the cache is written from the per-frame tick: the process leaves through _Exit, so no destructor runs
void VulkanBackend::TickPipelineCache() {
    if (!cache_) return;
    PerfClock::time_point now = PerfClock::now();
    if (now - cache_saved_ < std::chrono::seconds(10)) return;
    cache_saved_ = now;
    size_t size = 0;
    if (vkGetPipelineCacheData(ctx_.dev, cache_, &size, nullptr) != VK_SUCCESS ||
        size < sizeof(VkPipelineCacheHeaderVersionOne))
    {
        // a device that keeps its cache to itself costs nothing but the call, once
        static bool warned = false;
        if (DiagOn() && !warned) LOGR("pipeline cache: the driver reports %zu bytes, nothing to save", size);
        warned = true;
        return;
    }
    if (size <= cache_bytes_) return;  // nothing new in it: the file on disk still says the same
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(ctx_.dev, cache_, &size, data.data()) != VK_SUCCESS) return;
    data.resize(size);
    const std::string& path = PipelineCachePath();
    std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        if (DiagOn()) LOGR("pipeline cache: %s is not writable", path.c_str());
        return;
    }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    // the rename publishes the file: a crash mid-write leaves the last good one in place
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        if (DiagOn()) LOGR("pipeline cache: write to %s failed", path.c_str());
        return;
    }
    if (DiagOn())
        LOGR("pipeline cache: saved %zu bytes, grew from %zu (%zu pipelines live)", data.size(), cache_bytes_,
             psos_.size() + pass_psos_.size());
    cache_bytes_ = data.size();
}

void VulkanBackend::LoadSettings() {
    scale_ = headless_ ? headless_scale_ : app::RenderScale(guest_h_);
    msaa_set_ = game::Opt().msaa;
    msaa_ = Samples(msaa_set_);
}

// nothing can be made without the allocator Init leaves behind: the title keeps locking and drawing after a renderer
// that failed to start, so the request is dropped rather than asserted on
ImageRef VulkanBackend::MakeImage(const ImageDesc& d) {
    if (!ctx_.vma) {
        LOG_FIRST(8, "vulkan: a %ux%u image dropped, the renderer has no device", d.w, d.h);
        return nullptr;
    }
    return ctx_.CreateImage(d);
}

BufferRef VulkanBackend::MakeBuffer(uint64_t size, VkBufferUsageFlags usage, Memory mem) {
    if (!ctx_.vma) {
        LOG_FIRST(8, "vulkan: a %llu byte buffer dropped, the renderer has no device",
                  static_cast<unsigned long long>(size));
        return nullptr;
    }
    return ctx_.CreateBuffer(size, usage, mem);
}

// the sample count the device has for the scene's formats, the one asked for or the next under it
uint32_t VulkanBackend::Samples(uint32_t want) {
    // sample counts are powers of two: 3 asks for 2
    uint32_t n = std::bit_floor(std::clamp(want, 1u, 8u));
    while (n > 1 && !((ctx_.caps.color_samples & n) && (ctx_.caps.depth_samples & n))) n >>= 1;
    LOGI("vulkan: MSAA %ux (asked %u)", n, want);
    return n;
}

// ---------------------------------------------------------------- recording

void VulkanBackend::Begin() {
    // the pack's settings and files, once a frame: a listing that changed releases every image it made
    if (pack_frame_ != frame_) {
        pack_frame_ = frame_;
        if (texturedumper::Update()) ReleasePackTextures();
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(Cb(), &bi);
    rendering_ = false;
    open_ = {};
    bound_rt_ = bound_ds_ = nullptr;
}

void VulkanBackend::Submit(bool wait, VkSemaphore acquired, VkSemaphore done) {
    EndRendering();
    VkCommandBuffer cb = Cb();
    vkEndCommandBuffer(cb);
    Frame& f = CurrentFrame();
    f.fence = ++fence_value_;
    VkSemaphoreSubmitInfo waits{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waits.semaphore = acquired;
    waits.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signals[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    signals[0].semaphore = ctx_.timeline;
    signals[0].value = fence_value_;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].semaphore = done;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cb;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = acquired ? 1 : 0;
    si.pWaitSemaphoreInfos = &waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = done ? 2 : 1;
    si.pSignalSemaphoreInfos = signals;
    if (VkResult r = vkQueueSubmit2(ctx_.queue, 1, &si, VK_NULL_HANDLE); r != VK_SUCCESS) {
        LOGE("vulkan: submit failed (%d)", int(r));
    }
    if (wait) {
        ctx_.Wait(fence_value_);
        vkResetCommandPool(ctx_.dev, f.pool, 0);
        f.images.clear();
        f.buffers.clear();
        for (VkSampler s : f.samplers) vkDestroySampler(ctx_.dev, s, nullptr);
        f.samplers.clear();
        f.used = 0;
        Begin();
    }
    // the frame boundary: Present's submit, once a frame
    if (!wait) {
        if (DiagOn()) DiagLine(ctx_.vma, ctx_.phys, textures_.size(), targets_.size(), psos_.size(), pass_psos_.size());
        TickPipelineCache();
    }
}

void VulkanBackend::Flush() { Submit(true); }

VulkanBackend::Alloc VulkanBackend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload->buffer, o, f.upload->map + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    BufferRef b = MakeBuffer(std::max<uint64_t>(size, 256),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             Memory::Upload);
    f.buffers.push_back(b);
    return {b->buffer, 0, b->map};
}

void VulkanBackend::EndRendering() {
    if (!rendering_) return;
    vkCmdEndRendering(Cb());
    rendering_ = false;
    open_ = {};
}

void VulkanBackend::Barrier(Image& img, VkImageLayout to) {
    if (img.layout == to && to != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return;
    EndRendering();
    Transition(ctx_, Cb(), img, to);
}

void VulkanBackend::BeginRendering(const Attachments& a, uint32_t w, uint32_t h) {
    if (rendering_ && open_ == a) return;
    EndRendering();
    // a scope's loads read what an earlier scope stored: ordered by a dependency, not by luck
    AttachmentDependency(ctx_, Cb());
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (a.color) {
        Barrier(*a.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        color.imageView = a.color->target;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (a.depth) {
        Barrier(*a.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depth.imageView = a.depth->target;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = a.color ? 1 : 0;
    ri.pColorAttachments = a.color ? &color : nullptr;
    ri.pDepthAttachment = a.depth ? &depth : nullptr;
    ri.pStencilAttachment = a.depth ? &depth : nullptr;
    vkCmdBeginRendering(Cb(), &ri);
    rendering_ = true;
    open_ = a;
}

// Direct3D's y up: the viewport is flipped so clip space and winding stay as the title has them
void VulkanBackend::SetViewport(float x, float y, float w, float h) {
    VkViewport vp{x, y + h, w, -h, 0, 1};
    vkCmdSetViewport(Cb(), 0, 1, &vp);
}

void VulkanBackend::SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    VkRect2D r{{std::max(x0, 0), std::max(y0, 0)}, {uint32_t(std::max(x1 - std::max(x0, 0), 0)), uint32_t(std::max(y1 - std::max(y0, 0), 0))}};
    vkCmdSetScissor(Cb(), 0, 1, &r);
}

void VulkanBackend::SetFullViewport(uint32_t w, uint32_t h) {
    SetViewport(0, 0, float(w), float(h));
    SetScissor(0, 0, int32_t(w), int32_t(h));
}

VkShaderModule VulkanBackend::Compile(const std::string& src, const char* entry, Stage stage, uint64_t& hash) {
    uint64_t seed = Hash(entry, std::strlen(entry), uint64_t(stage) + 1);
    hash = Hash(src.data(), src.size(), seed);
    auto it = modules_.find(hash);
    if (it != modules_.end()) return it->second;
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    std::string error;
    std::vector<uint32_t> spirv = CompileHlsl(src, entry, stage, error);
    VkShaderModule m = VK_NULL_HANDLE;
    if (spirv.empty()) {
        LOGE("vulkan: %s compile failed: %s", entry, error.c_str());
        LOGE("%s", src.c_str());
    } else {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spirv.size() * 4;
        ci.pCode = spirv.data();
        vkCreateShaderModule(ctx_.dev, &ci, nullptr, &m);
    }
    modules_[hash] = m;
    return m;
}

VkSampler VulkanBackend::EnsureSampler(const SamplerDesc& d) {
    uint64_t key = Hash(&d, sizeof(d));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    // a device's sampler count is limited: the cache starts over, the old ones go with the frame
    constexpr size_t kMaxSamplers = 2000;
    if (samplers_.size() >= kMaxSamplers) {
        for (auto& [k, s] : samplers_) CurrentFrame().samplers.push_back(s);
        samplers_.clear();
    }
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = d.mag;
    ci.minFilter = d.min;
    ci.mipmapMode = d.mip;
    ci.addressModeU = d.u;
    ci.addressModeV = d.v;
    ci.addressModeW = d.w;
    ci.mipLodBias = d.lod_bias;
    ci.anisotropyEnable = d.anisotropy > 1 && ctx_.caps.max_anisotropy > 1;
    ci.maxAnisotropy = std::min(float(d.anisotropy), ctx_.caps.max_anisotropy);
    ci.compareEnable = d.compare != 0;
    ci.compareOp = d.compare_op;
    ci.minLod = d.min_lod;
    ci.maxLod = d.max_lod;
    VkSamplerCustomBorderColorCreateInfoEXT border{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (ctx_.caps.custom_border) {
        std::memcpy(border.customBorderColor.float32, d.border, sizeof(d.border));
        border.format = VK_FORMAT_UNDEFINED;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        ci.pNext = &border;
    } else {
        // the nearest of the three fixed ones
        bool white = d.border[0] + d.border[1] + d.border[2] > 1.5f;
        ci.borderColor = d.border[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                         : white            ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                            : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    }
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(ctx_.dev, &ci, nullptr, &s);
    samplers_[key] = s;
    return s;
}

// the full screen passes' clamp samplers: linear at s0, point at s1 to s3
VkSampler VulkanBackend::BlitSampler(bool linear) {
    SamplerDesc d;
    d.min = d.mag = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    d.mip = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    d.u = d.v = d.w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return EnsureSampler(d);
}

void VulkanBackend::PushDescriptors(VkBuffer cb0, uint64_t off0, uint64_t size0, VkBuffer cb1, uint64_t off1,
                                    uint64_t size1, Image* const tex[4], const uint8_t kinds[4], VkSampler const smp[4]) {
    VkDescriptorBufferInfo bufs[2] = {{cb0, off0, size0}, {cb1, off1, size1}};
    VkDescriptorImageInfo imgs[4], smps[4];
    VkWriteDescriptorSet w[kBindingCount];
    for (uint32_t i = 0; i < kBindingCount; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    for (int i = 0; i < 2; i++) {
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[i].pBufferInfo = &bufs[i];
    }
    for (int i = 0; i < 4; i++) {
        Image* img = tex[i] ? tex[i] : dummy_[kinds ? kinds[i] : kTex2D].get();
        imgs[i] = {VK_NULL_HANDLE, img->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[kBindingTextures + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[kBindingTextures + i].pImageInfo = &imgs[i];
        smps[i] = {smp[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[kBindingSamplers + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[kBindingSamplers + i].pImageInfo = &smps[i];
    }
    g_push(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, kBindingCount, w);
}

// ---------------------------------------------------------------- targets

void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    if (open_.color == it->second.res.get() || open_.depth == it->second.res.get()) EndRendering();
    Release(std::move(it->second.res));
    Release(std::move(it->second.fb));
    targets_.erase(it);
    g_diag_evicted_targets++;  // DIAG(churn)
}

// the host image of a new target at render scale, the scene and its depth multisampled, cleared as D3D12 clears a view
bool VulkanBackend::CreateTargetResource(Target& t) {
    t.w = std::max(1u, uint32_t(std::lround(t.gw * scale_)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * scale_)));
    ImageDesc d;
    d.format = t.depth ? ctx_.caps.depth_format : t.fmt;
    d.w = t.w;
    d.h = t.h;
    d.samples = t.gw == guest_w_ && t.gh == guest_h_ ? msaa_ : 1;
    d.target = true;
    d.depth_stencil = t.depth;
    t.res = MakeImage(d);
    if (!t.res) {
        LOGE("vulkan: no %ux%u target with %u samples", t.w, t.h, d.samples);
        return false;
    }
    Image& img = *t.res;
    Barrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange r{img.aspect, 0, 1, 0, 1};
    if (t.depth) {
        VkClearDepthStencilValue one{1, 0};
        vkCmdClearDepthStencilImage(Cb(), img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &r);
    } else {
        VkClearColorValue black{};
        vkCmdClearColorImage(Cb(), img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
    }
    return true;
}

VulkanBackend::TargetSlot VulkanBackend::FindTargetSlot(const Surface& s, bool depth) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    slot.key = TargetMapKey(slot.data, depth);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

Target* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    return slot.sized ? &slot.it->second : nullptr;
}

Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t)) return nullptr;
    g_perf.targets++;
    t.data = slot.data;
    t.bytes = depth ? slot.gw * slot.gh * 4 : GuestPitch(slot.gw) * slot.gh;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    Target& ref = targets_[slot.key];
    ref = std::move(t);
    return &ref;
}

bool VulkanBackend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h || rt->Samples() != ds->Samples())) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Barrier(*rt->res, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rt->drawn = true;
        rt->fb_current = false;
    }
    if (ds) {
        Barrier(*ds->res, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        ds->drawn = true;
        ds->fb_current = false;
    }
    bound_rt_ = rt;
    bound_ds_ = ds;
    return true;
}

// BGRA pixels into a new w x h texture, left readable
ImageRef VulkanBackend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    ImageRef tex = MakeImage({.format = kBgra, .w = w, .h = h});
    if (!tex) return nullptr;
    Alloc a = Upload(uint64_t(w) * h * 4, 16);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * w * 4, &px[size_t(y) * pitch], w * 4);
    Barrier(*tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = a.off;
    c.imageSubresource = Layers(*tex);
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(Cb(), a.buf, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Barrier(*tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return tex;
}

// CPU pixels of a colour surface into its host target, scaled
void VulkanBackend::UploadTarget(Target& t, const Surface& s) {
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "vulkan: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ImageRef tex = CreatePixelTexture(px, pitch, d.width, d.height);
    if (!tex) return;
    BeginRendering({t.res.get(), nullptr}, t.w, t.h);
    SetFullViewport(t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(*tex, uv, {.alpha = true, .rtv = t.fmt, .over = over, .samples = t.Samples()});
    bound_rt_ = bound_ds_ = nullptr;
    t.fb_current = false;
    Release(std::move(tex));
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool VulkanBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube || d.depth != 1)
        return false;
    auto di = targets_.find(TargetMapKey(mem::VirtualFromPhysical(s.rt.data), true));
    auto si = targets_.find(TargetMapKey(d.data, true));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    Image* from = Flat(src);
    if (!from) return false;
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    if (!DepthPass(*from, *dst.res)) return false;
    dst.drawn = true;
    dst.fb_current = false;
    return true;
}

VkPipeline VulkanBackend::EnsureDepthCopyPipeline(bool ms, uint32_t samples) {
    uint64_t key = ~(uint64_t(ms) | uint64_t(samples) << 1);
    VkPipeline& pso = pass_psos_[key];
    if (pso) return pso;
    std::string hlsl = std::string(ms ? "#define MS\n" : "") + kDepthHlsl;
    uint64_t h;
    VkShaderModule vs = Compile(hlsl, "vs", Stage::Vertex, h);
    VkShaderModule ps = Compile(hlsl, "psmain", Stage::Pixel, h);
    if (!vs || !ps) return VK_NULL_HANDLE;
    pso = FullScreenPipeline(vs, "vs", ps, "psmain", VK_FORMAT_UNDEFINED, samples, nullptr, true);
    return pso;
}

// src's depth over dst, a multisampled src's first sample
bool VulkanBackend::DepthPass(Image& src, Image& dst) {
    bool ms = src.desc.samples > 1;
    VkPipeline pso = EnsureDepthCopyPipeline(ms, dst.desc.samples);
    if (!pso) return false;
    Barrier(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    BeginRendering({nullptr, &dst}, dst.desc.w, dst.desc.h);
    SetFullViewport(dst.desc.w, dst.desc.h);
    Image* tex[4] = {&src, nullptr, nullptr, nullptr};
    float zero[4] = {};
    DrawFullScreen(pso, zero, sizeof(zero), tex, BlitSampler(true), BlitSampler(false));
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// a single sample copy of the target to read: while drawing to it, or of a multisampled one
Image* VulkanBackend::Feedback(Target& t) {
    uint32_t samples = t.Samples();
    // nothing drew into a multisampled target since its copy: the copy still holds
    if (t.fb && samples > 1 && t.fb_current) {
        Barrier(*t.fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return t.fb.get();
    }
    if (!t.fb) {
        ImageDesc d = t.res->desc;
        d.samples = 1;
        d.target = t.depth;  // the depth copy is drawn
        t.fb = MakeImage(d);
        if (!t.fb) return nullptr;
    }
    if (t.depth) {
        if (!DepthPass(*t.res, *t.fb)) {
            Release(std::move(t.fb));
            return nullptr;
        }
        Barrier(*t.fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        t.fb_current = true;
        return t.fb.get();
    }
    Barrier(*t.res, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Barrier(*t.fb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkExtent3D ext{t.w, t.h, 1};
    if (samples > 1) {
        VkImageResolve r{Layers(*t.res), {}, Layers(*t.fb), {}, ext};
        vkCmdResolveImage(Cb(), t.res->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb->image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    } else {
        VkImageCopy c{Layers(*t.res), {}, Layers(*t.fb), {}, ext};
        vkCmdCopyImage(Cb(), t.res->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    Barrier(*t.fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t.fb_current = true;
    return t.fb.get();
}

// what a read of the target takes: the target itself, or a multisampled one's single sample copy
Image* VulkanBackend::Flat(Target& t) { return t.Samples() > 1 ? Feedback(t) : t.res.get(); }

// ---------------------------------------------------------------- full screen passes

VkPipeline VulkanBackend::FullScreenPipeline(VkShaderModule vs, const char* vs_entry, VkShaderModule ps,
                                             const char* ps_entry, VkFormat rtv, uint32_t samples,
                                             const VkPipelineColorBlendAttachmentState* blend, bool depth_out) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = vs_entry;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = ps_entry;
    // DXC names every entry point main in SPIR-V unless told: the source entry is what -E chose
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(samples);
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (depth_out) {
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }
    VkPipelineColorBlendAttachmentState plain{};
    plain.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = rtv != VK_FORMAT_UNDEFINED ? 1 : 0;
    cb.pAttachments = blend ? blend : &plain;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = cb.attachmentCount;
    ri.pColorAttachmentFormats = &rtv;
    if (depth_out) ri.depthAttachmentFormat = ri.stencilAttachmentFormat = ctx_.caps.depth_format;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    PerfClock::time_point t = PerfClock::now();
    VkResult r = vkCreateGraphicsPipelines(ctx_.dev, cache_, 1, &pi, nullptr, &p);
    double pso_ms = PerfMs(t);
    if (DiagOn()) {
        g_diag.pso_ms += pso_ms;
        LOGR("pipeline cache: pass pipeline in %.2f ms", pso_ms);
    }
    if (r != VK_SUCCESS) LOGE("vulkan: pass pipeline failed");
    return p;
}

// the full screen triangle: constants at b0 and b1, tex at t0 to t3, s0 and s1 as given, s2 and s3 point
void VulkanBackend::DrawFullScreen(VkPipeline pso, const void* constants, uint32_t bytes, Image* const tex[4], VkSampler s0,
                                   VkSampler s1) {
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, constants, std::min<uint32_t>(bytes, 256));
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    VkSampler point = BlitSampler(false);
    VkSampler smp[4] = {s0, s1, point, point};
    PushDescriptors(cb.buf, cb.off, 256, cb.buf, cb.off, 256, tex, nullptr, smp);
    vkCmdDraw(Cb(), 3, 1, 0, 0);
}

VkPipeline VulkanBackend::EnsureBlitPipeline(VkFormat rtv, bool over, uint32_t samples) {
    VkPipeline& pso = pass_psos_[uint64_t(rtv) | uint64_t(over) << 32 | uint64_t(samples) << 40];
    if (pso) return pso;
    uint64_t h;
    VkShaderModule vs = Compile(kBlitHlsl, "vs", Stage::Vertex, h);
    VkShaderModule ps = Compile(kBlitHlsl, "psmain", Stage::Pixel, h);
    if (!vs || !ps) return VK_NULL_HANDLE;
    VkPipelineColorBlendAttachmentState b{};
    b.colorWriteMask = 0xF;
    if (over) {
        // premultiplied colour, the target keeps its alpha
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    }
    pso = FullScreenPipeline(vs, "vs", ps, "psmain", rtv, samples, &b);
    return pso;
}

// src over the open scope's viewport: src and the gamma lookup are readable, the target open
void VulkanBackend::Blit(Image& src, const float uvrect[4], const BlitOptions& o) {
    VkPipeline pso = EnsureBlitPipeline(o.rtv, o.over, o.samples);
    if (!pso) return;
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    Image* tex[4] = {&src, lut_.get(), nullptr, nullptr};
    DrawFullScreen(pso, c, sizeof(c), tex, BlitSampler(true), BlitSampler(false));
}

VkPipeline VulkanBackend::EnsurePassPipeline(const char* hlsl, const char* vs_entry, const char* entry, VkFormat rtv, float blend) {
    uint64_t seed = uint64_t(rtv) << 1 | (blend >= 0 ? 1 : 0);
    uint64_t key = Hash(vs_entry, std::strlen(vs_entry), Hash(entry, std::strlen(entry), Hash(hlsl, std::strlen(hlsl), seed)));
    VkPipeline& pso = pass_psos_[key];
    if (pso) return pso;
    uint64_t h;
    VkShaderModule vs = Compile(hlsl, vs_entry, Stage::Vertex, h);
    VkShaderModule ps = Compile(hlsl, entry, Stage::Pixel, h);
    if (!vs || !ps) return VK_NULL_HANDLE;
    VkPipelineColorBlendAttachmentState b{};
    b.colorWriteMask = 0xF;
    if (blend >= 0) {
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = b.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        b.dstColorBlendFactor = b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    }
    pso = FullScreenPipeline(vs, vs_entry, ps, entry, rtv, 1, &b);
    return pso;
}

// a full screen pass into rt: src readable as t0 to t2, p at b1, blend >= 0 blends by that factor
bool VulkanBackend::Pass(const char* hlsl, const char* vs_entry, const char* entry, VkFormat rtv, Image& rt,
                         Image* const src[3], const float p[8], float blend) {
    VkPipeline pso = EnsurePassPipeline(hlsl, vs_entry, entry, rtv, blend);
    if (!pso) return false;
    for (int i = 0; i < 3; i++)
        if (src[i]) Barrier(*src[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    BeginRendering({&rt, nullptr}, rt.desc.w, rt.desc.h);
    SetFullViewport(rt.desc.w, rt.desc.h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        vkCmdSetBlendConstants(Cb(), f);
    }
    Image* tex[4] = {src[0], src[1], src[2], nullptr};
    DrawFullScreen(pso, p, 8 * sizeof(float), tex, BlitSampler(true), BlitSampler(false));
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// ---------------------------------------------------------------- textures

Image* VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                        DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    bool blur = rt && t.gw < guest_w_ && rt->gw < guest_w_;
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (blur || t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        stages.blur[stage] = blur;
        info.flags |= kTexWide;
    }
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    Image* r = Flat(t);
    if (r) Barrier(*r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return r;
}

bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    Release(std::move(tx.res));
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (IsBlockCompressed(tx.fmt) && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    ImageDesc id;
    id.format = tx.fmt;
    id.w = w;
    id.h = h;
    id.depth = d.depth;
    id.layers = d.cube ? 6 : 1;
    id.levels = tx.levels;
    id.cube = d.cube;
    tx.res = MakeImage(id);
    if (!tx.res) {
        LOG_FIRST(8, "vulkan: no %ux%u texture, format %X", w, h, d.format);
        textures_.erase(key);
        g_diag_evicted_textures++;  // DIAG(churn)
        return false;
    }
    return true;
}

void VulkanBackend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch) {
    const ImageDesc& id = tx.res->desc;
    uint32_t w = std::max(1u, id.w >> l), h = std::max(1u, id.h >> l), z = std::max(1u, id.depth >> l);
    uint32_t row = RowBytes(tx.fmt, w), rows = RowCount(tx.fmt, h);
    Alloc a = Upload(uint64_t(row) * rows * z, 16);
    CopyLevelRows(a.cpu, rows, z, row, px, pitch);
    VkBufferImageCopy c{};
    c.bufferOffset = a.off;
    c.imageSubresource = Layers(*tx.res, l, f);
    c.imageExtent = {w, h, z};
    vkCmdCopyBufferToImage(Cb(), a.buf, tx.res->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours) {
    Barrier(*tx.res, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Barrier(*tx.res, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
Image* VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                       DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, p, colours);
        }
    }
    // the pack's image for these bytes, drawn in their place; dumping alone keys them and draws nothing else
    if (tx.pack_gen != texturedumper::Generation()) LatchPackKey(tx, d, fi, colours);
    if (tx.pack_key) LatchPackImage(tx);
    const ImageRef& drawn = tx.swap ? tx.swap : tx.res;
    stages.levels[stage] = tx.swap ? tx.swap->desc.levels : tx.levels;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    return drawn.get();
}

// after new bytes or a texture setting change: the name the pack and the dumps know the bytes by, a P8 texture's
// with the stage's colours
void VulkanBackend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    uint64_t key = texturedumper::Key(d, fi, p, colours);
    texturedumper::Dump(key, d, p, colours);
    // dumping alone keys the bytes too: the pack image is not drawn then
    tx.pack_key = texturedumper::Replacing() ? key : 0;
}

// the pack image for tx's bytes once it has loaded; textures with the same bytes share its GPU copy
void VulkanBackend::LatchPackImage(Tex& tx) {
    auto it = pack_textures_.find(tx.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        texturedumper::Lookup got = texturedumper::Take(tx.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tx.pack_key = 0;
            return;
        }
        it = pack_textures_.emplace(tx.pack_key, Tex{}).first;
        CreatePackTexture(it->second, img);
    }
    if (it->second.res) tx.swap = it->second.res;
    tx.pack_key = 0;
}

void VulkanBackend::CreatePackTexture(Tex& r, const texturedumper::Image& img) {
    r.fmt = PackFormat(img.format);
    r.levels = uint32_t(img.levels.size());
    r.d.width = img.width;
    r.d.height = img.height;
    r.d.levels = r.levels;
    if (r.fmt == VK_FORMAT_UNDEFINED || !r.levels) {
        LOGE("texture pack: a %ux%u image has no format this renderer uploads", img.width, img.height);
        return;
    }
    ImageDesc id;
    id.format = r.fmt;
    id.w = img.width;
    id.h = img.height;
    id.levels = r.levels;
    r.res = MakeImage(id);
    if (!r.res) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    Barrier(*r.res, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (uint32_t l = 0; l < r.levels; l++) UploadTextureLevel(r, 0, l, img.levels[l].px, img.levels[l].pitch);
    Barrier(*r.res, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// the pack's files changed: each image is made again when next drawn; Generation moved on, so every texture lets go
// of its old copy before it draws
void VulkanBackend::ReleasePackTextures() {
    for (auto& entry : pack_textures_) Release(std::move(entry.second.res));
    pack_textures_.clear();
}

Image* VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(TargetMapKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, true, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

// textures may have recorded copies or passes that moved the targets out of their attachment layouts
bool VulkanBackend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    auto perf_t = PerfClock::now();
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    g_perf.bind += PerfMs(perf_t);
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    if (bound_rt_) Barrier(*bound_rt_->res, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (bound_ds_) Barrier(*bound_ds_->res, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    return true;
}

// ---------------------------------------------------------------- draws

void VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vh = vit->second;
        sh.vs = modules_[sh.vh];
        return;
    }
    sh.vs = Compile(VertexHlsl(s, a, sh), "main", Stage::Vertex, sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

void VulkanBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ph = pit->second;
        sh.ps = modules_[sh.ph];
        return;
    }
    sh.ps = Compile(PixelHlsl(pk), "main", Stage::Pixel, sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void VulkanBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: bias in depth buffer units, the target is 24 bit (or float near 1, where its units match);
    // the slope is per console pixel, and an upscaled target's pixels are smaller
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
    }
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? uint32_t(bound_rt_->fmt) : 0;
    uint32_t samples = (bound_rt_ ? bound_rt_ : bound_ds_)->Samples();
    k.msaa = samples > 1 ? samples : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second;
    g_perf.psos++;
    if (DiagOn()) g_diag.pso_keys++;  // DIAG(draw churn): this pipeline is made mid level, this second
    // the key's vertex attributes, in register order: location n is the n-th register the shader reads
    VkVertexInputAttributeDescription attrs[16];
    VkVertexInputBindingDescription binds[16];
    uint32_t na = 0, nb = 0;
    bool bound[16] = {};
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        uint32_t buffer = (e >> 8) & 0xFF;
        attrs[na] = {na, buffer, VertexFormat(e & 0xFF), e >> 16};
        na++;
        if (!bound[buffer]) {
            bound[buffer] = true;
            binds[nb++] = {buffer, 0, VK_VERTEX_INPUT_RATE_VERTEX};  // the stride is set per draw
        }
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb;
    vi.pVertexBindingDescriptions = binds;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = "main";
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = TopologyClass(k.topo);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.fill == kFillWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zbias != 0 || k.zslope != 0;
    rs.depthBiasConstantFactor = float(k.zbias);
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max(k.msaa, 1u));
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen;
    ds.depthWriteEnable = k.zwrite;
    ds.depthCompareOp = Compare(k.zfunc);
    ds.stencilTestEnable = k.sten;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), Compare(k.sfunc), k.smask, k.swmask, 0};
    ds.front = ds.back = so;
    VkPipelineColorBlendAttachmentState b{};
    b.blendEnable = k.blend;
    b.srcColorBlendFactor = Blend(k.src, false);
    b.dstColorBlendFactor = Blend(k.dst, false);
    b.colorBlendOp = BlendOp(k.op);
    b.srcAlphaBlendFactor = Blend(k.src, true);
    b.dstAlphaBlendFactor = Blend(k.dst, true);
    b.alphaBlendOp = BlendOp(k.op);
    b.colorWriteMask = k.wmask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &b;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkFormat rtfmt = VkFormat(k.rtfmt);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.has_rt ? 1 : 0;
    ri.pColorAttachmentFormats = &rtfmt;
    if (k.has_ds) ri.depthAttachmentFormat = ri.stencilAttachmentFormat = ctx_.caps.depth_format;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    PerfClock::time_point t = PerfClock::now();
    VkResult r = vkCreateGraphicsPipelines(ctx_.dev, cache_, 1, &pi, nullptr, &p);
    double pso_ms = PerfMs(t);
    if (DiagOn()) {
        g_diag.pso_ms += pso_ms;
        LOGR("pipeline cache: draw pipeline in %.2f ms", pso_ms);
    }
    if (r != VK_SUCCESS) LOGE("vulkan: pipeline creation failed");
    psos_[h] = p;
    return p;
}

void VulkanBackend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, the host half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    c.texsize[i][2] = -0.5f / (float(d.width) * k);
    c.texsize[i][3] = -0.5f / (float(d.height) * k);
    if (stages.wide[i] > 0) {
        bool blur = stages.blur[i];
        float fw = blur ? kBlurTapTexels : kGlowBoxTexels;
        float per_texel = blur ? 1.0f : 0.5f;
        float n = std::ceil(fw * stages.wide[i] * per_texel);
        c.spread[i][0] = fw / (n * float(d.width) * k);
        c.spread[i][1] = fw / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void VulkanBackend::FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void VulkanBackend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey&, const DrawTextures& stages,
                                    GuestExtent size, Alloc& vc, Alloc& dc) {
    vc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    dc = Upload(sizeof(DrawConstants), 256);
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, c);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    std::memcpy(dc.cpu, &c, sizeof(c));
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void VulkanBackend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    SetViewport(0, 0, W, H);
    float sc = W / gw;
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
    SetScissor(std::clamp(x0, 0, int32_t(W)), std::clamp(y0, 0, int32_t(H)), std::clamp(x1, 0, int32_t(W)),
               std::clamp(y1, 0, int32_t(H)));
}

void VulkanBackend::DrawGeometry(const Assembled& a) {
    VkBuffer bufs[16];
    VkDeviceSize offs[16], strides[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(std::max(b.bytes, 4u), 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        bufs[i] = vb.buf;
        offs[i] = vb.off;
        strides[i] = b.stride;
    }
    if (a.buffer_count) vkCmdBindVertexBuffers2(Cb(), 0, a.buffer_count, bufs, offs, nullptr, strides);
    vkCmdSetPrimitiveTopology(Cb(), PrimitiveTopology(a));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        vkCmdBindIndexBuffer(Cb(), ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(Cb(), a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(Cb(), a.draw_count, 1, 0, 0);
    }
}

bool VulkanBackend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                                 Assembled& a) {
    auto perf_t = PerfClock::now();
    bool assembled = Assemble(s, prim, start, count, indices, a);
    g_perf.assemble += PerfMs(perf_t);
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) return;
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) return;
    if (!BindTargets(s)) return;

    DrawShaders sh = ClassifyShaders(s, a);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs || !sh.ps) return;

    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    VkPipeline pso = EnsurePipeline(k, sh.vs, sh.ps);
    if (!pso) return;

    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    Alloc vc, dc;
    UploadConstants(s, sh, k, stages, size, vc, dc);
    if (cap_) {
        DrawConstants c{};
        std::memcpy(&c, dc.cpu, sizeof(c));
        CaptureDraw(s, prim, count, k, c, stages);
        CaptureShader("vs", sh.vkey, VertexHlsl(s, a, sh));
        CaptureShader("ps", sh.pkey, PixelHlsl(pk));
    }

    // samplers read the bound views' levels; a stage without a texture gets a dummy of its kind
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    uint32_t aniso = headless_ ? 0 : app::Get().aniso;
    VkSampler smp[4];
    uint8_t kinds[4];
    for (int i = 0; i < 4; i++) {
        smp[i] = EnsureSampler(StageSampler(s, i, clamp, aniso, stages.levels[i] > 1, stages.info[i].shadow != 0));
        kinds[i] = stages.info[i].kind;
    }

    Target* dims = bound_rt_ ? bound_rt_ : bound_ds_;
    BeginRendering({bound_rt_ ? bound_rt_->res.get() : nullptr, bound_ds_ ? bound_ds_->res.get() : nullptr}, dims->w, dims->h);
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    PushDescriptors(vc.buf, vc.off, sizeof(s.vsc), dc.buf, dc.off, sizeof(DrawConstants), stages.res, kinds, smp);
    SetViewportScissor(s, size.w);
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(Cb(), bf);
    vkCmdSetStencilReference(Cb(), VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);
    DrawGeometry(a);
}

// ---------------------------------------------------------------- clears

void VulkanBackend::ClearTarget(Target& t, const State& s, const ClearArgs& c) {
    float sc = float(t.w) / t.gw;
    auto scaled = [&](int x0, int y0, int x1, int y1) {
        int32_t l = std::clamp(int32_t(float(x0) * sc), 0, int32_t(t.w));
        int32_t tp = std::clamp(int32_t(float(y0) * sc), 0, int32_t(t.h));
        int32_t r = std::clamp(int32_t(float(x1) * sc), 0, int32_t(t.w));
        int32_t b = std::clamp(int32_t(float(y1) * sc), 0, int32_t(t.h));
        return VkClearRect{{{l, tp}, {uint32_t(std::max(r - l, 0)), uint32_t(std::max(b - tp, 0))}}, 0, 1};
    };
    std::vector<VkClearRect> rs;
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(scaled(r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(scaled(int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    rs.erase(std::remove_if(rs.begin(), rs.end(), [](const VkClearRect& r) { return !r.rect.extent.width || !r.rect.extent.height; }),
             rs.end());
    VkClearAttachment ca{};
    if (t.depth) {
        ca.aspectMask = ((c.flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((c.flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca.clearValue.depthStencil = {std::clamp(c.z, 0.0f, 1.0f), c.stencil & 0xFF};
        if (!rs.empty() && ca.aspectMask) {
            BeginRendering({nullptr, t.res.get()}, t.w, t.h);
            vkCmdClearAttachments(Cb(), 1, &ca, uint32_t(rs.size()), rs.data());
        }
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Color(c.color, ca.clearValue.color.float32);
        if (!rs.empty()) {
            BeginRendering({t.res.get(), nullptr}, t.w, t.h);
            vkCmdClearAttachments(Cb(), 1, &ca, uint32_t(rs.size()), rs.data());
        }
    }
    t.drawn = true;
    t.fb_current = false;
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    if (flags & kClearOcclusion) {
        if (count != kOcclusionRects || !rects) return;
        float p[4 * kOcclusionRects];
        std::memcpy(p, rects, sizeof(p));
        Occlusion(s, p);
        return;
    }
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags, color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    ClearArgs c{count, rects, flags, color, z, stencil};
    for (Target* t : {rt, ds})
        if (t) ClearTarget(*t, s, c);
}

// ---------------------------------------------------------------- copies

// one Blit per rect: for formats that differ or a multisampled target
void VulkanBackend::CopyByDraw(Target& src_t, Image& src, Target& dst_t, const CopyList& l) {
    Barrier(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    BeginRendering({dst_t.res.get(), nullptr}, dst_t.w, dst_t.h);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        DrawnRect r{0, 0, float(std::min(src_t.gw, dst_t.gw)), float(std::min(src_t.gh, dst_t.gh))};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        float vx = r.dx * dst_scale, vy = r.dy * dst_scale, vw = (r.x1 - r.x0) * dst_scale, vh = (r.y1 - r.y0) * dst_scale;
        int32_t right = std::min(int32_t(vx + vw), int32_t(dst_t.w));
        int32_t bottom = std::min(int32_t(vy + vh), int32_t(dst_t.h));
        if (right <= int32_t(vx) || bottom <= int32_t(vy)) continue;
        SetViewport(vx, vy, vw, vh);
        SetScissor(int32_t(vx), int32_t(vy), right, bottom);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        Blit(src, uv, {.alpha = true, .taps = taps, .rtv = dst_t.fmt, .samples = dst_t.Samples()});
    }
    dst_t.drawn = true;
    dst_t.fb_current = false;
    bound_rt_ = bound_ds_ = nullptr;
}

// one image copy per rect, clamped to both targets
void VulkanBackend::CopyByRegion(Target& src_t, Image& src, Target& dst_t, const CopyList& l) {
    Barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Barrier(*dst_t.res, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    std::vector<VkImageCopy> regions;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t left = 0, top = 0, right = std::min(src_t.w, dst_t.w), bottom = std::min(src_t.h, dst_t.h);
        uint32_t dx = 0, dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            left = ScaleUnsigned(r.left, dst_scale);
            top = ScaleUnsigned(r.top, dst_scale);
            right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = left;
            dy = top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (right <= left || bottom <= top || dx >= dst_t.w || dy >= dst_t.h) continue;
        right = std::min(right, left + dst_t.w - dx);
        bottom = std::min(bottom, top + dst_t.h - dy);
        VkImageCopy c{Layers(src), {int32_t(left), int32_t(top), 0}, Layers(*dst_t.res), {int32_t(dx), int32_t(dy), 0},
                      {right - left, bottom - top, 1}};
        regions.push_back(c);
    }
    if (!regions.empty())
        vkCmdCopyImage(Cb(), src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t.res->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    dst_t.drawn = true;
    dst_t.fb_current = false;
    bound_rt_ = bound_ds_ = nullptr;
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points,
                         float /*src_kx*/, float /*src_ky*/) {
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = src_t ? (src_t->drawn ? "target" : "idle target") : "memory";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    // EnsureTarget may have made another target: src_t's entry is unmoved (unordered_map keeps nodes)
    if (!dst_t || src_t == dst_t) return;
    Image* from = Flat(*src_t);
    if (!from) return;
    CopyList l{count, src_rects, dst_points};
    if (src_t->fmt != dst_t->fmt || dst_t->Samples() > 1 || src_t->w != src_t->gw * dst_t->w / std::max(dst_t->gw, 1u)) {
        CopyByDraw(*src_t, *from, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, *from, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

void VulkanBackend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

// the slot is not reused before the caller's wait: the title runs at most one present ahead
void VulkanBackend::WaitGpuFrame(uint64_t n) {
    uint64_t fence = present_fence_[n % (kFrames + 1)].load();
    if (fence > ctx_.Completed()) ctx_.Wait(fence);
}

std::unique_ptr<VulkanBackend> CreateHeadless(float scale) { return std::make_unique<VulkanBackend>(true, scale); }

}  // namespace gpu::vk
