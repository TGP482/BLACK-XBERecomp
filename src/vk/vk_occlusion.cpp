// Ambient occlusion from the scene's depth: the raw pass (SSAO or GTAO), a blur across, and a blur down that multiplies
// it into the scene's colour. d3d::Occlusion sends it in order with the draws as a clear with kClearOcclusion
#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "d3d/occlusion.h"
#include "vk/vk_backend.h"

namespace gpu::vk {

namespace {

// the viewport in target pixels
VkRect2D ViewRect(const Viewport& v, const Target& t) {
    float sc = float(t.w) / t.gw;
    int32_t l = std::clamp(int32_t(v.x * sc), 0, int32_t(t.w));
    int32_t top = std::clamp(int32_t(v.y * sc), 0, int32_t(t.h));
    int32_t r = std::clamp(int32_t((v.x + v.w) * sc), 0, int32_t(t.w));
    int32_t b = std::clamp(int32_t((v.y + v.h) * sc), 0, int32_t(t.h));
    return {{l, top}, {uint32_t(std::max(r - l, 0)), uint32_t(std::max(b - top, 0))}};
}

// b1 of kAoHlsl: proj, zp with the viewport's depth range, vp, opt (its step written per pass), fog
void OcclusionConstants(const Viewport& v, const VkRect2D& r, const float* p, float c[kAoConstants]) {
    float zr = v.max_z - v.min_z;
    float w = float(r.extent.width);
    float h = float(r.extent.height);
    const float out[kAoConstants] = {p[0], p[1], p[2], p[3], p[4] * zr, v.min_z + p[5] * zr, 0, 0,
                                     float(r.offset.x), float(r.offset.y), w, h, kAoRadius, kAoMaxSpan * h, 0, 0,
                                     p[8], p[9], p[10], 0};
    std::memcpy(c, out, sizeof(out));
}

// kAoHlsl's opt.zw: the pass's blur step
constexpr int kAoStepX = 14;
constexpr int kAoStepY = 15;

}  // namespace

// the frame's own scene only: views drawn to smaller targets are left
bool VulkanBackend::OcclusionFits(const Target* rt, const Target* ds) const {
    if (!rt || !ds || rt->gw != guest_w_ || rt->gh != guest_h_) return false;
    return ds->w == rt->w && ds->h == rt->h && ds->Samples() == rt->Samples();
}

bool VulkanBackend::EnsureOcclusionTargets(uint32_t w, uint32_t h) {
    if (ao_[kAoTargets - 1] && ao_w_ == w && ao_h_ == h) return true;
    for (int i = 0; i < kAoTargets; i++) {
        Release(std::move(ao_[i]));
        ao_[i] = ctx_.CreateImage({.format = kAoFormat, .w = w, .h = h, .target = true});
        if (!ao_[i]) {
            LOGE("vulkan: no %ux%u ambient occlusion target", w, h);
            return false;
        }
    }
    ao_w_ = w;
    ao_h_ = h;
    return true;
}

VkPipeline VulkanBackend::EnsureOcclusionPipeline(const AoPass& a) {
    uint64_t bits = uint64_t(a.out) | uint64_t(a.ms_depth) << 32 | uint64_t(a.samples) << 40 | uint64_t(a.multiply) << 48;
    VkPipeline& pso = pass_psos_[Hash(a.entry, std::strlen(a.entry), bits)];
    if (pso) return pso;
    std::string text = std::string(a.ms_depth ? "#define MS 1\n" : "") + kAoHlsl;
    uint64_t h;
    VkShaderModule vs = Compile(text, "vs", Stage::Vertex, h);
    VkShaderModule ps = Compile(text, a.entry, Stage::Pixel, h);
    if (!vs || !ps) return VK_NULL_HANDLE;
    VkPipelineColorBlendAttachmentState b{};
    b.colorWriteMask = 0xF;
    if (a.multiply) {
        // dst = dst * src; the alpha the game's blends read stays
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        b.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    }
    pso = FullScreenPipeline(vs, "vs", ps, a.entry, a.out, a.samples, &b);
    return pso;
}

// t0 the scene's depth, t1 the pass before; rendered into a.rt over rect
bool VulkanBackend::OcclusionPass(const AoPass& a, float c[kAoConstants], const VkRect2D& rect, uint32_t w, uint32_t h) {
    VkPipeline pso = EnsureOcclusionPipeline(a);
    if (!pso) return false;
    if (a.depth) Barrier(*a.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (a.src) Barrier(*a.src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    BeginRendering({a.rt, nullptr}, w, h);
    c[kAoStepX] = a.step_x;
    c[kAoStepY] = a.step_y;
    SetViewport(0, 0, float(w), float(h));
    SetScissor(rect.offset.x, rect.offset.y, rect.offset.x + int32_t(rect.extent.width), rect.offset.y + int32_t(rect.extent.height));
    Image* tex[4] = {a.depth, a.src, nullptr, nullptr};
    DrawFullScreen(pso, c, kAoConstants * sizeof(float), tex, BlitSampler(true), BlitSampler(false));
    return true;
}

// multiplies occlusion from the scene's depth into its colour, p as d3d::Occlusion sends it
void VulkanBackend::Occlusion(const State& s, const float* p) {
    Target* rt = FindTarget(s.rt, false);
    Target* ds = FindTarget(s.zs, true);
    if (!OcclusionFits(rt, ds)) return;
    const Viewport& v = s.viewport;
    VkRect2D rect = ViewRect(v, *rt);
    if (!rect.extent.width || !rect.extent.height || v.max_z <= v.min_z) return;
    if (!EnsureOcclusionTargets(rt->w, rt->h)) return;
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u occlusion rt %08X ds %08X\n", cap_i_++, s.rt.data, s.zs.data);
    }
    uint32_t samples = ds->Samples();
    LOG_FIRST(1, "vulkan: ambient occlusion %s %ux%u, %u samples, projection %.4f %.4f, view z %.6f / (depth - %.8f)",
              p[6] ? "GTAO" : "SSAO", rt->w, rt->h, samples, 1 / p[0], 1 / p[1], p[4], p[5]);
    float c[kAoConstants];
    OcclusionConstants(v, rect, p, c);
    AoPass raw{.entry = p[6] ? "gtao" : "occlusion", .rt = ao_[0].get(), .depth = ds->res.get(), .ms_depth = samples > 1};
    AoPass across{.entry = "occlusion_blur", .rt = ao_[1].get(), .src = ao_[0].get(), .step_x = 1};
    AoPass down{.entry = "occlusion_apply", .out = rt->fmt, .samples = rt->Samples(), .multiply = true, .rt = rt->res.get(),
                .src = ao_[1].get(), .step_y = 1};
    bound_rt_ = bound_ds_ = nullptr;
    bool ok = OcclusionPass(raw, c, rect, rt->w, rt->h);
    // the depth target goes back to being one: a readable one would pass for one whose single sample copy is current
    Barrier(*ds->res, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    if (!ok || !OcclusionPass(across, c, rect, rt->w, rt->h)) return;
    if (OcclusionPass(down, c, rect, rt->w, rt->h)) {
        rt->drawn = true;
        rt->fb_current = false;
    }
}

}  // namespace gpu::vk
