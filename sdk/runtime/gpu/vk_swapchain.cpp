#include "gpu/vk_swapchain.h"

#include <algorithm>
#include <cstdlib>

#include "core/log.h"

namespace gpu::vk {

namespace {

// why the chain is made again, for the diag line
enum class Trigger { None, Create, Resize, Flags, OutOfDate, SuboptimalSize, Invalidated };

const char* TriggerName(Trigger t) {
    switch (t) {
        case Trigger::Create: return "create";
        case Trigger::Resize: return "resize";
        case Trigger::Flags: return "flags";
        case Trigger::OutOfDate: return "out-of-date";
        case Trigger::SuboptimalSize: return "suboptimal+size";
        case Trigger::Invalidated: return "invalidated";
        case Trigger::None: break;
    }
    return "none";
}

// XBE_DIAG: the recreate line, which says what asked for it
bool DiagOn() {
    static const bool on = std::getenv("XBE_DIAG") != nullptr;
    return on;
}

const char* ResultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        default: return "VkResult";
    }
}

// the first five results that aren't VK_SUCCESS, so a surface that keeps answering stops short of flooding the log
void LogResult(VkResult r, const char* what) {
    static int seen = 0;
    if (r == VK_SUCCESS || !DiagOn() || seen >= 5) return;
    seen++;
    LOGR("diag: swap chain %s: %s (%d)", what, ResultName(r), int(r));
}

// one window and one chain per run, so the bookkeeping sits here rather than in the header: what the caller asked
// for (before the surface's own size wins), what the surface last asked for, why dirty_ was set, and the counters
struct Book {
    uint32_t asked_w = 0, asked_h = 0;
    uint32_t surf_w = 0, surf_h = 0;
    Trigger why = Trigger::None;
    uint32_t recreates = 0;
};
Book g_book;

// has the surface actually moved since the chain was built? a pinned currentExtent is the only thing here that says
// so: where the extent is left to us, nothing outside our own request can have changed it
bool SurfaceMoved(VkPhysicalDevice phys, VkSurfaceKHR surface, uint32_t w, uint32_t h, VkSurfaceTransformFlagBitsKHR t) {
    VkSurfaceCapabilitiesKHR sc{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &sc) != VK_SUCCESS) return false;
    if (sc.currentExtent.width == UINT32_MAX) return false;
    return sc.currentExtent.width != w || sc.currentExtent.height != h || sc.currentTransform != t;
}

std::vector<VkSurfaceFormatKHR> SurfaceFormats(VkPhysicalDevice p, VkSurfaceKHR s) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(p, s, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> out(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(p, s, &n, out.data());
    return out;
}

VkPresentModeKHR PresentMode(VkPhysicalDevice p, VkSurfaceKHR s, bool vsync) {
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(p, s, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(p, s, &n, modes.data());
    for (VkPresentModeKHR want : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
        if (std::find(modes.begin(), modes.end(), want) != modes.end()) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkSemaphore NewSemaphore(VkDevice dev) {
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &ci, nullptr, &s);
    return s;
}

}  // namespace

Swapchain::~Swapchain() {
    Destroy();
    if (ctx_ && surface_) vkDestroySurfaceKHR(ctx_->instance, surface_, nullptr);
}

void Swapchain::Destroy() {
    if (!ctx_) return;
    vkDeviceWaitIdle(ctx_->dev);
    if (on_views_gone) on_views_gone();
    for (Image& img : images_) {
        if (img.target) vkDestroyImageView(ctx_->dev, img.target, nullptr);
        img.target = img.view = VK_NULL_HANDLE;
        img.image = VK_NULL_HANDLE;
    }
    images_.clear();
    for (VkSemaphore s : done_) vkDestroySemaphore(ctx_->dev, s, nullptr);
    for (VkSemaphore s : acquire_) vkDestroySemaphore(ctx_->dev, s, nullptr);
    done_.clear();
    acquire_.clear();
    if (swap_) vkDestroySwapchainKHR(ctx_->dev, swap_, nullptr);
    swap_ = VK_NULL_HANDLE;
}

bool Swapchain::Init(Context& ctx, VkSurfaceKHR surface) {
    ctx_ = &ctx;
    surface_ = surface;
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx.phys, ctx.family, surface, &ok);
    if (!ok) {
        LOGE("vulkan: the graphics queue can't present to the window");
        return false;
    }
    const std::vector<VkSurfaceFormatKHR> formats = SurfaceFormats(ctx.phys, surface);
    // desktop surfaces offer BGRA8; Android's WSI only RGBA8. The two hold the same logical channels in the other
    // byte order and the WSI lays them out for the display, so either presents correctly: prefer BGRA8, but take
    // RGBA8 where that is all there is. A lone UNDEFINED entry is the surface saying "any format"
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        sdr_format_ = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const VkSurfaceFormatKHR& f : formats) {
        bool sdr = (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                   f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        bool hdr10 = (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
                     f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
        // BGRA8 wins over RGBA8, however the WSI ordered the list
        if (sdr && (sdr_format_.format == VK_FORMAT_UNDEFINED || f.format == VK_FORMAT_B8G8R8A8_UNORM)) sdr_format_ = f;
        if (hdr10 && hdr_format_.format == VK_FORMAT_UNDEFINED) hdr_format_ = f;
    }
    if (sdr_format_.format == VK_FORMAT_UNDEFINED) {
        LOGE("vulkan: the window offers neither BGRA8 nor RGBA8 (%u surface formats)", unsigned(formats.size()));
        return false;
    }
    for (const VkSurfaceFormatKHR& f : formats)
        LOGI("vulkan: surface offers format %d in colour space %d", int(f.format), int(f.colorSpace));
    LOGI("vulkan: window HDR10 %s", HdrCapable() ? "available" : "not offered");
    return true;
}

bool Swapchain::Configure(uint32_t w, uint32_t h, bool hdr, bool vsync) {
    hdr = hdr && HdrCapable();
    // the fast path compares the caller's request, not the extent we built: MoltenVK and X11 pin the surface's size
    // (currentExtent is never UINT32_MAX there), so whenever the window's reported pixels lag behind the drawable --
    // which is every resize until SDL delivers the event -- the pinned extent never equals the request, and comparing
    // the two rebuilt the chain every frame
    if (swap_ && !dirty_ && w == g_book.asked_w && h == g_book.asked_h && hdr == hdr_on_ && vsync == vsync_) return true;
    VkSurfaceCapabilitiesKHR sc{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_->phys, surface_, &sc);
    g_book.asked_w = w;
    g_book.asked_h = h;
    // the surface's own size wins where it has one (X11); Wayland leaves it to us
    if (sc.currentExtent.width != UINT32_MAX) {
        w = sc.currentExtent.width;
        h = sc.currentExtent.height;
    }
    g_book.surf_w = w;
    g_book.surf_h = h;
    w = std::clamp(w, sc.minImageExtent.width, std::max(sc.maxImageExtent.width, 1u));
    h = std::clamp(h, sc.minImageExtent.height, std::max(sc.maxImageExtent.height, 1u));
    if (!w || !h) return false;
    // the request moved but the surface still asks for the chain we have: nothing to make again
    if (swap_ && w == w_ && h == h_ && hdr == hdr_on_ && vsync == vsync_) {
        dirty_ = false;
        g_book.why = Trigger::None;
        return true;
    }
    // what asked for the rebuild: the result that set dirty_ first, else the size or the flags that moved
    Trigger why = g_book.why;
    if (!swap_) {
        why = Trigger::Create;
    } else if (why != Trigger::OutOfDate && why != Trigger::SuboptimalSize) {
        if (w != w_ || h != h_) why = Trigger::Resize;
        else if (hdr != hdr_on_ || vsync != vsync_) why = Trigger::Flags;
        else if (why == Trigger::None) why = Trigger::Invalidated;
    }
    VkSwapchainKHR old = swap_;

    // the old chain's images go first, it is handed to the new one and destroyed after
    vkDeviceWaitIdle(ctx_->dev);
    if (on_views_gone) on_views_gone();
    for (Image& img : images_)
        if (img.target) vkDestroyImageView(ctx_->dev, img.target, nullptr);
    images_.clear();
    for (VkSemaphore s : done_) vkDestroySemaphore(ctx_->dev, s, nullptr);
    for (VkSemaphore s : acquire_) vkDestroySemaphore(ctx_->dev, s, nullptr);
    done_.clear();
    acquire_.clear();

    fmt_ = hdr ? hdr_format_ : sdr_format_;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(sc.minImageCount, 3u);
    if (sc.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, sc.maxImageCount);
    ci.imageFormat = fmt_.format;
    ci.imageColorSpace = fmt_.colorSpace;
    ci.imageExtent = {w, h};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = sc.currentTransform;
    transform_ = sc.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = PresentMode(ctx_->phys, surface_, vsync);
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VkResult r = vkCreateSwapchainKHR(ctx_->dev, &ci, nullptr, &swap_);
    if (old) vkDestroySwapchainKHR(ctx_->dev, old, nullptr);
    if (r != VK_SUCCESS) {
        swap_ = VK_NULL_HANDLE;
        LOGE("vulkan: no %ux%u swap chain (%d)", w, h, int(r));
        return false;
    }
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx_->dev, swap_, &n, nullptr);
    std::vector<VkImage> list(n);
    vkGetSwapchainImagesKHR(ctx_->dev, swap_, &n, list.data());
    images_.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        Image& img = images_[i];
        img.image = list[i];
        img.desc.format = fmt_.format;
        img.desc.w = w;
        img.desc.h = h;
        img.desc.target = true;
        img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = list[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt_.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx_->dev, &vi, nullptr, &img.target);
        done_.push_back(NewSemaphore(ctx_->dev));
    }
    for (uint32_t i = 0; i <= n; i++) acquire_.push_back(NewSemaphore(ctx_->dev));
    w_ = w;
    h_ = h;
    hdr_on_ = hdr;
    vsync_ = vsync;
    dirty_ = false;
    g_book.why = Trigger::None;
    LOGI("vulkan: swap chain %ux%u, %u images, %s, %s", w, h, n, hdr ? "HDR10" : "SDR",
         ci.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    // DIAG(swapchain): every rebuild and what drove it, so a churn loop is one grep away rather than inferred
    if (DiagOn())
        LOGR("diag: swap chain rebuild %u: %s, asked %ux%u, surface %ux%u, built %ux%u, %s, %s", ++g_book.recreates,
             TriggerName(why), g_book.asked_w, g_book.asked_h, g_book.surf_w, g_book.surf_h, w_, h_,
             hdr ? "HDR10" : "SDR",
             ci.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    if (hdr && ctx_->caps.hdr_metadata) {
        // the mastering display as the frame is graded: BT.2020 primaries, D65, the output's peak left to the panel
        VkHdrMetadataEXT md{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        md.displayPrimaryRed = {0.708f, 0.292f};
        md.displayPrimaryGreen = {0.170f, 0.797f};
        md.displayPrimaryBlue = {0.131f, 0.046f};
        md.whitePoint = {0.3127f, 0.3290f};
        md.maxLuminance = 1000.0f;
        md.minLuminance = 0.001f;
        auto set = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(vkGetDeviceProcAddr(ctx_->dev, "vkSetHdrMetadataEXT"));
        if (set) set(ctx_->dev, 1, &swap_, &md);
    }
    return true;
}

bool Swapchain::Acquire(uint32_t& index, VkSemaphore& wait) {
    if (!swap_) return false;
    wait = acquire_[acquire_next_];
    acquire_next_ = (acquire_next_ + 1) % uint32_t(acquire_.size());
    VkResult r = vkAcquireNextImageKHR(ctx_->dev, swap_, UINT64_MAX, wait, VK_NULL_HANDLE, &index);
    LogResult(r, "acquire");
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        dirty_ = true;
        g_book.why = Trigger::OutOfDate;
        return false;
    }
    // SUBOPTIMAL says the surface would prefer another chain, not that this one is unusable: MoltenVK in immediate
    // mode reports it on a chain it presents happily. It only asks for a rebuild when the surface really moved
    if (r == VK_SUBOPTIMAL_KHR && SurfaceMoved(ctx_->phys, surface_, w_, h_, transform_)) {
        dirty_ = true;
        g_book.why = Trigger::SuboptimalSize;
    }
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

bool Swapchain::Present(uint32_t index, VkSemaphore done) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swap_;
    pi.pImageIndices = &index;
    VkResult r = vkQueuePresentKHR(ctx_->queue, &pi);
    LogResult(r, "present");
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        dirty_ = true;
        g_book.why = Trigger::OutOfDate;
    } else if (r == VK_SUBOPTIMAL_KHR && SurfaceMoved(ctx_->phys, surface_, w_, h_, transform_)) {
        dirty_ = true;
        g_book.why = Trigger::SuboptimalSize;
    }
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

}  // namespace gpu::vk
