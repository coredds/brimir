#pragma once

// The Vulkan device the GPU renderer runs on: instance, physical device, device, one compute-capable queue and the
// per-device function table. Either owning (a headless device for tests and tools, see headless_device.hpp; the
// destructor destroys the device and the instance) or a non-owning view of handles a frontend provides (libretro's
// retro_hw_render_interface_vulkan; the destructor leaves them alone).
//
// Instance-level functions come from volk's globals (volk::vk*), device-level functions only from Table().

#include <volk.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace brimir::gpu {

// What the device was created with, as far as the renderer may rely on it.
struct DeviceFeatures {
    // min(the device's apiVersion, the API version the instance and device were created for).
    uint32_t apiVersion = 0;
    // Enabled Vulkan 1.0 features (gpu_version.hpp kRequiredCoreFeatures).
    VkPhysicalDeviceFeatures core{};
};

// Handles owned by someone else (the libretro frontend). The queue must support compute.
struct ExternalDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    // Optional; when null, the one getInstanceProcAddr returns for the instance is used.
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    // The API version the instance was created for (VkApplicationInfo::apiVersion); 0 means the device's.
    uint32_t apiVersion = 0;
    VkPhysicalDeviceFeatures enabledFeatures{};
};

class VulkanDevice {
public:
    // A non-owning view of a frontend's device. Loads volk for the instance (volkInitializeCustom with
    // getInstanceProcAddr) and a device table. Returns nullptr and sets *reason when the handles are unusable.
    static std::unique_ptr<VulkanDevice> WrapExternal(const ExternalDevice &handles, std::string *reason = nullptr);

    ~VulkanDevice();
    VulkanDevice(const VulkanDevice &) = delete;
    VulkanDevice &operator=(const VulkanDevice &) = delete;

    VkInstance Instance() const { return m_instance; }
    VkPhysicalDevice Physical() const { return m_physical; }
    VkDevice Device() const { return m_device; }
    VkQueue Queue() const { return m_queue; }
    uint32_t QueueFamily() const { return m_queueFamily; }
    const VolkDeviceTable &Table() const { return m_table; }
    const DeviceFeatures &Features() const { return m_features; }
    const VkPhysicalDeviceProperties &Properties() const { return m_properties; }
    const VkPhysicalDeviceMemoryProperties &MemoryProperties() const { return m_memoryProperties; }

    // True when this object destroys the device and instance (headless).
    bool OwnsDevice() const { return m_owning; }

    // Called around every vkQueueSubmit (libretro lock_queue/unlock_queue). No-ops by default.
    void SetQueueLockHooks(std::function<void()> lock, std::function<void()> unlock);

    // vkQueueSubmit on Queue() inside the lock hooks. Never waits.
    VkResult Submit(const VkSubmitInfo &submit, VkFence fence);

    // Headless devices with the Khronos validation layer loaded count its error messages here. The counter outlives
    // the device, so errors reported while it is destroyed (leaks) can be checked afterwards. Never null.
    bool ValidationEnabled() const { return m_validationEnabled; }
    std::shared_ptr<const std::atomic<uint32_t>> ValidationErrors() const { return m_validationErrors; }

private:
    friend struct HeadlessDeviceAccess;

    VulkanDevice() = default;
    bool Init(const ExternalDevice &handles, std::string *reason);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VolkDeviceTable m_table{};
    DeviceFeatures m_features{};
    VkPhysicalDeviceProperties m_properties{};
    VkPhysicalDeviceMemoryProperties m_memoryProperties{};

    std::function<void()> m_lockQueue;
    std::function<void()> m_unlockQueue;

    // Owning mode only.
    bool m_owning = false;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT m_destroyMessenger = nullptr;
    PFN_vkDestroyInstance m_destroyInstance = nullptr;

    bool m_validationEnabled = false;
    std::shared_ptr<std::atomic<uint32_t>> m_validationErrors = std::make_shared<std::atomic<uint32_t>>(0);
};

} // namespace brimir::gpu
