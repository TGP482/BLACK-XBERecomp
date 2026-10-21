// the game's frames drawn on the CPU: the SDK's harness rasterizer draws the console's calls into the guest's
// surfaces, the way the headless harness does
#include <cstdio>

#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "linux/window.h"
#include "reference_renderer.h"

namespace gpu {

namespace {

class CpuBackend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override {
        reference::BeginFrame();
        LOGI("renderer: CPU %ux%u", width, height);
        return true;
    }
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        reference::Clear(s, count, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        reference::Draw(s, prim, start, count, indices);
        draws_++;
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects_va, uint32_t dst_points_va) override {
        reference::Copy(src, dst, count, rects_va, dst_points_va);
    }
    void Present(const Surface& front) override {
        // the console's front buffer: a linear surface in the guest's memory
        TextureDesc d;
        if (!DescribeSurface(front, d) || !d.width || !d.height) return;
        const uint8_t* pixels = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        if (!pixels) return;
        window::Submit(pixels, d.width, d.height, d.pitch);
        if (++frames_ % 30 == 0)
            LOGI("renderer: frame %llu, %llu draws", (unsigned long long)frames_, (unsigned long long)draws_);
        reference::BeginFrame();
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}

private:
    uint64_t frames_ = 0;
    uint64_t draws_ = 0;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<CpuBackend>(); }

// the CPU draws the frame before it is shown: there is no GPU frame to wait for
void WaitGpuFrame(uint64_t) {}

}  // namespace gpu
