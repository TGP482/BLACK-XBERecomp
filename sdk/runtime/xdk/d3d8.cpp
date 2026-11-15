// Direct3D 8 (XDK 4831, 5659 and 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/simple_dirty.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace dispatch {
bool InCode(uint32_t va);  // cpu/dispatch.cpp
}

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define SYM_D3D_FvfShader 0
#define HAS_D3D_FvfShader 0
#endif
#ifndef HAS_D3D_InternalAddRef
#define SYM_D3D_InternalAddRef 0
#define HAS_D3D_InternalAddRef 0
#endif
#ifndef HAS_D3D_InternalRelease
#define SYM_D3D_InternalRelease 0
#define HAS_D3D_InternalRelease 0
#endif
#ifndef HAS_D3D_InitDefaultState
#define SYM_D3D_InitDefaultState 0
#define HAS_D3D_InitDefaultState 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_GetTile
#define SYM_D3DDevice_GetTile FFFFFE04
#define HAS_D3DDevice_GetTile 0
#endif
#ifndef SYM_D3DDevice_SetRenderTargetFast
#define SYM_D3DDevice_SetRenderTargetFast FFFFFE05
#define HAS_D3DDevice_SetRenderTargetFast 0
#endif
#ifndef SYM_D3DDevice_LoadVertexShaderProgram
#define SYM_D3DDevice_LoadVertexShaderProgram FFFFFE06
#define HAS_D3DDevice_LoadVertexShaderProgram 0
#endif
#ifndef SYM_D3DDevice_GetRasterStatus
#define SYM_D3DDevice_GetRasterStatus FFFFFE07
#define HAS_D3DDevice_GetRasterStatus 0
#endif
#ifndef SYM_D3DDevice_Reset
#define SYM_D3DDevice_Reset FFFFFE08
#define HAS_D3DDevice_Reset 0
#endif
#ifndef SYM_D3DDevice_RunPushBuffer
#define SYM_D3DDevice_RunPushBuffer FFFFFE09
#define HAS_D3DDevice_RunPushBuffer 0
#endif
#ifndef SYM_D3D_BlockOnNonSurfaceResource
#define SYM_D3D_BlockOnNonSurfaceResource FFFFFE0A
#define HAS_D3D_BlockOnNonSurfaceResource 0
#endif
#ifndef SYM_D3DDevice_SetBackBufferScale
#define SYM_D3DDevice_SetBackBufferScale FFFFFE0B
#define HAS_D3DDevice_SetBackBufferScale 0
#endif
#ifndef SYM_D3DDevice_DrawIndexedVerticesUP
#define SYM_D3DDevice_DrawIndexedVerticesUP FFFFFE0C
#define HAS_D3DDevice_DrawIndexedVerticesUP 0
#endif

namespace d3d {

namespace {

// D3D globals of this build
constexpr hle::Va kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr hle::Va kDevicePtr = SYM(D3D_pDevice);
constexpr hle::Va kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr hle::Va kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr hle::Va kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr hle::Va kRenderState = SYM(D3D_RenderState);            // [166]
constexpr hle::Va kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr hle::Va kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

// every write to g_state.simple goes through here or marks it
void SetSimple(uint32_t method, uint32_t v) {
    g_state.simple[method >> 2] = v;
    gpu::MarkSimple(method >> 2);
}

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
// the console's frame buffers are the multisample factor times the asked size, and after SetBackBufferScale it draws
// to only their top-left part; CopyRects blits those pixels as they lie
uint32_t g_ms_x = 1, g_ms_y = 1;
float g_bb_x = 1, g_bb_y = 1;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet
bool g_vblank_started = false;

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
    if (HAS(D3D_InternalAddRef)) {
        GUEST_CALL(D3D_InternalAddRef, ctx, {res});
        return;
    }
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
    if (HAS(D3D_InternalRelease)) {
        GUEST_CALL(D3D_InternalRelease, ctx, {res});
        return;
    }
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
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
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) g_state.tss[s][i] = R32(kTextureState + (s * kStageStates + i) * 4);
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        std::memcpy(HostPtr(p + 16), &v.min_z, 4);
        std::memcpy(HostPtr(p + 20), &v.max_z, 4);
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
    if (HAS(D3D_InitDefaultState)) GUEST_CALL(D3D_InitDefaultState, ctx, {});
    else LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
}

// The frame buffers a D3DPRESENT_PARAMETERS asks for become the device's targets, the viewport covers them. The
// renderer is made at the first ones' size.
void SetFrameBuffers(const char* caller, uint32_t pp) {
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    uint32_t ms = R32(pp + 0x10);  // D3DMULTISAMPLE_TYPE: samples across in bits 4-7, down in bits 0-3
    g_ms_x = std::max<uint32_t>(ms >> 4 & 0xF, 1);
    g_ms_y = std::max<uint32_t>(ms & 0xF, 1);
    LOGI("d3d: %s %ux%u format %X buffers %u depth %u format %X interval %X flags %X", caller, w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    uint32_t zf = auto_depth ? gpu::LinearDepthFormat(depth_fmt) : 0;
    // surfaces of the same shape are kept: a title wraps their memory in textures of its own across a Reset
    static std::array<uint32_t, 5> made{};
    std::array<uint32_t, 5> asked{w, h, color_fmt, std::min<uint32_t>(count + 1, 3), zf};
    if (asked != made) {
        made = asked;
        g_buffer_count = asked[3];
        for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
        g_depth = zf ? NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4) : 0;  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) {
            LOGE("d3d: renderer init failed");
            // no owner window: this runs on the title's thread and the window may already be fullscreen over the box
            MessageBoxW(nullptr,
                        L"Direct3D 12 could not be started. Make sure your graphics card supports it and its driver "
                        L"is up to date.",
                        app::kGame.short_title, MB_ICONERROR | MB_TOPMOST);
            kernel::Shutdown(1);
        }
        g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
    }
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    }
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
}

constexpr bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), shown = 0;
    auto next = clock::now();
    for (;;) {
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        std::this_thread::sleep_until(next);
        RestartPoint();
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }

}  // namespace d3d

using namespace d3d;

// device

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
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    W32(kDevicePtr, kDevice);
    ScratchPush();
    g_state = gpu::State{};
    gpu::MarkAllSimple();
    gpu::g_simple_tracked = true;
    SetFrameBuffers("CreateDevice", a[4]);
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
    // the miniport's hardware init leaves an identity ramp in slot 0, which GetGammaRamp reads until one is set
    if (XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint8_t* ramp = static_cast<uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps));  // red, green, blue
        for (uint32_t i = 0; i < 256; i++) ramp[i] = ramp[256 + i] = ramp[512 + i] = uint8_t(i);
    }
    if (HAS(D3D_FvfShader) && XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        if (!g_vblank_started) StartHostThread("vertical blank", VerticalBlank);
        g_vblank_started = true;
    }
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            // perf diagnostic: who blocks on which surface, once each
            static std::unordered_set<uint32_t> seen;
            if (diag::Perf() && seen.insert(data).second) {
                uint32_t code[4] = {}, n = 0;
                for (uint32_t i = 0; i < 32 && n < 4; i++) {
                    uint32_t v = R32(ctx.esp + i * 4);
                    if (dispatch::InCode(v)) code[n++] = v;
                }
                LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
            }
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
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
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
// (resource): Lock2 of a vertex buffer or palette. The library waits on GPU time stamps, which the host does not keep
// (BlockOnTime returns at once), so a title refilling a buffer would overwrite draws not yet taken.
HLE_STDCALL(D3D_BlockOnNonSurfaceResource, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
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
// (status): D3DRASTER_STATUS {InVBlank, ScanLine} on a 60 Hz frame clock. The library reads the GPU's scanline,
// which counts the front buffer's lines, then the blank: 45 lines per 480 shown, as on NTSC.
HLE_STDCALL(D3DDevice_GetRasterStatus, 1) {
    constexpr int64_t kFrameMicroseconds = 16667;
    constexpr uint32_t kBlankLines = 45;
    constexpr uint32_t kShownLines = 480;
    uint32_t shown = g_buffers[0] ? (R32(g_buffers[0] + 0x10) >> 12 & 0xFFF) + 1 : kShownLines;
    uint32_t lines = shown + shown * kBlankLines / kShownLines;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t into_frame = std::chrono::duration_cast<std::chrono::microseconds>(now).count() % kFrameMicroseconds;
    uint32_t line = uint32_t(into_frame * lines / kFrameMicroseconds);
    W32(a[0], line >= shown);
    W32(a[0] + 4, line < shown ? line : 0);
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
// (x, y): the console draws to a scaled-down part of the back buffer and enlarges it on display; the host draws
// every frame at its own render scale, and only a copy out of the back buffer places it as the console would
HLE_STDCALL(D3DDevice_SetBackBufferScale, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_bb_x = ArgF(a, 0);
    g_bb_y = ArgF(a, 1);
    return 0;
}
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t front = g_buffers[0], size = R32(front + 0x10);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64, height = (size >> 12 & 0xFFF) + 1, bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + 4));
    if (g_backend) g_backend->Sync(data, bytes);
    kernel::SavePersistedDisplay(R32(front), R32(front + 0xC), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

// The library keeps the ramp in one of two device slots for GetGammaRamp: both get it, whichever one its index names
HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    if (XDK_CDevice_GammaRamps != xdk::kUnknown)
        for (uint32_t slot = 0; slot < 2; slot++)
            std::memcpy(HostPtr(kDevice + XDK_CDevice_GammaRamps + slot * sizeof(ramp)), ramp, sizeof(ramp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

namespace {
// the device's targets become rt and zs, the references move from the old ones
void ApplyRenderTarget(X86Context& ctx, uint32_t rt, uint32_t zs) {
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
}
}  // namespace

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget);
    ApplyRenderTarget(ctx, rt, a[1]);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    UpdateViewportConstants();
    return 0;
}

// (rt, zs, flags): the surfaces alone, the viewport stays as it was
HLE_STDCALL(D3DDevice_SetRenderTargetFast, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    ApplyRenderTarget(ctx, a[0], a[1]);
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
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
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
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
    // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
    if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint32_t i = Dev(kDevSwapCount) & 1;
        if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
            SetDev(XDK_CDevice_GammaPending + i * 4, 0);
            g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
        }
    }
    Walk(Dev(kDevPush));  // reports asked for at the end of the frame
    if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]));
    app::Pace();
    // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
    uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
        W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
    }
    W32(g_buffers[0] + 4, data);
    W32(g_buffers[0] + 8, lk);
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

// (pp): the frame buffers pp asks for, the targets on them and cleared, as the library's Reset leaves the device.
// The device's references move back to the frame buffers first, so kept ones hold theirs as CreateDevice made them.
HLE_STDCALL(D3DDevice_Reset, 1) {
    constexpr uint32_t kClearTargetAndZ = 3;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    ApplyRenderTarget(ctx, g_buffers[1], g_depth);
    SetFrameBuffers("Reset", a[0]);
    if (g_backend) g_backend->Clear(g_state, 0, nullptr, kClearTargetAndZ, 0, 1.0f, 0);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    bool frame = std::find(g_buffers.begin(), g_buffers.end(), a[0]) != g_buffers.end();
    float kx = frame ? g_ms_x * g_bb_x : 1, ky = frame ? g_ms_y * g_bb_y : 1;
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4], kx, ky);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = a[0];
    // null keeps the stored viewport, as the library does
    if (v) g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
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
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    SetSimple(method, value);
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    uint32_t rs = a[0], v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        SetSimple(method, v);
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
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
    uint32_t stage = a[0] & 3, type = a[1], v = a[2];
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0], l = a[1];
    if (i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0], on = a[1];
    if (i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, tex = a[1];
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
    uint32_t stage = a[0] & 3, pal = a[1];
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
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    bool same = vb == old;
    if (!same) AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
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
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    if (!same) Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = a[0], func = a[1], out = a[2];
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

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
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
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    uint32_t reg = a[0], src = a[1];
    for (uint32_t i = 0; i < 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    uint32_t reg = a[0], src = a[1], dwords = a[2];
    for (uint32_t i = 0; i < dwords / 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

// Vertex program memory: Load writes instructions at a slot, Select runs the program that starts at one
namespace {
constexpr uint32_t kProgramSlots = 136;
constexpr uint32_t kInstructionDwords = 4;
constexpr uint32_t kLastInstruction = 1;  // bit 0 of an instruction's fourth dword
constexpr uint32_t kVertexProgramType = 0x2078;
constexpr uint32_t kInputRegisters = 16;

// guest vertex shader object, the record a handle points at
constexpr uint32_t kShaderInputs = 0x14;  // 16 inputs of {stream, offset, D3DVSDT type, unused}
constexpr uint32_t kShaderInputBytes = 16;
constexpr uint32_t kTypeNone = 0x02;  // D3DVSDT_NONE

std::array<uint32_t, kProgramSlots * kInstructionDwords> g_program_memory{};
std::map<uint64_t, std::shared_ptr<gpu::VertexShader>> g_selected;  // kept alive, the render thread keys by pointer

// bytes of one D3DVSDT element: kind in the low nibble, count in the next
uint32_t ElementBytes(uint32_t type) {
    uint32_t kind = type & 15;
    uint32_t count = (type >> 4) & 15;
    if (kind == 0 || kind == 6) return 4;          // D3DCOLOR, packed 11:11:10
    if (kind == 1 || kind == 5) return 2 * count;  // shorts
    if (kind == 4) return count;                   // bytes
    return 4 * count;                              // floats
}

void StoreProgram(uint32_t slot, const std::vector<uint32_t>& words) {
    if (slot >= kProgramSlots) return;
    size_t room = g_program_memory.size() - slot * kInstructionDwords;
    std::copy_n(words.begin(), std::min(words.size(), room), g_program_memory.begin() + slot * kInstructionDwords);
}

// a {type | count << 16, instructions} function's instructions
std::vector<uint32_t> FunctionWords(uint32_t function) {
    uint32_t count = R32(function) >> 16;
    std::vector<uint32_t> words(count * kInstructionDwords);
    for (uint32_t i = 0; i < words.size(); i++) words[i] = R32(function + 4 + i * 4);
    return words;
}

// the instructions from slot to the last one
std::vector<uint32_t> ProgramAt(uint32_t slot) {
    std::vector<uint32_t> program;
    for (; slot < kProgramSlots; slot++) {
        const uint32_t* insn = &g_program_memory[slot * kInstructionDwords];
        program.insert(program.end(), insn, insn + kInstructionDwords);
        if (insn[3] & kLastInstruction) break;
    }
    return program;
}

// a guest made shader's inputs, as its object holds them
void ApplyGuestInputs(uint32_t handle, gpu::VertexShader& vs) {
    uint32_t inputs = (handle & ~1u) + kShaderInputs;
    for (uint32_t reg = 0; reg < kInputRegisters; reg++) {
        uint32_t entry = inputs + reg * kShaderInputBytes;
        uint32_t stream = R32(entry);
        uint32_t offset = R32(entry + 4);
        uint32_t type = R32(entry + 8);
        if (type == kTypeNone || stream >= vs.stream_stride.size()) continue;
        vs.inputs[reg] = {int(stream), offset, type};
        uint32_t end = offset + ElementBytes(type);
        vs.stream_stride[stream] = std::max(vs.stream_stride[stream], end);
    }
}

// the vertex inputs a selected program reads: a host made shader's, else the guest object's
void ApplyShaderInputs(uint32_t handle, gpu::VertexShader& vs) {
    auto it = g_vs.find(handle);
    if (it == g_vs.end()) {
        ApplyGuestInputs(handle, vs);
        return;
    }
    vs.inputs = it->second->inputs;
    vs.stream_stride = it->second->stream_stride;
}

// one shader object per program and input layout; handle 0 keeps the bound shader's inputs
std::shared_ptr<gpu::VertexShader> SelectedShader(uint32_t handle, uint32_t slot) {
    gpu::VertexShader vs;
    vs.program = ProgramAt(slot);
    vs.program_type = kVertexProgramType;
    if (handle) {
        ApplyShaderInputs(handle, vs);
    } else {
        vs.inputs = g_state.vs->inputs;
        vs.stream_stride = g_state.vs->stream_stride;
    }
    uint64_t key = gpu::Hash(vs.program.data(), vs.program.size() * 4);
    key = gpu::Hash(vs.inputs.data(), sizeof(vs.inputs), key);
    key = gpu::Hash(vs.stream_stride.data(), sizeof(vs.stream_stride), key);
    auto& kept = g_selected[key];
    if (!kept) kept = std::make_shared<gpu::VertexShader>(std::move(vs));
    return kept;
}
}  // namespace

// (handle, slot): the program a shader was made with, stored at the slot
HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it == g_vs.end()) {
        LOG_FIRST(8, "d3d: LoadVertexShader with unknown handle %08X", a[0]);
        return 0;
    }
    StoreProgram(a[1], it->second->program);
    return 0;
}

// (function, slot)
HLE_STDCALL(D3DDevice_LoadVertexShaderProgram, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    StoreProgram(a[1], FunctionWords(a[0]));
    return 0;
}

// (handle, slot): the program at the slot runs with the handle's inputs; handle 0 only moves the start
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t handle = a[0];
    bool program_bound = (g_state.vs_handle & 1) && g_state.vs;
    if (!handle && !program_bound) {
        LOG_FIRST(8, "d3d: SelectVertexShader at slot %u without a bound vertex shader", a[1]);
        return 0;
    }
    g_state.vs = SelectedShader(handle, a[1]);
    if (handle) g_state.vs_handle = handle;
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

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
    for (int i = 0; i < 8; i++) {
        SetSimple(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        SetSimple(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        SetSimple(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        SetSimple(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        SetSimple(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        SetSimple(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    SetSimple(gpu::kMPsFinalAbcd, d[8]);
    SetSimple(gpu::kMPsFinalEfgh, d[9]);
    SetSimple(gpu::kMPsCompareMode, d[42]);
    SetSimple(gpu::kMPsFinalConstant0, d[43]);
    SetSimple(gpu::kMPsFinalConstant1, d[44]);
    SetSimple(gpu::kMPsCombinerCount, d[53]);
    SetSimple(gpu::kMPsDotMapping, d[55]);
    SetSimple(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0], src = a[1], n = a[2];
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    gpu::MarkAllSimple();  // it writes the combiner constants into simple
    return 0;
}

// drawing

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    SyncDeferred();
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncDeferred();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride, const uint16_t* indices = nullptr) {
    if (!g_backend || !count || !stride) return;
    SyncDeferred();
    // indexed: count is of indices, the ring takes the vertices up to the highest one
    uint32_t used = indices ? *std::max_element(indices, indices + count) + 1u : count;
    auto stream = g_state.streams[0];
    g_state.streams[0] = {UpVertices(vertices, used * stride), stride};
    g_backend->Draw(g_state, prim, 0, count, reinterpret_cast<const uint8_t*>(indices));
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

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.
namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
float g_im_attr[64];                    // 16 slots of x y z w
std::vector<float> g_im_vertices;       // every slot per vertex

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) SetSimple(gpu::kMCountEnable, v);
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64), handle = g_state.vs_handle;
    g_im_prim = 0;
    if (!prim || !count) return;
    if (!(handle & 1) && (handle & 0xE) == 4) LOG_FIRST(1, "d3d: pretransformed immediate mode vertices are not supported");
    auto bound = g_state.vs, from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) {
                m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};
                m.vs->stream_stride[0] += 16;
            }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) packed.insert(packed.end(), &g_im_vertices[v * 64 + s * 4], &g_im_vertices[v * 64 + s * 4] + 4);
    g_state.vs = m.vs;
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}

// a begin/end or vertex data method opens, feeds or draws the immediate primitive; false for any other method
bool ApplyImmediate(uint32_t m, uint32_t v) {
    float f;
    float* a = g_im_attr;
    bool last = true;  // the slot's last component
    std::memcpy(&f, &v, 4);
    if (m == 0x17FC) {  // begin/end, 0 ends
        if (v) {
            g_im_prim = v;
            g_im_mask = 0;
            g_im_vertices.clear();
        } else {
            EndPrimitive();
        }
        return true;
    } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
        a += (m - 0x1880) / 8 * 4;
        last = (m - 0x1880) / 4 & 1;
        a[last] = f;
        a[2] = 0;
        a[3] = 1;
    } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
        a += (m - 0x1940) / 4 * 4;
        for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
    } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
        a += (m - 0x1A00) / 16 * 4;
        last = ((m - 0x1A00) / 4 & 3) == 3;
        a[(m - 0x1A00) / 4 & 3] = f;
    } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
        last = m == 0x1508;
        a[(m - 0x1500) / 4] = f;
        a[3] = 1;
    } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
        last = m == 0x1524;
        a[(m - 0x1518) / 4] = f;
    } else {
        return false;
    }
    if (!g_im_prim) return true;
    g_im_mask |= 1u << (a - g_im_attr) / 4;
    if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
    return true;
}

// Vertex constants a title pushes itself (007 does a skinned mesh's ambient and material colour so): a load
// register, then four dwords per register from it on
constexpr uint32_t kConstantLoad = 0x1EA4;
constexpr uint32_t kConstantData = 0x0B80;
constexpr uint32_t kConstantDataEnd = 0x0C00;
constexpr uint32_t kConstantRegisters = 192;
uint32_t g_constant_register = 0;
uint32_t g_constant_component = 0;

// a constant load or data method updates the vertex constants; false for any other method
bool ApplyConstant(uint32_t m, uint32_t v) {
    if (m == kConstantLoad) {
        g_constant_register = v;
        g_constant_component = 0;
        return true;
    }
    if (m < kConstantData || m >= kConstantDataEnd) return false;
    if (g_constant_register < kConstantRegisters) {
        std::memcpy(&g_state.vsc[g_constant_register][g_constant_component], &v, 4);
    }
    if (++g_constant_component == 4) {
        g_constant_component = 0;
        g_constant_register++;
    }
    return true;
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, constants are stored, the rest is skipped
void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) break;  // anything but a method header
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4;
            uint32_t v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (!ApplyConstant(m, v)) ApplyImmediate(m, v);
        }
    }
    g_walk = end;
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}
// (prim, index count, 16 bit indices, vertices, stride)
HLE_STDCALL(D3DDevice_DrawIndexedVerticesUP, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    DrawUp(a[0], a[1], HostPtr(a[3]), a[4], static_cast<const uint16_t*>(HostPtr(a[2])));
    return 0;
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline or as vertex data, other methods as simple states.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

// a primitive whose vertices came as an inline array, in the bound vertex format
static void DrawInline(uint32_t prim, const std::vector<uint32_t>& vertices) {
    if (vertices.empty()) return;
    uint32_t stride = VertexBytes();
    uint32_t bytes = uint32_t(vertices.size()) * 4;
    if (stride && bytes % stride == 0) DrawUp(prim, bytes / stride, vertices.data(), stride);
    else LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
}

static void RunCommands(uint32_t begin, uint32_t end) {
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    // vertex array offsets and formats, then index elements and draw arrays up to the inline array
    constexpr uint32_t kArrayOffsets = 0x1720, kArrayFormatsEnd = 0x17A0, kArrayDraws = 0x1800;
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    for (uint32_t p = begin; begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: pushed commands stop at %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m == kBeginEnd) {
                if (!v && prim) DrawInline(prim, vertices);
                prim = v;
                vertices.clear();
                ApplyImmediate(m, v);
            } else if ((m >= kArrayOffsets && m < kArrayFormatsEnd) || (m >= kArrayDraws && m < kInlineArray)) {
                LOG_FIRST(16, "d3d: pushed commands draw from vertex arrays (method %04X), which is not run", m);
            } else if (!ApplyConstant(m, v) && !ApplyImmediate(m, v) && m >> 2 < g_state.simple.size()) {
                SetSimple(m, v);
            }
        }
    }
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    RunCommands(g_push_begin, a[0]);
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// (push buffer, fixup): its commands run as EndPush runs a title's, after the ones pushed before it. D3DPushBuffer is
// Common, Data, Lock, Size; the library runs Size less its last dword.
HLE_STDCALL(D3DDevice_RunPushBuffer, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t data = R32(a[0] + 4), size = R32(a[0] + 12);
    LOG_FIRST(8, "d3d: RunPushBuffer %08X, %u bytes, first command %08X", data, size, size ? R32(data) : 0);
    if (a[1]) LOG_FIRST(1, "d3d: RunPushBuffer fixups are not applied");
    Walk(Dev(kDevPush));
    if (size >= 4) RunCommands(data, data + size - 4);
    return 0;
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// D3DTILE: flags, memory, size, pitch, z start tag, z offset
constexpr uint32_t kTileMemory = 4;
constexpr uint32_t kTileSize = 8;
constexpr uint32_t kTilePitch = 12;
constexpr uint32_t kTileBytes = 24;
// tiles the library's CreateDevice sets
constexpr uint32_t kBackBufferTile = 0;
constexpr uint32_t kDepthTile = 1;
// surface header fields
constexpr uint32_t kSurfaceData = 4;
constexpr uint32_t kSurfaceSize = 16;

// (index, tile): the frame buffer memory a title wraps in its own textures, as 007 does for a depth texture
HLE_STDCALL(D3DDevice_GetTile, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t tile = a[1];
    std::memset(HostPtr(tile), 0, kTileBytes);
    uint32_t surface = 0;
    if (a[0] == kBackBufferTile) surface = g_buffers[1];
    if (a[0] == kDepthTile) surface = g_depth;
    if (!surface) return 0;
    uint32_t size = R32(surface + kSurfaceSize);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64;
    uint32_t height = (size >> 12 & 0xFFF) + 1;
    W32(tile + kTileMemory, mem::VirtualFromPhysical(R32(surface + kSurfaceData)));
    W32(tile + kTileSize, pitch * height);
    W32(tile + kTilePitch, pitch);
    return 0;
}

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
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

namespace kernel {

// kernel::Restart: the device as before the title created it; the renderer is made again with the next device
void ResetD3D() {
    std::lock_guard<std::recursive_mutex> lock(d3d::g_lock);
    d3d::g_state = gpu::State{};
    d3d::g_backend.reset();
    d3d::g_vs.clear();
    d3d::g_ps.clear();
    d3d::g_buffers = {};
    d3d::g_buffer_count = 2;
    d3d::g_depth = 0;
    d3d::g_scratch_push = d3d::g_scratch_size = d3d::g_walk = 0;
    d3d::g_vblank_started = false;
    d3d::g_own_constants = false;
    g_up_ring = g_up_pos = g_up_header = 0;
    g_up_fence[0] = g_up_fence[1] = 0;
    d3d::g_im_prim = d3d::g_im_mask = 0;
    d3d::g_im_vertices.clear();
}

}  // namespace kernel
