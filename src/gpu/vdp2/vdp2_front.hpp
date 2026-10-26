#pragma once

// Vulkan-free front half of the GPU VDP2 renderer: follows the VDP's calls on the emulator thread, tracks what changed
// (VRAM chunks, CRAM, packed parameters) and records each frame as a Vdp2FrameRecord: batches of line ranges with the
// state they render with, plus snapshots of everything the GPU needs. The Vulkan back half replays a record into one
// command buffer.
//
// Ported from the API-neutral VDP2 logic of Ymir's Direct3D 12 renderer
// (libs/ymir-core/src/ymir/hw/vdp/renderer/vdp_renderer_hw_d3d12.cpp at 8cdf0a53, "d3d" below); each ported function
// names its source range. Changes from Ymir, each marked "// Brimir:" where it applies:
// - parameter dirtiness: the three parameter structs are rebuilt every line and memcmp'd against the last flushed
//   copies, instead of generation counters fed by register writes (CRAM keeps its generation);
// - no firstInterlacedFrame: SetResolution schedules a composite clear exactly as the software renderer clears its
//   framebuffer (and Reset clears to black, as software does);
// - compose ranges are in dispatch rows (field line << deinterlace shift, like the layer ranges), and the frame's last
//   compose covers VRes >> (interlaced && !deinterlace) rows;
// - a compose-only change splits only compose: compose ranges wait until the layer rows they read are recorded;
// - the CRAM coefficient half is snapshotted on every CRAM generation change, whatever BGON says;
// - per-line arrays hold 512 rows (exclusive monitor);
// - NBG scroll, mosaic and fetcher state follow the software renderer's per-line state machine on
//   state2.nbgLayerStates (one VDP2NBGLine per NBG per row) instead of Ymir's closed-form shader scroll, and rotation
//   bases accumulate in state2.rotParamStates, so the save-state fields match software's.
// No Vulkan header may be included here.

#include "gpu/vdp2/vdp2_hw_params.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_defs.hpp>
#include <ymir/hw/vdp/vdp_configs.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>
#include <ymir/savestate/savestate_vdp.hpp>
#include <ymir/util/dirty_bitmap.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace brimir::gpu::vdp2 {

/// @brief Size of one VDP1 framebuffer slice (main, alt, mesh0 or mesh1) as the sprite shader reads it.
inline constexpr size_t kSpriteSliceSize = 256 * 1024;

/// @brief Sprite slice indices in Vdp2FrameRecord::spriteSliceOffset.
enum Vdp2SpriteSlice : uint32_t { kSpriteSliceMain, kSpriteSliceAlt, kSpriteSliceMesh0, kSpriteSliceMesh1 };

/// @brief Alignment of parameter snapshots in the arena: the largest minStorageBufferOffsetAlignment Vulkan allows, so
/// the back half can bind a snapshot with a dynamic offset straight from a copy of the arena.
inline constexpr uint32_t kParamSnapshotAlignment = 256;

/// @brief Per-frame feature usage, counted from register state at every line (stage1b-port-inventory.md section 7.4).
struct Vdp2FeatureCounters {
    uint32_t batches = 0;    // Vdp2FrameRecord::batches.size()
    uint32_t arenaBytes = 0; // Vdp2FrameRecord::arena.size()
    uint32_t copyBytes = 0;  // VRAM and CRAM bytes in Vdp2FrameRecord::copies

    // Lines on which...
    uint32_t mosaicLines = 0;         // ...an enabled NBG/RBG has mosaic on
    uint32_t vcellLines = 0;          // ...an enabled NBG0/NBG1 has vertical cell scroll on
    uint32_t charPatDelayLines = 0;   // ...an enabled NBG has charPatDelay != 0 (illegal access patterns)
    uint32_t ddLineScrollLines = 0;   // ...double-density NBG0/NBG1 line scroll/zoom has an interval > 0
    uint32_t extendedCCLines = 0;     // ...extended colour calculation is on
    uint32_t shadowLines = 0;         // ...SDCTL enables shadow on any layer or transparent shadow
    uint32_t meshSlices = 0;          // mesh sprite slices added this frame
    uint32_t cramModeMask = 0;        // bit n: CRAM mode n was in effect on some line
};

/// @brief One recorded batch: dispatches over a line range with the state that was current for those lines.
/// Rows are dispatch rows: field line << 1 when deinterlacing (both fields of a line), else the field line.
struct Vdp2Batch {
    uint32_t layerStartY, layerLines;     // 0 lines = no layer (sprite + bgs) dispatch
    uint32_t composeStartY, composeLines; // 0 lines = no compose dispatch
    VDP2CommonRenderParams common;        // push constants; startY is 0, the back half sets it per dispatch
    uint32_t layerParamsOffset, composeParamsOffset; // into Vdp2FrameRecord::arena (kParamSnapshotAlignment)
    uint32_t firstCopy, copyCount;        // into Vdp2FrameRecord::copies; applied before this batch's dispatches
};

/// @brief A buffer upload from the arena (a snapshot taken when the front half flushed).
struct Vdp2Copy {
    enum class Target : uint8_t { VRAM, CRAMColors, CRAMCoeffs };

    uint32_t arenaOffset, dstOffset, size;
    Target target;
};

/// @brief Everything one VDP2 frame needs on the GPU.
struct Vdp2FrameRecord {
    std::vector<uint8_t> arena; // param snapshots, VRAM chunks, CRAM snapshots, sprite slices; 4-byte aligned
    std::vector<Vdp2Batch> batches;
    std::vector<Vdp2Copy> copies;

    // Per-line arrays, final at EndFrame (each batch only reads its own lines).
    std::array<VDP2RotParamBase, 2 * kMaxResV> rotBases; // [param * kMaxResV + field line]
    std::array<uint32_t, 2 * kMaxResV> lnclBack;         // packed RGBA8: LNCL at [y], BACK at [kMaxResV + y]
    std::array<VDP2NBGLine, 4 * kMaxResV> nbgLines;      // [nbg * kMaxResV + dispatch row]

    // Display FB sprite slices captured at the frame's first line (UINT32_MAX = not uploaded this frame).
    std::array<uint32_t, 4> spriteSliceOffset;
    uint32_t spriteDisplayFB = 0; // the display framebuffer (0 or 1) the slices were captured from

    uint32_t hres, vres;
    uint32_t fieldLines;          // lines rendered per field (VRes, or VRes / 2 in normal interlaced modes)
    bool interlaced, deinterlace; // as of BeginFrame (batches use the TVMD in effect when they were recorded)
    bool exclusive;
    bool clearComposite;          // clear the composite to clearColor before this frame's dispatches
    uint32_t clearColor;          // packed XBGR, as the software framebuffer

    Vdp2FeatureCounters features;
};

/// @brief The CPU half of the GPU VDP2 renderer. Mirrors the VDP2 half of IVDPRenderer.
class Vdp2FrontEnd {
public:
    Vdp2FrontEnd(ymir::vdp::VDPState &state, const ymir::vdp::config::VDP2AccessPatternsConfig &accessPatternsConfig);

    void Reset(bool hard);
    void PostLoadStateSync(); // everything dirty: VRAM, CRAM; params are snapshotted per frame anyway

    void SetEnhancements(const ymir::vdp::config::Enhancements &enhancements);

    void WriteVRAM(uint32_t address);
    void WriteCRAM(uint32_t address);
    void WriteReg(uint32_t address, uint16_t value);

    void SetResolution(uint32_t h, uint32_t v, bool exclusive);

    void BeginFrame();
    void RenderLine(uint32_t y);
    void EndFrame();

    /// @brief Copies a display framebuffer slice into the current frame's arena (call between BeginFrame and EndFrame).
    void AddSpriteSlice(uint32_t index, std::span<const uint8_t, kSpriteSliceSize> data);

    bool HasFrame() const {
        return m_frameReady;
    }
    const Vdp2FrameRecord &Frame() const {
        return m_record;
    }
    void ConsumeFrame() {
        m_frameReady = false;
    }

    /// @brief Device (re)attach: the next frame uploads all of VRAM and CRAM.
    void MarkAllDirty();

    /// @brief Writes the NBG fetchers (vramFetchers[field][0-3]) into a renderer save state, as software saves them.
    void SaveFetchers(ymir::savestate::VDPSaveState::VDPRendererSaveState &state) const;
    /// @brief Reads the NBG fetchers from a renderer save state, as software loads them.
    void LoadFetchers(const ymir::savestate::VDPSaveState::VDPRendererSaveState &state);

    /// @brief The last vertical cell scroll value NBG `nbg` read on `field` (software: VRAMFetcher::lastVCellScroll).
    uint32_t LastVCellScroll(uint32_t field, uint32_t nbg) const {
        return m_nbgFetchers[field & 1][nbg & 3].lastVCellScroll;
    }

    /// @brief The mode-dependent RGBA8 CRAM colour cache (the first half of CRAMColors copies).
    const CRAMColorCache &ColorCache() const {
        return m_cramColorCache;
    }

    /// @brief The software renderer's RGB555 colour cache of every CRAM word (the second half of CRAMColors copies).
    const std::array<ymir::vdp::Color888, kVDP2CRAMColorCacheEntries> &Color555Cache() const {
        return m_cram555Cache;
    }

private:
    ymir::vdp::VDPState &m_state;
    const ymir::vdp::config::VDP2AccessPatternsConfig &m_accessPatternsConfig;
    ymir::vdp::config::VDP2DebugRender m_debugRenderOptions; // all layers on; used by UpdateEnabledBGs
    ymir::vdp::config::Enhancements m_enhancements;

    // Resolution (d3d:3442-3445)
    uint32_t m_HRes = ymir::vdp::kDefaultResH;
    uint32_t m_VRes = ymir::vdp::kDefaultResV;
    bool m_exclusiveMonitor = false;
    bool m_pendingClear = false;
    uint32_t m_pendingClearColor = 0xFF000000u;

    // Change tracking (d3d:514-590)
    static constexpr uint32_t kVRAMChunkShift = 8; // 256-byte chunks
    util::DirtyBitmap<(ymir::vdp::kVDP2VRAMSize >> kVRAMChunkShift)> m_vramDirty;
    CRAMColorCache m_cramColorCache{};
    // Brimir: SoftwareVDPRenderer::m_CRAMCache, maintained the same way (sw:97, 233-236, 905-914)
    std::array<ymir::vdp::Color888, kVDP2CRAMColorCacheEntries> m_cram555Cache{};
    uint64_t m_cramGeneration = 1;
    uint64_t m_flushedCRAMGeneration = 0;

    // Parameters: current (packed this line) and flushed (in effect for the open batch)
    VDP2CommonRenderParams m_common{};
    VDP2LayerRenderParams m_layer{};
    VDP2ComposeParams m_compose{};
    VDP2CommonRenderParams m_flushedCommon{};
    VDP2LayerRenderParams m_flushedLayer{};
    VDP2ComposeParams m_flushedCompose{};

    // Per-line arrays (persist across frames; lines prepared with the display latch off repeat the last computed state,
    // as software's)
    std::array<ColorR8G8B8A8, 2 * kMaxResV> m_lnclBack{};
    std::array<VDP2RotParamBase, 2 * kMaxResV> m_rotParamBases{};
    std::array<VDP2NBGLine, 4 * kMaxResV> m_nbgLines{};
    // Table address of the last computed rotation parameter line (Xst, Yst and KA are state2.rotParamStates)
    std::array<uint32_t, 2> m_rotTableAddress{};

    // Brimir: the parts of the software renderer's VRAMFetchers (m_vramFetchers[field][nbg], saved in the renderer
    // section of save states) that carry NBG state from one line to the next, in software's representation: the
    // character pipeline and the last vertical cell scroll value read. (charData/charDataAddress, a read cache, are
    // not kept.)
    struct NBGFetcher {
        ymir::vdp::Character currChar{};
        ymir::vdp::Character nextChar{};
        uint32_t lastCharIndex = 0xFFFFFFFF;
        uint8_t lastCellX = 0xFF; // software's VRAMFetcher leaves it indeterminate until the first PrepareLine
        uint32_t lastVCellScroll = 0xFFFFFFFF;
    };
    struct NBGCharLocation {
        uint32_t pageAddress;
        uint32_t charIndex;
        uint8_t cellX;
        uint32_t bank;
    };
    std::array<std::array<NBGFetcher, 4>, 2> m_nbgFetchers{};
    // Dispatch row of the last line each NBG was drawn on in this record, per field (vertical mosaic source)
    std::array<std::array<uint32_t, 4>, 2> m_nbgLastDrawnRow{};

    // Batching (d3d:586-587)
    uint32_t m_nextLayerRenderLine = 0;
    uint32_t m_nextComposeLine = 0;
    struct PendingCompose {
        uint32_t startY, lines; // dispatch rows
        uint32_t paramsOffset;
    };
    std::vector<PendingCompose> m_pendingComposes; // closed compose ranges waiting for their layer rows
    uint32_t m_pendingCopiesStart = 0;
    uint32_t m_layerSnapshotOffset = UINT32_MAX;
    uint32_t m_composeSnapshotOffset = UINT32_MAX;

    // Frame record
    Vdp2FrameRecord m_record{};
    bool m_inFrame = false;
    bool m_frameReady = false;

    // --- Ported functions -------------------------------------------------------------------------------------------
    void CacheCRAMColor(uint32_t address);
    void CacheAllCRAMColors();
    void UpdateEnabledLayers();
    void CalcAccessPatterns();
    void CalcVCellScrollDelay();
    void InitFrame();
    void InitNormalBG(uint32_t index);
    void UpdateLineScreenScroll(uint32_t y, const ymir::vdp::BGParams &bgParams,
                                ymir::vdp::NBGLayerState &bgState);
    void FinishLine();
    void DrawLineColorBackScreens(uint32_t y);
    void UpdateRotationParameterBases(uint32_t y);
    void UpdateCommonRenderParams();
    void UpdateLayerRenderParams();
    void UpdateComposeParams();
    void PackParams();
    void UpdateState();
    void FlushVRAM();
    void FlushCRAM();

    // --- Brimir ------------------------------------------------------------------------------------------------------
    void PrepareLine(uint32_t y);
    void StoreNBGLines(uint32_t y);
    void DrawNBGFetcher(uint32_t index, const VDP2NBGLine &line, NBGFetcher &fetcher);
    NBGCharLocation LocateNBGChar(const ymir::vdp::BGParams &bgParams, uint32_t scrollX, uint32_t scrollY) const;
    ymir::vdp::Character FetchNBGCharacter(const ymir::vdp::BGParams &bgParams, const NBGCharLocation &loc) const;
    void FetchNBGChar(const ymir::vdp::BGParams &bgParams, NBGFetcher &fetcher, uint32_t scrollX, uint32_t scrollY);
    void StartRecord();
    void Cache555Color(uint32_t address);
    void StoreLineColorBackScreens(uint32_t y);
    void CountFeatures();
    uint32_t YShift() const;
    uint32_t AppendArena(const void *data, size_t size, uint32_t alignment = 4);
    uint32_t SnapshotLayerParams();
    uint32_t SnapshotComposeParams();
    void CloseCompose(uint32_t lastY);
    void RenderLayerLines(uint32_t lastY);
    void EmitBatch(uint32_t layerStartY, uint32_t layerLines, const PendingCompose *compose, uint32_t layerOffset);
};

} // namespace brimir::gpu::vdp2
