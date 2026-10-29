#pragma once

// CPU half of the VDP2 scene harness (stage1b-port-inventory.md section 7.2). A scene is a deterministic recipe of
// VDP register, VRAM, CRAM and FBRAM writes, applied by Vdp2SceneWriter to one or more (VDPState, renderer) pairs in
// exactly the order and with exactly the latches ymir::vdp::VDP uses, so that every renderer sees the same calls.
// With BRIMIR_VULKAN, RenderBoth runs a scene through the software renderer and the Vulkan renderer and compares the
// last frames.

#include <ymir/hw/vdp/renderer/vdp_renderer_base.hpp>
#include <ymir/hw/vdp/vdp_configs.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#if BRIMIR_VULKAN
    #include <volk.h>
#endif

namespace brimir::test {

class Vdp2SceneWriter;

/// @brief A VDP1 command table (16 words: CMDCTRL, CMDLINK, CMDPMOD, CMDCOLR, CMDSRCA, CMDSIZE, XA, YA, XB, YB, XC, YC,
/// XD, YD, CMDGRDA, unused) built field by field. Coordinates are 16-bit raw values (13-bit signed on the VDP).
struct Vdp1Cmd {
    std::array<uint16_t, 16> w{};

    static Vdp1Cmd Normal(uint16_t pmod, uint16_t colr, uint16_t srca, uint16_t size, int x, int y);
    /// @brief Scaled sprite with zoom point 0: A (upper left) and C (lower right).
    static Vdp1Cmd Scaled(uint16_t pmod, uint16_t colr, uint16_t srca, uint16_t size, int ax, int ay, int cx, int cy);
    /// @brief Scaled sprite with zoom point `zp` (1-15, CMDCTRL bits 8-11): A is the zoom point, B the (width, height).
    static Vdp1Cmd ScaledZoom(uint16_t zp, uint16_t pmod, uint16_t colr, uint16_t srca, uint16_t size, int ax, int ay,
                              int width, int height);
    static Vdp1Cmd Distorted(uint16_t pmod, uint16_t colr, uint16_t srca, uint16_t size, int ax, int ay, int bx, int by,
                             int cx, int cy, int dx, int dy);
    static Vdp1Cmd Polygon(uint16_t pmod, uint16_t colr, int ax, int ay, int bx, int by, int cx, int cy, int dx, int dy);
    static Vdp1Cmd Polyline(uint16_t pmod, uint16_t colr, int ax, int ay, int bx, int by, int cx, int cy, int dx,
                            int dy);
    static Vdp1Cmd Line(uint16_t pmod, uint16_t colr, int ax, int ay, int bx, int by);
    static Vdp1Cmd UserClip(int x0, int y0, int x1, int y1);
    static Vdp1Cmd SystemClip(int x1, int y1);
    static Vdp1Cmd Local(int x, int y);
    static Vdp1Cmd End();

    /// @brief CMDCTRL direction bits (texture flips).
    Vdp1Cmd &Flip(bool h, bool v);
    Vdp1Cmd &Skip();
    /// @brief Gouraud table address (CMDGRDA, address / 8); the caller sets the Gouraud bit in CMDPMOD.
    Vdp1Cmd &Gouraud(uint16_t grda);
};

/// @brief CMDSIZE of a w x h texture (w a multiple of 8).
constexpr uint16_t Vdp1Size(uint32_t w, uint32_t h) {
    return static_cast<uint16_t>(((w / 8) << 8) | (h & 0xFF));
}

struct Vdp2Scene {
    std::function<void(Vdp2SceneWriter &)> setup;                // writes before the first frame
    std::function<void(Vdp2SceneWriter &, uint32_t y)> onLine;  // optional writes before RenderLine(y), every frame
    // Optional writes after VDP2EndFrame, every frame (the BlankingAndSync phase: e.g. the display erase, vdp.cpp:933-947)
    std::function<void(Vdp2SceneWriter &)> blanking;
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

    /// @brief Writes a word of VDP1 framebuffer `fb` through VDP::VDP1WriteFB (the renderer is notified; the software
    /// renderer mirrors it to the alternate field when deinterlacing an interlaced mode). The display framebuffer,
    /// which the bus cannot reach, is written as the draw framebuffer of a swap and swapped back (only the index flips:
    /// no erase latch, no renderer swap call), so a renderer with GPU-authoritative FBRAM sees the write too.
    void FBRAM16(uint32_t fb, uint32_t address, uint16_t value);
    /// @brief The same with a byte write (VDP::VDP1WriteFB<uint8>).
    void FBRAM8(uint32_t fb, uint32_t address, uint8_t value);

    /// @brief Forces a resolution, as VDP::UpdateResolution reports it (normally derived from TVMD at the top border).
    void SetResolution(uint32_t h, uint32_t v, bool exclusive);

    /// @brief A VDP reset, as VDP::Reset(hard) does it (soft: e.g. on an SMPC clock change): the state and the
    /// renderers reset, then the resolution update. Called from onLine(y), RunFrame then renders line 0 again instead
    /// of line y (VDP::Reset restarts the active display at VCNT 0) and continues the frame from there.
    void ResetVDP(bool hard = false);

    /// @brief Changes the enhancements of every renderer, as VDP::SetEnhancements does (the frontend's options).
    void ConfigureEnhancements(const ymir::vdp::config::Enhancements &enhancements);

    /// @brief Sets TVSTAT.ODD and notifies the renderers, as the VDP does at VBlank IN.
    void Field(bool odd);

    /// @brief VDP1 framebuffer swap, as VDP::VDP1SwapFramebuffer (renderer first, then the index flip).
    void SwapFramebuffers();

    /// @brief Erases the display framebuffer with the latched erase parameters, as the VDP's erase in blanking does
    /// (VDP::VDP1EraseFramebuffer with no cycle limit, or the VBlank erase's `cycles`; no swap follows).
    void EraseFramebuffer(uint64_t cycles = 0);

    /// @brief Writes a word of VDP1 VRAM (command tables, textures), as VDP::VDP1WriteVRAM.
    void VDP1VRAM16(uint32_t address, uint16_t value);

    /// @brief Writes VDP1 VRAM bytes (textures, look-up tables, Gouraud tables) 16 bits at a time, as the bus does (an
    /// odd count is padded with a zero byte).
    void VDP1VRAMBytes(uint32_t address, std::span<const uint8_t> bytes);
    /// @brief Writes a command table (32 bytes) at `address`.
    void VDP1Command(uint32_t address, const Vdp1Cmd &cmd);
    /// @brief Writes a Gouraud table (four RGB555 corner colours A-D) at `address`.
    void VDP1Gouraud(uint32_t address, const std::array<uint16_t, 4> &colors);
    /// @brief Writes a colour look-up table (16 colours) at `address`.
    void VDP1LUT(uint32_t address, const std::array<uint16_t, 16> &colors);

    /// @brief Draws the VDP1 command table at VRAM address 0 into the draw framebuffer, as VDP::VDP1BeginFrame,
    /// VDP1ProcessCommand and VDP1EndFrame do (sequential "next" jumps only, up to the end bit).
    void DrawVDP1();
    /// @brief Part of DrawVDP1: the command slots [first, first + count) of the table at 0 (32 bytes each, sequential,
    /// skip and end bits honoured), so CPU and VRAM writes can go between commands. first == 0 begins the VDP1 frame;
    /// reaching the end bit ends it (returns true then).
    bool DrawVDP1Partial(uint32_t first, uint32_t count);

    /// @brief A CPU read of the draw framebuffer as VDP::VDP1ReadFB does it (the renderer's VDP1SyncFB, then
    /// VDPState::VDP1ReadFB), per attached state in Attach order.
    std::vector<uint16_t> ReadFB16(uint32_t address);
    std::vector<uint8_t> ReadFB8(uint32_t address);
    /// @brief Every attached pair's state as VDP::SaveState saves it (PreSaveStateSync, VDPState::SaveState, the
    /// renderer's SaveState), in Attach order.
    std::vector<std::unique_ptr<ymir::savestate::VDPSaveState>> SaveStates();
    /// @brief Saves every pair (SaveStates) and loads pair i with pair (i + 1) % n's state (VDP::LoadState: VDPState,
    /// renderer LoadState, PostLoadStateSync): with two pairs, software loads the other renderer's state and vice versa.
    void CrossLoad();
    /// @brief Every attached renderer's state saved, a blank renderer section loaded, then the saved state loaded again
    /// (PreSaveStateSync, SaveState, LoadState, PostLoadStateSync as VDP::SaveState / LoadState call them).
    void SaveLoadMidFrame();

    /// @brief An FBRAM checkpoint (RenderBothFBRAM compares the GPU FBRAM with software's here); nothing without one.
    void Checkpoint() {
        if (checkpoint) {
            checkpoint(*this);
        }
    }
    std::function<void(Vdp2SceneWriter &)> checkpoint;

    /// @brief Runs one VDP frame from the top border: UpdateResolution, LatchTVMD, VDP2BeginFrame, the active lines
    /// (onLine(y) then VDP2RenderLine(y)), the VBlank IN field switch and VDP2EndFrame, in the order vdp.cpp uses.
    /// With firstLine > 0 the frame is continued from that line (no top border); with endLine below the field's line
    /// count it stops before that line (no VBlank, no EndFrame). `blanking` runs after VDP2EndFrame.
    void RunFrame(const std::function<void(Vdp2SceneWriter &, uint32_t)> &onLine = {}, uint32_t firstLine = 0,
                  uint32_t endLine = UINT32_MAX, const std::function<void(Vdp2SceneWriter &)> &blanking = {});

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

/// @brief The Vulkan renderer's VDP1 mode in RenderBoth: Default is BRIMIR_VDP1_MODE (gpu or software) when set, else
/// Software (the stage 1 path).
enum class Vdp1Path { Default, Software, Gpu };

struct FbramResult;

struct RenderBothOptions {
    Vdp1Path vdp1 = Vdp1Path::Default;
    // Set by RenderBothFBRAM: the GPU FBRAM is compared with software's after every submitted frame and at every
    // Vdp2SceneWriter::Checkpoint (Gpu VDP1 mode only).
    FbramResult *fbram = nullptr;
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
    // The Vulkan renderer's sync submits (InstallSyncSubmit) are counted here when set.
    struct SyncSubmitCounter *syncCounter = nullptr;
};

/// @brief Runs the scene into a software and a Vulkan renderer pair (separate VDPStates, the same writes), replays each
/// GPU frame in a one-shot submit and reads back the last frame's composite. With VDP1 on the GPU each submit also
/// carries the VDP1 ops (and a frame without pending VDP2 work but with VDP1 ops gets an ops-only submit).
SceneResult RenderBoth(const Vdp2Scene &scene, gpu::VulkanDevice &device, const RenderBothOptions &options = {});

/// @brief The FBRAM comparison of RenderBothFBRAM: the eight slices (main fb0/fb1, alt fb0/fb1, mesh[alt][fb]) of the
/// GPU FBRAM against the software renderer's (VDPState main, GetAltFBRAM, GetMeshFBRAM) at every checkpoint.
struct FbramResult {
    SceneResult frame;     // the frame comparison (RenderBoth's)
    bool identical = false; // FBRAM equal at every checkpoint (and at least one compared)
    std::string error;      // the harness failed (readback, recording, no Gpu VDP1 mode)
    uint32_t checkpoints = 0;
    uint64_t maxNonZero = 0; // the most non-zero software FBRAM bytes at one checkpoint (non-vacuous when > 0)
    // The first difference: where (the checkpoint's label), the slice, byte address, (x, y) decoded with the
    // framebuffer width and depth then, both values and how many bytes differed there.
    std::string where;
    uint32_t slice = 0, address = 0, x = 0, y = 0;
    uint8_t software = 0, gpu = 0;
    uint64_t count = 0;
    std::string Describe() const;
};

/// @brief RenderBoth in Gpu VDP1 mode with the FBRAM compared after every frame and at every checkpoint.
FbramResult RenderBothFBRAM(const Vdp2Scene &scene, gpu::VulkanDevice &device, RenderBothOptions options = {});

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

/// @brief Counts of the sync submits a test's renderer made through InstallSyncSubmit.
struct SyncSubmitCounter {
    uint64_t submits = 0; // submitted and completed
    uint64_t refused = 0; // refused (allowed == false) or failed
    bool allowed = true;  // false: refuse every sync submit (as VulkanOutput outside retro_run)
};

/// @brief Gives the renderer a sync submit (VulkanVDPRenderer::SetSyncSubmit) on its own command pool and fence: the
/// command buffer is submitted only when the renderer recorded something, then waited for. Counts into `counter` when
/// set (it must outlive the renderer's use of the callback).
void InstallSyncSubmit(gpu::VulkanVDPRenderer &renderer, gpu::VulkanDevice &device,
                       SyncSubmitCounter *counter = nullptr);

} // namespace brimir::test

#endif // BRIMIR_VULKAN
