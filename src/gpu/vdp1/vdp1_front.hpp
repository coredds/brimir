#pragma once

// Vulkan-free front half of the GPU VDP1 renderer (GPU stage 2; docs/ymir-hw-renderer-notes/stage2-vdp1-research.md
// sections 2.3, 4.1-4.3 and 5). It follows the VDP's VDP1 calls on the emulator thread and turns them into an op stream
// the Vulkan back half replays in emulation order:
// - draw batches: the commands' parameters (Vdp1CommandParams) and one Vdp1Span per line the software renderer plots,
//   with the fragment slot prefix sums, plus the VDP1 VRAM uploads the batch needs before it runs;
// - erases (VDP1DoEraseFramebuffer's region and flags, as Vdp1ErasePushConstants);
// - CPU framebuffer writes (Vdp1FbWrite records holding the value at the time of the write).
//
// The command handlers and the quad and line loops are ports of the unthreaded software renderer
// (src/core/src/ymir/hw/vdp/renderer/vdp_renderer_sw.cpp, "sw" below; each ported function names its range) with the
// shared steppers (vdp1_steppers.hpp) and quad termination (vdp1_clip.hpp). They read VDP1 VRAM, the registers and the
// clip state (VDPState::state1, which the clip commands write, as software's do) live at command time. A line software
// rejects (VDP1IsLineSystemClipped) or whose SystemClip leaves no pixel gets no span. The per-pixel half of
// VDP1PlotLine / VDP1PlotTexturedLine / VDP1PlotPixel runs on the GPU (the raster and resolve passes; the CPU model is
// tests/unit/vdp1_raster_reference.hpp).
//
// Batches. A batch is a run of draw commands resolved together; a batch boundary is a barrier. A batch closes:
// - before an op of another kind (erase, CPU framebuffer write);
// - before a VRAM write into a range a command of the batch uses: its texture, colour look-up table (CMDCOLR * 8,
//   32 bytes), Gouraud table and command table (32-byte granules);
// - when the draw framebuffer changed (a swap between commands): Vdp1BatchPushConstants::drawFB is per batch;
// - between spans at the fragment, span or command capacity (kMaxFragmentsPerBatch, kMaxSpansPerBatch,
//   kMaxCommandsPerBatch; a command split there is repeated in the next batch);
// - at MarkFrameBoundary, TakeOps, PostLoadStateSync and Reset.
// It does not close on register or enhancement changes: every command carries its raster flags and every fragment its
// 8-bpp and transparent-meshes bits, so a batch can mix modes.
//
// VRAM. The GPU's copy of VDP1 VRAM is updated by the uploads of each batch (256-byte chunks written since the last
// batch, snapshot when the batch closes). The VDP calls WriteVRAM after VDPState wrote the new value, so the front half
// keeps its own copy of VRAM as of the last call: a batch closed by a hazard uploads the value its commands read.
//
// Segments. MarkFrameBoundary (at VDP2EndFrame) ends a segment: segment k of a TakeOps result is replayed before the
// k-th VDP2 frame of that result and the last segment (the ops after the last boundary, for example the
// BlankingAndSync display erase) after it.
//
// Thread-confined: every call comes from the emulator thread (a worker can be added behind TakeOps later).
// No Vulkan header may be included here.

#include "gpu/vdp1/vdp1_bindings.hpp"
#include "gpu/vdp1/vdp1_hw_params.hpp"

#include <ymir/hw/vdp/vdp1_defs.hpp>
#include <ymir/hw/vdp/vdp_configs.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace brimir::gpu::vdp1 {

static_assert(kVRAMBufferSize == ymir::vdp::kVDP1VRAMSize, "the GPU's VDP1 VRAM copy is all of VDP1 VRAM");

/// @brief Why a batch was closed (diagnostics and tests).
enum class Vdp1BatchEnd : uint8_t {
    OpKind,        // an erase or a CPU framebuffer write followed
    VramHazard,    // a VRAM write into a range the batch reads
    FbSwap,        // the next command draws to the other framebuffer
    Capacity,      // the next span did not fit (fragments, spans or commands)
    FrameBoundary, // MarkFrameBoundary
    Take,          // TakeOps, PostLoadStateSync or Reset
};

/// @brief An upload into the GPU's copy of VDP1 VRAM: arena[arenaOffset, + size) to VRAM offset vramOffset.
struct Vdp1VramCopy {
    uint32_t arenaOffset;
    uint32_t vramOffset;
    uint32_t size;
};

/// @brief One draw batch.
struct Vdp1DrawBatch {
    uint32_t cmdOffset, cmdCount;   // Vdp1OpList::commands
    uint32_t spanOffset, spanCount; // Vdp1OpList::spans (Vdp1Span::cmdIndex is relative to cmdOffset)
    uint32_t fragmentCount;         // fragment slots: the last span's fragBase + its slots
    uint32_t drawFB;                // the framebuffer drawn to (0 or 1)
    uint32_t firstCopy, copyCount;  // Vdp1OpList::vramCopies, applied before the batch's passes
    Vdp1BatchEnd end;
};

enum class Vdp1OpKind : uint8_t { Draw, Erase, FbWrites };

/// @brief One op: index is into batches (Draw), erases (Erase) or fbWriteRuns (FbWrites).
struct Vdp1Op {
    Vdp1OpKind kind;
    uint32_t index;
};

/// @brief ops[firstOp, + opCount).
struct Vdp1Segment {
    uint32_t firstOp;
    uint32_t opCount;
};

/// @brief The ops since the last TakeOps, in emulation order, with the data they reference.
struct Vdp1OpList {
    std::vector<Vdp1Op> ops;
    std::vector<Vdp1Segment> segments; // segments.size() - 1 frame boundaries; the last segment is trailing

    std::vector<Vdp1DrawBatch> batches;
    std::vector<Vdp1CommandParams> commands;
    std::vector<Vdp1Span> spans;
    std::vector<Vdp1VramCopy> vramCopies;
    std::vector<uint8_t> arena; // VRAM snapshots

    std::vector<Vdp1ErasePushConstants> erases;
    std::vector<Vdp1FbWrite> fbWrites;
    std::vector<Vdp1FbWritePushConstants> fbWriteRuns; // [offset, + count) of fbWrites, applied in order

    void Clear();
    bool Empty() const {
        return ops.empty();
    }
    /// @brief The frame boundaries (MarkFrameBoundary calls) the list spans.
    uint32_t FrameBoundaries() const {
        return segments.empty() ? 0u : static_cast<uint32_t>(segments.size() - 1);
    }
};

/// @brief The raster, bucket, scatter and resolve passes' push constants for a batch.
constexpr Vdp1BatchPushConstants Vdp1BatchConstants(const Vdp1DrawBatch &batch) {
    return {batch.spanOffset, batch.spanCount, batch.cmdOffset, batch.fragmentCount, batch.drawFB, kRasterScale};
}

/// @brief Batch capacities (the kMax*PerBatch constants; tests lower them).
struct Vdp1BatchLimits {
    uint32_t fragments = kMaxFragmentsPerBatch;
    uint32_t spans = kMaxSpansPerBatch;
    uint32_t commands = kMaxCommandsPerBatch;
    uint32_t fbWrites = kMaxFbWritesPerBatch;
};

/// @brief Counters since construction (diagnostics, tests, the front-half CPU time measurement).
struct Vdp1FrontStats {
    uint64_t commands = 0;     // ExecuteCommand calls
    uint64_t drawCommands = 0; // draw commands that produced at least one span
    uint64_t spans = 0;
    uint64_t fragmentSlots = 0;
    uint64_t batches = 0;
    std::array<uint64_t, 6> batchEnds{}; // by Vdp1BatchEnd
    uint64_t erases = 0;
    uint64_t fbWrites = 0;
    uint64_t vramUploadBytes = 0;
};

class Vdp1FrontEnd {
public:
    /// @brief Reads VDPState live (VRAM, registers, fbIndex, state1 and state2.layerEnabled) and writes state1 (clip
    /// commands), as the unthreaded software renderer does. VDP1 VRAM is uploaded whole with the first batch.
    explicit Vdp1FrontEnd(ymir::vdp::VDPState &state);

    void SetEnhancements(const ymir::vdp::config::Enhancements &enhancements);

    /// @brief After VDPState::Reset (VDP::Reset order): closes the open batch and uploads all of VRAM with the next
    /// one. doubleV is kept, as software's (sw:94-136 leaves m_VDP1doubleV alone); pending ops are kept.
    void Reset(bool hard);
    /// @brief After a save state load: VRAM changed without WriteVRAM calls; all of it is uploaded with the next batch.
    void PostLoadStateSync();

    /// @brief VDP1BeginFrame (sw:487-500, Brimir's): doubleV from the emulator thread's registers.
    void BeginFrame();
    /// @brief VDP1ExecuteCommand, unthreaded (sw:502-513 -> VDP1Cmd_Handle).
    void ExecuteCommand(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control);
    /// @brief VDP1EraseFramebuffer (sw:453-470, 959-1022): `vres` is the renderer's vertical resolution (m_VRes, from
    /// VDP2SetResolution); `cycles` 0 erases the whole region.
    void Erase(uint64_t cycles, uint32_t vres);
    /// @brief VDP1WriteFB (sw:356-374), after VDPState wrote the draw framebuffer: `address` is the byte address
    /// (masked as VDPState masks it), `value` the 16-bit word value in the bits of `byteMask` (0xFF00 an even byte,
    /// 0x00FF an odd byte, 0xFFFF a word). Records the value now; mirrors it to the alternate field when deinterlacing
    /// an interlaced mode; leaves the mesh framebuffers alone.
    void WriteFB(uint32_t address, uint16_t value, uint16_t byteMask);
    void WriteFB8(uint32_t address, uint8_t value) {
        WriteFB(address, (address & 1) ? value : static_cast<uint16_t>(value << 8), (address & 1) ? 0x00FF : 0xFF00);
    }
    void WriteFB16(uint32_t address, uint16_t value) {
        WriteFB(address & ~1u, value, 0xFFFF);
    }
    /// @brief VDP1WriteVRAM, after VDPState wrote the new value: closes the open batch if it reads the written word.
    void WriteVRAM(uint32_t address);
    /// @brief At VDP2EndFrame: the ops so far are replayed before that VDP2 frame.
    void MarkFrameBoundary();

    bool DoubleV() const {
        return m_doubleV != 0;
    }
    void SetDoubleV(bool doubleV) {
        m_doubleV = doubleV ? 1u : 0u;
    }

    /// @brief Closes the open batch and returns the ops since the last call.
    Vdp1OpList TakeOps();
    /// @brief Whether TakeOps would return any op (the open batch included).
    bool HasPendingOps() const {
        return m_batchOpen || m_fbRunOpen || !m_ops.ops.empty();
    }

    const Vdp1FrontStats &Stats() const {
        return m_stats;
    }

    void SetBatchLimitsForTesting(const Vdp1BatchLimits &limits);
    /// @brief Off: VRAM writes never close a batch (to show the tests notice).
    void SetHazardTrackingForTesting(bool enabled) {
        m_hazardTracking = enabled;
    }

private:
    ymir::vdp::VDPState &m_state;
    ymir::vdp::config::Enhancements m_enhancements{};
    uint32_t m_doubleV = 0;

    Vdp1OpList m_ops;
    uint32_t m_segmentStart = 0;
    Vdp1BatchLimits m_limits;
    bool m_hazardTracking = true;
    Vdp1FrontStats m_stats;

    // Open batch (commands and spans are appended to m_ops directly)
    bool m_batchOpen = false;
    Vdp1DrawBatch m_batch{};
    bool m_fbRunOpen = false;

    // The command being executed
    Vdp1CommandParams m_cmd{};
    bool m_cmdInBatch = false;
    bool m_cmdHasSpans = false;
    struct Range {
        uint32_t address, size;
    };
    std::array<Range, 4> m_cmdReads{}; // command table, Gouraud table, texture, look-up table
    uint32_t m_cmdReadCount = 0;
    sint32 m_clipH = 0, m_clipV = 0; // the system clip in pixels (V with the doubleV doubling)

    // VRAM: the front half's copy (as of the last WriteVRAM call), the chunks not yet uploaded and the granules the open
    // batch reads
    static constexpr uint32_t kVRAMSize = static_cast<uint32_t>(ymir::vdp::kVDP1VRAMSize);
    static constexpr uint32_t kVRAMMask = kVRAMSize - 1;
    static constexpr uint32_t kUploadChunkShift = 8; // 256 bytes
    static constexpr uint32_t kHazardShift = 5;      // 32 bytes
    std::vector<uint8_t> m_vram;
    std::array<uint64_t, (kVRAMSize >> kUploadChunkShift) / 64> m_vramDirty{};
    std::array<uint64_t, (kVRAMSize >> kHazardShift) / 64> m_hazard{};

    // --- Ported functions (sw) ----------------------------------------------------------------------------------------
    void CmdDrawNormalSprite(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control);
    void CmdDrawScaledSprite(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control);
    void CmdDrawDistortedSprite(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control);
    void CmdDrawPolygon(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control);
    void CmdDrawPolylines(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control);
    void CmdDrawLine(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control);
    void CmdSetSystemClipping(uint32_t cmdAddress);
    void CmdSetUserClipping(uint32_t cmdAddress);
    void CmdSetLocalCoordinates(uint32_t cmdAddress);
    void PlotTexturedQuad(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control, ymir::vdp::VDP1Command::Size size,
                          ymir::vdp::CoordS32 coordA, ymir::vdp::CoordS32 coordB, ymir::vdp::CoordS32 coordC,
                          ymir::vdp::CoordS32 coordD);
    bool IsLineSystemClipped(ymir::vdp::CoordS32 coord1, ymir::vdp::CoordS32 coord2) const;
    bool IsQuadSystemClipped(ymir::vdp::CoordS32 coordA, ymir::vdp::CoordS32 coordB, ymir::vdp::CoordS32 coordC,
                             ymir::vdp::CoordS32 coordD) const;

    // --- Brimir --------------------------------------------------------------------------------------------------------
    uint16_t ReadVRAM16(uint32_t address) const;
    void BeginDrawCommand(uint32_t cmdAddress, ymir::vdp::VDP1Command::Control control, uint16_t cmdpmod, uint16_t color,
                          bool textured, bool antiAlias,
                          const std::array<ymir::vdp::CoordS32, 4> &vertices);
    void AddRead(uint32_t address, uint32_t size);
    void AddSpan(ymir::vdp::CoordS32 a, ymir::vdp::CoordS32 b, uint32_t quadStep, uint32_t texV, uint16_t gouraudL,
                 uint16_t gouraudR);
    void OpenBatch();
    void CloseBatch(Vdp1BatchEnd end);
    void CloseFbRun();
    void CloseAll(Vdp1BatchEnd end);
    void MarkHazards(uint32_t address, uint32_t size);
    void UploadDirtyVRAM(Vdp1DrawBatch &batch);
    void ResyncVRAM();
};

} // namespace brimir::gpu::vdp1
