// the SDK's backend hooks on Linux: the Vulkan renderer, and the title's waits on its frames
#include "d3d/gpu_frame.h"
#include "vk/vk_backend.h"

namespace gpu {

namespace {
vk::VulkanBackend* g_vulkan = nullptr;
}  // namespace

std::unique_ptr<Backend> CreateBackend() {
    auto backend = std::make_unique<vk::VulkanBackend>();
    g_vulkan = backend.get();
    return backend;
}

void WaitGpuFrame(uint64_t n) {
    if (g_vulkan) g_vulkan->WaitGpuFrame(n);
}

}  // namespace gpu
