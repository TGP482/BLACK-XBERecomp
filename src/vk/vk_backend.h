#pragma once
// The title's Direct3D 8 calls drawn with Vulkan: the D3D12 renderer (src/d3d/d3d12.cpp) carried over call for call.
// Console surfaces become targets at render scale (multisampled for the scene), guest textures are decoded and cached
// by address, draws translate their state to pipelines, and read-backs bring GPU results to guest memory.
// Split by job: vk_backend.cpp (frames, targets, textures, draws, clears, copies), vk_readback.cpp (Sync and the late
// copies, shrinks, F3 capture), vk_present.cpp (present, SMAA, HDR10, the panel), vk_occlusion.cpp (ambient occlusion).
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "render/pass_shaders.h"
#include "gpu/vk_context.h"
#include "gpu/vk_formats.h"
#include "platform/vk_shaders.h"
#include "gpu/vk_swapchain.h"

namespace gpu::texturedumper {
struct Image;
}

namespace gpu::vk {

constexpr int kFrames = 3;
constexpr uint64_t kUploadSize = 16ull << 20;  // per frame: real use is 5-10 MB; an oversized upload gets its own buffer
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat kOutFrameFormat = VK_FORMAT_R16G16B16A16_UNORM;  // the frame before PQ
constexpr VkFormat kAoFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kSmaaEdgeFormat = VK_FORMAT_R8G8_UNORM;
constexpr VkFormat kSmaaWeightFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr float kDefaultPaperNits = 200.0f;
constexpr float kDefaultPeakNits = 1000.0f;

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
inline double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    double draw = 0, assemble = 0, bind = 0, sig = 0, texup = 0, compile = 0, clear = 0, copy = 0, present = 0,
           wait = 0, swap = 0, sync = 0;
    uint64_t draws = 0, verts = 0, upload = 0, overflow = 0, sigs = 0, sig_bytes = 0, texups = 0, texup_bytes = 0,
             newtex = 0, targets = 0, rtups = 0, shrinks = 0, syncs = 0, psos = 0, shaders = 0, raw = 0, lates = 0;
};
extern Perf g_perf;
struct PerfScope {
    double& acc;
    PerfClock::time_point t = PerfClock::now();
    ~PerfScope() { acc += PerfMs(t); }
};

// a console surface the renderer draws into
struct Target {
    ImageRef res;
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;  // console size, host size
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    ImageRef fb;                  // single sample copy read by draws that sample their own target, or a multisampled one
    bool fb_current = false;      // fb holds what res holds: nothing drew into res since the copy
    VkFormat fmt = kBgra;
    uint32_t Samples() const { return res ? res->desc.samples : 1; }
};

// a guest texture as uploaded
struct Tex {
    ImageRef res;
    TextureDesc d;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t levels = 1;
    uint64_t sig = 0, frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint32_t pack_gen = 0;  // texturedumper::Generation the key was taken at, 0 after new bytes
    uint64_t pack_key = 0;
    ImageRef swap;          // the pack image drawn in its place, when the pack has one
};

// the pipeline's state, hashed as bytes: every field 4 bytes
struct PsoKey {
    uint64_t vs, ps;
    uint32_t regs, topo, has_rt, has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc, sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t msaa;  // samples of a multisampled target, 0 without
};

struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    VkFormat rtv = kBgra;     // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t samples = 1;     // the target's
};

struct ShrinkOptions {
    uint32_t taps = 1;
    ImageRef into;  // an image from an earlier call to draw over again
    float mul = 1;
};

// a draw's four texture stages
struct DrawTextures {
    TexInfo info[4];
    Image* res[4] = {};
    bool rt[4] = {};
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};
    bool blur[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
};

struct DrawShaders {
    bool prog = false;
    bool fixed_transform = false;
    FixedKey fk;
    uint64_t vkey = 0, vh = 0, pkey = 0, ph = 0;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
};

struct GuestExtent {
    uint32_t w, h;
};

// a colour or depth attachment of a rendering scope; null for none
struct Attachments {
    Image* color = nullptr;
    Image* depth = nullptr;
    bool operator==(const Attachments&) const = default;
};

// one full screen pass of kAoHlsl
struct AoPass {
    const char* entry;
    VkFormat out = kAoFormat;
    uint32_t samples = 1;
    bool multiply = false;
    Image* rt = nullptr;
    Image* depth = nullptr;
    bool ms_depth = false;
    Image* src = nullptr;
    float step_x = 0, step_y = 0;
};

class VulkanBackend final : public Backend {
public:
    // headless: no window, no swap chain, Present draws nothing, targets at headless_scale (the tests' renderer)
    explicit VulkanBackend(bool headless = false, float headless_scale = 1) : headless_(headless), headless_scale_(headless_scale) {}
    ~VulkanBackend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    // src_kx, src_ky are the source's guest pixels per pixel of its image: the backend reads both scales from the
    // targets it was given, as src/d3d/d3d12.cpp does
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points,
              float src_kx, float src_ky) override;
    void Present(const Surface& front) override;
    // the host panel on a paused frame: it draws without advancing the guest's present count
    void PresentPaused(const Surface& front) {
        paused_present_ = true;
        Present(front);
        paused_present_ = false;
    }
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void WaitGpuFrame(uint64_t n) override;
    uint64_t Presents() const { return presents_; }
    Context& Ctx() { return ctx_; }

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        BufferRef upload;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ImageRef> images;    // released once the GPU is past the frame
        std::vector<BufferRef> buffers;
        std::vector<VkSampler> samplers;
    };
    struct Alloc {
        VkBuffer buf;
        uint64_t off;
        uint8_t* cpu;
    };
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw, gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;
    };
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    struct CopyList {
        uint32_t count, src_rects, dst_points;
    };
    struct CopySource {
        Image* res;
        ImageRef down;
    };
    struct Late {
        BufferRef rb[kFrames];
        ImageRef down;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0, next = 0;
    };
    struct CapImage {
        Image* res;
        VkFormat fmt;
        uint32_t w, h;
        float mul = 1;
    };

    // frames and command recording (vk_backend.cpp)
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    VkCommandBuffer Cb() { return CurrentFrame().cb; }
    Alloc Upload(uint64_t size, uint64_t align = 256);
    void Begin();
    // waits: the CPU waits for the GPU, the frame's resources are reused. Present passes the swap chain's semaphores
    void Submit(bool wait, VkSemaphore acquired = VK_NULL_HANDLE, VkSemaphore done = VK_NULL_HANDLE);
    void Release(ImageRef r) {
        if (r) CurrentFrame().images.push_back(std::move(r));
    }
    // layout changes, copies and clears happen outside a rendering scope: these end the open one first
    void Barrier(Image& img, VkImageLayout to);
    void EndRendering();
    void BeginRendering(const Attachments& a, uint32_t w, uint32_t h);
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    void SetFullViewport(uint32_t w, uint32_t h);
    bool CreateFrames();
    void CreateLayouts();
    void CreateDummies();
    void LoadSettings();
    // a device that never came up, or one torn down: the allocator is what every image and buffer needs, and VMA
    // asserts on a null one, so the allocation is dropped with a log instead of reaching it
    ImageRef MakeImage(const ImageDesc& d);
    BufferRef MakeBuffer(uint64_t size, VkBufferUsageFlags usage, Memory mem);
    uint32_t Samples(uint32_t want);
    VkSampler EnsureSampler(const SamplerDesc& d);
    VkSampler BlitSampler(bool linear);
    // b0, b1, t0 to t3 (null: a dummy of that kind), s0 to s3
    void PushDescriptors(VkBuffer cb0, uint64_t off0, uint64_t size0, VkBuffer cb1, uint64_t off1, uint64_t size1,
                         Image* const tex[4], const uint8_t kinds[4], VkSampler const smp[4]);
    VkShaderModule Compile(const std::string& src, const char* entry, Stage stage, uint64_t& hash);

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    TargetSlot FindTargetSlot(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t);
    void UploadTarget(Target& t, const Surface& s);
    ImageRef CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    bool BindTargets(const State& s);
    bool DepthCopy(const State& s);
    bool DepthPass(Image& src, Image& dst);
    VkPipeline EnsureDepthCopyPipeline(bool ms, uint32_t samples);
    Image* Feedback(Target& t);
    Image* Flat(Target& t);

    // full screen passes
    void Blit(Image& src, const float uvrect[4], const BlitOptions& o);
    VkPipeline EnsureBlitPipeline(VkFormat rtv, bool over, uint32_t samples);
    VkPipeline FullScreenPipeline(VkShaderModule vs, const char* vs_entry, VkShaderModule ps, const char* ps_entry,
                                  VkFormat rtv, uint32_t samples,
                                  const VkPipelineColorBlendAttachmentState* blend, bool depth_out = false);
    void DrawFullScreen(VkPipeline pso, const void* constants, uint32_t bytes, Image* const tex[4], VkSampler s0, VkSampler s1);
    bool Pass(const char* hlsl, const char* vs_entry, const char* entry, VkFormat rtv, Image& rt, Image* const src[3],
              const float p[8], float blend);
    VkPipeline EnsurePassPipeline(const char* hlsl, const char* vs_entry, const char* entry, VkFormat rtv, float blend);

    // draws, in call order
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices, Assembled& a);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    Image* BindTexture(const State& s, int stage, DrawTextures& stages);
    Image* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box, DrawTextures& stages);
    Image* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours);
    void UploadTextureLevel(Tex& tx, uint32_t face, uint32_t level, const std::vector<uint8_t>& px, uint32_t pitch);
    // texture packs and dumps, taken at the bind as src/d3d/d3d12.cpp takes them
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours);
    void LatchPackImage(Tex& tx);
    void CreatePackTexture(Tex& r, const texturedumper::Image& img);
    void ReleasePackTextures();
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    VkPipeline EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps);
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages, GuestExtent size,
                         Alloc& vc, Alloc& dc);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c);
    void SetViewportScissor(const State& s, uint32_t gw);
    void DrawGeometry(const Assembled& a);

    // clears and copies
    void ClearTarget(Target& t, const State& s, const ClearArgs& c);
    void CopyByDraw(Target& src_t, Image& src, Target& dst_t, const CopyList& l);
    void CopyByRegion(Target& src_t, Image& src, Target& dst_t, const CopyList& l);

    // read-backs and capture (vk_readback.cpp)
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    ImageRef Shrink(Image& src, uint32_t w, uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(Image& res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void ResetLate(Late& l, const Target& t);
    Image* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, Image& src);
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name);
    void CapStep(const Surface& rt);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void FinishCapture(const Surface& front);
    void StartCapture();

    // present (vk_present.cpp)
    void ConfigureSwap();
    void UploadGammaRamp();
    void DrawFrame(Target* t, Image* bb);
    Image* PrepareShownFrame(Target* t);
    Image* Smaa(Image& src, VkFormat fmt, uint32_t w, uint32_t h);
    bool EnsureSmaaLookups();
    bool EnsureSmaaTargets(VkFormat fmt, uint32_t w, uint32_t h);
    bool DrawSmaaPasses(Image& src, uint32_t w, uint32_t h, const char* hlsl);
    void DrawSdrFrame(Target* t, Image* r, Image& bb);
    bool EnsureOutTargets();
    void DrawHdrFrame(Target* t, Image* r, Image& bb);
    void BlitFrame(Target& t, Image& r, VkFormat rtv);
    void InitUi();
    void ShutdownUi();
    void RenderUi(Image& into);
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    void TickPipelineCache();

    // ambient occlusion (vk_occlusion.cpp)
    void Occlusion(const State& s, const float* p);
    bool OcclusionFits(const Target* rt, const Target* ds) const;
    bool EnsureOcclusionTargets(uint32_t w, uint32_t h);
    VkPipeline EnsureOcclusionPipeline(const AoPass& a);
    bool OcclusionPass(const AoPass& a, float c[kAoConstants], const VkRect2D& rect, uint32_t w, uint32_t h);

    bool headless_;
    float headless_scale_;
    Context ctx_;
    std::unique_ptr<Swapchain> swap_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    size_t cache_bytes_ = 0;                       // what the file held when it was read or last written
    PerfClock::time_point cache_saved_ = PerfClock::now();
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    uint64_t pack_frame_ = ~0ull;  // the frame the pack's settings and files were last looked at
    bool paused_present_ = false;
    uint64_t fence_value_ = 0;
    Attachments open_;  // the rendering scope recording now
    bool rendering_ = false;
    std::unordered_map<uint64_t, VkShaderModule> modules_;  // by source hash, null for a failed compile
    std::unordered_map<uint64_t, uint64_t> shader_src_;     // vertex and pixel keys to source hash
    std::unordered_map<uint64_t, VkPipeline> psos_;
    std::unordered_map<uint64_t, VkPipeline> pass_psos_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint64_t, Tex> pack_textures_;  // the pack's images, by key: textures with the same bytes share one
    ImageRef dummy_[4];  // null views: 2D, 3D, cube, by TexKind
    ImageRef lut_;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1.0f;
    uint32_t msaa_ = 1, msaa_set_ = 0;
    ImageRef ao_[kAoTargets];
    uint32_t ao_w_ = 0, ao_h_ = 0;
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    std::atomic<uint64_t> presents_seen_{0};
    std::atomic<uint64_t> present_fence_[kFrames + 1] = {};
    Assembled asm_;
    // output: HDR10 asked and in use, the frame and panel targets drawn first for it
    bool hdr_display_ = false, hdr_out_ = false;
    float paper_nits_ = kDefaultPaperNits, peak_nits_ = kDefaultPeakNits;
    ImageRef out_frame_, out_panel_;
    ImageRef smaa_[kSmaaTargets];
    ImageRef smaa_lut_[2];
    uint32_t smaa_w_ = 0, smaa_h_ = 0;
    VkFormat smaa_fmt_ = VK_FORMAT_UNDEFINED;
    bool ui_ready_ = false;
    VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
    uint64_t presents_ = 0;
    double fps_ = 0;
    PerfClock::time_point fps_t_ = PerfClock::now();
    uint64_t fps_n_ = 0;
    // one frame capture: every draw in a text file, each target as the frame leaves it
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0, cap_rt_ = 0;
    std::unordered_set<uint64_t> cap_sh_;
};

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width);
// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth);
// a surface's bytes hashed: whole up to 64 KiB, a bigger one as evenly spaced samples
uint64_t MemSig(uint32_t data, uint32_t total);
// the CPU wrote the target's memory since the last look (a lock and changed bytes): true once per write
bool LatchCpuWrite(Target& t);
// vk_readback.cpp: the read-back line from the frame tick, so a quiet second still proves the counter is alive
void DiagReadbackTick();

// the backend the tests drive without a window
std::unique_ptr<VulkanBackend> CreateHeadless(float scale = 1);

}  // namespace gpu::vk
