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
//     CPU's FBRAM (VDPState main, the inner renderer's alt and mesh); leaving it (DetachDevice) syncs the GPU into
//     VDPState and the inner renderer (main, alt, mesh, doubleV).
//
// GPU-authoritative FBRAM with a lazily synchronised CPU shadow (stage2-vdp1-research.md section 3.5). The shadow is
// VDPState's main fb0/fb1 (which the CPU reads and the save state stores), m_meshShadow (the save state's meshFBRAM)
// and, while the deinterlace enhancement is on, m_altShadow (alt is never saved, as software; it is shadowed only so a
// lost device keeps it, rule 8). Each shadowed slice is, against the emulated FBRAM:
// - Valid: the CPU copy holds every GPU op emitted so far (and every CPU write);
// - Pending: a readback committed with a submitted command buffer holds every GPU op emitted so far;
// - GpuNewer: a GPU op was emitted after the latest readback.
// Rules:
// 1. Every op that changes FBRAM on the GPU (a draw command: main and, with transparent meshes, mesh of the draw
//    framebuffer, alt too with deinterlacing; an erase: the same of the display framebuffer) makes its slices
//    GpuNewer when it is emitted.
// 2. Every command buffer the renderer records (frame, hidden, ops-only, sync) ends with a copy of each GpuNewer
//    shadowed slice whose ops (and CPU writes) it covers into the context's host-cached readback buffer;
//    FrameSubmitted commits it (Pending, with the context's fence). A slice keeps every committed readback not merged
//    yet (one per context at most), each with the CPU writes made since its commit (its write mask), so the newest one
//    that has completed can always be merged.
// 3. A CPU framebuffer write (VDPState already holds it) is an op with its value and sets the bit of every committed
//    readback's write mask; it mirrors to alt as software does (an op) and never touches the meshes. No wait: the
//    main slice does not become GpuNewer.
// 4. VDP1SyncFB (a CPU read of the draw framebuffer): Valid returns at once; Pending waits for the newest readback's
//    fence and merges (byte i keeps the CPU's value when the readback's write mask has it, else takes the
//    readback's); GpuNewer makes a sync submit (every pending op, the pending VDP2 frame ordered with them, the
//    readbacks), waits and merges. So a burst of reads costs one sync submit, the first.
// 5. PreSaveStateSync: rule 4 for main and mesh. Between retro_run calls every slice is Valid or Pending (rule 2 and
//    the ops-only submit), so retro_serialize only waits for fences.
// 6. PostLoadStateSync: main and mesh uploaded from the loaded state, Valid; alt left as it is (as software).
// 7. Reset: main (hard reset), alt and mesh uploaded as the reset left them, after the ops so far.
// 8. DetachDevice syncs (every slice, alt included) and hands the framebuffers to the inner renderer
//    (SetAltFBRAM, SetMeshFBRAM). AbandonDevice (the device lost) makes no Vulkan call: the inner renderer continues
//    from the shadow (main, mesh, and alt while deinterlacing) as of the last merge. Merges happen at
//    PollVdp1Readbacks (the start of each retro_run: every readback whose command buffer has completed, no wait) and
//    whenever a context is reused (its previous command buffer has completed: the caller waited for it), so what is
//    lost is the VDP1 work of every retro_run the GPU had not completed when last polled: at most as many runs as the
//    caller has frame contexts (sync slots plus hidden contexts; typically 1-3 frames). Avoiding it would need waits.

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
    // With Vdp1Mode::Gpu requested it also creates the VDP1 back half (one more upload context, the sync submits')
    // and enters Gpu mode, uploading the CPU's FBRAM with the next command buffer; if that fails VDP1 stays in software.
    void AttachDevice(VulkanDevice &device, uint32_t frameContexts, uint32_t maxScale = 4);
    // Drops the pending frame and destroys the GPU resources. The caller has waited for the device to be idle. In Gpu
    // VDP1 mode the pending VDP1 ops are replayed and FBRAM is read back first (a sync submit, see SetSyncSubmit, on the
    // still valid device), so VDP1 continues in software from the GPU's framebuffers.
    void DetachDevice();
    // Grows the back half's frame contexts to at least `frameContexts` in place (a larger sync index mask): the
    // composite (the other field when weaving), the GPU copies of VRAM/CRAM and the pending frame are kept. The work of
    // every context whose index changes meaning must have completed. False when detached.
    bool EnsureFrameContexts(uint32_t frameContexts);
    // Forgets the back half without any Vulkan call (its device may already be gone): leaks its objects. In Gpu VDP1
    // mode VDP1 continues in software from the CPU shadow (rule 8 at the top of this file).
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
    // In Gpu VDP1 mode also the VDP1 times since the previous call (Vdp1ProbeTimes).
    struct Vdp1ProbeTimes {
        // CPU time in the VDP1 front half: commands, erases, CPU framebuffer writes, frame begin/boundary and taking the
        // ops (VRAM writes, which only mark chunks dirty, are not timed)
        double frontSeconds = 0;
        // CPU time recording the VDP1 work into command buffers (uploads, passes, readbacks; Record* of the back half)
        double recordSeconds = 0;
        // GPU time of the VDP1 work (timestamps; collected when a context's command buffer is reused, so it trails)
        double gpuSeconds = 0;
    };
    std::function<void(const vdp2::Vdp2FeatureCounters &features, double frontSeconds, const Vdp1ProbeTimes &vdp1)>
        FrameProbe;

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
    // The command buffer of the last RecordPendingFrame or RecordPendingVdp1Work for `context` was submitted; `fence`
    // signals its completion (VK_NULL_HANDLE: it has already completed). The caller must call this for every such
    // command buffer it submits (Gpu VDP1 mode: it commits the command buffer's readbacks, rule 2). The fence must stay
    // valid, and not be reset, until the context's next command buffer begins (the next record call with `context`) or
    // OwnWorkCompleted.
    void FrameSubmitted(uint32_t context, VkFence fence);
    // Every command buffer the caller submitted has completed (it waited for their fences, and may destroy them now).
    void OwnWorkCompleted();
    // Gpu VDP1 mode: merges the readbacks whose command buffers have completed (vkGetFenceStatus, no wait) into the
    // CPU shadow. The caller calls it at the start of each retro_run, which bounds what AbandonDevice loses.
    void PollVdp1Readbacks();

    // Gpu VDP1 mode: the caller's sync submit (rules 4, 5 and 8 at the top of this file). `submit` begins a command
    // buffer of its own (a pool and fence of its own; the renderer records into its sync context), calls `record`
    // (false: nothing to submit, end it without submitting), submits it under the queue lock and waits for its fence.
    // It returns true only when the command buffer was submitted and has completed. It refuses (false) where GPU work
    // is not allowed (outside retro_run and the context callbacks). Kept across AttachDevice. Unset: no sync submits
    // (a read or save that needs newer GPU data gets the CPU's older copy, logged).
    using SyncRecord = std::function<bool(VkCommandBuffer)>;
    void SetSyncSubmit(std::function<bool(const SyncRecord &record)> submit) { m_syncSubmit = std::move(submit); }
    // Why the renderer made a sync submit or a fence wait.
    enum class Vdp1SyncKind : uint32_t { Read, Save, Detach, Dump };
    static constexpr uint32_t kVdp1SyncKinds = 4;
    struct Vdp1SyncStats {
        std::array<uint64_t, kVdp1SyncKinds> syncSubmits{}; // a submit and a fence wait, by kind
        std::array<uint64_t, kVdp1SyncKinds> fenceWaits{};  // a wait for a submitted readback (no submit), by kind
        uint64_t refusedSyncs = 0;    // a sync submit was needed but refused or failed (older data used)
        uint64_t readbackSlices = 0;  // slices read back at the end of committed command buffers
        uint64_t mergedSlices = 0;    // readback slices merged into the shadow
        double syncSeconds = 0, maxSyncSeconds = 0; // sync submits: record, submit, wait and merge
        double waitSeconds = 0, maxWaitSeconds = 0; // fence waits and their merges
        double mergeSeconds = 0;                    // every merge (also those in the two above)
    };
    const Vdp1SyncStats &Vdp1Sync() const { return m_syncStats; }
    uint64_t Vdp1SyncSubmits() const;

    // VDP1 on the GPU (see the top of this file). The mode applies at the next AttachDevice (Software until set; the
    // libretro core sets Gpu unless BRIMIR_VDP1_GPU=0); GpuVdp1() is the effective mode: Gpu only while attached with a
    // VDP1 back half.
    void SetVdp1Mode(Vdp1Mode mode) { m_vdp1Mode = mode; }
    Vdp1Mode RequestedVdp1Mode() const { return m_vdp1Mode; }
    bool GpuVdp1() const { return m_gpuVdp1; }
    // Gpu mode: VDP1 ops (or the readback of a slice they changed, rule 2) wait for a command buffer
    // (RecordPendingFrame or RecordPendingVdp1Work).
    bool HasPendingVdp1Work() const;
    // Gpu mode: replays the pending VDP1 ops into cmd without a VDP2 frame (the ops-only submit; with a frame pending,
    // only the ops before its end). False when nothing was recorded or recording failed (the ops stay pending).
    bool RecordPendingVdp1Work(uint32_t context, VkCommandBuffer cmd);
    // The last RecordPendingFrame or RecordPendingVdp1Work recorded VDP1 work: a command buffer that is not submitted
    // must then be reported with RecordedFrameDropped even without a frame.
    bool RecordedVdp1Work() const { return !m_vdp1Recorded.empty() || m_proposed.mask != 0; }
    // A VDP2 frame was recorded and submitted by a VDP1 sync submit (ordered with the ops after it) since the last
    // call: the composite holds it (the caller presents the composite as it is).
    bool TakeFrameRecordedBySync();
    // Gpu VDP1 mode: the last PreSaveStateSync could not bring the CPU copy of main and mesh current (the sync submit
    // refused or failed, the readback buffer not allocated, ops left pending): a state saved now would hold older
    // framebuffers, and the caller must not use it (CoreWrapper::SaveState fails). Logged once per renderer.
    bool SaveSyncFailed() const { return m_saveSyncFailed; }
    // Test hook: every PreSaveStateSync fails as above.
    void FailSaveSyncForTest(bool fail) { m_failSaveSync = fail; }
    // The inner software renderer (VDP1 in Software mode and while detached), for tests.
    const ymir::vdp::SoftwareVDPRenderer &InnerVdp1ForTest() const { return *m_vdp1; }
    // The VDP1 back half (nullptr unless GpuVdp1()) and front half, for tests.
    vdp1::Vdp1VulkanBackend *Vdp1Backend() { return m_vdp1Backend.get(); }
    const vdp1::Vdp1FrontEnd &Vdp1Front() const { return m_vdp1Front; }
    // Test hook: the VDP1 front half's batch capacities.
    void SetVdp1BatchLimitsForTest(const vdp1::Vdp1BatchLimits &limits) { m_vdp1Front.SetBatchLimitsForTesting(limits); }
    // Test hook: VRAM hazard tracking off (wrong output: shows that a test notices).
    void SetVdp1HazardTrackingForTest(bool enabled) { m_vdp1Front.SetHazardTrackingForTesting(enabled); }
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

    // Gpu VDP1 mode (debug only): one Dump sync submit makes main, mesh and alt current, then alt and mesh are written
    // as software writes them. VDP::DumpVDP1Framebuffers writes main (VDPState) before calling this, so its main is
    // the shadow as of the last merge: call VDP1SyncFB (or PreSaveStateSync) first for current main framebuffers. No
    // Brimir caller dumps framebuffers.
    void DumpExtraVDP1Framebuffers(std::ostream &out) const override;

protected:
    void UpdateEnhancements() override;

private:
    // Gpu mode: an op list of the front half, or an FBRAM upload (sliceMask != 0, fbram holds all eight slices). seq:
    // the emissions (m_vdp1Seq) it and the work before it cover.
    struct Vdp1Work {
        vdp1::Vdp1OpList ops;
        std::vector<uint8_t> fbram;
        uint32_t sliceMask = 0;
        uint64_t seq = 0;
    };
    // The CPU shadow's state of one FBRAM slice (the top of this file), in emissions (m_vdp1Seq: every GPU op and CPU
    // write increments it).
    // Bytes of a main slice the CPU wrote since a readback was committed (rule 3).
    struct WriteMask {
        std::array<uint64_t, vdp1::kFbramSliceSize / 64> bits{};
        bool any = false;
    };
    // A committed readback not merged yet: it holds every emission up to seq, in `context`'s readback buffer.
    struct PendingReadback {
        uint64_t seq = 0;
        uint32_t context = 0;
        std::unique_ptr<WriteMask> mask; // main slices
    };
    struct SliceSync {
        uint64_t lastTouch = 0; // the last GPU op that changes the slice
        uint64_t lastWrite = 0; // the last CPU write into it (main slices)
        uint64_t valid = 0;     // the CPU copy holds every emission up to this one (Valid: valid >= lastTouch)
        std::vector<PendingReadback> pending; // oldest first
    };
    // The readback the last record call appended, committed by FrameSubmitted.
    struct ProposedReadback {
        int32_t context = -1;
        uint32_t mask = 0;
        uint64_t covered = 0; // emissions the command buffer covers
        std::array<uint64_t, 8> seq{};
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
    // Replays the pending work into cmd (see FramePolicy), then the readbacks of rule 2 plus the slices of `extra` (when
    // covered). False when recording failed (what was recorded is in m_vdp1Recorded, the rest stays queued).
    bool RecordVdp1Work(uint32_t context, VkCommandBuffer cmd, FramePolicy policy, bool &frameRecorded,
                        uint32_t extra = 0);
    bool ReplayVdp1Work(const Vdp1Work &work, uint32_t context, VkCommandBuffer cmd);
    void QueueVdp1Ops(); // the front half's pending ops, in order
    void QueueFbramUpload(uint32_t sliceMask, const ymir::vdp::SpriteFB *alt0, const ymir::vdp::SpriteFB *alt1);
    // CPU shadow (the top of this file)
    uint32_t ShadowedSlices() const;                     // main, mesh, and alt while deinterlacing
    void ResetShadow();                                  // every slice Valid, no readback pending
    void MarkShadowValid(uint32_t mask);                 // the CPU copy is the emulated state (uploads of it queued)
    void ClearPending(SliceSync &s);
    void Touch(uint32_t slice);                          // rule 1
    void CpuWrite(uint32_t address, uint32_t bytes);     // rule 3 (VDPState wrote it already)
    bool NeedsReadback(uint32_t slice, uint64_t covered) const;
    void CommitReadback(uint32_t context);
    void MergeEntry(uint32_t slice, size_t entry);       // merges pending[entry], drops it and the older ones
    void MergeContext(uint32_t context);                 // its command buffer has completed
    void WaitContext(uint32_t context, Vdp1SyncKind kind);
    // Makes the slices in `mask` Valid (rule 4): fence waits, or a sync submit when the GPU holds newer data. False
    // when one stays behind (sync refused or failed; logged).
    bool SyncShadow(uint32_t mask, Vdp1SyncKind kind, FramePolicy policy);
    // One sync submit of the pending work with the readbacks, plus `extra` (alt slices for the inner renderer); its
    // readback buffer holds them until the next sync submit. False when refused or failed.
    bool SyncSubmit(Vdp1SyncKind kind, FramePolicy policy, uint32_t extra);
    // Hands VDP1 to the inner renderer: doubleV, mesh and alt from the shadow (VDPState holds main).
    void HandToSoftware();

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
    std::unique_ptr<MeshFBRAM> m_meshShadow;  // Gpu mode: the CPU shadow of the mesh framebuffers
    bool m_stateLoaded = false;               // LoadState since the last PostLoadStateSync
    bool m_frameRecordedBySync = false;
    bool m_vdp2FrameRecorded = false;         // the last record call recorded a VDP2 frame (for RecordedFrameDropped)
    VulkanDevice *m_device = nullptr;
    uint32_t m_frameContexts = 0;             // the caller's; Gpu mode adds the sync context (index m_frameContexts)
    // CPU shadow
    uint64_t m_vdp1Seq = 0;                   // emissions so far
    uint64_t m_submittedSeq = 0;              // emissions the committed command buffers cover
    std::array<SliceSync, 8> m_slices{};
    std::vector<std::unique_ptr<WriteMask>> m_maskPool; // cleared masks for reuse
    std::unique_ptr<std::array<ymir::vdp::SpriteFB, 2>> m_altShadow; // Gpu mode: alt fb0, fb1
    ProposedReadback m_proposed;
    std::vector<VkFence> m_contextFences;     // per context: its last committed command buffer's (null: completed)
    std::function<bool(const SyncRecord &record)> m_syncSubmit;
    Vdp1SyncStats m_syncStats;

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
    Vdp1ProbeTimes m_vdp1Times;              // FrameProbe: VDP1 times since the last report (Gpu VDP1 mode)
    bool m_timestampsLogged = false;         // FrameProbe: "no GPU timestamps" logged once
    bool m_saveSyncFailed = false;           // the last PreSaveStateSync left main or mesh behind
    bool m_saveSyncFailLogged = false;
    bool m_readbackMemoryLogged = false;     // the VDP1 back half's readback memory choice (its only log line)
    bool m_failSaveSync = false;             // FailSaveSyncForTest
};

} // namespace brimir::gpu
