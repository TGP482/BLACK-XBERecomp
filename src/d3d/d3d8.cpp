// Direct3D 8 (XDK 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "common/menu.h"
#include "common/options.h"
#include "core/diag.h"
#include "d3d/gpu_frame.h"
#include "d3d/occlusion.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push;
constexpr uint32_t kDevThreshold = XDK_CDevice_PushThreshold;
constexpr uint32_t kDevBaseVertex = XDK_CDevice_BaseVertex;
constexpr uint32_t kDevIndexBuffer = XDK_CDevice_IndexBuffer;
constexpr uint32_t kDevTextures = XDK_CDevice_Textures;
constexpr uint32_t kDevRenderTarget = XDK_CDevice_RenderTarget;
constexpr uint32_t kDevDepth = XDK_CDevice_Depth;
constexpr uint32_t kDevSwapCount = XDK_CDevice_SwapCount;
constexpr uint32_t kDevBackBuffer = XDK_CDevice_BackBuffer;
constexpr uint32_t kDevFrontBuffer = XDK_CDevice_FrontBuffer;
constexpr uint32_t kDevZScale = XDK_CDevice_ZScale;
constexpr uint32_t kDevSuperX = XDK_CDevice_SuperSampleX;
constexpr uint32_t kDevSuperY = XDK_CDevice_SuperSampleY;
constexpr uint32_t kDevTransforms = XDK_CDevice_Transforms;
constexpr uint32_t kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage;
constexpr uint32_t kStreamSize = XDK_Stream_Size;

// D3DResource, D3DPixelContainer and D3DSurface fields
constexpr uint32_t kResCommon = 0;  // reference counts and type
constexpr uint32_t kResData = 4;
constexpr uint32_t kResLock = 8;
constexpr uint32_t kContainerFormat = 12;
constexpr uint32_t kContainerSize = 16;
constexpr uint32_t kSurfaceParent = 0x14;
constexpr uint32_t kSurfaceBytes = 24;
constexpr uint32_t kHeaderBytes = 20;  // a header without the surface parent

// Common: the device's references in bits 19 to 22, the type in bits 16 to 18
constexpr uint32_t kInternalRefs = 0x780000;
constexpr uint32_t kTitleRefs = 0xFFFF;  // the title's references
constexpr uint32_t kInternalRef = 0x80000;
constexpr uint32_t kTypeMask = 0x70000;
constexpr uint32_t kTypeTexture = 0x40000;
constexpr uint32_t kTypeSurface = 0x50000;

constexpr uint32_t kVsConstants = 192;  // hardware vertex constant registers
constexpr uint32_t kPsConstants = 16;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0;
uint32_t g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res + kResCommon);
    bool surface = (common & kTypeMask) == kTypeSurface;
    if (!(common & kInternalRefs) && surface) {
        if (uint32_t parent = R32(res + kSurfaceParent)) W32(parent, R32(parent) + kInternalRef);
    }
    W32(res, R32(res) + kInternalRef);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - kInternalRef);
        bool unheld = !(R32(r) & (kInternalRefs | kTitleRefs));
        if (unheld) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res + kResCommon);
    bool surface = (common & kTypeMask) == kTypeSurface;
    if ((common & kInternalRefs) == kInternalRef && surface) {
        if (uint32_t parent = R32(res + kSurfaceParent)) drop(parent);
    }
    drop(res);
#endif
}

void Walk(uint32_t end);

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000);
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + kResData);
        s.format = R32(header + kContainerFormat);
        s.size = R32(header + kContainerSize);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(kSurfaceBytes);
    W32(hdr + kResCommon, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + kResData, mem::PhysicalFromVirtual(data));
    W32(hdr + kResLock, 0);
    W32(hdr + kContainerFormat, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + kContainerSize, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + kSurfaceParent, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    for (int s = 0; s < 4; s++) {
        uint32_t stage = kTextureState + s * kStageStates * 4;
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) g_state.tss[s][i] = R32(stage + i * 4);
    }
}

// The game fogs its world with linear table fog (0x16FAB0), its vertex programs give the distance as
// oFog = min(clip z, c[18].z). The frame's latest fogged program draw tells ambient occlusion how thick it is
constexpr uint32_t kFogLinear = 3;
constexpr uint32_t kFogLimit = 18;  // c[18].z, the farthest fog distance

struct Fog {
    float start = 0;
    float end = 0;
    float limit = 0;
};
Fog g_fog;

float StateFloat(uint32_t rs) {
    float f;
    std::memcpy(&f, &g_state.rs[rs], 4);
    return f;
}

void LatchFog() {
    bool program = (g_state.vs_handle & 1) != 0;
    if (!program || !g_state.rs[gpu::kRsFogEnable] || g_state.rs[gpu::kRsFogTableMode] != kFogLinear) return;
    g_fog = {StateFloat(gpu::kRsFogStart), StateFloat(gpu::kRsFogEnd), g_state.vsc[kFogLimit][2]};
}

// fog amount at view depth z: min(saturate(slope * z + offset), most)
struct FogFade {
    float slope = 0;
    float offset = 0;
    float most = 0;
};

// m: the camera to screen matrix, clip z = m[10] * view z + m[14]
FogFade FogOverDepth(const float* m) {
    float range = g_fog.end - g_fog.start;
    if (range <= 0) return {};
    float most = std::clamp((g_fog.limit - g_fog.start) / range, 0.0f, 1.0f);
    return {m[10] / range, (m[14] - g_fog.start) / range, most};
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

float g_view_scale = 1.0f;

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
constexpr uint32_t kViewportScale = 58;
constexpr uint32_t kViewportOffset = 59;

void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f;
    float hh = v.h * 0.5f;
    g_state.vsc[kViewportScale] = {hw * g_view_scale, -hh * g_view_scale, (v.max_z - v.min_z) * g_state.zscale, 0};
    float x = v.x + g_state.ss_offset_x + hw;
    float y = v.y + g_state.ss_offset_y + hh;
    g_state.vsc[kViewportOffset] = {x, y, v.min_z * g_state.zscale, 0};
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
#endif
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }

// draws grow about the viewport's centre, depth stays
void ViewScale(float xy) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_view_scale = xy;
    UpdateViewportConstants();
}

// sent as p: 1 / the projection's x and y scale, its x and y centre, then view z = p[4] / (ndc depth - p[5]),
// then p[6]: 1 for GTAO, 0 for SSAO, then the fog already drawn: min(saturate(p[8] * view z + p[9]), p[10])
void Occlusion(const float* m, bool gtao) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    bool perspective = m[11] == 1.0f && m[15] == 0.0f;
    if (!g_backend || !perspective || !m[0] || !m[5]) return;
    float mode = gtao ? 1.0f : 0.0f;
    FogFade fog = FogOverDepth(m);
    const float p[4 * gpu::kOcclusionRects] = {1 / m[0], 1 / m[5], m[8], m[9], m[14], m[10], mode, 0,
                                               fog.slope, fog.offset, fog.most, 0};
    const uint8_t* rects = reinterpret_cast<const uint8_t*>(p);
    g_backend->Clear(g_state, gpu::kOcclusionRects, rects, gpu::kClearOcclusion, 0, 0, 0);
}

}  // namespace d3d

using namespace d3d;

// device

namespace {
// D3DPRESENT_PARAMETERS fields
constexpr uint32_t kPpWidth = 0;
constexpr uint32_t kPpHeight = 4;
constexpr uint32_t kPpFormat = 8;
constexpr uint32_t kPpCount = 12;
constexpr uint32_t kPpAutoDepth = 0x20;
constexpr uint32_t kPpDepthFormat = 0x24;
constexpr uint32_t kPpFlags = 0x28;
constexpr uint32_t kPpInterval = 0x30;

struct PresentParams {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t count;
    uint32_t auto_depth;
    uint32_t depth_format;
    uint32_t interval;
};

PresentParams ReadPresentParams(uint32_t pp) {
    PresentParams p;
    p.width = R32(pp + kPpWidth);
    p.height = R32(pp + kPpHeight);
    p.format = R32(pp + kPpFormat);
    p.count = std::max<uint32_t>(R32(pp + kPpCount), 1);
    p.auto_depth = R32(pp + kPpAutoDepth);
    p.depth_format = R32(pp + kPpDepthFormat);
    p.interval = R32(pp + kPpInterval);
    if (!p.width) p.width = 640;
    if (!p.height) p.height = 480;
    return p;
}

void CreateBuffers(const PresentParams& p) {
    uint32_t color_fmt = gpu::LinearColorFormat(p.format);
    g_buffer_count = std::min<uint32_t>(p.count + 1, 3);
    for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(p.width, p.height, color_fmt, 4);
    g_depth = 0;
    if (p.auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(p.depth_format);
        g_depth = NewSurface(p.width, p.height, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);  // 0x30 LIN_D16, 0x31 LIN_F16
    }
}

void EnsureBackend(uint32_t w, uint32_t h) {
    if (g_backend) return;
    g_backend = gpu::CreateBackend();
    if (!g_backend->Init(w, h)) LOGE("d3d: renderer init failed");
    if (kRenderThread) g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
}

// the device fields the recompiled getters read: targets, super sampling and depth scale
void ApplyDeviceTargets() {
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f;
    uint32_t depth_format = g_depth ? R32(g_depth + kContainerFormat) >> 8 & 0xFF : 0;
    float zs = depth_format == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
}
}  // namespace

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = a[4];  // D3DPRESENT_PARAMETERS
    PresentParams p = ReadPresentParams(pp);
    LOGI("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X", p.width, p.height,
         p.format, p.count, p.auto_depth, p.depth_format, p.interval, R32(pp + kPpFlags));
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    W32(kDevicePtr, kDevice);
    ScratchPush();

    CreateBuffers(p);
    EnsureBackend(p.width, p.height);
    g_state = gpu::State{};
    g_state.viewport = {0, 0, p.width, p.height, 0.0f, 1.0f};
    ApplyDeviceTargets();
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

namespace {
// perf diagnostic: who blocks on which surface, once each
void LogBlockOnce(X86Context& ctx, uint32_t data) {
    static std::unordered_set<uint32_t> seen;
    if (!diag::Perf() || !seen.insert(data).second) return;
    uint32_t code[4] = {};
    uint32_t n = 0;
    for (uint32_t i = 0; i < 32 && n < 4; i++) {
        uint32_t v = R32(ctx.esp + i * 4);
        if (v >= X86CodeBase && v < X86CodeEnd) code[n++] = v;
    }
    LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
}
}  // namespace

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res + kResCommon) & kTypeMask;
        if (type == kTypeTexture || type == kTypeSurface) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + kResData));
            LogBlockOnce(ctx, data);
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + kResData)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return a[0] && g_backend && g_backend->Busy(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t front = g_buffers[0];
    uint32_t size = R32(front + kContainerSize);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64;
    uint32_t height = (size >> 12 & 0xFFF) + 1;
    uint32_t bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + kResData));
    if (g_backend) g_backend->Sync(data, bytes);
    uint32_t common = R32(front + kResCommon);
    kernel::SavePersistedDisplay(common, R32(front + kContainerFormat), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

namespace {
constexpr uint32_t kGammaRampBytes = 3 * 256;  // D3DGAMMARAMP: red, green, blue
}  // namespace

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[kGammaRampBytes];
    std::memcpy(ramp, HostPtr(a[1]), kGammaRampBytes);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget);
    uint32_t zs = a[1];
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget);
    uint32_t old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w;
    uint32_t h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    // -1 the front buffer, 0 and up the back buffers
    uint32_t back = std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[back];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};
uint64_t g_backend_presents = 0;

constexpr bool kGammaKnown = XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown;

// the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
void ApplyPendingGamma() {
    if (!g_backend || !kGammaKnown) return;
    uint32_t i = Dev(kDevSwapCount) & 1;
    uint32_t pending = XDK_CDevice_GammaPending + i * 4;
    if (Dev(pending) != 1) return;
    SetDev(pending, 0);
    const void* ramp = HostPtr(kDevice + XDK_CDevice_GammaRamps + i * kGammaRampBytes);
    g_backend->SetGamma(static_cast<const uint8_t*>(ramp));
}

// like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
void RotateBuffers() {
    uint32_t last = g_buffers[g_buffer_count - 1];
    uint32_t data = R32(last + kResData);
    uint32_t lock = R32(last + kResLock);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + kResData, R32(g_buffers[i - 1] + kResData));
        W32(g_buffers[i] + kResLock, R32(g_buffers[i - 1] + kResLock));
    }
    W32(g_buffers[0] + kResData, data);
    W32(g_buffers[0] + kResLock, lock);
}
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
    game::WaitWhileUnfocused();
    app::SplashFirstFrame();
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_presents++;
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    ApplyPendingGamma();
    if (g_backend) {
        g_backend->Present(SurfaceOf(g_buffers[1]));
        // the title records one frame while the GPU draws the one before it: a GPU bound frame rate then paces the
        // game like a frame rate cap, and the frame times the game measures match the frames shown
        if (g_backend_presents) gpu::WaitGpuFrame(g_backend_presents - 1);
        g_backend_presents++;
    }
    g_fog = {};
    app::PaceFrame();
    RotateBuffers();
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = a[0];
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    uint32_t method = a[0] & 0x1FFC;
    uint32_t value = a[1];
    g_state.simple[method >> 2] = value;
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    uint32_t rs = a[0];
    uint32_t v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        g_state.simple[method >> 2] = v;
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) g_state.tss[a[0] & 3][a[1]] = a[2];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    uint32_t stage = a[0] & 3;
    uint32_t type = a[1];
    uint32_t v = a[2];
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

namespace {
// the XDK's transform types: view 0, projection 1, texture 2 to 5, world 6 to 9; the renderer keeps Direct3D 8's
// numbers (view 2, projection 3, texture 16, world 256)
uint32_t D3D8TransformType(uint32_t t) {
    if (t < 2) return t + 2;
    if (t < 6) return t + 14;
    return t + 250;
}
}  // namespace

// the device copy is what GetTransform reads
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = D3D8TransformType(t);
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0];
    uint32_t l = a[1];
    if (i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0];
    uint32_t on = a[1];
    if (i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3;
    uint32_t tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    // bound again: the AddRef and Release would cancel out
    bool same = tex == old;
    if (!same) AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    if (!same) Release(ctx, old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3;
    uint32_t pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3;
    uint32_t cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(kHeaderBytes);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), kHeaderBytes);
    W32(h + kResData, a[1]);
    W32(h + kContainerFormat, a[2]);
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = a[0] & 15;
    uint32_t vb = a[1];
    uint32_t stride = a[2];
    uint32_t stream = kStreams + n * kStreamSize;
    uint32_t old = R32(stream + XDK_Stream_Buffer);
    bool same = vb == old;
    if (!same) AddRef(ctx, vb);
    W32(stream + XDK_Stream_Stride, stride);
    W32(stream + XDK_Stream_Buffer, vb);
    if (!same) Release(ctx, old);
    g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    bool same = ib == old;
    if (!same) AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + kResData) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    if (!same) Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + kResData) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = a[0];
    uint32_t func = a[1];
    uint32_t out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t h = a[0];
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants) {
                if (reg < kVsConstants) g_state.vsc[reg] = v;
            }
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    uint32_t reg = a[0];
    if (reg < kVsConstants) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    uint32_t reg = a[0];
    if (reg < kVsConstants) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    uint32_t reg = a[0];
    uint32_t src = a[1];
    for (uint32_t i = 0; i < 4 && reg + i < kVsConstants; i++) {
        std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    uint32_t reg = a[0];
    uint32_t src = a[1];
    uint32_t dwords = a[2];
    for (uint32_t i = 0; i < dwords / 4 && reg + i < kVsConstants; i++) {
        std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    }
    return 0;
}

HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    LOG_FIRST(4, "d3d: LoadVertexShader %08X at %u is not supported", a[0], a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    LOG_FIRST(4, "d3d: SelectVertexShader %08X at %u is not supported", a[0], a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) { return 0; }

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(16, 16);
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.ps = nullptr;
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        const void* def = HostPtr(R32(a[0] + XDK_PixelShader_Definition));
        auto& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
    auto put = [&](uint32_t method, uint32_t v) { g_state.simple[method >> 2] = v; };
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0];
    uint32_t src = a[1];
    uint32_t n = a[2];
    for (uint32_t i = 0; i < n && reg + i < kPsConstants; i++) {
        std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    }
    gpu::ApplyPixelConstants(g_state, reg, n);
    return 0;
}

// drawing

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SyncDeferred();
    LatchFog();
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SyncDeferred();
    LatchFog();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20;
constexpr uint32_t kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0;
uint32_t g_up_pos = 0;
uint32_t g_up_header = 0;
uint32_t g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(kHeaderBytes);
        std::memset(HostPtr(g_up_header), 0, kHeaderBytes);
        W32(g_up_header + kResCommon, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    // the halves the last copy ended in and this one ends in
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1;
    uint32_t to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + kResData, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    SyncDeferred();
    LatchFog();
    auto stream = g_state.streams[0];
    g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the
// scratch buffer.
namespace d3d {
namespace {
// push buffer methods: begin/end, inline vertices, vertex data of 2 floats, 4 bytes and 4 floats per slot
constexpr uint32_t kMBeginEnd = 0x17FC;
constexpr uint32_t kMInlineArray = 0x1818;
constexpr uint32_t kMVertexData2F = 0x1880;
constexpr uint32_t kMVertexData2FEnd = 0x1900;
constexpr uint32_t kMVertexData4UB = 0x1940;
constexpr uint32_t kMVertexData4UBEnd = 0x1980;
constexpr uint32_t kMVertexData4F = 0x1A00;
constexpr uint32_t kMVertexData4FEnd = 0x1B00;

constexpr uint32_t kImSlots = 16;
constexpr uint32_t kImFloats = kImSlots * 4;

uint32_t g_im_prim = 0;           // open primitive
uint32_t g_im_mask = 0;           // slots it wrote
float g_im_attr[kImFloats];       // 16 slots of x y z w
std::vector<float> g_im_vertices;  // every slot per vertex

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from;
        std::shared_ptr<gpu::VertexShader> vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim;
    uint32_t count = uint32_t(g_im_vertices.size() / kImFloats);
    uint32_t handle = g_state.vs_handle;
    g_im_prim = 0;
    if (!prim || !count) return;
    bool pretransformed = !(handle & 1) && (handle & 0xE) == 4;  // D3DFVF_XYZRHW
    if (pretransformed) LOG_FIRST(1, "d3d: pretransformed immediate mode vertices are not supported");
    auto bound = g_state.vs;
    auto from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < kImSlots; s++) {
            if (!(g_im_mask >> s & 1)) continue;
            m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};  // FLOAT4
            m.vs->stream_stride[0] += 16;
        }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++) {
        for (uint32_t s = 0; s < kImSlots; s++) {
            const float* slot = &g_im_vertices[v * kImFloats + s * 4];
            if (g_im_mask >> s & 1) packed.insert(packed.end(), slot, slot + 4);
        }
    }
    g_state.vs = m.vs;
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}

// one value of a method: begin/end opens or draws the primitive, vertex data fills a slot
void ApplyImmediateMethod(uint32_t m, uint32_t v) {
    float f;
    float* a = g_im_attr;
    bool last = true;  // the slot's last component
    std::memcpy(&f, &v, 4);
    if (m == kMBeginEnd) {  // 0 ends
        if (v) {
            g_im_prim = v;
            g_im_mask = 0;
            g_im_vertices.clear();
        } else {
            EndPrimitive();
        }
        return;
    }
    if (m >= kMVertexData2F && m < kMVertexData2FEnd) {  // 2 floats, z = 0 and w = 1
        a += (m - kMVertexData2F) / 8 * 4;
        last = (m - kMVertexData2F) / 4 & 1;
        a[last] = f;
        a[2] = 0;
        a[3] = 1;
    } else if (m >= kMVertexData4UB && m < kMVertexData4UBEnd) {  // 4 bytes
        a += (m - kMVertexData4UB) / 4 * 4;
        for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
    } else if (m >= kMVertexData4F && m < kMVertexData4FEnd) {  // 4 floats
        uint32_t component = (m - kMVertexData4F) / 4 & 3;
        a += (m - kMVertexData4F) / 16 * 4;
        last = component == 3;
        a[component] = f;
    } else {
        return;
    }
    if (!g_im_prim) return;
    g_im_mask |= 1u << (a - g_im_attr) / 4;
    // slot 0, the position, completes a vertex
    if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + kImFloats);
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, the rest is skipped
void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p);
        uint32_t method = head & 0x1FFC;
        uint32_t n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) break;  // anything but a method header
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4;
            ApplyImmediateMethod(m, R32(p));
        }
    }
    g_walk = end;
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

static void DrawPushedPrimitive(uint32_t prim, const std::vector<uint32_t>& vertices) {
    uint32_t stride = VertexBytes();
    uint32_t bytes = uint32_t(vertices.size()) * 4;
    if (stride && bytes % stride == 0) {
        DrawUp(prim, bytes / stride, vertices.data(), stride);
    } else {
        LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
    }
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline, any other method as a simple state.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    uint32_t end = a[0];
    for (uint32_t p = g_push_begin; g_push_begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p);
        uint32_t method = head & 0x1FFC;
        uint32_t n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: EndPush stops at command %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4;
            uint32_t v = R32(p);
            if (m == kMInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m != kMBeginEnd) {
                if (m >> 2 < g_state.simple.size()) g_state.simple[m >> 2] = v;
            } else if (v) {
                prim = v;
                vertices.clear();
            } else if (prim) {
                DrawPushedPrimitive(prim, vertices);
                prim = 0;
            }
        }
    }
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

HLE_STDCALL(D3DDevice_Begin, 1) {
    LOG_FIRST(4, "d3d: Begin/End immediate mode is not supported");
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}

// [[hook]] behind an End the title inlined
void EndImmediate(X86Context&, uint8_t*) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
}
