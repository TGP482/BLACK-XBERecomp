#pragma once
// The Vulkan device the renderer draws with: instance, the GPU and its queue, memory through VMA, images, buffers
// and the timeline that stands in for the D3D12 fence. Nothing here knows the title.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

struct VmaAllocator_T;
struct VmaAllocation_T;

namespace gpu::vk {

class Context;

struct ImageDesc {
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t w = 1, h = 1, depth = 1;  // depth > 1: a 3D image
    uint32_t layers = 1;               // 6 with cube
    uint32_t levels = 1;
    uint32_t samples = 1;
    bool cube = false;
    bool target = false;  // a colour or depth attachment
    bool depth_stencil = false;
};

// one image with its views; the layout is tracked here as D3D12 tracked resource states
struct Image {
    Context* ctx = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation_T* alloc = nullptr;
    ImageDesc desc;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageView view = VK_NULL_HANDLE;    // sampled: every level and layer; a depth image's depth alone
    VkImageView target = VK_NULL_HANDLE;  // attachment: level 0, depth and stencil together
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ~Image();
};
using ImageRef = std::shared_ptr<Image>;

enum class Memory : uint8_t { Device, Upload, Readback };

struct Buffer {
    Context* ctx = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation_T* alloc = nullptr;
    uint8_t* map = nullptr;  // host visible ones stay mapped
    VkDeviceSize size = 0;
    ~Buffer();
    void Invalidate();  // a read-back buffer before the CPU reads what the GPU wrote
};
using BufferRef = std::shared_ptr<Buffer>;

struct Caps {
    // the device a saved pipeline cache has to belong to: the header names vendor, device and driver UUID
    uint32_t vendor_id = 0, device_id = 0;
    uint8_t cache_uuid[VK_UUID_SIZE] = {};
    VkFormat depth_format = VK_FORMAT_D24_UNORM_S8_UINT;  // else D32_SFLOAT_S8_UINT
    bool custom_border = false;   // VK_EXT_custom_border_color
    bool hdr_metadata = false;    // VK_EXT_hdr_metadata
    bool colorspace = false;      // VK_EXT_swapchain_colorspace
    bool queries = false;         // precise occlusion queries, reset from the host
    bool geometry = false;        // geometry shaders (points drawn as quads)
    // Vulkan 1.3 has these in core; a 1.1 or 1.2 device (Adreno 6xx, most Android) offers them as
    // extensions. rendering scopes are the one that a device may simply not have at all, and the
    // renderer then draws through render passes instead.
    bool dynamic_rendering = false;       // core 1.3 rendering scopes, else the render pass fallback
    bool sync2 = false;                   // vkCmdPipelineBarrier2 / vkQueueSubmit2
    bool timeline = false;                // timeline semaphores
    bool host_query_reset = false;        // vkResetQueryPool
    bool scalar_layout = false;           // VK_EXT_scalar_block_layout
    bool extended_dynamic_state = false;  // dynamic primitive topology and vertex binding stride
    float max_anisotropy = 1;
    VkSampleCountFlags color_samples = VK_SAMPLE_COUNT_1_BIT, depth_samples = VK_SAMPLE_COUNT_1_BIT;
    std::string device_name;
};

// entry points that only exist as KHR/EXT below the version that promoted them: Android's loader
// exports the core names alone and its drivers answer only the extension ones, so ask for both
struct Dispatch {
    PFN_vkCmdPipelineBarrier2 PipelineBarrier2 = nullptr;
    PFN_vkQueueSubmit2 QueueSubmit2 = nullptr;
    PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue = nullptr;
    PFN_vkWaitSemaphores WaitSemaphores = nullptr;
    PFN_vkResetQueryPool ResetQueryPool = nullptr;
    PFN_vkCmdBindVertexBuffers2 BindVertexBuffers2 = nullptr;
    PFN_vkCmdSetPrimitiveTopology SetPrimitiveTopology = nullptr;
    // rendering scopes: not a required entry point (a 1.1 device has no dynamic rendering), so resolved but not checked
    PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering CmdEndRendering = nullptr;
};

class Context {
public:
    ~Context();
    // instance_extensions: what the window system's surface needs (SDL_Vulkan_GetInstanceExtensions), none headless;
    // XBE_VK_VALIDATION in the environment turns the Khronos validation layer on when it is installed
    bool Init(const char* const* instance_extensions, uint32_t count, bool want_swapchain);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    uint32_t device_api = VK_API_VERSION_1_0;  // the version the device is used at
    VmaAllocator_T* vma = nullptr;
    Caps caps;
    Dispatch d;

    ImageRef CreateImage(const ImageDesc& d);
    BufferRef CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory mem);

    // the timeline semaphore: values signalled by submits, as the D3D12 fence's
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t Completed() const;
    void Wait(uint64_t value) const;

private:
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    uint32_t api_ = VK_API_VERSION_1_3, device_api_ = VK_API_VERSION_1_3;
    bool PickDevice(bool want_swapchain);
    bool CreateDevice(bool want_swapchain);
};

// the image's whole layout changed: the old layout's writes done and visible to the new layout's uses, its reads done
// before the new layout's writes; the copy destination layout is entered again with a barrier, so copies stay in order
void Transition(Context& ctx, VkCommandBuffer cb, Image& img, VkImageLayout to);
// a new rendering scope after another: attachment writes before it done and visible to its loads and writes
void AttachmentDependency(Context& ctx, VkCommandBuffer cb);
// the image at level 0 / layer 0 as a copy's subresource
VkImageSubresourceLayers Layers(const Image& img, uint32_t level = 0, uint32_t layer = 0);

}  // namespace gpu::vk
