#pragma once

// The N x VDP1 span generator (GPU stage 3, option A'; docs/ymir-hw-renderer-notes/stage3-research.md section 1.4 and
// the design doc's upscaling section). It turns one native draw batch of a Vdp1OpList into N x sub-batches of
// Vdp1NxSpan (vdp1_hw_params.hpp): the same commands in the same order, every command one of three span kinds, cut
// into sub-batches only at the N x capacities and only between spans. Each sub-batch has its own slot range from 0, in
// emission order, so it resolves on its own and the sub-batches resolve in order.
//
// The specification is the CPU reference's generator (tests/unit/vdp1_nx_reference.hpp: GenerateBatch and
// SplitSubBatches with the default Rules, R1 + T2 + L2, quad and rect paths on); tests/unit/test_vdp1_nx_front.cpp
// checks that both produce the same spans, slot ranges and sub-batches. In short:
//   - Rect (sprites and polygons with xa == xd, xb == xc, ya == yb, yc == yd): N sub-rows per native row, the texel row
//     and the edge Gouraud colours of each sub-row by the direction-aware clamp-and-phase rule; rows running upwards
//     map sub-row j > 0 to sub-step N - j of the previous row;
//   - Quad (other polygons and distorted sprites): the N x quad over v_N = N * v + floor((N - 1) / 2) with 32-bit
//     steppers, N x termination and system clip; thin (collinear) quads as N parallel copies; lines and polylines as N
//     parallel N x lines. Generated whole in the first batch holding the command; a continuation (the command repeated
//     after a capacity split) generates nothing;
//   - Replicate: commands whose native counters wrap (13-bit edge deltas, quad major >= 2048, any native line longer
//     than 1364 pixels), decided per command so a split command keeps one kind.
//
// Thread-confined; the arena's buffers are reused, so a call allocates only when they grow. No Vulkan header may be
// included here.

#include "gpu/vdp1/vdp1_front.hpp"
#include "gpu/vdp1/vdp1_hw_params.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace brimir::gpu::vdp1 {

/// @brief Spans [spanOffset, + spanCount) of Vdp1NxArena::spans; their slots are [0, slotCount).
struct Vdp1NxSubBatch {
    uint32_t spanOffset;
    uint32_t spanCount;
    uint32_t slotCount;
};

/// @brief Sub-batch capacities (tests lower them).
struct Vdp1NxLimits {
    uint32_t slots = kMaxNxSlotsPerSubBatch;
    uint32_t spans = kMaxNxSpansPerSubBatch;
};

/// @brief How the generator classified a command (once per batch it has spans in).
enum class Vdp1NxCommandClass : uint8_t {
    Rect,
    Quad,
    ThinParallel,  // collinear quad: N parallel copies
    LineParallel,  // line or polyline: N parallel N x lines
    ReplicateWrap, // native counters wrap
    Continuation,  // a quad-kind command repeated after a capacity split (generated in an earlier batch)
    Count
};

/// @brief Counters since construction or the last TakeStats.
struct Vdp1NxStats {
    std::array<uint64_t, 3> spans{}; // by kind (kNxSpanRect, kNxSpanQuad, kNxSpanReplicate)
    std::array<uint64_t, 3> slots{};
    uint64_t subBatches = 0;
    uint64_t batches = 0;
    std::array<uint64_t, static_cast<size_t>(Vdp1NxCommandClass::Count)> commands{};
    uint64_t nanoseconds = 0; // time in GenerateNx
    // Native spans skipped as malformed (the front half never makes one): a rect command's span that is not one of its
    // rows, or a span whose command index is out of the batch
    uint64_t malformedSpans = 0;
};

struct Vdp1NxArena {
    /// @brief The spans and sub-batches of the GenerateNx calls since the last Clear (sub-batches index `spans`).
    std::vector<Vdp1NxSpan> spans;
    std::vector<Vdp1NxSubBatch> subBatches;
    Vdp1NxLimits limits;
    Vdp1NxStats stats;

    /// @brief Forgets the spans and sub-batches (keeps the buffers, the limits and the stats).
    void Clear() {
        spans.clear();
        subBatches.clear();
    }
    Vdp1NxStats TakeStats() {
        const Vdp1NxStats s = stats;
        stats = {};
        return s;
    }

    // Scratch (per rect command)
    std::vector<int32_t> texV;
    std::vector<uint16_t> gouraudL, gouraudR;
};

/// @brief Appends the N x spans and sub-batches of ops.batches[batchIndex] at scale N (1 to kMaxNxScale) to the arena
/// and returns the batch's sub-batches (valid until the next call; none when no N x span is left, for example a
/// continuation or N x lines the system clip drops). Whether the batch's first command continues the previous batch's
/// last one (a capacity split) is read from ops.batches[batchIndex - 1]: the front half opens the continuing batch right
/// after closing the split one, so they are always adjacent in one op list. An invalid scale or batch gives nothing.
std::span<const Vdp1NxSubBatch> GenerateNx(const Vdp1OpList &ops, uint32_t batchIndex, uint32_t scale,
                                           Vdp1NxArena &arena);

} // namespace brimir::gpu::vdp1
