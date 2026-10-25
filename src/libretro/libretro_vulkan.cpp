// Brimir - libretro Vulkan hardware context (BRIMIR_VULKAN builds only)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "libretro_vulkan.hpp"

#include <gpu/frame_presenter.hpp>
#include <gpu/gpu_version.hpp>
#include <gpu/vk_device.hpp>
#include <gpu/vk_util.hpp>

#include <libretro_vulkan.h>

#include <algorithm>
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
    // The device create_device2 made, with exactly the features gpu_version.hpp lists.
    VkDevice device = VK_NULL_HANDLE;
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

// A queue family with graphics and compute (the frontend presents from the queue it gives the core), and present
// support when there is a surface. Empty optional and *rejection set when the device does not meet the baseline.
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

    VkPhysicalDeviceFeatures features{};
    fn.getFeatures(gpu, &features);
    for (const gpu::CoreFeature &feature : gpu::kRequiredCoreFeatures) {
        if (!(features.*feature.member)) {
            rejection = name + ": missing feature " + feature.name;
            return std::nullopt;
        }
    }

    uint32_t extensionCount = 0;
    fn.enumerateExtensions(gpu, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    fn.enumerateExtensions(gpu, nullptr, &extensionCount, extensions.data());
    extensions.resize(extensionCount);
    for (const char *required : gpu::kRequiredDeviceExtensions) {
        const bool found = std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties &ext) {
            return std::strcmp(ext.extensionName, required) == 0;
        });
        if (!found) {
            rejection = name + ": missing extension " + required;
            return std::nullopt;
        }
    }

    uint32_t familyCount = 0;
    fn.getQueueFamilies(gpu, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    fn.getQueueFamilies(gpu, &familyCount, families.data());
    constexpr VkQueueFlags kWanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & kWanted) != kWanted || families[i].queueCount == 0) {
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

bool CreateDevice2(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
                   PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                   retro_vulkan_create_device_wrapper_t createDeviceWrapper, void *opaque) {
    g_negotiation.device = VK_NULL_HANDLE;
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
    std::string rejections;
    for (VkPhysicalDevice candidate : candidates) {
        std::string rejection;
        if (const auto found = EvaluateDevice(fn, candidate, surface, rejection)) {
            chosen = candidate;
            family = *found;
            break;
        }
        rejections += (rejections.empty() ? "" : "; ") + rejection;
    }
    if (chosen == VK_NULL_HANDLE) {
        NegotiationLog(RETRO_LOG_WARN, "Vulkan create_device2: no suitable GPU (" +
                                           (rejections.empty() ? std::string("none offered") : rejections) + ")");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // Vulkan 1.0 features through pEnabledFeatures; any 1.1+ feature goes into pNext as its own per-feature struct
    // (never VkPhysicalDeviceVulkan11/12Features: RetroArch's wrapper may add per-feature structs of its own).
    VkPhysicalDeviceFeatures features{};
    for (const gpu::CoreFeature &feature : gpu::kRequiredCoreFeatures) {
        features.*feature.member = VK_TRUE;
    }
    const std::vector<const char *> extensions(gpu::kRequiredDeviceExtensions.begin(),
                                               gpu::kRequiredDeviceExtensions.end());

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
        // Persistent: the frontend keeps the pointer from set_image (and dereferences it for dupes).
        retro_vulkan_image image{};
    };

    const retro_hw_render_interface_vulkan *iface = nullptr;
    std::unique_ptr<gpu::VulkanDevice> device;
    VulkanOutputMode mode = VulkanOutputMode::None;
    std::unique_ptr<gpu::FramePresenter> presenter; // compute mode
    std::vector<Slot> slots;

    VkResult CreateSlots(uint32_t count, VulkanOutputMode wanted);
    void DestroySlots();
    VkResult WaitSlots();
    void Withdraw();
    void Teardown();
    void Abandon();
    Slot *BeginFrame(uint32_t &index, std::string &error);
    bool SubmitFrame(Slot &slot, std::string &error);
};

// The copy-only output: B8G8R8A8_UNORM has the byte order of XRGB8888, so the bridge's frame is copied as is; the
// view reads alpha as one (the X byte is 0).
constexpr VkFormat kCopyFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kCopyMaxSize = gpu::FramePresenter::kMaxOutputSize;

VkResult VulkanOutput::Impl::CreateSlots(uint32_t count, VulkanOutputMode wanted) {
    const auto &vk = device->Table();
    const VkDevice dev = device->Device();
    if (wanted == VulkanOutputMode::Compute) {
        presenter = std::make_unique<gpu::FramePresenter>(*device, count);
        if (!presenter->Ok()) {
            return presenter->Result();
        }
    }
    slots.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        Slot &slot = slots[i];
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = device->QueueFamily();
        VkResult result = vk.vkCreateCommandPool(dev, &poolInfo, nullptr, &slot.pool);
        if (result != VK_SUCCESS) {
            return result;
        }
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = slot.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        result = vk.vkAllocateCommandBuffers(dev, &allocInfo, &slot.cmd);
        if (result != VK_SUCCESS) {
            return result;
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vk.vkCreateFence(dev, &fenceInfo, nullptr, &slot.fence);
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
            view.image = presenter->OutputImage(i);
            view.format = VK_FORMAT_R8G8B8A8_UNORM;
            slot.image.image_view = presenter->OutputView(i);
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
    presenter.reset();
    mode = VulkanOutputMode::None;
}

// Takes the image back from the frontend (it keeps the last set_image pointer for dupes) before the slots go away.
void VulkanOutput::Impl::Withdraw() {
    if (iface) {
        iface->set_image(iface->handle, nullptr, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
    }
}

void VulkanOutput::Impl::Teardown() {
    if (device) {
        // Allowed in context_destroy: the frontend is idle and holds no reference to the images any more.
        device->Table().vkDeviceWaitIdle(device->Device());
    }
    DestroySlots();
    device.reset();
    iface = nullptr;
}

void VulkanOutput::Impl::Abandon() {
    slots.clear();
    (void)presenter.release();
    (void)device.release();
    mode = VulkanOutputMode::None;
    iface = nullptr;
}

// The frame prologue both modes share: slots sized for the current mask, the sync index, the frontend's and our own
// wait for the slot, and the slot's command buffer begun. Null (with error) when nothing can be recorded; the mode
// may have changed then (degraded to copy-only, or None when no output is possible any more).
VulkanOutput::Impl::Slot *VulkanOutput::Impl::BeginFrame(uint32_t &index, std::string &error) {
    const auto &vk = device->Table();
    const VkDevice dev = device->Device();

    // The mask may change (with the device idle); slots only grow so earlier indices stay valid.
    const uint32_t wanted = SlotCountForMask(iface->get_sync_index_mask(iface->handle));
    if (wanted > slots.size()) {
        const VulkanOutputMode current = mode;
        Withdraw(); // the frontend must not keep a pointer into the slots about to be freed
        if (const VkResult result = WaitSlots(); result != VK_SUCCESS) {
            error = std::string("waiting for the frame fences failed: ") + gpu::vk_util::ResultName(result);
            DestroySlots();
            return nullptr;
        }
        DestroySlots();
        VkResult result = CreateSlots(wanted, current);
        if (result != VK_SUCCESS && current == VulkanOutputMode::Compute) {
            DestroySlots();
            if (CreateSlots(wanted, VulkanOutputMode::CopyOnly) == VK_SUCCESS) {
                error = std::string("recreating the compute frame output failed (") +
                        gpu::vk_util::ResultName(result) + "); continuing with copy-only uploads";
                return nullptr;
            }
        }
        if (result != VK_SUCCESS) {
            error = std::string("recreating the frame output failed: ") + gpu::vk_util::ResultName(result);
            DestroySlots();
            return nullptr;
        }
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
bool VulkanOutput::Impl::SubmitFrame(Slot &slot, std::string &error) {
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
        return false;
    }
    slot.pending = true;
    iface->set_image(iface->handle, &slot.image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
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
    if (raw->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
        raw->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION) {
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
    if (negotiated) {
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

    // The compute frame output needs Vulkan 1.2, a compute queue and the baseline features. Without them the frames
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
    } else if (!negotiated && (!gpu::kRequiredCoreFeatures.empty() || !gpu::kRequiredDeviceExtensions.empty())) {
        computeIssue = "the frontend's device was not created through the negotiation interface, so the required "
                       "features are not enabled";
    }

    const uint32_t slotCount = SlotCountForMask(iface->get_sync_index_mask(iface->handle));
    if (computeIssue.empty()) {
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
    if (!m.SubmitFrame(*slot, error)) {
        return false;
    }
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

    // Host-coherent: visible to the device at vkQueueSubmit.
    const auto *src = static_cast<const uint8_t *>(xrgb);
    auto *dst = static_cast<uint8_t *>(slot->staging.mapped);
    const size_t rowBytes = size_t{w} * 4;
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(dst + y * rowBytes, src + y * pitch, rowBytes);
    }

    // Earlier readers of the image (the frontend) finish before the copy; the old contents are not needed. The
    // image stays in GENERAL, which the frontend samples without a transition.
    const VkImage image = slot->copyImage.image;
    gpu::vk_util::ImageBarrier(*m.device, slot->cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    m.device->Table().vkCmdCopyBufferToImage(slot->cmd, slot->staging.buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                             &region);
    gpu::vk_util::ImageBarrier(*m.device, slot->cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    return m.SubmitFrame(*slot, error);
}

} // namespace brimir::libretro
