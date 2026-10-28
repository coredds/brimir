// Brimir - libretro Vulkan hardware context (BRIMIR_VULKAN builds only)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "libretro_vulkan.hpp"

#include <gpu/frame_presenter.hpp>
#include <gpu/gpu_version.hpp>
#include <gpu/vk_device.hpp>
#include <gpu/vk_util.hpp>
#include <gpu/vdp/vulkan_vdp_renderer.hpp>

#include <libretro_vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <optional>
#include <vector>

namespace brimir::libretro {

namespace {

std::string VersionString(uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." + std::to_string(VK_API_VERSION_MINOR(version));
}

// ---------------------------------------------------------------------------------------------------------------
// Context negotiation (libretro_vulkan.h). The frontend keeps pointers to these structs: static storage only.

retro_log_printf_t g_log = nullptr;

struct NegotiationState {
    // get_application_info was called: the frontend created its instance for kRequiredApiVersion (or higher).
    bool appInfoRequested = false;
    // The device create_device2 made.
    VkDevice device = VK_NULL_HANDLE;
    // It is on a GPU that meets the baseline, with exactly the features and extensions gpu_version.hpp lists. When
    // no GPU did, create_device2 still made a device (on the frontend's GPU) for the copy-only output.
    bool baseline = false;
};
NegotiationState g_negotiation;

void NegotiationLog(retro_log_level level, const std::string &text) {
    if (g_log) {
        g_log(level, "[Brimir] %s\n", text.c_str());
    }
}

const VkApplicationInfo *GetApplicationInfo() {
    static const VkApplicationInfo info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Brimir",
        .applicationVersion = 0,
        .pEngineName = "Brimir",
        .engineVersion = 0,
        .apiVersion = gpu::kRequiredApiVersion,
    };
    g_negotiation.appInfoRequested = true;
    return &info;
}

// The device-independent half of device selection, through the frontend's vkGetInstanceProcAddr (volk is loaded in
// context_reset, for the device the frontend actually hands over).
struct InstanceFunctions {
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties getProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures getFeatures = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilies = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerateExtensions = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR getSurfaceSupport = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;

    bool Load(VkInstance instance, PFN_vkGetInstanceProcAddr gipa) {
        const auto load = [&](auto &fn, const char *name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(gipa(instance, name));
        };
        load(enumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
        load(getProperties, "vkGetPhysicalDeviceProperties");
        load(getFeatures, "vkGetPhysicalDeviceFeatures");
        load(getQueueFamilies, "vkGetPhysicalDeviceQueueFamilyProperties");
        load(enumerateExtensions, "vkEnumerateDeviceExtensionProperties");
        load(getSurfaceSupport, "vkGetPhysicalDeviceSurfaceSupportKHR");
        load(getDeviceProcAddr, "vkGetDeviceProcAddr");
        return enumeratePhysicalDevices && getProperties && getFeatures && getQueueFamilies && enumerateExtensions &&
               getDeviceProcAddr;
    }
};

// A queue family with graphics and compute (the frontend presents from the queue it gives the core), or with graphics
// only when `compute` is false, and present support when there is a surface.
std::optional<uint32_t> FindQueueFamily(const InstanceFunctions &fn, VkPhysicalDevice gpu, VkSurfaceKHR surface,
                                        bool compute) {
    uint32_t familyCount = 0;
    fn.getQueueFamilies(gpu, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    fn.getQueueFamilies(gpu, &familyCount, families.data());
    const VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | (compute ? VK_QUEUE_COMPUTE_BIT : 0);
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & wanted) != wanted || families[i].queueCount == 0) {
            continue;
        }
        if (surface != VK_NULL_HANDLE) {
            VkBool32 present = VK_FALSE;
            if (!fn.getSurfaceSupport || fn.getSurfaceSupport(gpu, i, surface, &present) != VK_SUCCESS || !present) {
                continue;
            }
        }
        return i;
    }
    return std::nullopt;
}

std::vector<VkExtensionProperties> DeviceExtensions(const InstanceFunctions &fn, VkPhysicalDevice gpu) {
    uint32_t extensionCount = 0;
    fn.enumerateExtensions(gpu, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    fn.enumerateExtensions(gpu, nullptr, &extensionCount, extensions.data());
    extensions.resize(extensionCount);
    return extensions;
}

bool HasExtension(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const VkExtensionProperties &ext) { return std::strcmp(ext.extensionName, name) == 0; });
}

// The queue family for the GPU renderer, or an empty optional and *rejection set when the device does not meet the
// baseline (gpu_version.hpp).
std::optional<uint32_t> EvaluateDevice(const InstanceFunctions &fn, VkPhysicalDevice gpu, VkSurfaceKHR surface,
                                       std::string &rejection) {
    VkPhysicalDeviceProperties properties{};
    fn.getProperties(gpu, &properties);
    const std::string name = properties.deviceName;
    if (properties.apiVersion < gpu::kRequiredApiVersion) {
        rejection = name + ": Vulkan " + VersionString(properties.apiVersion) + " < " +
                    VersionString(gpu::kRequiredApiVersion);
        return std::nullopt;
    }
    if (const std::string limit = gpu::UnmetLimit(properties.limits); !limit.empty()) {
        rejection = name + ": limit " + limit;
        return std::nullopt;
    }

    VkPhysicalDeviceFeatures features{};
    fn.getFeatures(gpu, &features);
    for (const gpu::CoreFeature &feature : gpu::kRequiredCoreFeatures) {
        if (!(features.*feature.member)) {
            rejection = name + ": missing feature " + feature.name;
            return std::nullopt;
        }
    }

    const std::vector<VkExtensionProperties> extensions = DeviceExtensions(fn, gpu);
    for (const char *required : gpu::kRequiredDeviceExtensions) {
        if (!HasExtension(extensions, required)) {
            rejection = name + ": missing extension " + required;
            return std::nullopt;
        }
    }

    if (const auto family = FindQueueFamily(fn, gpu, surface, true)) {
        return family;
    }
    rejection = name + (surface != VK_NULL_HANDLE ? ": no graphics + compute queue that can present"
                                                  : ": no graphics + compute queue");
    return std::nullopt;
}

// Negotiation v1: the core would have to create the device itself with the frontend's extensions, layers and
// features. Declining makes the frontend create its default device (graphics + compute queue); context_reset then
// checks that device. RetroArch only negotiates when this is non-null, and prefers create_device2.
bool CreateDevice(retro_vulkan_context *, VkInstance, VkPhysicalDevice, VkSurfaceKHR, PFN_vkGetInstanceProcAddr,
                  const char **, unsigned, const char **, unsigned, const VkPhysicalDeviceFeatures *) {
    NegotiationLog(RETRO_LOG_INFO, "Vulkan negotiation v1: using the frontend's default device");
    return false;
}

// Never refuses for capability reasons: a frontend left without a device after its retry may run with no GPU at all
// (RetroArch). Without a GPU that meets the baseline, the device goes on the frontend's GPU (or the first that can
// present) with what it supports, and context_reset falls back to the copy-only output with the reason logged.
bool CreateDevice2(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
                   PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                   retro_vulkan_create_device_wrapper_t createDeviceWrapper, void *opaque) {
    g_negotiation.device = VK_NULL_HANDLE;
    g_negotiation.baseline = false;
    InstanceFunctions fn;
    if (!context || instance == VK_NULL_HANDLE || !getInstanceProcAddr || !createDeviceWrapper ||
        !fn.Load(instance, getInstanceProcAddr)) {
        NegotiationLog(RETRO_LOG_ERROR, "Vulkan create_device2: unusable arguments from the frontend");
        return false;
    }

    std::vector<VkPhysicalDevice> candidates;
    if (gpu != VK_NULL_HANDLE) {
        candidates.push_back(gpu);
    } else {
        uint32_t count = 0;
        fn.enumeratePhysicalDevices(instance, &count, nullptr);
        candidates.resize(count);
        fn.enumeratePhysicalDevices(instance, &count, candidates.data());
        candidates.resize(count);
    }

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t family = 0;
    bool baseline = false;
    std::string rejections;
    for (VkPhysicalDevice candidate : candidates) {
        std::string rejection;
        if (const auto found = EvaluateDevice(fn, candidate, surface, rejection)) {
            chosen = candidate;
            family = *found;
            baseline = true;
            break;
        }
        rejections += (rejections.empty() ? "" : "; ") + rejection;
    }
    if (chosen == VK_NULL_HANDLE) {
        for (VkPhysicalDevice candidate : candidates) {
            auto found = FindQueueFamily(fn, candidate, surface, true);
            if (!found) {
                found = FindQueueFamily(fn, candidate, surface, false);
            }
            if (found) {
                chosen = candidate;
                family = *found;
                break;
            }
        }
        if (chosen == VK_NULL_HANDLE) {
            NegotiationLog(RETRO_LOG_ERROR, "Vulkan create_device2: no GPU with a graphics queue that can present (" +
                                                (rejections.empty() ? std::string("none offered") : rejections) + ")");
            return false;
        }
        VkPhysicalDeviceProperties properties{};
        fn.getProperties(chosen, &properties);
        NegotiationLog(RETRO_LOG_WARN, "Vulkan create_device2: no GPU meets the GPU renderer's baseline (" + rejections +
                                           "); creating the device on " + properties.deviceName +
                                           " for the copy-only frame output");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // Vulkan 1.0 features through pEnabledFeatures; any 1.1+ feature goes into pNext as its own per-feature struct
    // (never VkPhysicalDeviceVulkan11/12Features: RetroArch's wrapper may add per-feature structs of its own). A
    // device below the baseline gets only what it supports.
    VkPhysicalDeviceFeatures supported{};
    fn.getFeatures(chosen, &supported);
    VkPhysicalDeviceFeatures features{};
    for (const gpu::CoreFeature &feature : gpu::kRequiredCoreFeatures) {
        features.*feature.member = supported.*feature.member;
    }
    const std::vector<VkExtensionProperties> available = DeviceExtensions(fn, chosen);
    std::vector<const char *> extensions;
    for (const char *required : gpu::kRequiredDeviceExtensions) {
        if (HasExtension(available, required)) {
            extensions.push_back(required);
        }
    }

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    info.pEnabledFeatures = &features;

    const VkDevice device = createDeviceWrapper(chosen, opaque, &info);
    if (device == VK_NULL_HANDLE) {
        NegotiationLog(RETRO_LOG_WARN, "Vulkan create_device2: the frontend failed to create the device");
        return false;
    }
    const auto getDeviceQueue =
        reinterpret_cast<PFN_vkGetDeviceQueue>(fn.getDeviceProcAddr(device, "vkGetDeviceQueue"));
    VkQueue queue = VK_NULL_HANDLE;
    if (getDeviceQueue) {
        getDeviceQueue(device, family, 0, &queue);
    }
    if (queue == VK_NULL_HANDLE) {
        // Returning false hands the frontend no device, so this one is ours to destroy.
        NegotiationLog(RETRO_LOG_ERROR, "Vulkan create_device2: no queue on the new device");
        if (const auto destroyDevice =
                reinterpret_cast<PFN_vkDestroyDevice>(fn.getDeviceProcAddr(device, "vkDestroyDevice"))) {
            destroyDevice(device, nullptr);
        }
        return false;
    }

    context->gpu = chosen;
    context->device = device;
    context->queue = queue;
    context->queue_family_index = family;
    context->presentation_queue = queue;
    context->presentation_queue_family_index = family;
    g_negotiation.device = device;
    g_negotiation.baseline = baseline;
    return true;
}

// Frees nothing: the device is the frontend's and every resource on it is gone after context_destroy.
void DestroyDevice() {
    g_negotiation.device = VK_NULL_HANDLE;
}

retro_hw_render_context_negotiation_interface_vulkan g_negotiationInterface{
    .interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
    .interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
    .get_application_info = GetApplicationInfo,
    .create_device = CreateDevice,
    .destroy_device = DestroyDevice,
    .create_instance = nullptr,
    .create_device2 = CreateDevice2,
};

retro_hw_render_callback g_hwRender{};

// Slots are sync indices: index i needs i + 1 slots.
uint32_t SlotCountForMask(uint32_t mask) {
    return mask == 0 ? 1u : 32u - static_cast<uint32_t>(std::countl_zero(mask));
}

} // namespace

VulkanRequest RequestVulkanContext(retro_environment_t environment, retro_hw_context_reset_t contextReset,
                                   retro_hw_context_reset_t contextDestroy, retro_log_printf_t log,
                                   std::string &reason) {
    g_log = log;
    g_negotiation = {};
    if (!environment) {
        reason = "no environment callback";
        return VulkanRequest::Refused;
    }

    // Never make the frontend switch video drivers: RetroArch accepts SET_HW_RENDER(VULKAN) on a gl/d3d driver
    // when driver switching is allowed, and a Vulkan driver that then fails to start ends the session. Only when
    // the frontend cannot say which driver it runs is SET_HW_RENDER tried (it refuses what it cannot provide).
    unsigned preferred = RETRO_HW_CONTEXT_NONE;
    if (environment(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred) && preferred != RETRO_HW_CONTEXT_VULKAN) {
        reason = "the frontend's video driver is not Vulkan (preferred context " + std::to_string(preferred) + ")";
        return VulkanRequest::NotVulkanDriver;
    }

    g_hwRender = {};
    g_hwRender.context_type = RETRO_HW_CONTEXT_VULKAN;
    g_hwRender.context_reset = contextReset;
    g_hwRender.context_destroy = contextDestroy;
    g_hwRender.version_major = VK_API_VERSION_MAJOR(gpu::kRequiredApiVersion);
    g_hwRender.version_minor = VK_API_VERSION_MINOR(gpu::kRequiredApiVersion);
    g_hwRender.cache_context = false;
    g_hwRender.bottom_left_origin = false;
    if (!environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hwRender)) {
        reason = "the frontend refused a Vulkan context (SET_HW_RENDER); use the vulkan video driver";
        return VulkanRequest::Refused;
    }

    // A frontend without negotiation v2 rejects a v2 struct: offer v1 (application info only) then.
    retro_hw_render_context_negotiation_interface support{};
    support.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
    support.interface_version = 0;
    environment(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &support);
    g_negotiationInterface.interface_version =
        support.interface_version >= 2 ? RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION : 1;
    if (!environment(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &g_negotiationInterface)) {
        // Not fatal: context_reset checks whatever device the frontend picks.
        NegotiationLog(RETRO_LOG_WARN, "the frontend did not take the Vulkan context negotiation interface");
    }
    return VulkanRequest::Accepted;
}

// ---------------------------------------------------------------------------------------------------------------

struct VulkanOutput::Impl {
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false; // submitted, fence not waited yet
        // Copy-only mode: the slot's output image and the staging buffer the CPU frame is uploaded from.
        gpu::vk_util::Image copyImage;
        gpu::vk_util::Buffer staging;
        // Copy-only mode. Persistent: the frontend keeps the pointer from set_image (and dereferences it for dupes).
        retro_vulkan_image image{};
    };

    // A replaced compute presenter and the image structs it handed over (the frontend keeps those pointers, also for
    // dupes, and reads the images on its own schedule). Kept until the frontend is done with every image it got: for
    // each sync index in `awaiting` (the ones its images were handed at) wait_sync_index and this output's fence of
    // that slot have returned since the retirement, and none of its images is the frontend's current one
    // (libretro_vulkan.h: an image must not be destroyed before wait_sync_index for the sync index it was handed at).
    // A retired image that is the frontend's current one is repeated by it, possibly in frames at later sync indices
    // (after a failed record, present or submit): when it stops being current, `awaiting` becomes every sync index.
    struct RetiredPresenter {
        std::unique_ptr<gpu::FramePresenter> presenter;
        std::unique_ptr<retro_vulkan_image[]> images;
        uint32_t count = 0;
        uint32_t awaiting = 0;
        // One of its images is (or was, until the last ReleaseRetired) the frontend's current image: while it is, the
        // frontend may repeat it at any later sync index, so `awaiting` is only (re)set once it stops being current.
        bool held = false;
    };

    // A renderer frame context for frames with video disabled (context index slots.size() + i).
    struct HiddenContext {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false; // submitted, fence not waited yet
    };

    const retro_hw_render_interface_vulkan *iface = nullptr;
    std::unique_ptr<gpu::VulkanDevice> device;
    VulkanOutputMode mode = VulkanOutputMode::None;
    std::unique_ptr<gpu::FramePresenter> presenter; // compute mode
    // Compute mode: the image struct handed over for each slot (one per presenter, never changed in place), and the
    // sync indices they were handed at (bit i).
    std::unique_ptr<retro_vulkan_image[]> presenterImages;
    uint32_t presenterHanded = 0;
    std::vector<RetiredPresenter> retiredPresenters;
    // The image struct the frontend holds (the last set_image), null after a withdrawal.
    const retro_vulkan_image *currentImage = nullptr;
    std::vector<Slot> slots;

    // The attached Vulkan VDP renderer (compute mode only), its hidden-frame contexts (created at the first attach)
    // and whether its composite holds a frame (a fresh back half has none until its first submitted record).
    gpu::VulkanVDPRenderer *renderer = nullptr;
    std::array<HiddenContext, kHiddenContexts> hidden{};
    bool hiddenCreated = false;
    uint32_t nextHidden = 0;
    bool compositeValid = false;
    // VDP1 on the GPU: submits of VDP1 ops without a VDP2 frame (SubmitHidden), since the output was created
    uint64_t vdp1OpsOnlySubmits = 0;
    // ... the renderer's sync submits: their command buffer and fence (created with the hidden contexts), whether GPU
    // work is allowed now (SetGpuWorkAllowed) and the refused ones.
    HiddenContext sync{};
    bool gpuWorkAllowed = false;
    uint64_t refusedSyncSubmits = 0;
    // This retro_run (BeginRun): a frame or ops-only submit was accepted, or refused.
    bool runSubmitted = false;
    bool runRefused = false;
    bool opsDeferredLogged = false;

    // Internal resolution: requested (brimir_internal_resolution), whether a change waits for the next shown frame with
    // a VDP2 frame, the scale of the presenter's images (CreateSlots), and of the last frame handed over.
    uint32_t requestedScale = 1;
    bool scaleChangePending = false;
    uint32_t presenterScale = 1;
    uint32_t presentedScale = 1;

    VkResult CreateSlots(uint32_t count, VulkanOutputMode wanted);
    // The image structs of the current presenter's slots.
    void FillPresenterImages();
    // A new presenter at `scale` (images of 704 * scale) for the current slots, or null with error set. The current
    // one is untouched.
    std::unique_ptr<gpu::FramePresenter> CreatePresenter(uint32_t scale, std::string &error);
    // Makes `created` the current presenter; the previous one is retired (RetiredPresenter), not destroyed.
    void InstallPresenter(std::unique_ptr<gpu::FramePresenter> created);
    // A sync index the frontend is done with (after wait_sync_index and the slot's fence): frees the retired presenters
    // that waited only for it (pass UINT32_MAX to only re-check after a set_image).
    void ReleaseRetired(uint32_t syncIndex);
    void DestroyRetired();
    // Applies requestedScale to the attached renderer and the presenter (see VulkanOutput::SetInternalScale): the
    // presenter's images first, then the renderer; either failing leaves both at the previous scale (logged). The
    // renderer's scale never exceeds the presenter's. False with error when a wait failed (nothing changed, the change
    // stays pending).
    bool ApplyInternalScale(std::string &error);
    void DestroySlots();
    VkResult WaitSlots();
    VkResult CreateCommandContext(VkCommandPool &pool, VkCommandBuffer &cmd, VkFence &fence);
    VkResult CreateHidden();
    void DestroyHidden();
    VkResult WaitOwnWork();
    VkResult DrainQueue();
    // The renderer's sync submit (VulkanVDPRenderer::SetSyncSubmit).
    bool SyncSubmit(const gpu::VulkanVDPRenderer::SyncRecord &record);
    bool AttachRendererTo(gpu::VulkanVDPRenderer &target, std::string &error);
    void DetachRendererIdle();
    void Withdraw();
    void Teardown();
    void Abandon();
    bool EnsureSlots(std::string &error);
    Slot *BeginFrame(uint32_t &index, std::string &error);
    bool SubmitFrame(Slot &slot, uint32_t index, std::string &error);
};

// The copy-only output: B8G8R8A8_UNORM has the byte order of XRGB8888, so the bridge's frame is copied as is; the
// view reads alpha as one (the X byte is 0).
constexpr VkFormat kCopyFormat = VK_FORMAT_B8G8R8A8_UNORM;
// Native frames only (the copy-only output never upscales), independent of the internal resolution.
constexpr uint32_t kCopyMaxSize = 704;
static_assert(kCopyMaxSize == gpu::FramePresenter::kMaxOutputSize);

// A transient command pool with one primary command buffer, and an unsignalled fence.
VkResult VulkanOutput::Impl::CreateCommandContext(VkCommandPool &pool, VkCommandBuffer &cmd, VkFence &fence) {
    const auto &vk = device->Table();
    const VkDevice dev = device->Device();
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device->QueueFamily();
    VkResult result = vk.vkCreateCommandPool(dev, &poolInfo, nullptr, &pool);
    if (result != VK_SUCCESS) {
        return result;
    }
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    result = vk.vkAllocateCommandBuffers(dev, &allocInfo, &cmd);
    if (result != VK_SUCCESS) {
        return result;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return vk.vkCreateFence(dev, &fenceInfo, nullptr, &fence);
}

VkResult VulkanOutput::Impl::CreateSlots(uint32_t count, VulkanOutputMode wanted) {
    const auto &vk = device->Table();
    const VkDevice dev = device->Device();
    if (wanted == VulkanOutputMode::Compute) {
        presenter = std::make_unique<gpu::FramePresenter>(*device, count, presenterScale);
        if (!presenter->Ok()) {
            return presenter->Result();
        }
    }
    slots.resize(count);
    if (wanted == VulkanOutputMode::Compute) {
        FillPresenterImages();
    }
    for (uint32_t i = 0; i < count; ++i) {
        Slot &slot = slots[i];
        VkResult result = CreateCommandContext(slot.pool, slot.cmd, slot.fence);
        if (result != VK_SUCCESS) {
            return result;
        }

        VkImageViewCreateInfo &view = slot.image.create_info;
        view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (wanted == VulkanOutputMode::Compute) {
            // The presenter's image structs (FillPresenterImages) are handed over instead.
        } else {
            // Vulkan 1.0 transfer operations only: a staging buffer and an image the frontend samples.
            result = gpu::vk_util::CreateBuffer(*device, VkDeviceSize{kCopyMaxSize} * kCopyMaxSize * 4,
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                slot.staging);
            if (result == VK_SUCCESS && slot.staging.mapped == nullptr) {
                result = VK_ERROR_MEMORY_MAP_FAILED;
            }
            if (result == VK_SUCCESS) {
                const gpu::vk_util::ImageDesc desc{
                    .width = kCopyMaxSize,
                    .height = kCopyMaxSize,
                    .format = kCopyFormat,
                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                    .createView = false,
                };
                result = gpu::vk_util::CreateImage2D(*device, desc, slot.copyImage);
            }
            if (result == VK_SUCCESS) {
                view.image = slot.copyImage.image;
                view.format = kCopyFormat;
                view.components.a = VK_COMPONENT_SWIZZLE_ONE;
                result = vk.vkCreateImageView(dev, &view, nullptr, &slot.copyImage.view);
            }
            if (result != VK_SUCCESS) {
                return result;
            }
            slot.image.image_view = slot.copyImage.view;
        }
        slot.image.image_layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    mode = wanted;
    return VK_SUCCESS;
}

void VulkanOutput::Impl::FillPresenterImages() {
    const uint32_t count = presenter->SlotCount();
    presenterImages = std::make_unique<retro_vulkan_image[]>(count);
    presenterHanded = 0;
    for (uint32_t i = 0; i < count; ++i) {
        retro_vulkan_image &image = presenterImages[i];
        VkImageViewCreateInfo &view = image.create_info;
        view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        view.image = presenter->OutputImage(i);
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.image_view = presenter->OutputView(i);
        image.image_layout = VK_IMAGE_LAYOUT_GENERAL;
    }
}

std::unique_ptr<gpu::FramePresenter> VulkanOutput::Impl::CreatePresenter(uint32_t scale, std::string &error) {
    auto created = std::make_unique<gpu::FramePresenter>(*device, static_cast<uint32_t>(slots.size()), scale);
    if (!created->Ok()) {
        error = std::string("creating the ") + std::to_string(scale * gpu::FramePresenter::kMaxOutputSize) +
                " px output images failed: " + gpu::vk_util::ResultName(created->Result());
        return nullptr;
    }
    return created;
}

void VulkanOutput::Impl::InstallPresenter(std::unique_ptr<gpu::FramePresenter> created) {
    RetiredPresenter old;
    old.presenter = std::move(presenter);
    old.images = std::move(presenterImages);
    old.count = old.presenter ? old.presenter->SlotCount() : 0;
    old.awaiting = presenterHanded;
    presenter = std::move(created);
    presenterScale = presenter->Scale();
    FillPresenterImages();
    if (old.presenter) {
        retiredPresenters.push_back(std::move(old));
        ReleaseRetired(UINT32_MAX); // one never handed over goes now
    }
}

void VulkanOutput::Impl::ReleaseRetired(uint32_t syncIndex) {
    for (auto it = retiredPresenters.begin(); it != retiredPresenters.end();) {
        bool current = false;
        for (uint32_t i = 0; i < it->count; ++i) {
            current = current || currentImage == &it->images[i];
        }
        if (current) {
            // The frontend repeats this image (a failed record, present or submit after the change) at whatever sync
            // indices come next, so no wait counts while it is current.
            it->held = true;
        } else if (it->held) {
            // It stopped being current (the next set_image, or a withdrawal): its last repeat may have been at any sync
            // index, so it goes only after a full ring of waits from now on. (The wait that returned in this call, if
            // any, is not counted.)
            it->held = false;
            it->awaiting = slots.size() >= 32 ? UINT32_MAX : (1u << slots.size()) - 1;
        } else if (syncIndex < 32) {
            it->awaiting &= ~(1u << syncIndex);
        }
        if (it->awaiting == 0 && !current) {
            it = retiredPresenters.erase(it); // destroys the presenter (its images and views)
        } else {
            ++it;
        }
    }
}

void VulkanOutput::Impl::DestroyRetired() {
    retiredPresenters.clear();
}

VkResult VulkanOutput::Impl::WaitSlots() {
    const auto &vk = device->Table();
    for (Slot &slot : slots) {
        if (slot.pending) {
            const VkResult result = vk.vkWaitForFences(device->Device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS) {
                return result;
            }
            slot.pending = false;
        }
    }
    return VK_SUCCESS;
}

VkResult VulkanOutput::Impl::CreateHidden() {
    if (hiddenCreated) {
        return VK_SUCCESS;
    }
    for (HiddenContext &context : hidden) {
        if (const VkResult result = CreateCommandContext(context.pool, context.cmd, context.fence);
            result != VK_SUCCESS) {
            DestroyHidden(); // frees whatever was created; hiddenCreated stays false
            return result;
        }
    }
    if (const VkResult result = CreateCommandContext(sync.pool, sync.cmd, sync.fence); result != VK_SUCCESS) {
        DestroyHidden();
        return result;
    }
    hiddenCreated = true;
    nextHidden = 0;
    return VK_SUCCESS;
}

// Destroys every hidden-context handle that exists (null handles are skipped by Vulkan).
void VulkanOutput::Impl::DestroyHidden() {
    if (device) {
        const auto &vk = device->Table();
        for (HiddenContext &context : hidden) {
            vk.vkDestroyFence(device->Device(), context.fence, nullptr);
            vk.vkDestroyCommandPool(device->Device(), context.pool, nullptr);
        }
        vk.vkDestroyFence(device->Device(), sync.fence, nullptr);
        vk.vkDestroyCommandPool(device->Device(), sync.pool, nullptr);
    }
    hidden = {};
    sync = {};
    hiddenCreated = false;
    nextHidden = 0;
}

// Waits for every submission of this output (slots and hidden contexts). The frontend's own work is not included:
// it only samples the slots' output images, never the renderer's resources.
VkResult VulkanOutput::Impl::WaitOwnWork() {
    if (const VkResult result = WaitSlots(); result != VK_SUCCESS) {
        return result;
    }
    const auto &vk = device->Table();
    for (HiddenContext &context : hidden) {
        if (context.pending) {
            const VkResult result = vk.vkWaitForFences(device->Device(), 1, &context.fence, VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS) {
                return result;
            }
            context.pending = false;
        }
    }
    if (renderer) {
        renderer->OwnWorkCompleted(); // the fences may be destroyed or reset from here on
    }
    return VK_SUCCESS;
}

// The renderer's sync submit: a command buffer and fence of their own, the queue lock held only around vkQueueSubmit,
// then a wait for the fence (the only CPU wait VDP1 on the GPU adds inside retro_run; refused elsewhere).
bool VulkanOutput::Impl::SyncSubmit(const gpu::VulkanVDPRenderer::SyncRecord &record) {
    if (!gpuWorkAllowed || !device || !hiddenCreated) {
        ++refusedSyncSubmits;
        return false;
    }
    const auto &vk = device->Table();
    const VkDevice dev = device->Device();
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk.vkResetCommandPool(dev, sync.pool, 0) != VK_SUCCESS || vk.vkBeginCommandBuffer(sync.cmd, &begin) != VK_SUCCESS) {
        ++refusedSyncSubmits;
        return false;
    }
    if (!record(sync.cmd)) {
        vk.vkEndCommandBuffer(sync.cmd);
        return false;
    }
    VkResult result = vk.vkEndCommandBuffer(sync.cmd);
    if (result == VK_SUCCESS) {
        result = vk.vkResetFences(dev, 1, &sync.fence);
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &sync.cmd;
        result = device->Submit(submit, sync.fence);
    }
    if (result != VK_SUCCESS) {
        ++refusedSyncSubmits;
        return false;
    }
    if (vk.vkWaitForFences(dev, 1, &sync.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        NegotiationLog(RETRO_LOG_ERROR, "VDP1 on the GPU: waiting for a sync submit failed");
        return false;
    }
    return true;
}

// Attaches `target` with the current slot count; the caller has made sure no submission uses a previous back half.
bool VulkanOutput::Impl::AttachRendererTo(gpu::VulkanVDPRenderer &target, std::string &error) {
    renderer = nullptr;
    compositeValid = false;
    if (const VkResult result = CreateHidden(); result != VK_SUCCESS) {
        error = std::string("creating the hidden-frame command buffers failed: ") + gpu::vk_util::ResultName(result);
        return false;
    }
    // The output images first, at the requested internal resolution: the renderer is then attached at most at their
    // scale, so it never renders more than can be presented. The renderer tries the request, then its previous
    // effective scale, then 1, each allocated directly (fallbacks logged by the renderer).
    target.SetInternalScale(requestedScale);
    if (presenter->Scale() < requestedScale) {
        std::string presenterError;
        if (auto created = CreatePresenter(requestedScale, presenterError)) {
            InstallPresenter(std::move(created));
        } else {
            NegotiationLog(RETRO_LOG_WARN, "Internal resolution " + std::to_string(requestedScale) + "x: " +
                                               presenterError + "; at most " + std::to_string(presenter->Scale()) +
                                               "x");
        }
    }
    target.SetSyncSubmit([this](const gpu::VulkanVDPRenderer::SyncRecord &record) { return SyncSubmit(record); });
    target.AttachDevice(*device, static_cast<uint32_t>(slots.size()) + kHiddenContexts, presenter->Scale());
    if (!target.Attached()) {
        target.SetSyncSubmit(nullptr);
        error = "creating the VDP2 GPU resources failed";
        if (presenter->Scale() > 1) {
            // The software renderer's frames are native: no N x images for them (kept if 1x cannot be created).
            std::string presenterError;
            if (auto created = CreatePresenter(1, presenterError)) {
                InstallPresenter(std::move(created));
            }
        }
        return false;
    }
    renderer = &target;
    scaleChangePending = false;
    if (presenter->Scale() > renderer->InternalScale()) {
        // The renderer fell back: smaller output images (if they cannot be created the larger ones still present it).
        std::string presenterError;
        if (auto created = CreatePresenter(renderer->InternalScale(), presenterError)) {
            InstallPresenter(std::move(created));
        }
    }
    return true;
}

bool VulkanOutput::Impl::ApplyInternalScale(std::string &error) {
    const uint32_t target = requestedScale;
    const uint32_t before = renderer->InternalScale();
    if (before == target && presenter->Scale() == target) {
        scaleChangePending = false;
        return true;
    }
    // SetInternalScale's contract: no submitted work of this output uses the renderer's resources (and no command
    // buffer is recorded yet in this run). The frontend's use of the output images needs no wait: the old presenter
    // is retired, not destroyed (InstallPresenter). A failed wait keeps the change pending for the next frame.
    if (const VkResult result = WaitOwnWork(); result != VK_SUCCESS) {
        error = std::string("waiting for the frame fences failed: ") + gpu::vk_util::ResultName(result);
        return false;
    }
    scaleChangePending = false;
    // 1. Output images of the new scale, before anything changes: without them the renderer stays where it is.
    std::unique_ptr<gpu::FramePresenter> grown;
    if (presenter->Scale() < target) {
        std::string presenterError;
        grown = CreatePresenter(target, presenterError);
        if (!grown) {
            NegotiationLog(RETRO_LOG_WARN, "Internal resolution " + std::to_string(target) + "x: " + presenterError +
                                               "; staying at " + std::to_string(before) + "x");
            return true;
        }
    }
    // 2. The renderer (its new resources before its old ones go; a failure keeps the previous scale, logged).
    if (before != target && !renderer->SetInternalScale(target)) {
        NegotiationLog(RETRO_LOG_WARN, "Internal resolution " + std::to_string(target) +
                                           "x could not be allocated; staying at " + std::to_string(before) + "x");
    }
    const uint32_t effective = renderer->InternalScale();
    // 3. The presenter follows: the grown one when the renderer got there (otherwise it is dropped, never handed
    // over), or smaller images when the scale went down (a failure keeps the larger ones, which still present it).
    if (grown && grown->Scale() == effective) {
        InstallPresenter(std::move(grown));
    } else if (presenter->Scale() > effective) {
        std::string presenterError;
        if (auto created = CreatePresenter(effective, presenterError)) {
            InstallPresenter(std::move(created));
        }
    }
    if (effective != before) {
        // The composite is rescaled by this run's record; until that succeeds there is nothing to re-present (the
        // frontend repeats its current image, which stays alive: InstallPresenter retires it).
        compositeValid = false;
    }
    return true;
}

// Detaches the renderer; the device is idle or this output's submissions have completed.
void VulkanOutput::Impl::DetachRendererIdle() {
    if (renderer) {
        const bool gpuVdp1 = renderer->GpuVdp1();
        renderer->DetachDevice(); // with VDP1 on the GPU: its sync submit (counted below as a detach)
        if (gpuVdp1) {
            // Diagnostics (brimir_vkhost checks): the submits and waits VDP1 on the GPU added since the renderer was
            // created, by kind, with their times.
            using Kind = gpu::VulkanVDPRenderer::Vdp1SyncKind;
            const gpu::VulkanVDPRenderer::Vdp1SyncStats &s = renderer->Vdp1Sync();
            const auto count = [](uint64_t n) { return std::to_string(n); };
            const auto at = [](const auto &a, Kind k) { return a[static_cast<uint32_t>(k)]; };
            const auto ms = [](double seconds) {
                char text[32];
                std::snprintf(text, sizeof(text), "%.3f", seconds * 1e3);
                return std::string(text);
            };
            NegotiationLog(RETRO_LOG_INFO,
                           "VDP1 on the GPU: " + count(vdp1OpsOnlySubmits) + " ops-only submits, " +
                               count(renderer->Vdp1SyncSubmits()) + " sync submits (reads " +
                               count(at(s.syncSubmits, Kind::Read)) + ", saves " + count(at(s.syncSubmits, Kind::Save)) +
                               ", detaches " + count(at(s.syncSubmits, Kind::Detach)) + ", dumps " +
                               count(at(s.syncSubmits, Kind::Dump)) + "; " + ms(s.syncSeconds) + " ms, max " +
                               ms(s.maxSyncSeconds) + " ms), fence waits (reads " +
                               count(at(s.fenceWaits, Kind::Read)) + ", saves " + count(at(s.fenceWaits, Kind::Save)) +
                               "; " + ms(s.waitSeconds) + " ms, max " + ms(s.maxWaitSeconds) + " ms), refused " +
                               count(s.refusedSyncs) + " (output " + count(refusedSyncSubmits) + "), readback slices " +
                               count(s.readbackSlices) + ", merged " + count(s.mergedSlices) + " (" +
                               ms(s.mergeSeconds) + " ms)");
        }
        renderer->SetSyncSubmit(nullptr);
        renderer = nullptr;
    }
    compositeValid = false;
}

// Waits until everything submitted to the frontend's queue so far (its own frames, which may still sample our slots,
// included) has completed: an empty submit with a fence, in submission order after all earlier work. The queue lock
// is held only around the submit, never while waiting (the equivalent of vkQueueWaitIdle without a wait under
// lock_queue, which the libretro Vulkan contract forbids).
VkResult VulkanOutput::Impl::DrainQueue() {
    const auto &vk = device->Table();
    const VkDevice dev = device->Device();
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VkResult result = vk.vkCreateFence(dev, &fenceInfo, nullptr, &fence);
    if (result != VK_SUCCESS) {
        return result;
    }
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    result = device->Submit(submit, fence);
    if (result == VK_SUCCESS) {
        result = vk.vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vk.vkDestroyFence(dev, fence, nullptr);
    return result;
}

void VulkanOutput::Impl::DestroySlots() {
    if (device) {
        const auto &vk = device->Table();
        const VkDevice dev = device->Device();
        for (Slot &slot : slots) {
            vk.vkDestroyFence(dev, slot.fence, nullptr);
            vk.vkDestroyCommandPool(dev, slot.pool, nullptr); // frees the command buffer
            gpu::vk_util::DestroyImage(*device, slot.copyImage);
            gpu::vk_util::DestroyBuffer(*device, slot.staging);
        }
    }
    slots.clear();
    // Every caller has withdrawn the image and waited for the device (or the frontend's queue) first.
    DestroyRetired();
    presenter.reset();
    presenterImages.reset();
    presenterHanded = 0;
    mode = VulkanOutputMode::None;
}

// Takes the image back from the frontend (it keeps the last set_image pointer for dupes) before the slots go away.
void VulkanOutput::Impl::Withdraw() {
    if (iface) {
        iface->set_image(iface->handle, nullptr, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
    }
    currentImage = nullptr;
}

void VulkanOutput::Impl::Teardown() {
    Withdraw();
    if (device) {
        // Allowed in context_destroy: the frontend is idle and holds no reference to the images any more.
        device->Table().vkDeviceWaitIdle(device->Device());
        for (Slot &slot : slots) {
            slot.pending = false;
        }
        for (HiddenContext &context : hidden) {
            context.pending = false;
        }
    }
    // Wait idle, renderer, slots, device: the renderer's pending frame (possibly ended by VDP2EndFrame but not
    // recorded yet) is dropped, and the next frame after an attach uploads everything again.
    DetachRendererIdle();
    DestroySlots();
    DestroyHidden();
    device.reset();
    iface = nullptr;
    presenterScale = 1;
}

void VulkanOutput::Impl::Abandon() {
    if (renderer) {
        renderer->AbandonDevice();
        renderer->SetSyncSubmit(nullptr);
        renderer = nullptr;
    }
    compositeValid = false;
    hidden = {};
    sync = {};
    hiddenCreated = false;
    slots.clear();
    (void)presenter.release();
    (void)presenterImages.release();
    for (RetiredPresenter &retired : retiredPresenters) {
        (void)retired.presenter.release();
        (void)retired.images.release();
    }
    retiredPresenters.clear();
    presenterHanded = 0;
    currentImage = nullptr;
    (void)device.release();
    mode = VulkanOutputMode::None;
    iface = nullptr;
    presenterScale = 1;
}

// The frame prologue both modes share: slots sized for the current mask, the sync index, the frontend's and our own
// wait for the slot, and the slot's command buffer begun. Null (with error) when nothing can be recorded; the mode
// may have changed then (degraded to copy-only, or None when no output is possible any more).
// The mask may change (with the device idle); slots only grow so earlier indices stay valid. An attached renderer keeps
// its back half (the composite holds the other field when weaving) and only gains frame contexts: the hidden contexts
// move up to the new slot count, which is safe because all of this output's work has completed here. Called only for
// shown frames (BeginFrame), never in a frame with video disabled. False (with error) when the output is not usable as
// it was: the mode may have changed (copy-only, or None) and the renderer is then detached.
bool VulkanOutput::Impl::EnsureSlots(std::string &error) {
    const uint32_t wanted = SlotCountForMask(iface->get_sync_index_mask(iface->handle));
    if (wanted <= slots.size()) {
        return true;
    }
    const VulkanOutputMode current = mode;
    Withdraw(); // the frontend must not keep a pointer into the slots about to be freed
    if (const VkResult result = WaitOwnWork(); result != VK_SUCCESS) {
        error = std::string("waiting for the frame fences failed: ") + gpu::vk_util::ResultName(result);
        DetachRendererIdle();
        DestroySlots();
        return false;
    }
    // The frontend's own work that still samples the old images.
    if (const VkResult result = DrainQueue(); result != VK_SUCCESS) {
        // The slots may still be in use: leak them (and the output) rather than free them under the frontend.
        error = std::string("waiting for the frontend's queue failed: ") + gpu::vk_util::ResultName(result);
        Abandon();
        return false;
    }
    DestroySlots();
    VkResult result = CreateSlots(wanted, current);
    // Above 1x, smaller output images before the compute output is given up: the renderer falls back to their scale
    // (it never renders more than can be presented; its composite is rescaled by the next record).
    const uint32_t grownScale = presenterScale;
    while (result != VK_SUCCESS && current == VulkanOutputMode::Compute && presenterScale > 1) {
        DestroySlots();
        --presenterScale;
        result = CreateSlots(wanted, current);
    }
    if (result == VK_SUCCESS && presenterScale != grownScale) {
        bool fits = renderer == nullptr || renderer->InternalScale() <= presenterScale;
        for (uint32_t n = presenterScale; !fits && n >= 1; --n) {
            fits = renderer->FallBackInternalScale(n);
        }
        if (fits) {
            compositeValid = false;
            NegotiationLog(RETRO_LOG_WARN, "Internal resolution " + std::to_string(grownScale) +
                                               "x: recreating the output images for the new sync index mask failed; "
                                               "using " +
                                               std::to_string(renderer ? renderer->InternalScale() : presenterScale) +
                                               "x");
        } else {
            result = VK_ERROR_OUT_OF_DEVICE_MEMORY; // the renderer cannot get down to the output's scale
        }
    }
    if (result != VK_SUCCESS && current == VulkanOutputMode::Compute) {
        DestroySlots();
        DetachRendererIdle(); // the copy-only output carries no renderer
        if (CreateSlots(wanted, VulkanOutputMode::CopyOnly) == VK_SUCCESS) {
            error = std::string("recreating the compute frame output failed (") + gpu::vk_util::ResultName(result) +
                    "); continuing with copy-only uploads";
            return false;
        }
    }
    if (result != VK_SUCCESS) {
        error = std::string("recreating the frame output failed: ") + gpu::vk_util::ResultName(result);
        DetachRendererIdle();
        DestroySlots();
        return false;
    }
    if (renderer && !renderer->EnsureFrameContexts(static_cast<uint32_t>(slots.size()) + kHiddenContexts)) {
        error = "the Vulkan VDP renderer lost its device objects";
        DetachRendererIdle();
        return false;
    }
    return true;
}

VulkanOutput::Impl::Slot *VulkanOutput::Impl::BeginFrame(uint32_t &index, std::string &error) {
    const auto &vk = device->Table();
    const VkDevice dev = device->Device();

    if (!EnsureSlots(error)) {
        return nullptr;
    }
    index = iface->get_sync_index(iface->handle);
    if (index >= slots.size()) {
        error = "sync index " + std::to_string(index) + " outside the sync index mask";
        return nullptr;
    }

    // The frontend is done with this slot's image, and so is the GPU work submitted for it earlier.
    iface->wait_sync_index(iface->handle);
    Slot &slot = slots[index];
    if (slot.pending) {
        if (const VkResult result = vk.vkWaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX);
            result != VK_SUCCESS) {
            error = std::string("waiting for the frame fence failed: ") + gpu::vk_util::ResultName(result);
            return nullptr;
        }
        slot.pending = false;
    }
    // Retired output images handed at this sync index are free now (unless one is the frontend's current image).
    ReleaseRetired(index);

    if (const VkResult result = vk.vkResetCommandPool(dev, slot.pool, 0); result != VK_SUCCESS) {
        error = std::string("vkResetCommandPool failed: ") + gpu::vk_util::ResultName(result);
        return nullptr;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (const VkResult result = vk.vkBeginCommandBuffer(slot.cmd, &begin); result != VK_SUCCESS) {
        error = std::string("vkBeginCommandBuffer failed: ") + gpu::vk_util::ResultName(result);
        return nullptr;
    }
    return &slot;
}

// Ends the slot's command buffer, submits it (queue lock held only around vkQueueSubmit) and hands the slot's
// image over.
bool VulkanOutput::Impl::SubmitFrame(Slot &slot, uint32_t index, std::string &error) {
    const auto &vk = device->Table();
    if (const VkResult result = vk.vkEndCommandBuffer(slot.cmd); result != VK_SUCCESS) {
        error = std::string("vkEndCommandBuffer failed: ") + gpu::vk_util::ResultName(result);
        return false;
    }
    if (const VkResult result = vk.vkResetFences(device->Device(), 1, &slot.fence); result != VK_SUCCESS) {
        error = std::string("vkResetFences failed: ") + gpu::vk_util::ResultName(result);
        return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.cmd;
    if (const VkResult result = device->Submit(submit, slot.fence); result != VK_SUCCESS) {
        error = std::string("vkQueueSubmit failed: ") + gpu::vk_util::ResultName(result);
        runRefused = true;
        return false;
    }
    runSubmitted = true;
    slot.pending = true;
    const retro_vulkan_image *image = &slot.image;
    if (mode == VulkanOutputMode::Compute) {
        image = &presenterImages[index];
        presenterHanded |= 1u << index;
    }
    iface->set_image(iface->handle, image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
    currentImage = image;
    ReleaseRetired(UINT32_MAX); // a retired presenter that held the previous current image may go now
    return true;
}

VulkanOutput::VulkanOutput() : m_impl(std::make_unique<Impl>()) {}

VulkanOutput::~VulkanOutput() {
    // The owner calls ContextDestroy (context_destroy) or Abandon (deinit without one) first.
    if (m_impl->device) {
        m_impl->Abandon();
    }
}

VulkanOutputMode VulkanOutput::Mode() const {
    return m_impl->mode;
}

bool VulkanOutput::Live() const {
    return m_impl->mode != VulkanOutputMode::None;
}

void VulkanOutput::ContextDestroy() {
    m_impl->Teardown();
}

void VulkanOutput::Abandon() {
    m_impl->Abandon();
}

bool VulkanOutput::ContextReset(retro_environment_t environment, std::string &reason) {
    Impl &m = *m_impl;
    reason.clear();
    const retro_hw_render_interface *raw = nullptr;
    if (!environment || !environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &raw) || raw == nullptr) {
        reason = "the frontend provided no hardware render interface";
        return false;
    }
    // Later interface versions only append members.
    if (raw->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
        raw->interface_version < RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION) {
        reason = "unsupported hardware render interface (type " + std::to_string(static_cast<int>(raw->interface_type)) +
                 ", version " + std::to_string(raw->interface_version) + ")";
        return false;
    }
    const auto *iface = reinterpret_cast<const retro_hw_render_interface_vulkan *>(raw);

    // A reset without a destroy in between: tear the old context down first when it is on the same device; a
    // different device means the old one may already be gone, so it is only forgotten.
    if (m.device) {
        if (m.device->Device() == iface->device) {
            m.Teardown();
        } else {
            m.Abandon();
        }
    }

    if (!iface->get_instance_proc_addr || !iface->get_sync_index || !iface->get_sync_index_mask ||
        !iface->set_image || !iface->wait_sync_index || !iface->lock_queue || !iface->unlock_queue) {
        reason = "the hardware render interface lacks required callbacks";
        return false;
    }

    // The instance version: what get_application_info asked for, if the loader supports it. Without the
    // negotiation the frontend's instance may be older than the shaders need.
    uint32_t instanceVersion = VK_API_VERSION_1_0;
    if (g_negotiation.appInfoRequested) {
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            iface->get_instance_proc_addr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (enumerateVersion && enumerateVersion(&loaderVersion) != VK_SUCCESS) {
            loaderVersion = VK_API_VERSION_1_0;
        }
        instanceVersion = std::min(loaderVersion, gpu::kRequiredApiVersion);
    }

    gpu::ExternalDevice handles;
    handles.instance = iface->instance;
    handles.physical = iface->gpu;
    handles.device = iface->device;
    handles.queue = iface->queue;
    handles.queueFamily = iface->queue_index;
    handles.getInstanceProcAddr = iface->get_instance_proc_addr;
    handles.getDeviceProcAddr = iface->get_device_proc_addr;
    handles.apiVersion = instanceVersion;
    const bool negotiated = g_negotiation.device != VK_NULL_HANDLE && g_negotiation.device == iface->device;
    const bool baselineDevice = negotiated && g_negotiation.baseline;
    if (baselineDevice) {
        for (const gpu::CoreFeature &feature : gpu::kRequiredCoreFeatures) {
            handles.enabledFeatures.*feature.member = VK_TRUE;
        }
    }

    std::string wrapError;
    std::unique_ptr<gpu::VulkanDevice> device = gpu::VulkanDevice::WrapExternal(handles, &wrapError);
    if (!device) {
        reason = wrapError;
        return false;
    }
    device->SetQueueLockHooks([iface] { iface->lock_queue(iface->handle); },
                              [iface] { iface->unlock_queue(iface->handle); });
    m.device = std::move(device);
    m.iface = iface;

    // The compute frame output needs Vulkan 1.2, a compute queue and the baseline features and limits (gpu_version.hpp;
    // the limits are those of the VDP2 pipelines, so the GPU renderer uses the same check). Without them the frames
    // are still presented through Vulkan (the frontend ignores CPU frames once it runs a hardware context): the
    // bridge's converted frame is uploaded with plain Vulkan 1.0 copies.
    std::string computeIssue;
    uint32_t familyCount = 0;
    volk::vkGetPhysicalDeviceQueueFamilyProperties(m.device->Physical(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    volk::vkGetPhysicalDeviceQueueFamilyProperties(m.device->Physical(), &familyCount, families.data());
    if (m.device->Features().apiVersion < gpu::kRequiredApiVersion) {
        computeIssue = "Vulkan " + VersionString(m.device->Features().apiVersion) + " (" +
                       std::string(m.device->Properties().deviceName) +
                       (g_negotiation.appInfoRequested ? "" : ", instance version not negotiated") + ") < " +
                       VersionString(gpu::kRequiredApiVersion);
    } else if (iface->queue_index >= familyCount ||
               !(families[iface->queue_index].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        computeIssue = "the frontend's queue does not support compute";
    } else if (const std::string limit = gpu::UnmetLimit(m.device->Properties().limits); !limit.empty()) {
        computeIssue = std::string(m.device->Properties().deviceName) + ": device limit " + limit;
    } else if (!baselineDevice && (!gpu::kRequiredCoreFeatures.empty() || !gpu::kRequiredDeviceExtensions.empty())) {
        computeIssue = negotiated ? "the negotiated device lacks the required features"
                                  : "the frontend's device was not created through the negotiation interface, so the "
                                    "required features are not enabled";
    }

    const uint32_t slotCount = SlotCountForMask(iface->get_sync_index_mask(iface->handle));
    if (computeIssue.empty()) {
        // Native output images (a few MiB); the renderer's attach replaces them by images of its internal resolution.
        m.presenterScale = 1;
        if (const VkResult result = m.CreateSlots(slotCount, VulkanOutputMode::Compute); result != VK_SUCCESS) {
            computeIssue = std::string("creating the compute frame output failed: ") + gpu::vk_util::ResultName(result);
            m.DestroySlots();
        }
    }
    if (!computeIssue.empty()) {
        if (const VkResult result = m.CreateSlots(slotCount, VulkanOutputMode::CopyOnly); result != VK_SUCCESS) {
            reason = computeIssue + "; the copy-only output failed too: " + gpu::vk_util::ResultName(result);
            m.Teardown();
            return false;
        }
        reason = computeIssue;
    }
    return true;
}

bool VulkanOutput::Present(const uint32_t *xbgr, uint32_t w, uint32_t h, const VulkanFrameSettings &settings,
                           unsigned &outW, unsigned &outH, std::string &error) {
    Impl &m = *m_impl;
    if (m.mode != VulkanOutputMode::Compute) {
        error = "the compute frame output is not active";
        return false;
    }
    uint32_t index = 0;
    Impl::Slot *slot = m.BeginFrame(index, error);
    if (!slot) {
        return false;
    }
    const gpu::OutputSettings output{settings.cropH, settings.cropV, settings.rotation};
    if (!m.presenter->Present(index, xbgr, w, h, output, slot->cmd)) {
        error = "frame " + std::to_string(w) + "x" + std::to_string(h) + " cannot be presented";
        m.device->Table().vkEndCommandBuffer(slot->cmd);
        return false;
    }
    if (!m.SubmitFrame(*slot, index, error)) {
        return false;
    }
    m.presentedScale = 1;
    const gpu::OutputSize size = gpu::ComputeOutputSize(w, h, output);
    outW = size.width;
    outH = size.height;
    return true;
}

bool VulkanOutput::PresentCopy(const void *xrgb, uint32_t w, uint32_t h, size_t pitch, std::string &error) {
    Impl &m = *m_impl;
    if (m.mode != VulkanOutputMode::CopyOnly) {
        error = "the copy-only frame output is not active";
        return false;
    }
    if (xrgb == nullptr || w == 0 || h == 0 || w > kCopyMaxSize || h > kCopyMaxSize || pitch < size_t{w} * 4) {
        error = "frame " + std::to_string(w) + "x" + std::to_string(h) + " cannot be presented";
        return false;
    }
    uint32_t index = 0;
    Impl::Slot *slot = m.BeginFrame(index, error);
    if (!slot) {
        return false;
    }

    // Host-coherent: visible to the device at vkQueueSubmit. The frame plus a border column and row repeating its last
    // column and row (as the compute output, gpu::WrittenSize): a frontend sampling bilinearly at the right and
    // bottom edges never reads undefined texels.
    const gpu::OutputSize written = gpu::WrittenSize({w, h});
    const auto *src = static_cast<const uint8_t *>(xrgb);
    auto *dst = static_cast<uint8_t *>(slot->staging.mapped);
    const size_t rowBytes = size_t{w} * 4;
    const size_t stagingPitch = size_t{written.width} * 4;
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *row = dst + y * stagingPitch;
        std::memcpy(row, src + y * pitch, rowBytes);
        if (written.width > w) {
            std::memcpy(row + rowBytes, row + rowBytes - 4, 4);
        }
    }
    if (written.height > h) {
        std::memcpy(dst + h * stagingPitch, dst + (h - 1) * stagingPitch, stagingPitch);
    }

    // Earlier readers of the image (the frontend) finish before the copy; the old contents are not needed. The
    // image stays in GENERAL, which the frontend samples without a transition.
    const VkImage image = slot->copyImage.image;
    gpu::vk_util::ImageBarrier(*m.device, slot->cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {written.width, written.height, 1};
    m.device->Table().vkCmdCopyBufferToImage(slot->cmd, slot->staging.buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                             &region);
    gpu::vk_util::ImageBarrier(*m.device, slot->cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    if (!m.SubmitFrame(*slot, index, error)) {
        return false;
    }
    m.presentedScale = 1;
    return true;
}

bool VulkanOutput::AttachRenderer(gpu::VulkanVDPRenderer &renderer, std::string &error) {
    Impl &m = *m_impl;
    if (m.mode != VulkanOutputMode::Compute) {
        error = "the compute frame output is not active";
        return false;
    }
    if (m.renderer == &renderer && renderer.Attached()) {
        return true;
    }
    // Another renderer (or a back half of this one) may still be in use by submitted frames.
    if (m.renderer != nullptr || renderer.Attached()) {
        if (const VkResult result = m.WaitOwnWork(); result != VK_SUCCESS) {
            error = std::string("waiting for the frame fences failed: ") + gpu::vk_util::ResultName(result);
            return false;
        }
        m.DetachRendererIdle();
    }
    return m.AttachRendererTo(renderer, error);
}

bool VulkanOutput::DetachRenderer(std::string &error) {
    Impl &m = *m_impl;
    if (m.renderer == nullptr) {
        return true;
    }
    // A failed wait means a lost device, whose objects may be destroyed anyway.
    const VkResult result = m.WaitOwnWork();
    m.DetachRendererIdle();
    if (result != VK_SUCCESS) {
        error = std::string("waiting for the frame fences failed: ") + gpu::vk_util::ResultName(result);
        return false;
    }
    return true;
}

bool VulkanOutput::RendererAttached() const {
    return m_impl->renderer != nullptr && m_impl->renderer->Attached();
}

bool VulkanOutput::PresentRenderer(uint32_t w, uint32_t h, const VulkanFrameSettings &settings, unsigned &outW,
                                   unsigned &outH, std::string &error) {
    Impl &m = *m_impl;
    error.clear();
    if (m.mode != VulkanOutputMode::Compute || !RendererAttached()) {
        error = "the Vulkan VDP renderer is not attached";
        return false;
    }
    // A new internal resolution applies with a frame to record: the composite is rescaled by that record (a re-present
    // without one would show a composite nothing was written to yet). Before BeginFrame: nothing of this run is
    // recorded, so SetInternalScale may rewrite the renderer's descriptor sets.
    if (m.scaleChangePending && m.renderer->HasPendingFrame() && !m.ApplyInternalScale(error)) {
        return false;
    }
    uint32_t index = 0;
    Impl::Slot *slot = m.BeginFrame(index, error);
    if (!slot) {
        return false;
    }
    gpu::VulkanVDPRenderer *renderer = m.renderer;
    if (renderer == nullptr) {
        // BeginFrame re-created the slots and could not re-attach the renderer.
        m.device->Table().vkEndCommandBuffer(slot->cmd);
        error = "the Vulkan VDP renderer was detached";
        return false;
    }
    // This retro_run's VDP2 frame, if one ended; else the composite as it is (the last frame, at the last size). With
    // VDP1 on the GPU the pending VDP1 ops go in the same command buffer, around the frame (with or without one). A
    // frame a VDP1 sync submit already recorded is in the composite.
    if (renderer->TakeFrameRecordedBySync()) {
        m.compositeValid = true;
    }
    const bool recorded = renderer->RecordPendingFrame(index, slot->cmd);
    const bool work = recorded || renderer->RecordedVdp1Work();
    if (!recorded && !m.compositeValid) {
        // A fresh back half: nothing to show until its first frame (VDP1 ops recorded here wait for the ops-only
        // submit, SubmitHidden).
        m.device->Table().vkEndCommandBuffer(slot->cmd);
        if (work) {
            renderer->RecordedFrameDropped();
        }
        return false;
    }
    // The composite is at the renderer's internal resolution (a change is applied only together with a record).
    const gpu::OutputSettings output{settings.cropH, settings.cropV, settings.rotation, renderer->InternalScale()};
    if (!m.presenter->PresentDevice(index, renderer->CompositeBuffer(), w, h, output, slot->cmd)) {
        m.device->Table().vkEndCommandBuffer(slot->cmd);
        if (work) {
            renderer->RecordedFrameDropped();
        }
        error = "frame " + std::to_string(w) + "x" + std::to_string(h) + " cannot be presented";
        return false;
    }
    if (!m.SubmitFrame(*slot, index, error)) {
        if (work) {
            renderer->RecordedFrameDropped();
        }
        return false;
    }
    renderer->FrameSubmitted(index, slot->fence);
    if (recorded) {
        m.compositeValid = true;
    }
    m.presentedScale = output.scale;
    const gpu::OutputSize size = gpu::ComputeOutputSize(w, h, output);
    outW = size.width;
    outH = size.height;
    return true;
}

void VulkanOutput::SetInternalScale(uint32_t n) {
    Impl &m = *m_impl;
    n = std::clamp<uint32_t>(n, 1, gpu::FramePresenter::kMaxScale);
    if (n != m.requestedScale) {
        m.requestedScale = n;
        m.scaleChangePending = true;
    }
}

uint32_t VulkanOutput::InternalScale() const {
    return RendererAttached() ? m_impl->renderer->InternalScale() : 1;
}

uint32_t VulkanOutput::PresentedScale() const {
    return m_impl->presentedScale;
}

void VulkanOutput::BeginRun() {
    Impl &m = *m_impl;
    m.runSubmitted = false;
    m.runRefused = false;
    if (m.mode == VulkanOutputMode::Compute && RendererAttached()) {
        m.renderer->PollVdp1Readbacks();
    }
}

void VulkanOutput::SetGpuWorkAllowed(bool allowed) {
    m_impl->gpuWorkAllowed = allowed;
}

bool VulkanOutput::SubmitHidden(std::string &error) {
    Impl &m = *m_impl;
    if (m.mode != VulkanOutputMode::Compute || !RendererAttached()) {
        return true;
    }
    if (m.renderer->TakeFrameRecordedBySync()) {
        m.compositeValid = true;
    }
    const bool frame = m.renderer->HasPendingFrame();
    if (!frame && !m.renderer->HasPendingVdp1Work()) {
        return true;
    }
    if (!frame && m.runSubmitted) {
        // A frame submit of this retro_run was accepted but did not take every VDP1 op (a recording failure): never a
        // second accepted submit in one retro_run; the ops wait for the next one.
        if (!m.opsDeferredLogged) {
            m.opsDeferredLogged = true;
            NegotiationLog(RETRO_LOG_WARN, "VDP1 on the GPU: ops left after this retro_run's submit wait for the next");
        }
        return true;
    }
    if (!frame && m.runRefused) {
        NegotiationLog(RETRO_LOG_WARN, "VDP1 ops-only submit after a refused frame submit");
    }
    const auto &vk = m.device->Table();
    const VkDevice dev = m.device->Device();
    const uint32_t index = m.nextHidden;
    Impl::HiddenContext &context = m.hidden[index];
    // The previous-but-one hidden frame (the only CPU wait of a hidden frame).
    if (context.pending) {
        if (const VkResult result = vk.vkWaitForFences(dev, 1, &context.fence, VK_TRUE, UINT64_MAX);
            result != VK_SUCCESS) {
            error = std::string("waiting for a hidden frame fence failed: ") + gpu::vk_util::ResultName(result);
            return false;
        }
        context.pending = false;
    }
    if (const VkResult result = vk.vkResetCommandPool(dev, context.pool, 0); result != VK_SUCCESS) {
        error = std::string("vkResetCommandPool failed: ") + gpu::vk_util::ResultName(result);
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (const VkResult result = vk.vkBeginCommandBuffer(context.cmd, &begin); result != VK_SUCCESS) {
        error = std::string("vkBeginCommandBuffer failed: ") + gpu::vk_util::ResultName(result);
        return false;
    }
    const uint32_t rendererContext = static_cast<uint32_t>(m.slots.size()) + index;
    // The frame (with the VDP1 ops around it), or the VDP1 ops alone (the ops-only submit).
    const bool recorded = frame ? m.renderer->RecordPendingFrame(rendererContext, context.cmd)
                                : m.renderer->RecordPendingVdp1Work(rendererContext, context.cmd);
    if (!recorded && !m.renderer->RecordedVdp1Work()) {
        vk.vkEndCommandBuffer(context.cmd);
        error = frame ? "recording a hidden VDP2 frame failed" : "recording the pending VDP1 ops failed";
        return false;
    }
    VkResult result = vk.vkEndCommandBuffer(context.cmd);
    if (result == VK_SUCCESS) {
        result = vk.vkResetFences(dev, 1, &context.fence);
    }
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &context.cmd;
        result = m.device->Submit(submit, context.fence);
    }
    if (result != VK_SUCCESS) {
        m.renderer->RecordedFrameDropped();
        m.runRefused = true;
        error = std::string(frame ? "submitting a hidden VDP2 frame failed: " : "submitting the pending VDP1 ops failed: ") +
                gpu::vk_util::ResultName(result);
        return false;
    }
    m.runSubmitted = true;
    m.renderer->FrameSubmitted(rendererContext, context.fence);
    context.pending = true;
    m.nextHidden = (index + 1) % kHiddenContexts;
    if (frame && recorded) {
        m.compositeValid = true;
    }
    if (!frame) {
        ++m.vdp1OpsOnlySubmits;
    }
    return true;
}

} // namespace brimir::libretro
