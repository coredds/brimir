#pragma once

// CPU half of the VDP2 scene harness (stage1b-port-inventory.md section 7.2). A scene is a deterministic recipe of
// VDP register, VRAM, CRAM and FBRAM writes, applied by Vdp2SceneWriter to one or more (VDPState, renderer) pairs in
// exactly the order and with exactly the latches ymir::vdp::VDP uses, so that every renderer sees the same calls.
// With BRIMIR_VULKAN, RenderBoth runs a scene through the software renderer and the Vulkan renderer and compares the
// last frames.

#include <ymir/hw/vdp/renderer/vdp_renderer_base.hpp>
#include <ymir/hw/vdp/vdp_configs.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#if BRIMIR_VULKAN
    #include <volk.h>
#endif

namespace brimir::test {

class Vdp2SceneWriter;

struct Vdp2Scene {
    std::function<void(Vdp2SceneWriter &)> setup;                // writes before the first frame
    std::function<void(Vdp2SceneWriter &, uint32_t y)> onLine;  // optional writes before RenderLine(y), every frame
    uint32_t frames = 2;                                         // >= 2 exercises fields and frame-to-frame state
    ymir::vdp::config::Enhancements enhancements{};
};

class Vdp2SceneWriter {
public:
    explicit Vdp2SceneWriter(const ymir::vdp::config::Enhancements &enhancements = {});

    /// @brief Adds a (state, renderer) pair, installing the renderer as VDP::InstallRenderer does (enhancements,
    /// resolution, field, PostLoadStateSync). Every later write and frame step applies to all pairs, in Attach order.
    void Attach(ymir::vdp::VDPState &state, ymir::vdp::IVDPRenderer &renderer);

    // Bus-side writes, as VDP::VDP2WriteReg / VDP2WriteVRAM / VDP2WriteCRAM / VDP1WriteReg do them.
    void Reg(uint32_t address, uint16_t value);
    void VRAM16(uint32_t address, uint16_t value);
    void VRAM32(uint32_t address, uint32_t value);
    void CRAM16(uint32_t address, uint16_t value);
    void VDP1Reg(uint32_t address, uint16_t value);

    /// @brief Writes a word of VDP1 framebuffer `fb`. The draw framebuffer goes through VDP::VDP1WriteFB (the renderer
    /// is notified); the display framebuffer, which the bus cannot reach, is written directly into the state.
    void FBRAM16(uint32_t fb, uint32_t address, uint16_t value);

    /// @brief Forces a resolution, as VDP::UpdateResolution reports it (normally derived from TVMD at the top border).
    void SetResolution(uint32_t h, uint32_t v, bool exclusive);

    /// @brief A soft VDP reset, as VDP::Reset(false) does it (e.g. on an SMPC clock change): the state and the
    /// renderers reset, then the resolution update. Called from onLine(y), RunFrame then renders line 0 again instead
    /// of line y (VDP::Reset restarts the active display at VCNT 0) and continues the frame from there.
    void ResetVDP();

    /// @brief Sets TVSTAT.ODD and notifies the renderers, as the VDP does at VBlank IN.
    void Field(bool odd);

    /// @brief VDP1 framebuffer swap, as VDP::VDP1SwapFramebuffer (renderer first, then the index flip).
    void SwapFramebuffers();

    /// @brief Erases the display framebuffer with the latched erase parameters, as the VDP's erase in blanking does
    /// (VDP::VDP1EraseFramebuffer with no cycle limit; no swap follows).
    void EraseFramebuffer();

    /// @brief Writes a word of VDP1 VRAM (command tables, textures), as VDP::VDP1WriteVRAM.
    void VDP1VRAM16(uint32_t address, uint16_t value);

    /// @brief Draws the VDP1 command table at VRAM address 0 into the draw framebuffer, as VDP::VDP1BeginFrame,
    /// VDP1ProcessCommand and VDP1EndFrame do (sequential "next" jumps only, up to the end bit).
    void DrawVDP1();

    /// @brief Runs one VDP frame from the top border: UpdateResolution, LatchTVMD, VDP2BeginFrame, the active lines
    /// (onLine(y) then VDP2RenderLine(y)), the VBlank IN field switch and VDP2EndFrame, in the order vdp.cpp uses.
    /// With firstLine > 0 the frame is continued from that line (no top border); with endLine below the field's line
    /// count it stops before that line (no VBlank, no EndFrame).
    void RunFrame(const std::function<void(Vdp2SceneWriter &, uint32_t)> &onLine = {}, uint32_t firstLine = 0,
                  uint32_t endLine = UINT32_MAX);

    /// @brief Runs a whole scene: setup, then `frames` frames.
    void Run(const Vdp2Scene &scene);

    uint32_t HRes() const {
        return m_HRes;
    }
    uint32_t VRes() const {
        return m_VRes;
    }
    bool ExclusiveMonitor() const {
        return m_exclusiveMonitor;
    }
    /// @brief Active lines per field of the current resolution (RenderLine calls per frame).
    uint32_t FieldLines() const;

    /// @brief The VDP1 draw framebuffer index (the same in every attached state).
    uint32_t DrawFramebuffer() const;

private:
    struct Target {
        ymir::vdp::VDPState *state;
        ymir::vdp::IVDPRenderer *renderer;
    };

    void UpdateResolution();

    ymir::vdp::config::Enhancements m_enhancements;
    std::vector<Target> m_targets;
    uint32_t m_HRes = ymir::vdp::kDefaultResH;
    uint32_t m_VRes = ymir::vdp::kDefaultResV;
    bool m_exclusiveMonitor = false;
    bool m_restartFrame = false; // ResetVDP was called: RunFrame continues at line 0
};

/// @brief Renders the scene with a SoftwareVDPRenderer and returns the last frame (XBGR, HRes x VRes).
std::vector<uint32_t> RenderSoftware(const Vdp2Scene &scene, uint32_t *outWidth = nullptr,
                                     uint32_t *outHeight = nullptr);

/// @brief The last frame of a scene rendered by software and by the Vulkan renderer, compared in RGB.
struct SceneResult {
    bool identical = false;
    std::string error; // the harness itself failed (no frame, recording or readback failure)
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> software, gpu; // XBGR, width x height
    // First differing pixel in row-major order (RGB only), and how many differ.
    uint32_t diffX = 0, diffY = 0, softwarePixel = 0, gpuPixel = 0;
    uint32_t diffCount = 0;
    uint32_t distinctColors = 0; // in the software frame (RGB)
    uint32_t frame = 0;          // the compared frame (the last one, or the first differing one with compareFrame)
    uint32_t comparedFrames = 0;

    std::string Describe() const;
};

} // namespace brimir::test

#if BRIMIR_VULKAN

namespace brimir::gpu {
class VulkanDevice;
class VulkanVDPRenderer;
} // namespace brimir::gpu

namespace brimir::test {

struct RenderBothOptions {
    uint32_t frameContexts = 2;
    size_t uploadBlockSize = 0; // 0 = the backend's default
    bool threadedVDP1 = false;  // the Vulkan renderer's inner software VDP1 runs on its own thread
    bool poisonImages = false;  // Vdp2VulkanBackend::SetPoisonImagesForTest (also with BRIMIR_VDP2_POISON=1)
    // This frame is recorded into a command buffer that is never submitted, then RecordedFrameDropped.
    uint32_t dropFrame = UINT32_MAX;
    // Before RenderLine(reloadLine) of this frame, the Vulkan renderer's state is saved, lost and loaded again.
    uint32_t reloadFrame = UINT32_MAX;
    uint32_t reloadLine = 0;
    // Called before each frame (0-based), after setup, with the Vulkan renderer and the device.
    std::function<void(uint32_t frame, gpu::VulkanVDPRenderer &, gpu::VulkanDevice &)> beforeFrame;
    // The frame context each frame records into (default frame % frameContexts).
    std::function<uint32_t(uint32_t frame)> contextForFrame;
    // Compare every frame for which this returns true (a shown frame), not just the last one; the result describes the
    // first differing frame (SceneResult::frame).
    std::function<bool(uint32_t frame)> compareFrame;
    // When set, each submitted frame's GPU time in milliseconds (timestamps around RecordPendingFrame's commands) is
    // appended; nothing is appended when the queue has no timestamps.
    std::vector<double> *gpuMilliseconds = nullptr;
};

/// @brief Runs the scene into a software and a Vulkan renderer pair (separate VDPStates, the same writes), replays each
/// GPU frame in a one-shot submit and reads back the last frame's composite.
SceneResult RenderBoth(const Vdp2Scene &scene, gpu::VulkanDevice &device, const RenderBothOptions &options = {});

/// @brief Reads the first `words` 32-bit words of a device buffer (one-shot submit, waits).
std::vector<uint32_t> ReadbackBuffer(gpu::VulkanDevice &device, VkBuffer buffer, uint32_t words);

} // namespace brimir::test

#endif // BRIMIR_VULKAN
