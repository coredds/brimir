#include "vulkan_vdp_renderer.hpp"

#include <span>

namespace brimir::gpu {

using namespace ymir;
using namespace ymir::vdp;

VulkanVDPRenderer::VulkanVDPRenderer(VDPState &state, config::VDP2DebugRender &vdp2DebugRenderOptions,
                                     const config::VDP2AccessPatternsConfig &vdp2AccessPatternsConfig)
    : HardwareVDPRendererBase(VDPRendererType::External)
    , m_state(state)
    , m_vdp1(std::make_unique<SoftwareVDPRenderer>(state, vdp2DebugRenderOptions, vdp2AccessPatternsConfig))
    , m_front(state, vdp2AccessPatternsConfig) {}

VulkanVDPRenderer::~VulkanVDPRenderer() = default;

// -----------------------------------------------------------------------------
// Device

void VulkanVDPRenderer::AttachDevice(VulkanDevice &device, uint32_t frameContexts) {
    m_backend.reset();
    auto backend = std::make_unique<vdp2::Vdp2VulkanBackend>(device, frameContexts);
    if (!backend->Ok()) {
        return;
    }
    m_backend = std::move(backend);
    // A new back half has none of the state: everything is uploaded with the next frame, and its composite starts
    // cleared (Vdp2VulkanBackend clears it on first use).
    m_front.ConsumeFrame();
    m_front.MarkAllDirty();
}

void VulkanVDPRenderer::DetachDevice() {
    m_front.ConsumeFrame();
    m_front.MarkAllDirty();
    m_backend.reset();
}

void VulkanVDPRenderer::EnableThreadedVDP1(bool enable) {
    m_vdp1->EnableThreadedVDP1(enable);
}

bool VulkanVDPRenderer::HasPendingFrame() const {
    return m_backend != nullptr && m_front.HasFrame();
}

bool VulkanVDPRenderer::RecordPendingFrame(uint32_t context, VkCommandBuffer cmd) {
    if (!HasPendingFrame()) {
        return false;
    }
    const bool recorded = m_backend->Record(m_front.Frame(), context, cmd);
    m_front.ConsumeFrame();
    if (!recorded) {
        // The frame's snapshots never reached the GPU.
        m_front.MarkAllDirty();
    }
    return recorded;
}

VkBuffer VulkanVDPRenderer::CompositeBuffer() const {
    return m_backend ? m_backend->CompositeBuffer() : VK_NULL_HANDLE;
}

// -----------------------------------------------------------------------------
// Basics and configuration

void VulkanVDPRenderer::Reset(bool hard) {
    m_vdp1->Reset(hard);
    m_front.Reset(hard);
    m_HRes = kDefaultResH;
    m_VRes = kDefaultResV;
    m_displayEraseSinceCapture = false;
}

void VulkanVDPRenderer::UpdateEnhancements() {
    m_vdp1->ConfigureEnhancements(m_enhancements);
    m_front.SetEnhancements(m_enhancements);
}

// -----------------------------------------------------------------------------
// Save states (the renderer section is the software renderer's, hybrid section 4)

void VulkanVDPRenderer::PreSaveStateSync() {
    m_vdp1->PreSaveStateSync();
}

void VulkanVDPRenderer::PostLoadStateSync() {
    m_vdp1->PostLoadStateSync();
    m_front.PostLoadStateSync();
}

void VulkanVDPRenderer::SaveState(savestate::VDPSaveState::VDPRendererSaveState &state) {
    m_vdp1->SaveState(state);
}

bool VulkanVDPRenderer::ValidateState(const savestate::VDPSaveState::VDPRendererSaveState &state) const {
    return m_vdp1->ValidateState(state);
}

void VulkanVDPRenderer::LoadState(const savestate::VDPSaveState::VDPRendererSaveState &state) {
    m_vdp1->LoadState(state);
}

// -----------------------------------------------------------------------------
// VDP1 (software)

void VulkanVDPRenderer::VDP1WriteVRAM(uint32_t address, uint8_t value) {
    m_vdp1->VDP1WriteVRAM(address, value);
}

void VulkanVDPRenderer::VDP1WriteVRAM(uint32_t address, uint16_t value) {
    m_vdp1->VDP1WriteVRAM(address, value);
}

void VulkanVDPRenderer::VDP1SyncFB() {
    m_vdp1->VDP1SyncFB();
}

void VulkanVDPRenderer::VDP1DebugSyncFB() {
    m_vdp1->VDP1DebugSyncFB();
}

void VulkanVDPRenderer::VDP1WriteFB(uint32_t address, uint8_t value) {
    m_vdp1->VDP1WriteFB(address, value);
}

void VulkanVDPRenderer::VDP1WriteFB(uint32_t address, uint16_t value) {
    m_vdp1->VDP1WriteFB(address, value);
}

void VulkanVDPRenderer::VDP1WriteReg(uint32_t address, uint16_t value) {
    m_vdp1->VDP1WriteReg(address, value);
}

void VulkanVDPRenderer::VDP1EraseFramebuffer(uint64_t cycles) {
    m_vdp1->VDP1EraseFramebuffer(cycles);
    // A threaded erase is queued, not waited for; the capture syncs unless a swap (which waits) comes first.
    m_displayEraseSinceCapture = true;
}

void VulkanVDPRenderer::VDP1SwapFramebuffer() {
    m_vdp1->VDP1SwapFramebuffer();
    m_displayEraseSinceCapture = false;
    Callbacks.VDP1FramebufferSwap();
}

void VulkanVDPRenderer::VDP1BeginFrame() {
    m_vdp1->VDP1BeginFrame();
}

void VulkanVDPRenderer::VDP1ExecuteCommand(uint32_t cmdAddress, VDP1Command::Control control) {
    m_vdp1->VDP1ExecuteCommand(cmdAddress, control);
}

void VulkanVDPRenderer::VDP1EndFrame() {
    m_vdp1->VDP1EndFrame();
    Callbacks.VDP1DrawFinished();
}

void VulkanVDPRenderer::DumpExtraVDP1Framebuffers(std::ostream &out) const {
    m_vdp1->DumpExtraVDP1Framebuffers(out);
}

// -----------------------------------------------------------------------------
// VDP2 (front half, then the GPU)

void VulkanVDPRenderer::VDP2WriteVRAM(uint32_t address, uint8_t) {
    m_front.WriteVRAM(address);
}

void VulkanVDPRenderer::VDP2WriteVRAM(uint32_t address, uint16_t) {
    m_front.WriteVRAM(address);
}

void VulkanVDPRenderer::VDP2WriteCRAM(uint32_t address, uint8_t) {
    m_front.WriteCRAM(address);
}

void VulkanVDPRenderer::VDP2WriteCRAM(uint32_t address, uint16_t) {
    m_front.WriteCRAM(address);
}

void VulkanVDPRenderer::VDP2WriteReg(uint32_t address, uint16_t value) {
    m_front.WriteReg(address, value);
}

void VulkanVDPRenderer::VDP2SetResolution(uint32_t h, uint32_t v, bool exclusive) {
    // The software VDP1 erase clamps to the resolution (hybrid section 1.1).
    m_vdp1->VDP2SetResolution(h, v, exclusive);
    m_front.SetResolution(h, v, exclusive);
    m_HRes = h;
    m_VRes = v;
    m_resolutionChanged = true;
}

void VulkanVDPRenderer::VDP2SetField(bool) {}

void VulkanVDPRenderer::VDP2LatchTVMD() {}

void VulkanVDPRenderer::VDP2BeginFrame() {
    m_front.BeginFrame();
    m_slicesCaptured = false;
}

void VulkanVDPRenderer::VDP2RenderLine(uint32_t y) {
    m_front.RenderLine(y);
    if (!m_slicesCaptured && m_backend) {
        // The frame's first line (normally y = 0, after the VBlank swap): the display framebuffer is final for the
        // whole frame (VDP1 draws and the CPU writes into the draw framebuffer; erases happen in blanking). Without a
        // device the frame is dropped, so nothing is copied.
        CaptureSpriteSlices();
        m_slicesCaptured = true;
    }
}

void VulkanVDPRenderer::VDP2EndFrame() {
    m_front.EndFrame();
    m_slicesCaptured = false;
    if (!m_backend) {
        // No device: the record is dropped; AttachDevice uploads everything again.
        m_front.ConsumeFrame();
    }
    if (m_resolutionChanged) {
        m_resolutionChanged = false;
        Callbacks.VDP2ResolutionChanged(m_HRes, m_VRes);
    }
    Callbacks.VDP2DrawFinished();
    if (FrameReady) {
        FrameReady(m_HRes, m_VRes);
    }
}

void VulkanVDPRenderer::UpdateEnabledLayers() {
    // Debug layer toggles are not supported by the GPU path: the front half renders every layer.
}

void VulkanVDPRenderer::CaptureSpriteSlices() {
    if (m_displayEraseSinceCapture) {
        // An erase of the display framebuffer without a swap may still be running on the VDP1 thread.
        m_vdp1->VDP1SyncFB();
        m_displayEraseSinceCapture = false;
    }
    const uint8_t display = m_state.fbIndex.display & 1u;
    const bool doubleDensity = m_state.regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity;
    const bool deinterlaceDD = m_enhancements.deinterlace && doubleDensity;

    using Slice = std::span<const uint8_t, vdp2::kSpriteSliceSize>;
    static_assert(sizeof(SpriteFB) == vdp2::kSpriteSliceSize);
    m_front.AddSpriteSlice(vdp2::kSpriteSliceMain, Slice{m_state.mem1.FBRAM[display]});
    if (deinterlaceDD) {
        m_front.AddSpriteSlice(vdp2::kSpriteSliceAlt, Slice{m_vdp1->GetAltFBRAM(display)});
    }
    if (m_enhancements.transparentMeshes) {
        m_front.AddSpriteSlice(vdp2::kSpriteSliceMesh0, Slice{m_vdp1->GetMeshFBRAM(false, display)});
        if (deinterlaceDD) {
            m_front.AddSpriteSlice(vdp2::kSpriteSliceMesh1, Slice{m_vdp1->GetMeshFBRAM(true, display)});
        }
    }
}

} // namespace brimir::gpu
