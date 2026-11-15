#include "gpu/vk_context.h"

#define VMA_IMPLEMENTATION
// Android's libvulkan.so is a loader and is not a symbol for every entry point: the Vulkan 1.1 ones VMA needs
// (vkBindBufferMemory2, vkGetBufferMemoryRequirements2, vkGetImageMemoryRequirements2, ...) are simply absent, so
// VMA resolves its own through the instance and device proc addresses instead of linking them
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"

namespace gpu::vk {

namespace {

// the floor is Vulkan 1.1: everything the renderer needs that came later (rendering scopes, barrier2,
// timeline semaphores, scalar layout) is available as an extension on a 1.1 device, and where rendering
// scopes are missing the renderer draws through render passes
constexpr uint32_t kMinApi = VK_API_VERSION_1_1;

// Vulkan 1.4 (push descriptors in core) is used where the headers carry it; a build against 1.3 headers,
// as the Android NDK's, leaves it out and takes the extension instead
#if defined(VK_API_VERSION_1_4) && defined(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES)
#define XBE_VK_HAVE_1_4 1
#endif

// what the instance asks the loader for: its own version, capped at what an instance is used at here. The entry
// point arrived in Vulkan 1.1 and Android's loader does not export it, so it is looked up; absent means 1.0
uint32_t InstanceApi() {
    uint32_t v = VK_API_VERSION_1_0;
    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerate) enumerate(&v);
#ifdef XBE_VK_HAVE_1_4
    if (v >= VK_API_VERSION_1_4) return VK_API_VERSION_1_4;
#endif
    if (v >= VK_API_VERSION_1_3) return VK_API_VERSION_1_3;
    return kMinApi;
}
constexpr const char* kValidation = "VK_LAYER_KHRONOS_validation";

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
}

std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice p) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(p, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> out(n);
    vkEnumerateDeviceExtensionProperties(p, nullptr, &n, out.data());
    return out;
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) LOGE("vulkan: %s", data->pMessage);
    else LOG_FIRST(64, "vulkan: %s", data->pMessage);
    return VK_FALSE;
}

// a format the device can draw into and sample as depth with stencil
bool DepthUsable(VkPhysicalDevice p, VkFormat f) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(p, f, &fp);
    VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    return (fp.optimalTilingFeatures & want) == want;
}

VkImageAspectFlags AspectOf(const ImageDesc& d) {
    if (!d.depth_stencil) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
}

}  // namespace

Image::~Image() {
    if (!ctx) return;
    if (view) vkDestroyImageView(ctx->dev, view, nullptr);
    if (target) vkDestroyImageView(ctx->dev, target, nullptr);
    if (image) vmaDestroyImage(ctx->vma, image, alloc);
}

Buffer::~Buffer() {
    if (ctx && buffer) vmaDestroyBuffer(ctx->vma, buffer, alloc);
}

void Buffer::Invalidate() { vmaInvalidateAllocation(ctx->vma, alloc, 0, VK_WHOLE_SIZE); }

Context::~Context() {
    if (dev) vkDeviceWaitIdle(dev);
    if (timeline) vkDestroySemaphore(dev, timeline, nullptr);
    if (vma) vmaDestroyAllocator(vma);
    if (dev) vkDestroyDevice(dev, nullptr);
    if (messenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, messenger_, nullptr);
    }
    // a Swapchain made from this instance is gone by now: its owner destroys it first
    if (instance) vkDestroyInstance(instance, nullptr);
}

bool Context::Init(const char* const* instance_extensions, uint32_t count, bool want_swapchain) {
    std::vector<const char*> exts(instance_extensions, instance_extensions + count);
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> avail(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, avail.data());
    caps.colorspace = want_swapchain && HasExtension(avail, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    if (caps.colorspace) exts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    std::vector<const char*> layers;
    if (std::getenv("XBE_VK_VALIDATION")) {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> lp(ln);
        vkEnumerateInstanceLayerProperties(&ln, lp.data());
        bool have = std::any_of(lp.begin(), lp.end(), [](const VkLayerProperties& l) { return !std::strcmp(l.layerName, kValidation); });
        if (have && HasExtension(avail, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            layers.push_back(kValidation);
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            LOGW("vulkan: XBE_VK_VALIDATION set but %s is not installed", kValidation);
        }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "XBE Recomp";
    app.pEngineName = "XboxXBE-Recomp";
    api_ = InstanceApi();
    app.apiVersion = api_;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    // MoltenVK is a portability driver: the loader refuses an instance that does not ask for it (-9)
    if (HasExtension(avail, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        ici.enabledExtensionCount = uint32_t(exts.size());
        ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    if (VkResult r = vkCreateInstance(&ici, nullptr, &instance); r != VK_SUCCESS) {
        LOGE("vulkan: no instance (%d)", int(r));
        return false;
    }
    if (!layers.empty()) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = OnValidation;
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) create(instance, &mi, nullptr, &messenger_);
    }
    if (!PickDevice(want_swapchain) || !CreateDevice(want_swapchain)) return false;
    VmaAllocatorCreateInfo ai{};
    ai.physicalDevice = phys;
    ai.device = dev;
    ai.instance = instance;
    ai.vulkanApiVersion = std::min(api_, device_api_);
    // the two VMA needs to fetch the rest itself (VMA_DYNAMIC_VULKAN_FUNCTIONS): every other member stays null
    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    ai.pVulkanFunctions = &vf;
    if (vmaCreateAllocator(&ai, &vma) != VK_SUCCESS) return false;
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    vkCreateSemaphore(dev, &sci, nullptr, &timeline);
    LOGI("vulkan: %s, depth %s, custom border colours %s", caps.device_name.c_str(),
         caps.depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", caps.custom_border ? "yes" : "no");
    return true;
}

// a Vulkan 1.1 device with a graphics queue, discrete first; XBE_VK_DEVICE picks by name
bool Context::PickDevice(bool want_swapchain) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> list(n);
    vkEnumeratePhysicalDevices(instance, &n, list.data());
    const char* want = std::getenv("XBE_VK_DEVICE");
    int best = -1;
    for (VkPhysicalDevice p : list) {
        VkPhysicalDeviceProperties pr{};
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.apiVersion < kMinApi) continue;
        if (want_swapchain && !HasExtension(DeviceExtensions(p), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, qs.data());
        int fam = -1;
        for (uint32_t i = 0; i < qn && fam < 0; i++)
            if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) fam = int(i);
        if (fam < 0) continue;
        int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                    : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (want && std::strstr(pr.deviceName, want)) score = 10;
        if (score > best) {
            best = score;
            phys = p;
            family = uint32_t(fam);
            caps.device_name = pr.deviceName;
        }
    }
    if (!phys) LOGE("vulkan: no device with Vulkan 1.1 and a graphics queue");
    return phys != VK_NULL_HANDLE;
}

bool Context::CreateDevice(bool want_swapchain) {
    std::vector<VkExtensionProperties> avail = DeviceExtensions(phys);
    VkPhysicalDeviceProperties pr{};
    vkGetPhysicalDeviceProperties(phys, &pr);
    caps.vendor_id = pr.vendorID;
    caps.device_id = pr.deviceID;
    std::memcpy(caps.cache_uuid, pr.pipelineCacheUUID, VK_UUID_SIZE);
    // the version the device is used at: its own, capped by what the instance asked for
    device_api_ = std::min(pr.apiVersion, api_);
    device_api = device_api_;
    const bool v12 = device_api_ >= VK_API_VERSION_1_2, v13 = device_api_ >= VK_API_VERSION_1_3;
#ifdef XBE_VK_HAVE_1_4
    const bool v14 = device_api_ >= VK_API_VERSION_1_4;
#else
    const bool v14 = false;
#endif
    const bool modern = v13;
    auto has = [&](const char* n) { return HasExtension(avail, n); };

    std::vector<const char*> exts;
    if (want_swapchain) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (!v14) {
        if (!has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
            LOGE("vulkan: %s has no push descriptors", caps.device_name.c_str());
            return false;
        }
        exts.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        if (has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) exts.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    }

    // feature availability defaults by core version; the extensions below may be required for 1.1/1.2 devices
    caps.dynamic_rendering = v13;
    caps.sync2 = v13;
    caps.timeline = v12;
    caps.host_query_reset = v12;
    caps.scalar_layout = v12;
    caps.extended_dynamic_state = v13;

    if (!v13 && has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        caps.sync2 = true;
    }
    if (!v13 && has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        caps.dynamic_rendering = true;
    }
    if (!v12 && has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        caps.timeline = true;
    }
    if (!v12 && has(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
        caps.host_query_reset = true;
    }
    if (!v12 && has(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME);
        caps.scalar_layout = true;
    }
    if (!v13 && has(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
        caps.extended_dynamic_state = true;
    }

    if (!caps.timeline) {
        LOGE("vulkan: %s has no timeline semaphores", caps.device_name.c_str());
        return false;
    }
    if (!caps.sync2) {
        LOGE("vulkan: %s has no synchronization2", caps.device_name.c_str());
        return false;
    }
    if (!caps.extended_dynamic_state) {
        LOGE("vulkan: %s has no extended dynamic state", caps.device_name.c_str());
        return false;
    }

    caps.custom_border = has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (caps.custom_border) exts.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    caps.hdr_metadata = want_swapchain && has(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    if (caps.hdr_metadata) exts.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);

    // what the device has, then what is turned on
    VkPhysicalDeviceCustomBorderColorFeaturesEXT have_border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceSynchronization2Features have_s2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, &have_border};
    VkPhysicalDeviceTimelineSemaphoreFeatures have_ts{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, &have_s2};
    VkPhysicalDeviceHostQueryResetFeatures have_hq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, &have_ts};
    VkPhysicalDeviceScalarBlockLayoutFeatures have_sb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, &have_hq};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &have_sb};
    // the Vulkan 1.1 query, likewise not a symbol on Android: without it the core features are asked for the 1.0
    // way and the extension feature structs above stay false
    auto get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    if (!get_features2) {
        get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if (get_features2) {
        get_features2(phys, &have);
    } else {
        vkGetPhysicalDeviceFeatures(phys, &have.features);
    }
    caps.custom_border = caps.custom_border && have_border.customBorderColors && have_border.customBorderColorWithoutFormat;
    caps.timeline = caps.timeline && have_ts.timelineSemaphore;
    caps.sync2 = caps.sync2 && have_s2.synchronization2;
    caps.host_query_reset = caps.host_query_reset && have_hq.hostQueryReset;
    caps.scalar_layout = caps.scalar_layout && have_sb.scalarBlockLayout;
    caps.queries = caps.host_query_reset && have.features.occlusionQueryPrecise;
    caps.geometry = have.features.geometryShader;
    // XBE_VK_RENDERPASS: draw through render passes even where rendering scopes exist, to exercise the fall-back
    if (std::getenv("XBE_VK_RENDERPASS")) caps.dynamic_rendering = false;

    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    border.customBorderColors = caps.custom_border;
    border.customBorderColorWithoutFormat = caps.custom_border;
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    m5.maintenance5 = VK_TRUE;
#ifdef XBE_VK_HAVE_1_4
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    f14.pushDescriptor = VK_TRUE;
    f14.maintenance5 = VK_TRUE;
#endif
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    f12.scalarBlockLayout = caps.scalar_layout;
    f12.hostQueryReset = caps.host_query_reset;

    VkPhysicalDeviceDynamicRenderingFeatures f_dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    f_dr.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceSynchronization2Features f_s2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    f_s2.synchronization2 = VK_TRUE;
    VkPhysicalDeviceTimelineSemaphoreFeatures f_ts{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    f_ts.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceHostQueryResetFeatures f_hq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
    f_hq.hostQueryReset = caps.host_query_reset ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceScalarBlockLayoutFeatures f_sb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES};
    f_sb.scalarBlockLayout = caps.scalar_layout ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.features.samplerAnisotropy = have.features.samplerAnisotropy;
    f.features.textureCompressionBC = have.features.textureCompressionBC;
    f.features.fillModeNonSolid = have.features.fillModeNonSolid;
    f.features.geometryShader = have.features.geometryShader;
    f.features.depthBiasClamp = have.features.depthBiasClamp;
    f.features.occlusionQueryPrecise = have.features.occlusionQueryPrecise;

    // what was asked for: on a 1.3 device the 1.2 and 1.3 structs, and below it the same features through
    // the structs of the extensions that carry them
    void** tail = nullptr;
    if (modern) {
        f.pNext = &f12;
        f12.pNext = &f13;
        tail = &f13.pNext;
    } else {
        f.pNext = &f_ts;
        f_ts.pNext = &f_sb;
        f_sb.pNext = &f_hq;
        f_hq.pNext = &f_s2;
        tail = &f_s2.pNext;
        if (caps.dynamic_rendering) {
            *tail = &f_dr;
            tail = &f_dr.pNext;
        }
    }
#ifdef XBE_VK_HAVE_1_4
    if (v14) {
        *tail = &f14;
        tail = &f14.pNext;
    }
#endif
    bool maintenance5 = !v14 && std::find_if(exts.begin(), exts.end(), [](const char* e) {
                                     return !std::strcmp(e, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
                                 }) != exts.end();
    if (maintenance5) {
        *tail = &m5;
        tail = &m5.pNext;
    }
    if (caps.custom_border) *tail = &border;
    if (!have.features.textureCompressionBC) LOGW("vulkan: no BC textures, DXT textures will not show");

    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    if (VkResult r = vkCreateDevice(phys, &dci, nullptr, &dev); r != VK_SUCCESS) {
        LOGE("vulkan: no device (%d)", int(r));
        return false;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);

    // resolve promoted entry points: the core name where the device has the version that promoted it, else
    // the KHR/EXT name, which is what a 1.1 driver (and Android's loader) answers
    auto get = [&](const char* core, const char* alias) -> PFN_vkVoidFunction {
        PFN_vkVoidFunction p = vkGetDeviceProcAddr(dev, core);
        if (!p && alias) p = vkGetDeviceProcAddr(dev, alias);
        return p;
    };
    d.PipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)get("vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR");
    d.QueueSubmit2 = (PFN_vkQueueSubmit2)get("vkQueueSubmit2", "vkQueueSubmit2KHR");
    d.GetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)get("vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR");
    d.WaitSemaphores = (PFN_vkWaitSemaphores)get("vkWaitSemaphores", "vkWaitSemaphoresKHR");
    d.ResetQueryPool = (PFN_vkResetQueryPool)get("vkResetQueryPool", "vkResetQueryPoolEXT");
    d.BindVertexBuffers2 = (PFN_vkCmdBindVertexBuffers2)get("vkCmdBindVertexBuffers2", "vkCmdBindVertexBuffers2EXT");
    d.SetPrimitiveTopology = (PFN_vkCmdSetPrimitiveTopology)get("vkCmdSetPrimitiveTopology", "vkCmdSetPrimitiveTopologyEXT");
    // rendering scopes, where the device has them; Android's loader does not export them, so they only ever come
    // from a proc address (and a 1.1 device does not offer them at all)
    d.CmdBeginRendering = (PFN_vkCmdBeginRendering)get("vkCmdBeginRendering", "vkCmdBeginRenderingKHR");
    d.CmdEndRendering = (PFN_vkCmdEndRendering)get("vkCmdEndRendering", "vkCmdEndRenderingKHR");
    // check required ones
    if (!d.PipelineBarrier2 || !d.QueueSubmit2 || !d.GetSemaphoreCounterValue || !d.WaitSemaphores || !d.BindVertexBuffers2 || !d.SetPrimitiveTopology) {
        LOGE("vulkan: failed to resolve promoted entry points");
        return false;
    }
    // rendering scopes need their entry points to have resolved
    if (caps.dynamic_rendering && (!d.CmdBeginRendering || !d.CmdEndRendering)) {
        LOGW("vulkan: %s is offered but its entry points do not resolve: drawing through render passes", VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        caps.dynamic_rendering = false;
    }
    if (caps.host_query_reset && !d.ResetQueryPool) {
        caps.host_query_reset = false;
        caps.queries = false;
    }

    LOGI("vulkan: device api %u.%u.%u (instance %u.%u.%u), dynamic rendering %d, sync2 %d", VK_API_VERSION_MAJOR(device_api_),
         VK_API_VERSION_MINOR(device_api_), VK_API_VERSION_PATCH(device_api_), VK_API_VERSION_MAJOR(api_),
         VK_API_VERSION_MINOR(api_), VK_API_VERSION_PATCH(api_), int(caps.dynamic_rendering), int(caps.sync2));
    caps.depth_format = DepthUsable(phys, VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    caps.max_anisotropy = have.features.samplerAnisotropy ? pr.limits.maxSamplerAnisotropy : 1;
    caps.color_samples = pr.limits.framebufferColorSampleCounts & pr.limits.sampledImageColorSampleCounts;
    caps.depth_samples = pr.limits.framebufferDepthSampleCounts & pr.limits.sampledImageDepthSampleCounts;
    return true;
}
ImageRef Context::CreateImage(const ImageDesc& d) {
    auto img = std::make_shared<Image>();
    img->ctx = this;
    img->desc = d;
    img->aspect = AspectOf(d);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = d.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = d.format;
    ci.extent = {d.w, d.h, d.depth};
    ci.mipLevels = d.levels;
    ci.arrayLayers = d.layers;
    ci.samples = VkSampleCountFlagBits(d.samples);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (d.target) ci.usage |= d.depth_stencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (d.cube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (d.target) ac.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(vma, &ci, &ac, &img->image, &img->alloc, nullptr) != VK_SUCCESS) {
        img->image = VK_NULL_HANDLE;
        return nullptr;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.format = d.format;
    vi.viewType = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    // a depth image is sampled as depth, and drawn into as depth and stencil
    VkImageAspectFlags sampled = d.depth_stencil ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange = {sampled, 0, d.levels, 0, d.layers};
    if (vkCreateImageView(dev, &vi, nullptr, &img->view) != VK_SUCCESS) return nullptr;
    if (d.target) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {img->aspect, 0, 1, 0, 1};
        if (vkCreateImageView(dev, &vi, nullptr, &img->target) != VK_SUCCESS) return nullptr;
    }
    return img;
}

BufferRef Context::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory mem) {
    auto buf = std::make_shared<Buffer>();
    buf->ctx = this;
    buf->size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO;
    if (mem == Memory::Upload)
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    else if (mem == Memory::Readback)
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &ci, &ac, &buf->buffer, &buf->alloc, &info) != VK_SUCCESS) {
        buf->buffer = VK_NULL_HANDLE;
        return nullptr;
    }
    buf->map = static_cast<uint8_t*>(info.pMappedData);
    return buf;
}

uint64_t Context::Completed() const {
    uint64_t v = 0;
    d.GetSemaphoreCounterValue(dev, timeline, &v);
    return v;
}

void Context::Wait(uint64_t value) const {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &value;
    d.WaitSemaphores(dev, &wi, UINT64_MAX);
}

namespace {

// the stages and accesses an image in a layout is used with, the two sides of a barrier
struct Use {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

Use UseOf(VkImageLayout l) {
    switch (l) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
        default: return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    }
}

// the writes among a use's accesses: what a later access must wait for
VkAccessFlags2 Writes(VkAccessFlags2 a) {
    return a & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

}  // namespace

// the layout change waits for the old layout's uses and makes its writes visible to the new one's; staying in the
// copy destination layout still orders one copy after the other
void Transition(Context& ctx, VkCommandBuffer cb, Image& img, VkImageLayout to) {
    if (img.layout == to && to != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return;
    LOG_FIRST(24, "vulkan: barrier image %p format %d aspect %u layout %d -> %d (%ux%u)", (void*)img.image,
              int(img.desc.format), unsigned(img.aspect), int(img.layout), int(to), img.desc.w, img.desc.h);
    Use from = UseOf(img.layout), next = UseOf(to);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = from.stage;
    b.srcAccessMask = Writes(from.access);
    b.dstStageMask = next.stage;
    b.dstAccessMask = next.access;
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    ctx.d.PipelineBarrier2(cb, &di);
    img.layout = to;
}

void AttachmentDependency(Context& ctx, VkCommandBuffer cb) {
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    m.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    m.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &m;
    ctx.d.PipelineBarrier2(cb, &di);
}

VkImageSubresourceLayers Layers(const Image& img, uint32_t level, uint32_t layer) {
    return {img.aspect, level, layer, 1};
}

}  // namespace gpu::vk
