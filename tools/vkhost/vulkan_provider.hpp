// Brimir - brimir_vkhost: headless libretro host
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// The Vulkan side of the host, standing in for RetroArch's vulkan driver: an instance and device of its own (with
// the validation layer when installed), the context negotiation (get_application_info, create_device2 through a
// create_device_wrapper), retro_hw_render_interface_vulkan with a 3-slot sync ring (mask 0b111, as RetroArch's
// threaded-video ring) and readback of every set_image image. It also checks the core's side of the contract: the
// queue is only used with lock_queue held (vkQueueSubmit is wrapped through get_device_proc_addr /
// get_instance_proc_addr), no wait happens while the lock is held, and frames with video disabled do no GPU work.
//
// The host's volk and the core's volk are separate copies (the core is a separately loaded library with its own
// globals), so the core's VulkanDevice::WrapExternal (volkInitializeCustom with the host's vkGetInstanceProcAddr,
// volkLoadInstanceOnly on the host's instance) never disturbs the host's function pointers, and vice versa.

#pragma once

#include <libretro.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct retro_hw_render_interface_vulkan;

namespace brimir::gpu {
class VulkanDevice;
}

namespace vkhost {

struct HostFrame;

// No Vulkan loader or no device meeting the baseline (tests skip on this).
class VulkanUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class VulkanProvider {
public:
    // Creates the host's instance and device. Throws VulkanUnavailable. A non-zero fakeApiVersion caps the apiVersion
    // vkGetPhysicalDeviceProperties reports to the core (--fake-api). alidation = false (--no-validation) leaves the
    // validation layer out, for timing runs.
    explicit VulkanProvider(uint32_t fakeApiVersion = 0, bool validation = true);
    ~VulkanProvider();
    VulkanProvider(const VulkanProvider&) = delete;
    VulkanProvider& operator=(const VulkanProvider&) = delete;

    // The device the core gets, as RetroArch builds it at video driver init: through the core's negotiation
    // interface when it registered one (get_application_info, then create_device2 on the host's GPU and, if that is
    // refused, with no GPU), else on the host's own device. Returns false (with Problems()) on failure.
    bool CreateContext(const retro_hw_render_context_negotiation_interface* negotiation);
    // Waits for the device, calls the core's destroy_device (when callCore and the device came from negotiation)
    // and destroys the device. The core must have run context_destroy before.
    void DestroyContext(bool callCore = true);
    bool ContextLive() const { return m_live; }

    // For GET_HW_RENDER_INTERFACE; valid while ContextLive().
    const retro_hw_render_interface* Interface() const;

    // GET_AUDIO_VIDEO_ENABLE's video bit for the frame about to run; GPU calls while false are problems.
    void SetVideoEnabled(bool enabled) { m_videoEnabled = enabled; }

    // Reads back the image of the last set_image (top-left w x h) as XRGB8888. Returns false when there is none.
    bool ReadImage(unsigned w, unsigned h, HostFrame& out);
    // The frontend's end of a displayed frame: fences the current sync slot and advances the sync index.
    void EndFrame();

    void AddProblem(const std::string& text);
    const std::vector<std::string>& Problems() const { return m_problems; }
    uint64_t LockViolations() const { return m_lockViolations; }
    uint64_t HiddenGpuCalls() const { return m_hiddenGpuCalls; }
    uint64_t CoreSubmits() const { return m_coreSubmits; }
    bool ValidationEnabled() const;
    uint32_t ValidationErrors() const;
    // Outlives the provider (errors reported while the instance is destroyed count too).
    std::shared_ptr<const std::atomic<uint32_t>> ValidationCounter() const;

    // Hooks behind the interface and the wrapped Vulkan functions (one provider per process).
    void CheckQueueUse(const char* function);
    void CheckWait(const char* function);
    void CheckFrameCall(const char* function);
    void OnSetImage(const void* image, uint32_t numSemaphores, uint32_t srcQueueFamily);
    void OnWaitSyncIndex();
    void LockQueue();
    void UnlockQueue();
    uint32_t SyncIndex() const { return m_syncIndex; }
    uint32_t FakeApiVersion() const { return m_fakeApi; }
    uint32_t SyncSlots() const { return m_syncSlots; }
    bool FailPipelines() const { return m_failPipelines; }

    // The sync ring size (mask = (1 << count) - 1, at most 3). Changing it waits for the device first, as the
    // contract promises. --grow-mask starts at 1 and grows to 3.
    void SetSyncSlots(uint32_t count);
    // --fail-compute-on-grow: once the ring grows, vkCreateComputePipelines fails for the core.
    void SetFailComputeOnGrow(bool fail) { m_failComputeOnGrow = fail; }

    struct RealFunctions; // the unwrapped device functions
    const RealFunctions& Real() const;

private:
    struct Slot;

    void SelfTest();
    void HostLock();
    void HostUnlock();

    std::unique_ptr<brimir::gpu::VulkanDevice> m_host; // instance + the host's own device
    std::unique_ptr<brimir::gpu::VulkanDevice> m_ctxView; // non-owning view of a negotiated device
    brimir::gpu::VulkanDevice* m_ctx = nullptr; // the device the core uses (m_ctxView or m_host)
    void* m_ownedDevice = nullptr; // VkDevice created through create_device2 (destroyed by the host)
    void (*m_destroyDevice)() = nullptr;
    std::unique_ptr<RealFunctions> m_real;
    std::unique_ptr<retro_hw_render_interface_vulkan> m_iface;
    bool m_live = false;
    uint32_t m_fakeApi = 0;
    uint32_t m_syncSlots = 3;
    bool m_failComputeOnGrow = false;
    bool m_failPipelines = false;

    std::vector<Slot> m_slots;
    uint32_t m_syncIndex = 0;
    const void* m_image = nullptr; // the core's retro_vulkan_image (the frontend keeps the pointer)

    struct Readback;
    std::unique_ptr<Readback> m_readback;

    std::mutex m_queueMutex;
    std::atomic<std::thread::id> m_lockOwner{};
    bool m_hostLocked = false;
    bool m_videoEnabled = true;
    bool m_selfTest = false;

    std::vector<std::string> m_problems;
    uint64_t m_lockViolations = 0;
    uint64_t m_hiddenGpuCalls = 0;
    uint64_t m_coreSubmits = 0;
};

} // namespace vkhost
