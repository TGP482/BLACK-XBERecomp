// the Linux renderer stand-in: it counts the frames and the draws and draws nothing. The CPU rasterizer of the SDK's
// harness draws them in milestone 2, the Vulkan backend in milestone 4
#include <cstdio>

#include "app/app.h"
#include "core/log.h"
#include "d3d/gpu_frame.h"
#include "gpu/backend.h"
#include "gpu/texture.h"

namespace gpu {

namespace {

class NullBackend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override {
        LOGI("renderer: Linux stand-in %ux%u", width, height);
        return true;
    }
    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Draw(const State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override { draws_++; }
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va, uint32_t dst_points_va) override {
        CopySurfaceMemory(src, dst, count, src_rects_va, dst_points_va);
    }
    void Present(const Surface&) override {
        if (++frames_ % 60 == 0) LOGI("renderer: frame %llu, %llu draws", frames_, draws_);
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

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<NullBackend>(); }

// the stand-in draws nothing: there is no GPU frame to wait for
void WaitGpuFrame(uint64_t) {}

}  // namespace gpu
