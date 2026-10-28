// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "vulkan_provider.hpp"

#include "host.hpp"

#include <gpu/gpu_version.hpp>
#include <gpu/headless_device.hpp>
#include <gpu/vk_device.hpp>
#include <gpu/vk_util.hpp>

#include <libretro_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace vkhost {

namespace gpu = brimir::gpu;

namespace {

constexpr uint32_t kSyncSlots = 3; // RetroArch's threaded-video ring: mask 0b111

VulkanProvider* g_provider = nullptr;

std::string VersionString(uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." + std::to_string(VK_API_VERSION_MINOR(version));
}

} // namespace

struct VulkanProvider::RealFunctions {
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
    PFN_vkQueueSubmit2KHR queueSubmit2KHR = nullptr;
    PFN_vkQueueWaitIdle queueWaitIdle = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
};

struct VulkanProvider::Slot {
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
};

struct VulkanProvider::Readback {
    gpu::vk_util::Buffer buffer;
};

namespace {

template <typename Handle>
uint64_t HandleValue(Handle handle) {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    } else {
        return static_cast<uint64_t>(handle);
    }
}

// --- Wrapped Vulkan functions handed to the core ----------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL HookQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits,
                                               VkFence fence) {
    uint32_t commandBuffers = 0;
    for (uint32_t i = 0; submits != nullptr && i < count; ++i) {
        commandBuffers += submits[i].commandBufferCount;
    }
    g_provider->CheckQueueUse("vkQueueSubmit", commandBuffers);
    if (commandBuffers > 0 && g_provider->TakeFailedSubmit()) {
        // --fail-submit-at: the core's frame submit fails (fault injection); nothing reaches the queue, so it is
        // counted as refused, not as one of the retro_run's submits.
        std::fprintf(stderr, "vkhost: vulkan: vkQueueSubmit refused (--fail-submit-at)\n");
        g_provider->OnRefusedSubmit();
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    const VkResult result = g_provider->Real().queueSubmit(queue, count, submits, fence);
    if (result == VK_SUCCESS) {
        g_provider->OnSubmitted(HandleValue(fence), commandBuffers);
    }
    return result;
}

uint32_t CommandBufferCount(uint32_t count, const VkSubmitInfo2* submits) {
    uint32_t commandBuffers = 0;
    for (uint32_t i = 0; submits != nullptr && i < count; ++i) {
        commandBuffers += submits[i].commandBufferInfoCount;
    }
    return commandBuffers;
}

VKAPI_ATTR VkResult VKAPI_CALL HookQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits,
                                                VkFence fence) {
    g_provider->CheckQueueUse("vkQueueSubmit2", CommandBufferCount(count, submits));
    return g_provider->Real().queueSubmit2(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL HookQueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits,
                                                   VkFence fence) {
    g_provider->CheckQueueUse("vkQueueSubmit2KHR", CommandBufferCount(count, submits));
    return g_provider->Real().queueSubmit2KHR(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL HookQueueWaitIdle(VkQueue queue) {
    // A queue use (needs the lock) that waits (must not hold it): never allowed.
    g_provider->CheckQueueUse("vkQueueWaitIdle", 0);
    g_provider->CheckFrameCall("vkQueueWaitIdle");
    g_provider->CheckWait("vkQueueWaitIdle");
    return g_provider->Real().queueWaitIdle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL HookWaitForFences(VkDevice device, uint32_t count, const VkFence* fences,
                                                 VkBool32 waitAll, uint64_t timeout) {
    g_provider->CheckWait("vkWaitForFences");
    std::vector<uint64_t> handles;
    for (uint32_t i = 0; fences != nullptr && i < count; ++i) {
        handles.push_back(HandleValue(fences[i]));
    }
    g_provider->OnWaitFences(static_cast<uint32_t>(handles.size()), handles.data());
    return g_provider->Real().waitForFences(device, count, fences, waitAll, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL HookDeviceWaitIdle(VkDevice device) {
    g_provider->CheckWait("vkDeviceWaitIdle");
    g_provider->OnDeviceWaitIdle();
    return g_provider->Real().deviceWaitIdle(device);
}

// --fail-compute-on-grow: pipeline creation fails once the sync mask has grown (fault injection).
VKAPI_ATTR VkResult VKAPI_CALL HookCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                          const VkComputePipelineCreateInfo* infos,
                                                          const VkAllocationCallbacks* allocator,
                                                          VkPipeline* pipelines) {
    if (g_provider->FailPipelines()) {
        for (uint32_t i = 0; i < count; ++i) {
            pipelines[i] = VK_NULL_HANDLE;
        }
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return g_provider->Real().createComputePipelines(device, cache, count, infos, allocator, pipelines);
}

template <typename Handle>
Handle HandleFrom(uint64_t value) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(static_cast<uintptr_t>(value));
    } else {
        return static_cast<Handle>(value);
    }
}

// --fail-alloc-above: large allocations fail as on a GPU short of memory (fault injection). Successful allocations
// are tracked for the core device memory summary (OnAllocateMemory / OnFreeMemory).
VKAPI_ATTR VkResult VKAPI_CALL HookAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
                                                  const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
    const uint64_t limit = g_provider->FailAllocAbove();
    if (limit != 0 && info != nullptr && info->allocationSize > limit) {
        g_provider->CountFailedAllocation();
        std::fprintf(stderr, "vkhost: vulkan: vkAllocateMemory of %llu MiB refused (--fail-alloc-above)\n",
                     static_cast<unsigned long long>(info->allocationSize >> 20));
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    const VkResult result = g_provider->Real().allocateMemory(device, info, allocator, memory);
    if (result == VK_SUCCESS && info != nullptr && memory != nullptr) {
        g_provider->OnAllocateMemory(HandleValue(*memory), info->allocationSize);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL HookFreeMemory(VkDevice device, VkDeviceMemory memory,
                                          const VkAllocationCallbacks* allocator) {
    if (memory != VK_NULL_HANDLE) {
        g_provider->OnFreeMemory(HandleValue(memory));
    }
    g_provider->Real().freeMemory(device, memory, allocator);
}

// --fail-output-images-above: the images the core hands to set_image (MUTABLE_FORMAT, which libretro_vulkan.h
// requires for them) fail to be created above 704 * N px.
VKAPI_ATTR VkResult VKAPI_CALL HookCreateImage(VkDevice device, const VkImageCreateInfo* info,
                                               const VkAllocationCallbacks* allocator, VkImage* image) {
    const uint32_t scale = g_provider->FailOutputImagesAbove();
    if (scale != 0 && info != nullptr && (info->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0 &&
        info->extent.width > 704 * scale) {
        g_provider->CountFailedAllocation();
        std::fprintf(stderr, "vkhost: vulkan: vkCreateImage of a %ux%u output image refused (--fail-output-images-above)\n",
                     info->extent.width, info->extent.height);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return g_provider->Real().createImage(device, info, allocator, image);
}

VKAPI_ATTR void VKAPI_CALL HookDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    if (image != VK_NULL_HANDLE) {
        g_provider->OnDestroy(HandleValue(image), "image");
    }
    g_provider->Real().destroyImage(device, image, allocator);
}

VKAPI_ATTR void VKAPI_CALL HookDestroyImageView(VkDevice device, VkImageView view,
                                                const VkAllocationCallbacks* allocator) {
    if (view != VK_NULL_HANDLE) {
        g_provider->OnDestroy(HandleValue(view), "image view");
    }
    g_provider->Real().destroyImageView(device, view, allocator);
}

PFN_vkVoidFunction Intercept(const char* name) {
    const VulkanProvider::RealFunctions& real = g_provider->Real();
    if (std::strcmp(name, "vkQueueSubmit") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookQueueSubmit);
    }
    if (std::strcmp(name, "vkQueueSubmit2") == 0 && real.queueSubmit2) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookQueueSubmit2);
    }
    if (std::strcmp(name, "vkQueueSubmit2KHR") == 0 && real.queueSubmit2KHR) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookQueueSubmit2KHR);
    }
    if (std::strcmp(name, "vkQueueWaitIdle") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookQueueWaitIdle);
    }
    if (std::strcmp(name, "vkWaitForFences") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookWaitForFences);
    }
    if (std::strcmp(name, "vkDeviceWaitIdle") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookDeviceWaitIdle);
    }
    if (std::strcmp(name, "vkCreateComputePipelines") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookCreateComputePipelines);
    }
    if (std::strcmp(name, "vkAllocateMemory") == 0 && real.allocateMemory) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookAllocateMemory);
    }
    if (std::strcmp(name, "vkFreeMemory") == 0 && real.freeMemory) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookFreeMemory);
    }
    if (std::strcmp(name, "vkCreateImage") == 0 && real.createImage) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookCreateImage);
    }
    if (std::strcmp(name, "vkDestroyImage") == 0 && real.destroyImage) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookDestroyImage);
    }
    if (std::strcmp(name, "vkDestroyImageView") == 0 && real.destroyImageView) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookDestroyImageView);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL HookGetDeviceProcAddr(VkDevice device, const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookGetDeviceProcAddr);
    }
    if (PFN_vkVoidFunction hook = Intercept(name)) {
        return hook;
    }
    return g_provider->Real().getDeviceProcAddr(device, name);
}

// --fake-api: the GPU reports an older Vulkan version (to the core only).
VKAPI_ATTR void VKAPI_CALL HookGetPhysicalDeviceProperties(VkPhysicalDevice physical,
                                                           VkPhysicalDeviceProperties* properties) {
    g_provider->Real().getPhysicalDeviceProperties(physical, properties);
    properties->apiVersion = std::min(properties->apiVersion, g_provider->FakeApiVersion());
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL HookGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    if (g_provider->FakeApiVersion() != 0 && std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookGetPhysicalDeviceProperties);
    }
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookGetInstanceProcAddr);
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&HookGetDeviceProcAddr);
    }
    if (instance != VK_NULL_HANDLE) {
        if (PFN_vkVoidFunction hook = Intercept(name)) {
            return hook;
        }
    }
    return g_provider->Real().getInstanceProcAddr(instance, name);
}

// --- retro_hw_render_interface_vulkan callbacks ------------------------------------------------------------------

VulkanProvider* FromHandle(void* handle) {
    return static_cast<VulkanProvider*>(handle);
}

void SetImage(void* handle, const retro_vulkan_image* image, uint32_t numSemaphores, const VkSemaphore*,
              uint32_t srcQueueFamily) {
    FromHandle(handle)->OnSetImage(image, numSemaphores, srcQueueFamily);
}

uint32_t GetSyncIndex(void* handle) {
    FromHandle(handle)->CheckFrameCall("get_sync_index");
    return FromHandle(handle)->SyncIndex();
}

uint32_t GetSyncIndexMask(void* handle) {
    return (1u << FromHandle(handle)->SyncSlots()) - 1;
}

void SetCommandBuffers(void* handle, uint32_t, const VkCommandBuffer*) {
    FromHandle(handle)->AddProblem("the core called set_command_buffers (not part of Brimir's design)");
}

void WaitSyncIndex(void* handle) {
    FromHandle(handle)->OnWaitSyncIndex();
}

void LockQueueCallback(void* handle) {
    FromHandle(handle)->LockQueue();
}

void UnlockQueueCallback(void* handle) {
    FromHandle(handle)->UnlockQueue();
}

void SetSignalSemaphore(void* handle, VkSemaphore) {
    FromHandle(handle)->AddProblem("the core called set_signal_semaphore (barriers only by design)");
}

// --- create_device2's wrapper -------------------------------------------------------------------------------------

VkDevice CreateDeviceWrapper(VkPhysicalDevice gpuHandle, void* opaque, const VkDeviceCreateInfo* info) {
    auto* provider = static_cast<VulkanProvider*>(opaque);
    if (info == nullptr || info->queueCreateInfoCount == 0) {
        provider->AddProblem("create_device_wrapper: no queue requested");
        return VK_NULL_HANDLE;
    }
    // Features only through per-feature structs (RetroArch's wrapper may add its own; both forms are invalid).
    for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next != nullptr; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
            next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
            next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            provider->AddProblem("create_device_wrapper: features requested through VkPhysicalDeviceVulkan1xFeatures");
        }
    }
    uint32_t familyCount = 0;
    volk::vkGetPhysicalDeviceQueueFamilyProperties(gpuHandle, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    volk::vkGetPhysicalDeviceQueueFamilyProperties(gpuHandle, &familyCount, families.data());
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
        const uint32_t family = info->pQueueCreateInfos[i].queueFamilyIndex;
        constexpr VkQueueFlags kWanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if (family >= familyCount || (families[family].queueFlags & kWanted) != kWanted) {
            provider->AddProblem("create_device_wrapper: queue family " + std::to_string(family) +
                                 " is not graphics + compute");
        }
    }

    // As a frontend: add what the device needs on top of the core's request (here only the portability subset,
    // which must be enabled where it exists).
    std::vector<const char*> extensions(info->ppEnabledExtensionNames,
                                        info->ppEnabledExtensionNames + info->enabledExtensionCount);
    uint32_t extensionCount = 0;
    volk::vkEnumerateDeviceExtensionProperties(gpuHandle, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    volk::vkEnumerateDeviceExtensionProperties(gpuHandle, nullptr, &extensionCount, available.data());
    available.resize(extensionCount);
    const char* portability = "VK_KHR_portability_subset";
    const auto has = [](const auto& list, const char* name, auto get) {
        return std::any_of(list.begin(), list.end(), [&](const auto& e) { return std::strcmp(get(e), name) == 0; });
    };
    if (has(available, portability, [](const VkExtensionProperties& e) { return e.extensionName; }) &&
        !has(extensions, portability, [](const char* e) { return e; })) {
        extensions.push_back(portability);
    }
    VkDeviceCreateInfo patched = *info;
    patched.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    patched.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    VkDevice device = VK_NULL_HANDLE;
    if (const VkResult result = volk::vkCreateDevice(gpuHandle, &patched, nullptr, &device); result != VK_SUCCESS) {
        provider->AddProblem(std::string("create_device_wrapper: vkCreateDevice failed: ") +
                             gpu::vk_util::ResultName(result));
        return VK_NULL_HANDLE;
    }
    return device;
}

} // namespace

VulkanProvider::VulkanProvider(uint32_t fakeApiVersion, bool validation)
    : m_real(std::make_unique<RealFunctions>()), m_fakeApi(fakeApiVersion) {
    if (g_provider != nullptr) {
        throw std::runtime_error("only one Vulkan provider at a time");
    }
    std::string reason;
    m_host = gpu::CreateHeadlessDevice({.validation = validation}, &reason);
    if (!m_host) {
        throw VulkanUnavailable(reason);
    }
    g_provider = this;
    m_real->getInstanceProcAddr = volk::vkGetInstanceProcAddr;
    m_real->getDeviceProcAddr = volk::vkGetDeviceProcAddr;
    m_real->getPhysicalDeviceProperties = volk::vkGetPhysicalDeviceProperties;
    if (m_fakeApi != 0) {
        std::fprintf(stderr, "vkhost: vulkan: the core is told the GPU supports Vulkan %s\n",
                     VersionString(m_fakeApi).c_str());
    }
    std::fprintf(stderr, "vkhost: vulkan: %s (Vulkan %s), validation %s\n", m_host->Properties().deviceName,
                 VersionString(m_host->Properties().apiVersion).c_str(), m_host->ValidationEnabled() ? "on" : "off");
}

VulkanProvider::~VulkanProvider() {
    DestroyContext(false);
    m_host.reset();
    if (g_provider == this) {
        g_provider = nullptr;
    }
}

const VulkanProvider::RealFunctions& VulkanProvider::Real() const {
    return *m_real;
}

bool VulkanProvider::ValidationEnabled() const {
    return m_host && m_host->ValidationEnabled();
}

uint32_t VulkanProvider::ValidationErrors() const {
    return m_host ? m_host->ValidationErrors()->load() : 0;
}

std::shared_ptr<const std::atomic<uint32_t>> VulkanProvider::ValidationCounter() const {
    return m_host ? m_host->ValidationErrors() : nullptr;
}

void VulkanProvider::AddProblem(const std::string& text) {
    if (m_selfTest) {
        return;
    }
    // Keep the first few of each kind; the counters carry the totals.
    if (std::count(m_problems.begin(), m_problems.end(), text) == 0 && m_problems.size() < 64) {
        m_problems.push_back(text);
        std::fprintf(stderr, "vkhost: problem: %s\n", text.c_str());
    }
}

bool VulkanProvider::CreateContext(const retro_hw_render_context_negotiation_interface* negotiation) {
    if (m_live) {
        AddProblem("context created twice");
        return false;
    }
    if (m_failComputeOnReset && m_contextsCreated > 0) {
        m_failPipelines = true;
    }
    ++m_contextsCreated;
    const VkInstance instance = m_host->Instance();
    VkPhysicalDevice physical = m_host->Physical();
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    m_ownedDevice = nullptr;
    m_destroyDevice = nullptr;

    if (negotiation != nullptr) {
        const auto* neg = reinterpret_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(negotiation);
        if (neg->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN) {
            AddProblem("negotiation interface of another type");
        } else {
            if (neg->create_device == nullptr) {
                AddProblem("negotiation: create_device (v1) is null, so RetroArch would not negotiate at all");
            }
            if (neg->get_application_info == nullptr) {
                AddProblem("negotiation: get_application_info is null (required in v2)");
            } else if (const VkApplicationInfo* app = neg->get_application_info(); app == nullptr) {
                AddProblem("negotiation: get_application_info returned null");
            } else if (app->apiVersion > gpu::kRequiredApiVersion) {
                AddProblem("negotiation: the core asks for Vulkan " + VersionString(app->apiVersion) +
                           "; this host's instance is " + VersionString(gpu::kRequiredApiVersion));
            }
            if (neg->interface_version >= 2 && neg->create_device2 != nullptr) {
                retro_vulkan_context context{};
                bool ok = neg->create_device2(&context, instance, physical, VK_NULL_HANDLE, &HookGetInstanceProcAddr,
                                              &CreateDeviceWrapper, this);
                if (!ok) {
                    AddProblem("create_device2 refused the frontend's GPU");
                    std::fprintf(stderr, "vkhost: vulkan: create_device2 refused the GPU, retrying without one\n");
                    context = {};
                    ok = neg->create_device2(&context, instance, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                             &HookGetInstanceProcAddr, &CreateDeviceWrapper, this);
                }
                if (ok) {
                    if (context.device == VK_NULL_HANDLE || context.queue == VK_NULL_HANDLE ||
                        context.presentation_queue != context.queue) {
                        AddProblem("create_device2: incomplete retro_vulkan_context");
                    }
                    device = context.device;
                    queue = context.queue;
                    family = context.queue_family_index;
                    physical = context.gpu != VK_NULL_HANDLE ? context.gpu : physical;
                    m_ownedDevice = device;
                    m_destroyDevice = neg->destroy_device;
                    std::fprintf(stderr, "vkhost: vulkan: device created through create_device2 (negotiation v%u)\n",
                                 neg->interface_version);
                } else {
                    // A refusal after the retry leaves RetroArch master without a GPU: the core must create a
                    // device even on a GPU below its baseline (and fall back to the copy-only output).
                    AddProblem("create_device2 refused both times; using the host's default device");
                }
            } else {
                std::fprintf(stderr, "vkhost: vulkan: negotiation v%u without create_device2; default device\n",
                             neg->interface_version);
            }
        }
    }

    if (device == VK_NULL_HANDLE) {
        device = m_host->Device();
        queue = m_host->Queue();
        family = m_host->QueueFamily();
        m_ctx = m_host.get();
    } else {
        gpu::ExternalDevice handles;
        handles.instance = instance;
        handles.physical = physical;
        handles.device = device;
        handles.queue = queue;
        handles.queueFamily = family;
        handles.getInstanceProcAddr = m_real->getInstanceProcAddr;
        handles.apiVersion = gpu::kRequiredApiVersion;
        std::string reason;
        m_ctxView = gpu::VulkanDevice::WrapExternal(handles, &reason);
        if (!m_ctxView) {
            AddProblem("cannot use the negotiated device: " + reason);
            if (m_destroyDevice) {
                m_destroyDevice();
            }
            reinterpret_cast<PFN_vkDestroyDevice>(m_real->getDeviceProcAddr(device, "vkDestroyDevice"))(device,
                                                                                                      nullptr);
            m_ownedDevice = nullptr;
            return false;
        }
        m_ctx = m_ctxView.get();
    }
    m_ctx->SetQueueLockHooks([this] { HostLock(); }, [this] { HostUnlock(); });

    const auto load = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(m_real->getDeviceProcAddr(device, name));
    };
    load(m_real->queueSubmit, "vkQueueSubmit");
    load(m_real->queueSubmit2, "vkQueueSubmit2");
    load(m_real->queueSubmit2KHR, "vkQueueSubmit2KHR");
    load(m_real->queueWaitIdle, "vkQueueWaitIdle");
    load(m_real->waitForFences, "vkWaitForFences");
    load(m_real->deviceWaitIdle, "vkDeviceWaitIdle");
    load(m_real->createComputePipelines, "vkCreateComputePipelines");
    load(m_real->allocateMemory, "vkAllocateMemory");
    load(m_real->freeMemory, "vkFreeMemory");
    load(m_real->createImage, "vkCreateImage");
    load(m_real->destroyImage, "vkDestroyImage");
    load(m_real->destroyImageView, "vkDestroyImageView");
    m_pinned.clear();

    m_slots.assign(kSyncSlots, Slot{});
    for (Slot& slot : m_slots) {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (const VkResult result = m_ctx->Table().vkCreateFence(device, &fenceInfo, nullptr, &slot.fence);
            result != VK_SUCCESS) {
            AddProblem(std::string("vkCreateFence failed: ") + gpu::vk_util::ResultName(result));
            slot.fence = VK_NULL_HANDLE;
        }
    }
    m_syncIndex = 0;
    m_image = nullptr;
    m_readback = std::make_unique<Readback>();

    m_iface = std::make_unique<retro_hw_render_interface_vulkan>();
    retro_hw_render_interface_vulkan& iface = *m_iface;
    iface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    iface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    iface.handle = this;
    iface.instance = instance;
    iface.gpu = physical;
    iface.device = device;
    iface.get_device_proc_addr = &HookGetDeviceProcAddr;
    iface.get_instance_proc_addr = &HookGetInstanceProcAddr;
    iface.queue = queue;
    iface.queue_index = family;
    iface.set_image = &SetImage;
    iface.get_sync_index = &GetSyncIndex;
    iface.get_sync_index_mask = &GetSyncIndexMask;
    iface.set_command_buffers = &SetCommandBuffers;
    iface.wait_sync_index = &WaitSyncIndex;
    iface.lock_queue = &LockQueueCallback;
    iface.unlock_queue = &UnlockQueueCallback;
    iface.set_signal_semaphore = &SetSignalSemaphore;
    m_live = true;

    SelfTest();
    return true;
}

void VulkanProvider::SelfTest() {
    // The checks must be able to fire: an unlocked submit and a wait under the lock through the wrapped functions.
    const uint64_t before = m_lockViolations;
    m_selfTest = true;
    const auto submit = reinterpret_cast<PFN_vkQueueSubmit>(HookGetDeviceProcAddr(m_ctx->Device(), "vkQueueSubmit"));
    submit(m_ctx->Queue(), 0, nullptr, VK_NULL_HANDLE);
    const bool submitCaught = m_lockViolations == before + 1;

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence = VK_NULL_HANDLE;
    bool waitCaught = false;
    if (m_ctx->Table().vkCreateFence(m_ctx->Device(), &fenceInfo, nullptr, &fence) == VK_SUCCESS) {
        const auto wait = reinterpret_cast<PFN_vkWaitForFences>(
            HookGetInstanceProcAddr(m_ctx->Instance(), "vkWaitForFences"));
        LockQueue();
        wait(m_ctx->Device(), 1, &fence, VK_TRUE, UINT64_MAX);
        UnlockQueue();
        m_ctx->Table().vkDestroyFence(m_ctx->Device(), fence, nullptr);
        waitCaught = m_lockViolations == before + 2;
    }

    m_selfTest = false;
    m_lockViolations = before;
    if (!submitCaught || !waitCaught) {
        AddProblem("the queue-lock check did not fire in its self-test");
    }
}

void VulkanProvider::DestroyContext(bool callCore) {
    if (!m_live) {
        return;
    }
    const VkDevice device = m_ctx->Device();
    const auto& vk = m_ctx->Table();
    vk.vkDeviceWaitIdle(device);
    for (Slot& slot : m_slots) {
        vk.vkDestroyFence(device, slot.fence, nullptr);
    }
    m_slots.clear();
    if (m_readback) {
        gpu::vk_util::DestroyBuffer(*m_ctx, m_readback->buffer);
        m_readback.reset();
    }
    m_image = nullptr;
    m_iface.reset();
    m_ctxView.reset();
    m_ctx = nullptr;
    if (m_ownedDevice != nullptr) {
        // RetroArch's order: destroy_device, then vkDestroyDevice.
        if (callCore && m_destroyDevice) {
            m_destroyDevice();
        }
        const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(
            m_real->getDeviceProcAddr(static_cast<VkDevice>(m_ownedDevice), "vkDestroyDevice"));
        destroy(static_cast<VkDevice>(m_ownedDevice), nullptr);
        m_ownedDevice = nullptr;
    }
    m_destroyDevice = nullptr;
    m_live = false;
}

const retro_hw_render_interface* VulkanProvider::Interface() const {
    return m_live ? reinterpret_cast<const retro_hw_render_interface*>(m_iface.get()) : nullptr;
}

void VulkanProvider::CheckQueueUse(const char* function, uint32_t commandBuffers) {
    if (!m_selfTest) {
        ++m_coreSubmits;
        if (commandBuffers > 0) {
            // Frames with video disabled still submit their VDP2 work (no presentation).
            if (!m_videoEnabled) {
                ++m_hiddenSubmits;
            }
        }
    }
    if (m_lockOwner.load() != std::this_thread::get_id()) {
        ++m_lockViolations;
        AddProblem(std::string(function) + " without lock_queue held");
    }
}

void VulkanProvider::BeginRun() {
    m_inRun = true;
    m_runSubmits = 0;
    m_runRefused = 0;
    m_runOpsOnlyLine = false;
    ++m_runIndex;
    m_phase = CorePhase::Run;
}

void VulkanProvider::EndRun() {
    m_inRun = false;
    m_phase = CorePhase::Other;
    if (m_runSubmits > 1) {
        AddProblem("more than one command buffer submit in one retro_run (sync submits not counted)");
    }
    if (m_runRefused > 0 && m_runSubmits > 0) {
        // A second submit after a refused one: only the ops-only submit, which the core logs
        if (m_runOpsOnlyLine) {
            ++m_submitWaits.extraOpsOnlyRuns;
        } else {
            AddProblem("a submit after a refused one in the same retro_run without the core's ops-only line");
        }
    }
    m_maxRunSubmits = std::max(m_maxRunSubmits, m_runSubmits);
}

void VulkanProvider::OnSubmitted(uint64_t fence, uint32_t commandBuffers) {
    if (m_selfTest || commandBuffers == 0) {
        return;
    }
    switch (m_phase) {
    case CorePhase::Run: ++m_runSubmits; break;
    case CorePhase::Context: ++m_submitWaits.contextSubmits; break;
    default:
        ++m_submitWaits.outsideSubmits;
        AddProblem("a command buffer submit outside retro_run and the context callbacks");
        break;
    }
    if (fence != 0) {
        m_fenceRuns[fence] = m_phase == CorePhase::Run ? m_runIndex : 0;
    }
}

void VulkanProvider::OnRefusedSubmit() {
    ++m_submitWaits.refusedSubmits;
    if (m_phase == CorePhase::Run) {
        ++m_runRefused;
    }
}

void VulkanProvider::OnWaitFences(uint32_t count, const uint64_t* fences) {
    if (m_selfTest) {
        return;
    }
    switch (m_phase) {
    case CorePhase::Run: {
        bool sync = false;
        for (uint32_t i = 0; i < count; ++i) {
            const auto it = m_fenceRuns.find(fences[i]);
            if (it != m_fenceRuns.end() && it->second == m_runIndex) {
                // Submitted and waited for in the same retro_run: a sync submit (not one of the run's frame submits)
                sync = true;
                m_fenceRuns.erase(it);
                ++m_submitWaits.syncSubmits;
                if (m_runSubmits > 0) {
                    --m_runSubmits;
                }
            }
        }
        ++(sync ? m_submitWaits.syncWaits : m_submitWaits.frameWaits);
        break;
    }
    case CorePhase::Serialize: ++m_submitWaits.serializeWaits; break;
    case CorePhase::Context: ++m_submitWaits.contextWaits; break;
    default:
        ++m_submitWaits.outsideWaits;
        AddProblem("a fence wait outside retro_run, retro_serialize and the context callbacks");
        break;
    }
}

void VulkanProvider::OnDeviceWaitIdle() {
    if (m_selfTest) {
        return;
    }
    if (m_phase == CorePhase::Context) {
        ++m_submitWaits.contextWaits;
    } else {
        ++m_submitWaits.outsideWaits;
        AddProblem("vkDeviceWaitIdle outside the context callbacks");
    }
}

void VulkanProvider::CheckWait(const char* function) {
    if (m_lockOwner.load() == std::this_thread::get_id()) {
        ++m_lockViolations;
        AddProblem(std::string(function) + " while holding lock_queue");
    }
}

void VulkanProvider::CheckFrameCall(const char* function) {
    if (!m_videoEnabled && !m_selfTest) {
        ++m_hiddenGpuCalls;
        AddProblem(std::string(function) + " in a frame with video disabled");
    }
}

void VulkanProvider::OnSetImage(const void* rawImage, uint32_t numSemaphores, uint32_t srcQueueFamily) {
    CheckFrameCall("set_image");
    const auto* image = static_cast<const retro_vulkan_image*>(rawImage);
    m_image = image;
    if (image == nullptr) {
        return;
    }
    if (image->image_layout != VK_IMAGE_LAYOUT_GENERAL) {
        AddProblem("set_image: layout is not VK_IMAGE_LAYOUT_GENERAL");
    }
    if (srcQueueFamily != VK_QUEUE_FAMILY_IGNORED) {
        AddProblem("set_image: src_queue_family is not VK_QUEUE_FAMILY_IGNORED");
    }
    if (numSemaphores != 0) {
        AddProblem("set_image: semaphores passed (barriers only by design)");
    }
    if (image->create_info.image == VK_NULL_HANDLE || image->image_view == VK_NULL_HANDLE) {
        AddProblem("set_image: no image or view");
    }
}

void VulkanProvider::OnWaitSyncIndex() {
    CheckFrameCall("wait_sync_index");
    CheckWait("wait_sync_index");
    Slot& slot = m_slots[m_syncIndex];
    if (slot.pending) {
        m_real->waitForFences(m_ctx->Device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
        m_ctx->Table().vkResetFences(m_ctx->Device(), 1, &slot.fence);
        slot.pending = false;
    }
}

void VulkanProvider::LockQueue() {
    if (m_lockOwner.load() == std::this_thread::get_id()) {
        ++m_lockViolations;
        AddProblem("lock_queue while already holding it");
        return;
    }
    m_queueMutex.lock();
    m_lockOwner = std::this_thread::get_id();
}

void VulkanProvider::UnlockQueue() {
    if (m_lockOwner.load() != std::this_thread::get_id()) {
        ++m_lockViolations;
        AddProblem("unlock_queue without holding the lock");
        return;
    }
    m_lockOwner = std::thread::id{};
    m_queueMutex.unlock();
}

void VulkanProvider::HostLock() {
    if (m_lockOwner.load() == std::this_thread::get_id()) {
        // The core called into the frontend (video_refresh) with the queue lock held.
        ++m_lockViolations;
        AddProblem("the queue lock was held while the frontend needed the queue");
        m_hostLocked = false;
        return;
    }
    m_queueMutex.lock();
    m_hostLocked = true;
}

void VulkanProvider::HostUnlock() {
    if (m_hostLocked) {
        m_hostLocked = false;
        m_queueMutex.unlock();
    }
}

void VulkanProvider::SetSyncSlots(uint32_t count) {
    count = std::clamp<uint32_t>(count, 1, kSyncSlots);
    if (count == m_syncSlots) {
        return;
    }
    // libretro_vulkan.h: when the mask changes the device is idle.
    if (m_live) {
        m_real->deviceWaitIdle(m_ctx->Device());
    }
    if (count > m_syncSlots && m_failComputeOnGrow) {
        m_failPipelines = true;
    }
    if (count > m_syncSlots && m_failOutputImagesOnGrow != 0) {
        m_failOutputImagesAbove = m_failOutputImagesOnGrow;
    }
    m_syncSlots = count;
    m_syncIndex = 0;
    std::fprintf(stderr, "vkhost: vulkan: sync index mask now 0x%x\n", (1u << count) - 1);
}

void VulkanProvider::EndFrame() {
    if (!m_live) {
        return;
    }
    // The frontend's use of the slot: done when this fence signals (wait_sync_index waits for it).
    Slot& slot = m_slots[m_syncIndex];
    const VkDevice device = m_ctx->Device();
    if (slot.pending) {
        m_real->waitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        m_ctx->Table().vkResetFences(device, 1, &slot.fence);
    }
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (m_ctx->Submit(submit, slot.fence) == VK_SUCCESS) {
        slot.pending = true;
    } else {
        AddProblem("frame fence submit failed");
        slot.pending = false;
    }
    m_syncIndex = (m_syncIndex + 1) % m_syncSlots;
}

bool VulkanProvider::ReadImage(unsigned w, unsigned h, HostFrame& out) {
    const auto* image = static_cast<const retro_vulkan_image*>(m_image);
    if (!m_live || image == nullptr) {
        return false;
    }
    const VkImageSubresourceRange& range = image->create_info.subresourceRange;
    return ReadHandles(HandleValue(image->create_info.image), image->create_info.format, range.baseMipLevel,
                       range.baseArrayLayer, w, h, out);
}

bool VulkanProvider::CaptureImage(unsigned w, unsigned h, DeferredImage& out) {
    const auto* image = static_cast<const retro_vulkan_image*>(m_image);
    if (!m_live || image == nullptr) {
        return false;
    }
    out.image = HandleValue(image->create_info.image);
    out.view = HandleValue(image->image_view);
    out.format = image->create_info.format;
    out.baseMip = image->create_info.subresourceRange.baseMipLevel;
    out.baseLayer = image->create_info.subresourceRange.baseArrayLayer;
    out.w = w;
    out.h = h;
    m_pinned.insert(out.image);
    m_pinned.insert(out.view);
    return true;
}

bool VulkanProvider::ReadDeferred(const DeferredImage& image, HostFrame& out) {
    const bool ok = m_live && m_pinned.count(image.image) != 0 &&
                    ReadHandles(image.image, image.format, image.baseMip, image.baseLayer, image.w, image.h, out);
    for (uint64_t handle : {image.image, image.view}) {
        if (const auto it = m_pinned.find(handle); it != m_pinned.end()) {
            m_pinned.erase(it);
        }
    }
    return ok;
}

void VulkanProvider::OnDestroy(uint64_t handle, const char* what) {
    if (const auto it = m_pinned.find(handle); it != m_pinned.end()) {
        AddProblem(std::string("the core destroyed an ") + what + " it handed over with set_image before the " +
                   "frontend read it (wait_sync_index for its sync index had not returned)");
        // Not read any more: the handle may be reused.
        while (m_pinned.count(handle) != 0) {
            m_pinned.erase(m_pinned.find(handle));
        }
    }
}

bool VulkanProvider::ReadHandles(uint64_t imageHandle, int formatValue, uint32_t baseMip, uint32_t baseLayer,
                                 unsigned w, unsigned h, HostFrame& out) {
    if (!m_live || imageHandle == 0 || w == 0 || h == 0 || w > 4096 || h > 4096) {
        return false;
    }
    const VkFormat format = static_cast<VkFormat>(formatValue);
    bool bgra = false;
    if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) {
        bgra = true;
    } else if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) {
        AddProblem("set_image: unsupported format " + std::to_string(static_cast<int>(format)));
        return false;
    }

    gpu::vk_util::Buffer& buffer = m_readback->buffer;
    const VkDeviceSize bytes = VkDeviceSize{w} * h * 4;
    if (buffer.size < bytes) {
        gpu::vk_util::DestroyBuffer(*m_ctx, buffer);
        constexpr VkMemoryPropertyFlags kCoherent =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (gpu::vk_util::CreateBuffer(*m_ctx, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       kCoherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, buffer) != VK_SUCCESS &&
            gpu::vk_util::CreateBuffer(*m_ctx, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kCoherent, buffer) !=
                VK_SUCCESS) {
            AddProblem("cannot create the readback buffer");
            return false;
        }
    }

    const VkImage vkImage = HandleFrom<VkImage>(imageHandle);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, baseMip, 1, baseLayer, 1};
    const VkResult result = gpu::vk_util::SubmitOneShot(*m_ctx, [&](VkCommandBuffer cmd) {
        // GENERAL: the frontend reads the image in the layout the core left it in, without a transition.
        gpu::vk_util::ImageBarrier(*m_ctx, cmd, vkImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, range.baseMipLevel, range.baseArrayLayer, 1};
        region.imageExtent = {w, h, 1};
        m_ctx->Table().vkCmdCopyImageToBuffer(cmd, vkImage, VK_IMAGE_LAYOUT_GENERAL, buffer.buffer, 1, &region);
        gpu::vk_util::GlobalBarrier(*m_ctx, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    });
    if (result != VK_SUCCESS || buffer.mapped == nullptr) {
        AddProblem(std::string("readback failed: ") + gpu::vk_util::ResultName(result));
        return false;
    }

    out.w = w;
    out.h = h;
    out.xrgb.resize(static_cast<size_t>(w) * h);
    const auto* bytesIn = static_cast<const uint8_t*>(buffer.mapped);
    for (size_t i = 0; i < out.xrgb.size(); ++i) {
        const uint8_t* px = bytesIn + i * 4;
        const uint32_t r = bgra ? px[2] : px[0];
        const uint32_t b = bgra ? px[0] : px[2];
        out.xrgb[i] = (r << 16) | (uint32_t{px[1]} << 8) | b;
    }
    return true;
}

} // namespace vkhost
