// Present: the front surface into the window, SMAA over it when on, SDR or HDR10, the settings panel on top; then
// the frame turns over and the settings that remake targets apply
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "app/app.h"
#include "core/host_pause.h"
#include "common/aspect.h"
#include "common/options.h"
#include "core/diag.h"
#include "core/log.h"
#include "d3d/smaa_shader.h"
#include "d3d/smaa_textures.h"
#include "vk/vk_backend.h"
#include "platform/vk_shaders.h"

namespace gpu::vk {

namespace {

// HDR10 nits: SDR white and the panel's peak, from XBE_HDR_PAPER_NITS / XBE_HDR_PEAK_NITS (Linux has no system
// setting to read); the Windows defaults otherwise
float NitsFromEnv(const char* name, float fallback) {
    const char* v = std::getenv(name);
    float n = v ? float(std::atof(v)) : 0.0f;
    return n >= 80 && n <= 10000 ? n : fallback;
}

// XBE_DIAG: the resize line, which says where the bars come from
bool DiagOn() {
    static const bool on = std::getenv("XBE_DIAG") != nullptr;
    return on;
}

// the frame's rectangle in an output of w x h: the game's aspect centred, whatever is left is the bars. Recomputed
// from the swap chain's current extent, so a resize moves and resizes it in the same frame
struct FrameRect {
    float x, y, w, h;
};

FrameRect FrameIn(float w, float h) {
    float aspect = game::Aspect();
    float vw = std::min(w, h * aspect);
    return {(w - vw) / 2, (h - vw / aspect) / 2, vw, vw / aspect};
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el < 1000.0) return;
    Perf& p = g_perf;
    double n = double(perf_frames);
    const gpu::vk::CacheStats shaders = gpu::vk::Stats();
    double host = p.draw + p.clear + p.copy + p.present + p.sync;
    // DIAG(percentiles): frame time over 10 s, the shape a PR quotes rather than a single average
    if (std::getenv("XBE_DIAG")) {
        static std::array<double, 16> window{};
        static size_t at = 0;
        static int seconds = 0;
        window[at % window.size()] = el / n;
        at++;
        if (++seconds % 10 == 0) {
            std::array<double, 16> sorted = window;
            const size_t count = std::min(at, sorted.size());
            std::sort(sorted.begin(), sorted.begin() + long(count));
            LOGR("diag: frame ms p50 %.2f p95 %.2f max %.2f over %zu s", sorted[count / 2],
                 sorted[(count * 95) / 100], sorted[count - 1], count);
        }
    }
    LOGI("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] compile %.2f; "
         "clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
         n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n, p.compile / n,
         p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n, (el - host) / n);
    LOGI("shaders: %u compiled this run, %u answered from the cache", shaders.compiled, shaders.cached);
    LOGI("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits) | read-backs waited for: %.1f lock, %.1f NOOVERWRITE",
         g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6, g_perf_waits.exchange(0) / n,
         g_perf_sync_block.exchange(0) / n, g_perf_sync_late.exchange(0) / n);
    LOGI("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB hashed | %.1f tex uploads "
         "%.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, %.1f read-backs %.1f late | %llu shaders %llu psos this second",
         p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
         p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n, p.targets / n, p.rtups / n,
         p.shrinks / n, p.syncs / n, p.lates / n, (unsigned long long)p.shaders, (unsigned long long)p.psos);
    g_perf = Perf{};
    perf_start = PerfClock::now();
    perf_frames = 0;
}

constexpr VkFormat kUiFormat = kBgra;

// colour blind modes: Machado 2009 simulation at full severity, then Fidaner's daltonize, which moves the difference
// the eye loses into channels it still sees. Rows of protanopia, deuteranopia, tritanopia; the D3D12 path's tables
constexpr float kColourBlindSimulate[3][9] = {
    {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
    {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
    {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f},
};
constexpr float kColourBlindShift[3][9] = {
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0},
};

// mode 1 to 3: identity + shift * (identity - simulate)
std::array<float, 9> ColourBlindMatrix(uint32_t mode) {
    const float* sim = kColourBlindSimulate[mode - 1];
    const float* shift = kColourBlindShift[mode - 1];
    std::array<float, 9> m{};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float lost = 0;
            for (int k = 0; k < 3; k++) lost += shift[r * 3 + k] * (float(k == c) - sim[k * 3 + c]);
            m[r * 3 + c] = float(r == c) + lost;
        }
    }
    return m;
}

}  // namespace

// ---------------------------------------------------------------- the settings panel

void VulkanBackend::InitUi() {
    if (ui_ready_ || headless_ || !ImGui::GetCurrentContext()) return;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(ctx_.dev, &pi, nullptr, &ui_pool_) != VK_SUCCESS) return;
    static const VkFormat ui_format = kUiFormat;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = ctx_.instance;
    ii.PhysicalDevice = ctx_.phys;
    ii.Device = ctx_.dev;
    ii.QueueFamily = ctx_.family;
    ii.Queue = ctx_.queue;
    ii.DescriptorPool = ui_pool_;
    ii.MinImageCount = 2;
    ii.ImageCount = kFrames;
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineCache = cache_;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &ui_format;
    std::lock_guard lock(app::UiLock());
    ui_ready_ = ImGui_ImplVulkan_Init(&ii);
    if (!ui_ready_) LOGE("vulkan: the settings panel could not start");
}

void VulkanBackend::ShutdownUi() {
    if (ui_ready_) {
        std::lock_guard lock(app::UiLock());
        ImGui_ImplVulkan_Shutdown();
        ui_ready_ = false;
    }
    if (ui_pool_) vkDestroyDescriptorPool(ctx_.dev, ui_pool_, nullptr);
    ui_pool_ = VK_NULL_HANDLE;
}

// the panel into the open BGRA target
void VulkanBackend::RenderUi(Image& into) {
    InitUi();
    if (!ui_ready_) return;
    BeginRendering({&into, nullptr}, into.desc.w, into.desc.h);
    std::lock_guard lock(app::UiLock());
    host::Pause().FrameReady();
    app::UiNewFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), Cb());
}

// ---------------------------------------------------------------- output

// the window's size and display: the swap chain made again when they, HDR or vsync changed
void VulkanBackend::ConfigureSwap() {
    hdr_display_ = swap_->HdrCapable();
    game::g_hdr_display = hdr_display_;
    static bool logged = false;
    if (!logged) {
        paper_nits_ = NitsFromEnv("XBE_HDR_PAPER_NITS", kDefaultPaperNits);
        peak_nits_ = NitsFromEnv("XBE_HDR_PEAK_NITS", kDefaultPeakNits);
        LOGR("Display HDR: %s, peak %.0f nits, SDR white %.0f nits", hdr_display_ ? "on" : "off", peak_nits_, paper_nits_);
        logged = true;
    }
    uint32_t w = 0, h = 0;
    app::WindowPixels(w, h);
    bool want_hdr = hdr_display_ && game::Opt().hdr_output;
    EndRendering();
    if (!swap_->Configure(std::max(w, 1u), std::max(h, 1u), want_hdr, app::Get().vsync)) return;
    if (want_hdr && !swap_->Hdr()) LOG_FIRST(1, "Display HDR: the swap chain can't show HDR10, SDR it is");
    // DIAG(hdr): every change, so a 1.4 s on-then-off is explained rather than inferred
    if (std::getenv("XBE_DIAG")) {
        static int last = -1;
        if (last != int(swap_->Hdr())) {
            LOGR("diag: hdr %s (display %s, setting %s, peak %.0f nits)", swap_->Hdr() ? "on" : "off",
                 hdr_display_ ? "capable" : "not capable", game::Opt().hdr_output ? "on" : "off", peak_nits_);
            last = int(swap_->Hdr());
        }
    }
    hdr_out_ = swap_->Hdr();
    // one line per resize: the window's pixels, the chain's extent and the rectangle the frame gets in it
    if (DiagOn()) {
        static uint32_t seen_w = 0, seen_h = 0;
        uint32_t sw = swap_->Width(), sh = swap_->Height();
        if (seen_w && (sw != seen_w || sh != seen_h)) {
            static uint32_t recreates = 0;
            recreates++;
            const FrameRect f = FrameIn(float(sw), float(sh));
            LOGR("diag: resize: window %ux%u, swap chain %ux%u (was %ux%u), recreate %u, aspect %.3f, frame %.0fx%.0f "
                 "at %.0f,%.0f, bars %.0f left/right %.0f top/bottom",
                 w, h, sw, sh, seen_w, seen_h, recreates, game::Aspect(), f.w, f.h, f.x, f.y, f.x, f.y);
        }
        seen_w = sw;
        seen_h = sh;
    }
}

// ramp_ holds the red, green and blue ramps, 256 entries each
void VulkanBackend::UploadGammaRamp() {
    ramp_dirty_ = false;
    Alloc a = Upload(256 * 4, 16);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[512 + i];
        p[1] = ramp_[256 + i];
        p[2] = ramp_[i];
        p[3] = 255;
        identity &= ramp_[i] == i && ramp_[256 + i] == i && ramp_[512 + i] == i;
    }
    gamma_ = !identity;
    Barrier(*lut_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = a.off;
    c.imageSubresource = Layers(*lut_);
    c.imageExtent = {256, 1, 1};
    vkCmdCopyBufferToImage(Cb(), a.buf, lut_->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Barrier(*lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// what the frame shows: the target made readable, SMAA's blend of it when that is on
Image* VulkanBackend::PrepareShownFrame(Target* t) {
    if (!t) return nullptr;
    Image* r = Flat(*t);
    if (!r) return nullptr;
    Barrier(*r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (game::Opt().smaa)
        if (Image* aa = Smaa(*r, t->fmt, t->w, t->h)) r = aa;
    return r;
}

Image* VulkanBackend::Smaa(Image& src, VkFormat fmt, uint32_t w, uint32_t h) {
    static const std::string hlsl = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaPasses;
    if (!EnsureSmaaLookups() || !EnsureSmaaTargets(fmt, w, h)) return nullptr;
    if (!DrawSmaaPasses(src, w, h, hlsl.c_str())) return nullptr;
    return smaa_[2].get();
}

// smaa_textures.h: area 160 x 560 in two channels, search 64 x 16 in one
bool VulkanBackend::EnsureSmaaLookups() {
    if (smaa_lut_[1]) return true;
    struct Lookup {
        VkFormat fmt;
        uint32_t w, h, bpp;
        const uint8_t* data;
    };
    const Lookup luts[2] = {{VK_FORMAT_R8G8_UNORM, 160, 560, 2, kSmaaArea}, {VK_FORMAT_R8_UNORM, 64, 16, 1, kSmaaSearch}};
    static_assert(sizeof(kSmaaArea) == 160 * 560 * 2 && sizeof(kSmaaSearch) == 64 * 16);
    for (int i = 0; i < 2; i++) {
        const Lookup& l = luts[i];
        smaa_lut_[i] = ctx_.CreateImage({.format = l.fmt, .w = l.w, .h = l.h});
        if (!smaa_lut_[i]) return false;
        Alloc a = Upload(uint64_t(l.w) * l.h * l.bpp, 16);
        std::memcpy(a.cpu, l.data, size_t(l.w) * l.h * l.bpp);
        Barrier(*smaa_lut_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = a.off;
        c.imageSubresource = Layers(*smaa_lut_[i]);
        c.imageExtent = {l.w, l.h, 1};
        vkCmdCopyBufferToImage(Cb(), a.buf, smaa_lut_[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        Barrier(*smaa_lut_[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return true;
}

// SMAA's own targets: edges, then blend weights, then the blended frame in the frame's format
bool VulkanBackend::EnsureSmaaTargets(VkFormat fmt, uint32_t w, uint32_t h) {
    if (smaa_[2] && smaa_w_ == w && smaa_h_ == h && smaa_fmt_ == fmt) return true;
    smaa_w_ = smaa_h_ = 0;
    const VkFormat f[kSmaaTargets] = {kSmaaEdgeFormat, kSmaaWeightFormat, fmt};
    for (int i = 0; i < kSmaaTargets; i++) {
        Release(std::move(smaa_[i]));
        smaa_[i] = ctx_.CreateImage({.format = f[i], .w = w, .h = h, .target = true});
        if (!smaa_[i]) {
            for (auto& r : smaa_) Release(std::move(r));
            return false;
        }
    }
    smaa_w_ = w;
    smaa_h_ = h;
    smaa_fmt_ = fmt;
    LOGI("vulkan: SMAA %ux%u", w, h);
    return true;
}

// edges, blend weights, then the blend of src into smaa_[2], left readable
bool VulkanBackend::DrawSmaaPasses(Image& src, uint32_t w, uint32_t h, const char* hlsl) {
    // edge detection discards where there is no edge, so edges and weights start clear each frame
    VkClearColorValue clear{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int i = 0; i < 2; i++) {
        Barrier(*smaa_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(Cb(), smaa_[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    float p[8] = {1.0f / w, 1.0f / h, float(w), float(h)};
    Image* const edges[3] = {&src, nullptr, nullptr};
    if (!Pass(hlsl, "vs_edges", "smaa_edges", kSmaaEdgeFormat, *smaa_[0], edges, p, -1)) return false;
    Image* const weights[3] = {smaa_[0].get(), smaa_lut_[0].get(), smaa_lut_[1].get()};
    if (!Pass(hlsl, "vs_weights", "smaa_weights", kSmaaWeightFormat, *smaa_[1], weights, p, -1)) return false;
    Image* const blend[3] = {&src, smaa_[1].get(), nullptr};
    if (!Pass(hlsl, "vs_blend", "smaa_blend", smaa_fmt_, *smaa_[2], blend, p, -1)) return false;
    Barrier(*smaa_[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// the frame letterboxed to the game's aspect into the open target
void VulkanBackend::BlitFrame(Target& t, Image& r, VkFormat rtv) {
    float w = float(swap_->Width());
    float h = float(swap_->Height());
    const FrameRect f = FrameIn(w, h);
    SetViewport(f.x, f.y, f.w, f.h);
    SetScissor(0, 0, int32_t(w), int32_t(h));
    float uv[4] = {0, 0, 1, 1};
    (void)t;
    // off is the plain blit; a mode only fills the rows in the blit's own b1, so both use the one pipeline
    uint32_t mode = std::min(game::Opt().colour_blind, 3u);
    VkPipeline pso = mode ? EnsureBlitPipeline(rtv, false, 1) : VK_NULL_HANDLE;
    if (!pso) {
        Blit(r, uv, {.gamma = gamma_, .rtv = rtv});
        return;
    }
    std::array<float, 9> m = ColourBlindMatrix(mode);
    // opts.x 2: through the lookup as opts.x 1 does, then the matrix
    float c[20] = {uv[0], uv[1], uv[2], uv[3], (gamma_ ? 1.0f : 0.0f) + 2.0f, 0.0f, 1.0f, 1.0f};
    for (int row = 0; row < 3; row++) std::memcpy(c + 8 + row * 4, m.data() + row * 3, 3 * sizeof(float));
    Image* tex[4] = {&r, lut_.get(), nullptr, nullptr};
    DrawFullScreen(pso, c, sizeof(c), tex, BlitSampler(true), BlitSampler(false));
}

void VulkanBackend::DrawSdrFrame(Target* t, Image* r, Image& bb) {
    BeginRendering({&bb, nullptr}, bb.desc.w, bb.desc.h);
    VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    ca.clearValue.color.float32[3] = 1;
    VkClearRect cr{{{0, 0}, {bb.desc.w, bb.desc.h}}, 0, 1};
    vkCmdClearAttachments(Cb(), 1, &ca, 1, &cr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t && r) BlitFrame(*t, *r, bb.desc.format);
    RenderUi(bb);
}

bool VulkanBackend::EnsureOutTargets() {
    uint32_t w = swap_->Width(), h = swap_->Height();
    if (out_frame_ && out_panel_ && out_frame_->desc.w == w && out_frame_->desc.h == h) return true;
    Release(std::move(out_frame_));
    Release(std::move(out_panel_));
    out_frame_ = ctx_.CreateImage({.format = kOutFrameFormat, .w = w, .h = h, .target = true});
    out_panel_ = ctx_.CreateImage({.format = kBgra, .w = w, .h = h, .target = true});
    if (!out_frame_ || !out_panel_) LOGE("vulkan: no %ux%u HDR10 output targets", w, h);
    return out_frame_ && out_panel_;
}

// frame and panel into their own targets, then both into the swap chain image as HDR10, panel at SDR white
void VulkanBackend::DrawHdrFrame(Target* t, Image* r, Image& bb) {
    VkClearColorValue clear{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (Image* img : {out_frame_.get(), out_panel_.get()}) {
        Barrier(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(Cb(), img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    BeginRendering({out_frame_.get(), nullptr}, out_frame_->desc.w, out_frame_->desc.h);
    bound_rt_ = bound_ds_ = nullptr;
    if (t && r) BlitFrame(*t, *r, kOutFrameFormat);
    RenderUi(*out_panel_);
    Image* const src[3] = {out_frame_.get(), out_panel_.get(), nullptr};
    float p[8] = {paper_nits_, peak_nits_, 0, 0};
    Pass(kHdrHlsl, "vs", "hdr10", bb.desc.format, bb, src, p, -1);
}

void VulkanBackend::DrawFrame(Target* t, Image* bb) {
    Image* r = PrepareShownFrame(t);
    if (!hdr_out_) {
        DrawSdrFrame(t, r, *bb);
    } else if (EnsureOutTargets()) {
        DrawHdrFrame(t, r, *bb);
    } else {
        // without its targets an HDR10 image stays black: the SDR pipelines don't match its format
        BeginRendering({bb, nullptr}, bb->desc.w, bb->desc.h);
        VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        VkClearRect cr{{{0, 0}, {bb->desc.w, bb->desc.h}}, 0, 1};
        vkCmdClearAttachments(Cb(), 1, &ca, 1, &cr);
    }
    Barrier(*bb, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

// ---------------------------------------------------------------- the frame turns over

void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 2048;
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            Release(std::move(it->second.res));
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanBackend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && ctx_.Completed() < f.fence) {
        auto perf_wait = PerfClock::now();
        ctx_.Wait(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    vkResetCommandPool(ctx_.dev, f.pool, 0);
    f.images.clear();
    f.buffers.clear();
    for (VkSampler s : f.samplers) vkDestroySampler(ctx_.dev, s, nullptr);
    f.samplers.clear();
    f.used = 0;
    Begin();
}

void VulkanBackend::CountFps() {
    fps_n_++;
    auto now = PerfClock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void VulkanBackend::ApplySettings() {
    if (headless_) return;
    float want = app::RenderScale(guest_h_);
    uint32_t msaa = game::Opt().msaa;
    if (std::fabs(want - scale_) <= 1e-4f && msaa == msaa_set_) return;
    Flush();
    for (auto& [key, tg] : targets_) {
        Release(std::move(tg.res));
        Release(std::move(tg.fb));
    }
    targets_.clear();
    scale_ = want;
    if (msaa != msaa_set_) {
        msaa_set_ = msaa;
        msaa_ = Samples(msaa_set_);
    }
    bound_rt_ = bound_ds_ = nullptr;
}

void VulkanBackend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
    if (cap_) {
        FinishCapture(front);
    } else if (g_capture.exchange(false)) {
        if (diag::CaptureDir().empty()) LOG_FIRST(1, "capture: set XBE_CAPTURE_DIR");
        else StartCapture();
    }
    if (swap_) ConfigureSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (ramp_dirty_) UploadGammaRamp();
    Barrier(*lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    uint32_t index = 0;
    VkSemaphore acquired = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    Image* bb = nullptr;
    if (swap_) {
        auto perf_swap = PerfClock::now();
        EndRendering();
        if (swap_->Acquire(index, acquired)) {
            bb = &swap_->At(index);
            done = swap_->DoneSemaphore(index);
        }
        g_perf.swap += PerfMs(perf_swap);
    }
    if (bb) DrawFrame(t, bb);
    Submit(false, acquired, done);
    if (!paused_present_) present_fence_[presents_ % (kFrames + 1)].store(fence_value_);
    if (bb) swap_->Present(index, done);

    if (!paused_present_) {
        presents_++;
        presents_seen_.store(presents_);
    }
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    if (diag::Perf()) LogPerf();
    if (!host::Pause().Paused()) ApplySettings();
}

}  // namespace gpu::vk
