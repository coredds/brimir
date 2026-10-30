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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct retro_hw_render_interface_vulkan;

namespace brimir::gpu {
class VulkanDevice;
}

namespace vkhost {

struct HostFrame;

// The core's submits and waits by kind (the Global Constraints of GPU stage 2: at most one frame submit per retro_run,
// plus an ops-only submit after a refused one; sync submits and their waits inside retro_run; fence waits in
// retro_serialize; nothing else outside retro_run and the context callbacks). A sync submit is one with the fence the
// core logs for its sync command context ("VDP1 sync submit fence 0x...").
struct SubmitWaitStats {
    uint64_t refusedSubmits = 0;   // --fail-submit-at: never reached the queue (not one of the run's submits)
    uint64_t syncSubmits = 0;      // VDP1 sync submits (in retro_run or a context callback; the core's fence)
    uint64_t contextSubmits = 0;   // in context_reset / context_destroy outside retro_run (a detach's sync submit)
    uint64_t extraOpsOnlyRuns = 0; // runs with an accepted submit after a refused one, with the core's ops-only line
    uint64_t outsideSubmits = 0;   // anywhere else (retro_serialize, retro_unserialize, ...): must stay 0
    uint64_t frameWaits = 0;       // in retro_run, for fences of earlier runs (the frame slots, hidden contexts)
    uint64_t syncWaits = 0;        // in retro_run, for a sync submit's fence
    uint64_t serializeWaits = 0;   // in retro_serialize / retro_serialize_size (submitted before it)
    uint64_t contextWaits = 0;     // in context_reset / context_destroy (vkDeviceWaitIdle included)
    uint64_t outsideWaits = 0;     // anywhere else: must stay 0
    void Add(const SubmitWaitStats& o) {
        refusedSubmits += o.refusedSubmits;
        syncSubmits += o.syncSubmits;
        contextSubmits += o.contextSubmits;
        extraOpsOnlyRuns += o.extraOpsOnlyRuns;
        outsideSubmits += o.outsideSubmits;
        frameWaits += o.frameWaits;
        syncWaits += o.syncWaits;
        serializeWaits += o.serializeWaits;
        contextWaits += o.contextWaits;
        outsideWaits += o.outsideWaits;
    }
};

// What the host is calling into the core (for SubmitWaitStats).
enum class CorePhase { Other, Run, Serialize, Unserialize, Context };

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

    // GET_AUDIO_VIDEO_ENABLE's video bit for the frame about to run. While false, presentation calls (set_image,
    // get_sync_index, wait_sync_index, vkQueueWaitIdle) are problems; submits are counted (HiddenSubmits).
    void SetVideoEnabled(bool enabled) { m_videoEnabled = enabled; }
    // Around each retro_run: more than one accepted frame submit (command buffers; sync submits not counted) in one run
    // is a problem, and so is a second submit after a refused one unless the core logged its ops-only line.
    void BeginRun();
    void EndRun();
    // The call into the core the next submits and waits belong to; returns the previous phase (restore it after).
    CorePhase SetPhase(CorePhase phase) {
        const CorePhase previous = m_phase;
        m_phase = phase;
        return previous;
    }
    // The core logged "VDP1 ops-only submit after a refused frame submit" (in the current retro_run).
    void NoteOpsOnlyAfterRefused() { m_runOpsOnlyLine = true; }
    // The core logged "VDP1 sync submit fence 0x..." (its sync command context's fence).
    // The latest one replaces the earlier (the core creates one per attach and destroys it at detach).
    void NoteSyncFence(uint64_t fence) { m_syncFence = fence; }
    // A submit the hook accepted (fence: its fence, commandBuffers: how many), a refused one, and waits.
    void OnSubmitted(uint64_t fence, uint32_t commandBuffers);
    void OnRefusedSubmit();
    void OnWaitFences(uint32_t count, const uint64_t* fences);
    void OnDeviceWaitIdle();
    const SubmitWaitStats& SubmitWaits() const { return m_submitWaits; }

    // Reads back the image of the last set_image (top-left w x h) as XRGB8888. Returns false when there is none.
    bool ReadImage(unsigned w, unsigned h, HostFrame& out);

    // --defer-image-read: the image of the last set_image as a frontend keeps it to read later (the handles copied
    // from the struct, as RetroArch's threaded video path does), pinned: destroying the image or its view before
    // ReadDeferred is a problem (the core must keep it until wait_sync_index for its sync index has returned).
    struct DeferredImage {
        uint64_t image = 0; // VkImage
        uint64_t view = 0;  // VkImageView
        int format = 0;     // VkFormat
        uint32_t baseMip = 0, baseLayer = 0;
        unsigned w = 0, h = 0;
    };
    bool CaptureImage(unsigned w, unsigned h, DeferredImage& out);
    // Reads a captured image back (as ReadImage) and unpins it.
    bool ReadDeferred(const DeferredImage& image, HostFrame& out);
    // The core destroyed an image or image view (wrapped vkDestroyImage / vkDestroyImageView).
    void OnDestroy(uint64_t handle, const char* what);
    // The frontend's end of a displayed frame: fences the current sync slot and advances the sync index.
    void EndFrame();

    void AddProblem(const std::string& text);
    const std::vector<std::string>& Problems() const { return m_problems; }
    uint64_t LockViolations() const { return m_lockViolations; }
    uint64_t HiddenGpuCalls() const { return m_hiddenGpuCalls; }
    uint64_t CoreSubmits() const { return m_coreSubmits; }
    uint64_t HiddenSubmits() const { return m_hiddenSubmits; }
    uint64_t MaxRunSubmits() const { return m_maxRunSubmits; }
    bool ValidationEnabled() const;
    uint32_t ValidationErrors() const;
    // Outlives the provider (errors reported while the instance is destroyed count too).
    std::shared_ptr<const std::atomic<uint32_t>> ValidationCounter() const;

    // Hooks behind the interface and the wrapped Vulkan functions (one provider per process).
    // A queue use by the core; commandBuffers: how many command buffers it submits.
    void CheckQueueUse(const char* function, uint32_t commandBuffers);
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
    // --fail-compute-on-reset: vkCreateComputePipelines fails for every context after the first (a reinit then ends
    // in the copy-only output).
    void SetFailComputeOnReset(bool fail) { m_failComputeOnReset = fail; }
    // --fail-alloc-above: the core's vkAllocateMemory fails with VK_ERROR_OUT_OF_DEVICE_MEMORY for allocations larger
    // than `bytes` (0: never); FailedAllocations() counts them.
    void SetFailAllocAbove(uint64_t bytes) { m_failAllocAbove = bytes; }
    uint64_t FailAllocAbove() const { return m_failAllocAbove; }
    void CountFailedAllocation() { ++m_failedAllocations; }
    uint64_t FailedAllocations() const { return m_failedAllocations; }
    // The core's device memory (wrapped vkAllocateMemory / vkFreeMemory): live bytes, the peak and the number of
    // successful allocations, over every context of the run.
    void OnAllocateMemory(uint64_t memory, uint64_t bytes) {
        std::lock_guard lock(m_memoryMutex);
        m_memory[memory] = bytes;
        m_memoryBytes += bytes;
        m_memoryPeak = std::max(m_memoryPeak, m_memoryBytes);
        ++m_memoryAllocations;
    }
    void OnFreeMemory(uint64_t memory) {
        std::lock_guard lock(m_memoryMutex);
        if (const auto it = m_memory.find(memory); it != m_memory.end()) {
            m_memoryBytes -= it->second;
            m_memory.erase(it);
        }
    }
    uint64_t CoreMemoryPeak() const { return m_memoryPeak; }
    uint64_t CoreMemoryLive() {
        std::lock_guard lock(m_memoryMutex);
        return m_memoryBytes;
    }
    uint64_t CoreMemoryAllocations() const { return m_memoryAllocations; }
    // --fail-output-images-above N: the core's vkCreateImage fails (VK_ERROR_OUT_OF_DEVICE_MEMORY) for images created
    // MUTABLE_FORMAT (the images it hands to set_image; libretro_vulkan.h requires the bit, its internal images do not
    // use it) wider than 704 * N (0: never).
    // --fail-submit-at F --fail-submits N: the core's command-buffer submits (vkQueueSubmit with command buffers) fail
    // in the runs F .. F + N - 1 (the host arms it in BeginRun); the core then repeats the previous frame.
    void SetFailSubmits(int firstRun, int count) {
        m_failSubmitFirst = firstRun;
        m_failSubmitCount = count;
    }
    void ArmFailedSubmit(int run) {
        m_failSubmitArmed = m_failSubmitFirst >= 0 && run >= m_failSubmitFirst && run < m_failSubmitFirst + m_failSubmitCount;
    }
    bool TakeFailedSubmit() {
        const bool fail = m_failSubmitArmed;
        m_failSubmitArmed = false;
        return fail;
    }

    // --defer-image-read: pins the image of a captured frame again (a dupe: the frontend repeats the image it holds).
    void PinImage(const DeferredImage& image) {
        m_pinned.insert(image.image);
        m_pinned.insert(image.view);
    }
    void SetFailOutputImagesAbove(uint32_t scale) { m_failOutputImagesAbove = scale; }
    uint32_t FailOutputImagesAbove() const { return m_failOutputImagesAbove; }
    // --fail-output-images-on-grow: the --fail-output-images-above limit takes effect once the ring grows.
    void SetFailOutputImagesOnGrow(uint32_t scale) { m_failOutputImagesOnGrow = scale; }

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
    bool m_failComputeOnReset = false;
    uint32_t m_contextsCreated = 0;
    bool m_failPipelines = false;
    uint64_t m_failAllocAbove = 0;
    uint64_t m_failedAllocations = 0;
    std::mutex m_memoryMutex;
    std::unordered_map<uint64_t, uint64_t> m_memory; // live core allocations: VkDeviceMemory -> bytes
    uint64_t m_memoryBytes = 0;
    uint64_t m_memoryPeak = 0;
    uint64_t m_memoryAllocations = 0;
    uint32_t m_failOutputImagesAbove = 0;
    uint32_t m_failOutputImagesOnGrow = 0;
    int m_failSubmitFirst = -1;
    int m_failSubmitCount = 1;
    bool m_failSubmitArmed = false;
    std::multiset<uint64_t> m_pinned; // handles of captured images and views not read yet

    bool ReadHandles(uint64_t image, int format, uint32_t baseMip, uint32_t baseLayer, unsigned w, unsigned h,
                     HostFrame& out);

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
    uint64_t m_hiddenSubmits = 0;
    // Accepted frame submits of the current run; the core's sync submits (fence == m_syncFence) are not counted
    uint64_t m_runSubmits = 0;
    uint64_t m_runRefused = 0;
    bool m_runOpsOnlyLine = false;
    uint64_t m_maxRunSubmits = 0;
    bool m_inRun = false;
    uint64_t m_runIndex = 0;
    CorePhase m_phase = CorePhase::Other;
    uint64_t m_syncFence = 0; // the core's VDP1 sync submit fence (NoteSyncFence)
    SubmitWaitStats m_submitWaits;
};

} // namespace vkhost
