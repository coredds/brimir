#pragma once

// The Vulkan VDP renderer (GPU stage 1): VDP2 on the GPU, VDP1 in software. Installed with VDP::UseExternalRenderer
// (type External).
//
// - VDP1 runs in an inner SoftwareVDPRenderer (the software VDP1 backend), which gets the VDP1 calls, resolution
//   changes, resets, save-state calls and enhancements (docs/ymir-hw-renderer-notes/stage1-hybrid-renderer.md section
//   1.2). Its threaded VDP2 is never enabled; VDP1 threading follows EnableThreadedVDP1. Its callbacks stay empty: this
//   renderer fires Callbacks itself.
// - VDP2 runs in the front half (Vdp2FrontEnd, CPU, always) and, while a device is attached, the back half
//   (Vdp2VulkanBackend). Each frame is recorded by the front half; the caller replays it into its command buffer with
//   RecordPendingFrame and presents CompositeBuffer(). Without a device the front half still runs (VDPState stays
//   current) and its frames are dropped at VDP2EndFrame.
// - The display framebuffer's sprite slices (main; alt with deinterlacing in double density; mesh with transparent
//   meshes) are copied into the frame record at the frame's first VDP2 line, after the framebuffer is synchronized when
//   VDP1 erased it without a swap since the last capture (section 1.5).

#include "gpu/vdp2/vdp2_front.hpp"
#include "gpu/vdp2/vdp2_vulkan_backend.hpp"
#include "gpu/vk_device.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_hw_base.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace brimir::gpu {

class VulkanVDPRenderer final : public ymir::vdp::HardwareVDPRendererBase {
public:
    VulkanVDPRenderer(ymir::vdp::VDPState &state, ymir::vdp::config::VDP2DebugRender &vdp2DebugRenderOptions,
                      const ymir::vdp::config::VDP2AccessPatternsConfig &vdp2AccessPatternsConfig);
    ~VulkanVDPRenderer() override;

    // -------------------------------------------------------------------------
    // Device

    // Creates the back half on `device` (frameContexts upload contexts, see Vdp2VulkanBackend). The next frame uploads
    // all of VRAM and CRAM and starts from a cleared composite. Attached() is false if the back half failed. When
    // already attached, the old back half is destroyed immediately: the caller must have waited for the device to be
    // idle first (as for DetachDevice).
    void AttachDevice(VulkanDevice &device, uint32_t frameContexts);
    // Drops the pending frame and destroys the GPU resources. The caller has waited for the device to be idle.
    void DetachDevice();
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

    // Fired at the end of every VDP2 frame, after Callbacks.VDP2DrawFinished, with the frame's size.
    std::function<void(uint32_t w, uint32_t h)> FrameReady;

    // A recorded frame waits for RecordPendingFrame (only while attached).
    bool HasPendingFrame() const;
    // Replays the pending frame into cmd (see Vdp2VulkanBackend::Record) and consumes it. When recording fails, the
    // next frame uploads everything again. Returns false when there was nothing to record or recording failed.
    bool RecordPendingFrame(uint32_t context, VkCommandBuffer cmd);
    // The command buffer of the last successful RecordPendingFrame will not be submitted (submission failed, or the
    // frame was abandoned): its uploads and one-time work (composite clear) are redone with the next frame. Task 7's
    // caller must call this whenever a recorded command buffer does not reach the queue.
    void RecordedFrameDropped();
    // The persistent composite (VK_NULL_HANDLE while detached).
    VkBuffer CompositeBuffer() const;

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
    void SyncDisplayErase();
    void CaptureSpriteSlices();
    void CaptureAltSpriteSlices();

    ymir::vdp::VDPState &m_state;
    std::unique_ptr<ymir::vdp::SoftwareVDPRenderer> m_vdp1; // the software VDP1 backend
    vdp2::Vdp2FrontEnd m_front;
    std::unique_ptr<vdp2::Vdp2VulkanBackend> m_backend;

    uint32_t m_HRes = ymir::vdp::kDefaultResH;
    uint32_t m_VRes = ymir::vdp::kDefaultResV;
    bool m_exclusiveMonitor = false;
    bool m_threadedVDP1 = false;
    bool m_resolutionChanged = false;
    bool m_displayEraseSinceCapture = false; // VDP1 erased a framebuffer without a swap since the last capture
    bool m_slicesCaptured = false;           // the open frame has its main (and mesh0) sprite slices
    bool m_altSlicesCaptured = false;        // the open frame has its alt (and mesh1) sprite slices
};

} // namespace brimir::gpu
