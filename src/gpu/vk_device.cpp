#include "vk_device.hpp"

#include <algorithm>
#include <utility>

namespace brimir::gpu {

namespace {

void SetReason(std::string *reason, std::string text) {
    if (reason) {
        *reason = std::move(text);
    }
}

} // namespace

std::unique_ptr<VulkanDevice> VulkanDevice::WrapExternal(const ExternalDevice &handles, std::string *reason) {
    if (!handles.getInstanceProcAddr) {
        SetReason(reason, "no vkGetInstanceProcAddr");
        return nullptr;
    }
    // Only re-initialize when the handler differs: volkInitializeCustom drops volkInitialize()'s loader handle.
    if (volk::vkGetInstanceProcAddr != handles.getInstanceProcAddr) {
        volk::volkInitializeCustom(handles.getInstanceProcAddr);
    }
    // Always reload the instance-level functions: after a frontend context reset a new instance can have the
    // destroyed one's address, and pointers resolved through the old one (layers, extension commands) dangle.
    volk::volkLoadInstanceOnly(handles.instance);

    std::unique_ptr<VulkanDevice> device(new VulkanDevice());
    if (!device->Init(handles, reason)) {
        return nullptr;
    }
    return device;
}

bool VulkanDevice::Init(const ExternalDevice &handles, std::string *reason) {
    if (handles.instance == VK_NULL_HANDLE || handles.physical == VK_NULL_HANDLE || handles.device == VK_NULL_HANDLE ||
        handles.queue == VK_NULL_HANDLE) {
        SetReason(reason, "missing Vulkan instance, physical device, device or queue handle");
        return false;
    }
    if (!volk::vkGetPhysicalDeviceProperties || !volk::vkGetPhysicalDeviceMemoryProperties ||
        !volk::vkGetDeviceProcAddr) {
        SetReason(reason, "instance-level Vulkan functions could not be loaded");
        return false;
    }

    m_instance = handles.instance;
    m_physical = handles.physical;
    m_device = handles.device;
    m_queue = handles.queue;
    m_queueFamily = handles.queueFamily;

    // volkLoadDeviceTable resolves through volk's global vkGetDeviceProcAddr; use the frontend's when it gave one.
    const PFN_vkGetDeviceProcAddr instanceGdpa = volk::vkGetDeviceProcAddr;
    if (handles.getDeviceProcAddr) {
        volk::vkGetDeviceProcAddr = handles.getDeviceProcAddr;
    }
    volk::volkLoadDeviceTable(&m_table, m_device);
    volk::vkGetDeviceProcAddr = instanceGdpa;
    if (!m_table.vkQueueSubmit || !m_table.vkCreateComputePipelines) {
        SetReason(reason, "device-level Vulkan functions could not be loaded");
        return false;
    }

    volk::vkGetPhysicalDeviceProperties(m_physical, &m_properties);
    volk::vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memoryProperties);
    m_features.apiVersion =
        handles.apiVersion ? std::min(handles.apiVersion, m_properties.apiVersion) : m_properties.apiVersion;
    m_features.core = handles.enabledFeatures;
    return true;
}

VulkanDevice::~VulkanDevice() {
    if (!m_owning) {
        return;
    }
    if (m_device != VK_NULL_HANDLE && m_table.vkDestroyDevice) {
        m_table.vkDeviceWaitIdle(m_device);
        m_table.vkDestroyDevice(m_device, nullptr);
    }
    if (m_messenger != VK_NULL_HANDLE && m_destroyMessenger) {
        m_destroyMessenger(m_instance, m_messenger, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE && m_destroyInstance) {
        m_destroyInstance(m_instance, nullptr);
        // Don't leave volk's instance-level globals resolved through the destroyed instance.
        if (volk::volkGetLoadedInstance() == m_instance) {
            volk::volkLoadInstanceOnly(VK_NULL_HANDLE);
        }
    }
}

void VulkanDevice::SetQueueLockHooks(std::function<void()> lock, std::function<void()> unlock) {
    m_lockQueue = std::move(lock);
    m_unlockQueue = std::move(unlock);
}

VkResult VulkanDevice::Submit(const VkSubmitInfo &submit, VkFence fence) {
    if (m_lockQueue) {
        m_lockQueue();
    }
    const VkResult result = m_table.vkQueueSubmit(m_queue, 1, &submit, fence);
    if (m_unlockQueue) {
        m_unlockQueue();
    }
    return result;
}

} // namespace brimir::gpu
