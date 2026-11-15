#pragma once
// The window's swap chain: SDR BGRA, or HDR10 (PQ over BT.2020) where the surface offers it. Vsync off presents
// without waiting, tearing as the D3D12 path's ALLOW_TEARING did, where the driver has that mode.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "gpu/vk_context.h"

namespace gpu::vk {

class Swapchain {
public:
    ~Swapchain();
    // takes the surface; false when the context's queue can't present to it
    bool Init(Context& ctx, VkSurfaceKHR surface);
    // the surface has an HDR10 format
    bool HdrCapable() const { return hdr_format_.format != VK_FORMAT_UNDEFINED; }
    // made again when the size, the HDR choice or vsync changed; false when the window has no size (minimized)
    bool Configure(uint32_t w, uint32_t h, bool hdr, bool vsync);
    // the next image to draw into; wait is signalled once it may be written. false: out of date, Configure again
    bool Acquire(uint32_t& index, VkSemaphore& wait);
    // presents image index once done is signalled; false: out of date
    bool Present(uint32_t index, VkSemaphore done);
    VkSemaphore DoneSemaphore(uint32_t index) const { return done_[index]; }
    Image& At(uint32_t index) { return images_[index]; }
    uint32_t Width() const { return w_; }
    uint32_t Height() const { return h_; }
    VkFormat Format() const { return fmt_.format; }
    VkSurfaceTransformFlagBitsKHR Transform() const { return transform_; }
    bool Hdr() const { return hdr_on_; }
    void Invalidate() { dirty_ = true; }
    // called with the old chain idle and its images about to go: caches holding their views drop what refers to them
    std::function<void()> on_views_gone;

private:
    void Destroy();
    Context* ctx_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR sdr_format_{}, hdr_format_{}, fmt_{};
    std::vector<Image> images_;
    std::vector<VkSemaphore> done_;     // per image: drawing finished, the present waits for it
    std::vector<VkSemaphore> acquire_;  // a ring, one more than the images
    uint32_t acquire_next_ = 0;
    uint32_t w_ = 0, h_ = 0;
    VkSurfaceTransformFlagBitsKHR transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    bool hdr_on_ = false, vsync_ = true, dirty_ = true;
};

}  // namespace gpu::vk
