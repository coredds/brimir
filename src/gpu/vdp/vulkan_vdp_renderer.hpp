#pragma once

// The Vulkan VDP renderer: VDP2 on the GPU (stage 1), VDP1 in software or on the GPU (stage 2). Installed with
// VDP::UseExternalRenderer (type External).
//
// - VDP2 runs in the front half (Vdp2FrontEnd, CPU, always) and, while a device is attached, the back half
//   (Vdp2VulkanBackend). Each frame is recorded by the front half; the caller replays it into its command buffer with
//   RecordPendingFrame and presents CompositeBuffer(). Without a device the front half still runs (VDPState stays
//   current) and its frames are dropped at VDP2EndFrame.
// - VDP1 has two modes (Vdp1Mode; SetVdp1Mode, applied at AttachDevice):
//   - Software (the stage 1 path, and always while detached): VDP1 runs in an inner SoftwareVDPRenderer (the software
//     VDP1 backend), which gets the VDP1 calls, resolution changes, resets, save-state calls and enhancements
//     (docs/ymir-hw-renderer-notes/stage1-hybrid-renderer.md section 1.2). Its threaded VDP2 is never enabled; VDP1
//     threading follows EnableThreadedVDP1. Its callbacks stay empty: this renderer fires Callbacks itself. The display
//     framebuffer's sprite slices (main; alt with deinterlacing in double density; mesh with transparent meshes) are
//     copied into the frame record at the frame's first VDP2 line, after the framebuffer is synchronized when VDP1
//     erased it without a swap since the last capture (section 1.5).
//   - Gpu (while attached): the VDP1 front half (Vdp1FrontEnd, emulator thread) turns the VDP1 calls into op lists and
//     the VDP1 back half (Vdp1VulkanBackend) replays them into the VDP2 back half's sprite framebuffer, which is the
//     GPU FBRAM (no sprite slices; stage2-vdp1-research.md sections 2.3 and 5). The ops before a VDP2 frame's end are
//     replayed before that frame's VDP2 work and the ops after it (the BlankingAndSync display erase) after it, in the
//     same command buffer; RecordPendingVdp1Work replays pending ops without a frame. Entering Gpu mode uploads the
//     CPU's FBRAM (VDPState main, the inner renderer's alt and mesh); leaving it (DetachDevice) drains the GPU into
//     VDPState and the inner renderer (alt excepted, stale as across save states).
//     TODO(Task 6): CPU framebuffer reads (VDP1SyncFB) and save states drain the GPU with a submit and a fence wait
//     each time the GPU holds newer data (SyncVdp1ToCpu); the CPU shadow model of research section 3.5 replaces this.

#include "gpu/vdp1/vdp1_front.hpp"
#include "gpu/vdp1/vdp1_vulkan_backend.hpp"
#include "gpu/vdp2/vdp2_front.hpp"
#include "gpu/vdp2/vdp2_vulkan_backend.hpp"
#include "gpu/vk_device.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_hw_base.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace brimir::gpu {

enum class Vdp1Mode { Software, Gpu };

class VulkanVDPRenderer final : public ymir::vdp::HardwareVDPRendererBase {
public:
    VulkanVDPRenderer(ymir::vdp::VDPState &state, ymir::vdp::config::VDP2DebugRender &vdp2DebugRenderOptions,
                      const ymir::vdp::config::VDP2AccessPatternsConfig &vdp2AccessPatternsConfig);
    ~VulkanVDPRenderer() override;

    // -------------------------------------------------------------------------
    // Device

    // Creates the back half on `device` (frameContexts upload contexts, see Vdp2VulkanBackend) at the requested
    // internal resolution (RequestedInternalScale, at most maxScale: what the caller can present), allocated for it
    // directly. When that cannot be allocated it tries the previous effective scale (if lower), then 1, each allocated
    // directly (never 1 then another), and logs the effective one; the request is kept (the next attach tries it
    // again). The next frame uploads all of VRAM and CRAM and starts from a cleared composite. Attached() is false if
    // the back half failed. When already attached, the old back half is destroyed immediately: the caller must have
    // waited for the device to be idle first (as for DetachDevice).
    // With Vdp1Mode::Gpu requested it also creates the VDP1 back half (one more upload context, for the sync submits)
    // and enters Gpu mode, uploading the CPU's FBRAM with the next command buffer; if that fails VDP1 stays in software.
    void AttachDevice(VulkanDevice &device, uint32_t frameContexts, uint32_t maxScale = 4);
    // Drops the pending frame and destroys the GPU resources. The caller has waited for the device to be idle. In Gpu
    // VDP1 mode the pending VDP1 ops are replayed and FBRAM is read back first (a submit and a fence wait on the still
    // valid device), so VDP1 continues in software from the GPU's framebuffers.
    void DetachDevice();
    // Grows the back half's frame contexts to at least `frameContexts` in place (a larger sync index mask): the
    // composite (the other field when weaving), the GPU copies of VRAM/CRAM and the pending frame are kept. The work of
    // every context whose index changes meaning must have completed. False when detached.
    bool EnsureFrameContexts(uint32_t frameContexts);
    // Forgets the back half without any Vulkan call (its device may already be gone): leaks its objects.
    void AbandonDevice();
    bool Attached() const { return m_backend != nullptr; }

    // The back half, for tests (nullptr while detached).
    vdp2::Vdp2VulkanBackend *Backend() { return m_backend.get(); }
    // The front half, for tests.
    const vdp2::Vdp2FrontEnd &FrontEnd() const { return m_front; }

    void EnableThreadedVDP1(bool enable);
    // The last EnableThreadedVDP1 value (VDP1 starts unthreaded).
    bool ThreadedVDP1() const { return m_threadedVDP1; }

    // Fired at the end of every VDP2 frame, after Callbacks.VDP2DrawFinished, with the frame's (native) size.
    std::function<void(uint32_t w, uint32_t h)> FrameReady;
    // Diagnostics (brimir_vkhost --probe): when set, fired at the end of every VDP2 frame, before FrameReady, with the
    // front half's feature counters and the CPU time the frame took in the front half (VDP2BeginFrame, the lines with
    // their sprite captures, VDP2EndFrame; register and memory writes, which only mark state dirty, are not timed).
    std::function<void(const vdp2::Vdp2FeatureCounters &features, double frontSeconds)> FrameProbe;

    // A recorded frame waits for RecordPendingFrame (only while attached).
    bool HasPendingFrame() const;
    // Replays the pending frame into cmd (see Vdp2VulkanBackend::Record) and consumes it. When recording fails, the
    // next frame uploads everything again. Returns false when there was nothing to record or recording failed.
    // In Gpu VDP1 mode it also replays the pending VDP1 ops (whether or not a frame is pending): those before the
    // frame's end first, then the frame, then the ops after it (RecordedVdp1Work says whether any were recorded).
    bool RecordPendingFrame(uint32_t context, VkCommandBuffer cmd);
    // The command buffer of the last successful RecordPendingFrame (or RecordPendingVdp1Work) will not be submitted
    // (submission failed, or the frame was abandoned): its uploads and one-time work (composite clear) are redone with
    // the next frame, and the VDP1 ops it carried are pending again (replayed before any later op). The caller must
    // call this whenever a recorded command buffer does not reach the queue.
    void RecordedFrameDropped();

    // VDP1 on the GPU (see the top of this file). The mode applies at the next AttachDevice (Software until then);
    // GpuVdp1() is the effective mode: Gpu only while attached with a VDP1 back half.
    void SetVdp1Mode(Vdp1Mode mode) { m_vdp1Mode = mode; }
    Vdp1Mode RequestedVdp1Mode() const { return m_vdp1Mode; }
    bool GpuVdp1() const { return m_gpuVdp1; }
    // Gpu mode: VDP1 ops wait for a command buffer (RecordPendingFrame or RecordPendingVdp1Work).
    bool HasPendingVdp1Work() const;
    // Gpu mode: replays the pending VDP1 ops into cmd without a VDP2 frame (the ops-only submit; with a frame pending,
    // only the ops before its end). False when nothing was recorded or recording failed (the ops stay pending).
    bool RecordPendingVdp1Work(uint32_t context, VkCommandBuffer cmd);
    // The last RecordPendingFrame or RecordPendingVdp1Work recorded VDP1 work: a command buffer that is not submitted
    // must then be reported with RecordedFrameDropped even without a frame.
    bool RecordedVdp1Work() const { return !m_vdp1Recorded.empty(); }
    // A VDP2 frame was recorded and submitted by a VDP1 sync (SyncVdp1ToCpu, ordered with the ops after it) since the
    // last call: the composite holds it (the caller presents the composite as it is).
    bool TakeFrameRecordedBySync();
    // GPU VDP1 sync submits (each with a fence wait) since construction.
    uint64_t Vdp1SyncSubmits() const { return m_vdp1SyncSubmits; }
    // The VDP1 back half (nullptr unless GpuVdp1()) and front half, for tests.
    vdp1::Vdp1VulkanBackend *Vdp1Backend() { return m_vdp1Backend.get(); }
    const vdp1::Vdp1FrontEnd &Vdp1Front() const { return m_vdp1Front; }
    // Test hook: the VDP1 front half's batch capacities.
    void SetVdp1BatchLimitsForTest(const vdp1::Vdp1BatchLimits &limits) { m_vdp1Front.SetBatchLimitsForTesting(limits); }
    // The persistent composite (VK_NULL_HANDLE while detached): InternalScale() x the frame size FrameReady reports,
    // pitch InternalScale() * HRes.
    VkBuffer CompositeBuffer() const;

    // Requests internal resolution n (1 to 4) of the VDP2 background layers and the composite, from the next recorded
    // frame on (Vdp2VulkanBackend::SetScale: the previous composite is rescaled into the new one by that frame, so a
    // woven field survives). Contract while attached: every submitted command buffer that used the renderer's resources
    // has completed (the caller waited for their fences) and no command buffer recorded by RecordPendingFrame is
    // still waiting to be submitted (report a dropped one with RecordedFrameDropped first): the descriptor sets are
    // rewritten and the old resources destroyed here. While detached it is kept for the next AttachDevice. The request
    // is remembered either way; false, with the previous effective scale kept and the failure logged, when the
    // resources cannot be allocated; false (nothing changes) for an n out of range.
    bool SetInternalScale(uint32_t n);
    // The effective internal resolution (the back half's while attached; while detached the last effective one, or
    // the request made while detached).
    uint32_t InternalScale() const { return m_internalScale; }
    // The last internal resolution requested with SetInternalScale (1 until then).
    uint32_t RequestedInternalScale() const { return m_requestedScale; }
    // Changes the effective internal resolution only (as SetInternalScale, same contract), keeping the request: for a
    // caller that cannot present the requested one (its output images could not be allocated).
    bool FallBackInternalScale(uint32_t n);
    // Where the back half logs its allocation failures and the renderer the effective scale after a fallback (stderr
    // when unset); kept across AttachDevice.
    void SetLog(std::function<void(const std::string &)> log);
    // Test hook: every back half this renderer creates (and SetInternalScale) fails to allocate the resources of an
    // internal resolution that need more than `bytes` (0: no limit), as
    // Vdp2VulkanBackend::SetScaleAllocationLimitForTest.
    void SetScaleAllocationLimitForTest(VkDeviceSize bytes);

    // -------------------------------------------------------------------------
    // IVDPRenderer

    bool IsValid() const override { return true; }
    void Reset(bool hard) override;

    void PreSaveStateSync() override;
    void PostLoadStateSync() override;
    void SaveState(ymir::savestate::VDPSaveState::VDPRendererSaveState &state) override;
    bool ValidateState(const ymir::savestate::VDPSaveState::VDPRendererSaveState &state) const override;
    void LoadState(const ymir::savestate::VDPSaveState::VDPRendererSaveState &state) override;

    void VDP1WriteVRAM(uint32_t address, uint8_t value) override;
    void VDP1WriteVRAM(uint32_t address, uint16_t value) override;
    void VDP1SyncFB() override;
    void VDP1DebugSyncFB() override;
    void VDP1WriteFB(uint32_t address, uint8_t value) override;
    void VDP1WriteFB(uint32_t address, uint16_t value) override;
    void VDP1WriteReg(uint32_t address, uint16_t value) override;

    void VDP2WriteVRAM(uint32_t address, uint8_t value) override;
    void VDP2WriteVRAM(uint32_t address, uint16_t value) override;
    void VDP2WriteCRAM(uint32_t address, uint8_t value) override;
    void VDP2WriteCRAM(uint32_t address, uint16_t value) override;
    void VDP2WriteReg(uint32_t address, uint16_t value) override;

    void VDP1EraseFramebuffer(uint64_t cycles) override;
    void VDP1SwapFramebuffer() override;
    void VDP1BeginFrame() override;
    void VDP1ExecuteCommand(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control) override;
    void VDP1EndFrame() override;

    void VDP2SetResolution(uint32_t h, uint32_t v, bool exclusive) override;
    void VDP2SetField(bool odd) override;
    void VDP2LatchTVMD() override;
    void VDP2BeginFrame() override;
    void VDP2RenderLine(uint32_t y) override;
    void VDP2EndFrame() override;

    void UpdateEnabledLayers() override;

    void DumpExtraVDP1Framebuffers(std::ostream &out) const override;

protected:
    void UpdateEnhancements() override;

private:
    // Gpu mode: an op list of the front half, or an FBRAM upload (sliceMask != 0, fbram holds all eight slices).
    struct Vdp1Work {
        vdp1::Vdp1OpList ops;
        std::vector<uint8_t> fbram;
        uint32_t sliceMask = 0;
    };
    // What RecordVdp1Work does with a pending VDP2 frame: record it between the ops before and after its end; keep it
    // (only the ops before its end); record it only if ops after its end are pending, else keep it (a sync); ignore
    // it (every op; the caller drops the frame).
    enum class FramePolicy { Record, Keep, Sync, Ignore };
    using MeshFBRAM = std::array<std::array<ymir::vdp::SpriteFB, 2>, 2>;

    void Log(const std::string &text) const;
    void SyncDisplayErase();
    void CaptureSpriteSlices();
    void CaptureAltSpriteSlices();

    // Gpu VDP1 mode
    bool EnterGpuVdp1(VulkanDevice &device, uint32_t contexts);
    void LeaveGpuVdp1();
    void DestroyGpuVdp1();
    bool RecordVdp1Work(uint32_t context, VkCommandBuffer cmd, FramePolicy policy, bool &frameRecorded);
    bool ReplayVdp1Work(const Vdp1Work &work, uint32_t context, VkCommandBuffer cmd);
    void QueueVdp1Ops(); // the front half's pending ops, in order
    void QueueFbramUpload(uint32_t sliceMask, const ymir::vdp::SpriteFB *alt0, const ymir::vdp::SpriteFB *alt1);
    // TODO(Task 6): the CPU shadow model replaces this full drain. Replays every pending VDP1 op in a sync submit,
    // waits for it and reads FBRAM back into VDPState (main) and m_meshShadow, when the GPU may hold newer data.
    bool SyncVdp1ToCpu(FramePolicy policy);
    uint64_t Vdp1GpuOpCount() const;

    ymir::vdp::VDPState &m_state;
    std::unique_ptr<ymir::vdp::SoftwareVDPRenderer> m_vdp1; // the software VDP1 backend
    vdp2::Vdp2FrontEnd m_front;
    std::unique_ptr<vdp2::Vdp2VulkanBackend> m_backend;

    Vdp1Mode m_vdp1Mode = Vdp1Mode::Software;
    bool m_gpuVdp1 = false;
    vdp1::Vdp1FrontEnd m_vdp1Front;
    std::unique_ptr<vdp1::Vdp1VulkanBackend> m_vdp1Backend;
    std::vector<Vdp1Work> m_vdp1Queue;        // pending, in emulation order
    size_t m_vdp1BeforeFrame = 0;             // m_vdp1Queue[0, this) precede the pending VDP2 frame's end
    std::vector<Vdp1Work> m_vdp1Recorded;     // what the last record call recorded (for RecordedFrameDropped)
    std::unique_ptr<MeshFBRAM> m_meshShadow;  // Gpu mode: the mesh framebuffers as of the last sync or load
    bool m_stateLoaded = false;               // LoadState since the last PostLoadStateSync
    uint64_t m_vdp1SyncedOps = 0;             // Vdp1GpuOpCount at the last sync: VDPState and the shadow are current
    uint64_t m_vdp1SyncSubmits = 0;
    bool m_frameRecordedBySync = false;
    VulkanDevice *m_device = nullptr;
    uint32_t m_frameContexts = 0;             // the caller's; Gpu mode adds the sync context (index m_frameContexts)
    VkCommandPool m_syncPool = VK_NULL_HANDLE;
    VkCommandBuffer m_syncCmd = VK_NULL_HANDLE;
    VkFence m_syncFence = VK_NULL_HANDLE;

    uint32_t m_HRes = ymir::vdp::kDefaultResH;
    uint32_t m_VRes = ymir::vdp::kDefaultResV;
    bool m_exclusiveMonitor = false;
    bool m_threadedVDP1 = false;
    uint32_t m_internalScale = 1;
    uint32_t m_requestedScale = 1;
    VkDeviceSize m_scaleAllocationLimit = 0;
    std::function<void(const std::string &)> m_log;
    bool m_resolutionChanged = false;
    bool m_displayEraseSinceCapture = false; // VDP1 erased a framebuffer without a swap since the last capture
    bool m_slicesCaptured = false;           // the open frame has its main (and mesh0) sprite slices
    bool m_altSlicesCaptured = false;        // the open frame has its alt (and mesh1) sprite slices
    double m_frontSeconds = 0;               // FrameProbe: front-half time of the open frame
};

} // namespace brimir::gpu
