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

    /// @brief Applies `fn` to every attached state, in Attach order (for state no bus write can produce, e.g. derived
    /// register state the renderers read).
    void ForEachState(const std::function<void(ymir::vdp::VDPState &)> &fn);

    /// @brief Called right before every VDP2RenderLine(y) of RunFrame with the line actually rendered (after onLine
    /// and a frame restart), the first attached state and the writer.
    std::function<void(const Vdp2SceneWriter &, const ymir::vdp::VDPState &, uint32_t y)> lineObserver;

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
    // Internal resolution N of the compared GPU frame. At N > 1, gpuScaled is the read-back N x composite (pitch
    // N * width) and gpu, software and the diff fields above are native (width x height): a native pixel differs when
    // the comparison (CompareNearestPixels or CompareSamplePointPixels) found a difference there, and gpu holds the
    // differing GPU pixel, else software's: in sample-point mode gpu shows software's value at the skipped points of
    // the excluded lattice too (whatever the GPU drew there; see gpuScaled). comparedPixels counts the GPU pixels
    // compared, skippedPixels the sample points of the excluded lattice.
    uint32_t scale = 1;
    std::vector<uint32_t> gpuScaled;
    uint64_t comparedPixels = 0;
    uint64_t skippedPixels = 0;

    std::string Describe() const;
};

/// @brief Per native composite row: which sample points of an N x frame are excluded from the sample-point invariant
/// (stage1c-upscaling-research.md section 2; the brief's Global Constraints): odd native columns where an RBG is
/// enabled in a hi-res mode; odd composite rows where an RBG is drawn in an interlaced mode (deinterlaced or woven:
/// both rows of the pair show the RBG line from the same rotation bases, the odd one half a line on); the second row
/// of each identical pair for NBGs in single-density interlace.
struct SampleLatticeRow {
    bool skipOddColumns = false;
    bool skipRow = false;
};

/// @brief The lattice rows a VDP2 line writes, from the line's TVMD and BGON (the frame's resolution from the writer):
/// calls set(row, flags) for each composite row line y writes (one, or two with deinterlacing in interlaced modes).
/// The rules are per composite row, from the layers enabled on the line that wrote it: a hi-res line with an RBG
/// enabled skips its odd columns; in an interlaced mode (not exclusive monitor modes), the odd row skips when an RBG
/// is enabled (deinterlaced or woven), or (single density) when any NBG is enabled, the whole row, even where the
/// layer is transparent or covered (the lattice does not know which layer is on top). So in single density the odd
/// rows of the lines with every NBG and RBG off are compared (sprites, back screen; the scene "single-density
/// interlace with NBGs off on some lines").
void LatticeForLine(const Vdp2SceneWriter &writer, const ymir::vdp::VDPState &state, uint32_t y, bool deinterlace,
                    const std::function<void(uint32_t row, SampleLatticeRow flags)> &set);

/// @brief Nearest-upscale comparison: every pixel (X, Y) of `scaled` (n * width x n * height) must equal
/// software(X / n, Y / n) in RGB. Fills result (scale, gpuScaled, the native gpu image, diffs, comparedPixels,
/// distinctColors, identical). False with result.error set when the sizes do not match or it would compare fewer than
/// all n^2 * width * height pixels (a vacuous pass).
bool CompareNearestPixels(const std::vector<uint32_t> &software, uint32_t width, uint32_t height,
                          std::vector<uint32_t> scaled, uint32_t n, SceneResult &result);

/// @brief Sample-point comparison: pixel (n * x, n * y) of `scaled` must equal software(x, y) for every native pixel
/// outside the excluded lattice (`lattice`, one entry per native row; missing rows exclude nothing). False with
/// result.error set when the sizes do not match, or no native pixel or fewer than a quarter of them are compared (more
/// than the lattice can exclude: a vacuous pass).
bool CompareSamplePointPixels(const std::vector<uint32_t> &software, uint32_t width, uint32_t height,
                              std::vector<uint32_t> scaled, uint32_t n, const std::vector<SampleLatticeRow> &lattice,
                              SceneResult &result);

} // namespace brimir::test

#if BRIMIR_VULKAN

namespace brimir::gpu {
class VulkanDevice;
class VulkanVDPRenderer;
} // namespace brimir::gpu

namespace brimir::test {

/// @brief How an N x GPU frame (N > 1) is compared with the 1x software frame.
enum class ScaleCompare { Nearest, SamplePoints };

struct RenderBothOptions {
    // Internal resolution of the Vulkan renderer (VulkanVDPRenderer::SetInternalScale before the first frame); 0 =
    // BRIMIR_VDP2_SCALE (1-4) when set, else 1. beforeFrame may change it; each frame is compared at the renderer's
    // scale then.
    uint32_t scale = 0;
    // The comparison at N > 1: the sample points (BRIMIR_VDP2_SCALE runs; sub-pixel evaluation makes the other
    // sub-pixels differ from a nearest upscale wherever a background is zoomed, rotated or finely scrolled), or a
    // nearest upscale (CompareNearest, for scenes without sub-pixel features).
    ScaleCompare scaleCompare = ScaleCompare::SamplePoints;
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
    // Called after each frame whose GPU frame was recorded and submitted (0-based), before any comparison.
    std::function<void(uint32_t frame, gpu::VulkanVDPRenderer &, gpu::VulkanDevice &)> afterFrame;
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

/// @brief The scene at internal resolution n compared as a nearest upscale of software (CompareNearestPixels). Not
/// vacuous: result.error is set (and identical false) when the software frame has fewer than two distinct colours or
/// the GPU frame was not rendered at n.
SceneResult CompareNearest(const Vdp2Scene &scene, gpu::VulkanDevice &device, uint32_t n,
                           RenderBothOptions options = {});

/// @brief The scene at internal resolution n compared at the sample points (CompareSamplePointPixels, the lattice
/// derived per line from the scene's TVMD and BGON). Not vacuous, as CompareNearest.
SceneResult CompareSamplePoints(const Vdp2Scene &scene, gpu::VulkanDevice &device, uint32_t n,
                                RenderBothOptions options = {});

/// @brief BRIMIR_VDP2_SCALE (1-4), else 1.
uint32_t EnvScale();

/// @brief Reads the first `words` 32-bit words of a device buffer (one-shot submit, waits).
std::vector<uint32_t> ReadbackBuffer(gpu::VulkanDevice &device, VkBuffer buffer, uint32_t words);

} // namespace brimir::test

#endif // BRIMIR_VULKAN
