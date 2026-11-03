#pragma once

// A CPU executable specification of the N x VDP1 rasteriser (GPU stage 3, option A'; docs/ymir-hw-renderer-notes/
// stage3-research.md section 1.4). It is written from the research and the native reference (vdp1_raster_reference.hpp)
// independently of the GPU code, in integer math, and replays the native front half's op lists into two surfaces:
//   - native FBRAM, exactly as the native reference does (the N x path never touches it);
//   - the N x surface: the same eight slices with every native FBRAM word stored as an N x N block of raw words,
//     N x word index = nativeWord * N^2 + j * N + i for the sub-texel (i, j) (research 1.1).
//
// Rules: the developer chose R1 + T2 + L2 (stage 3 Ruling 3), the default of Rules below; the other candidates stay
// selectable for tests and renders.
//
// Generator (per native batch, Vulkan-free; what Task 3 moves into the front half). Every command of the batch becomes
// N x spans of one of three kinds, in command order:
//   - Rect (normal sprites, scaled sprites, distorted sprites and polygons with xa == xd, xb == xc, ya == yb, yc == yd):
//     area mapping. Native row k (a native span) gives N sub-rows and native pixel p of the row N sub-steps, so the
//     native pixel's N x N block is covered and the min vertex maps to N * v, the max vertex to N * v + N - 1 per axis.
//     Sub-step s of pixel (or row) P counted from the line's start is the position P + s / N. Texture (U per pixel, V
//     per row) and Gouraud (along the row, and on the edges per row) take the step count
//       K = clamp(phase((N * P + s) * num / (N * den)), c(P), c(P + 1)),   K = c(P) at s = 0,
//     with c(P) the native stepper's count (texels stepped; Gouraud: 5-bit steps from the value after Setup) and
//     num / den its steps per native pixel (Gouraud: (|intInc| * den + num) / den of the reduced channel). phase is
//     floor when the line (rows) runs towards larger coordinates and ceil when it runs towards smaller ones: rounding
//     towards the native pixel that owns the block. The native steppers round (round(u) of the end-point mapping);
//     reading their accumulators at sub-positions ("native DDA + sub-step", research 1.4) would put every step about
//     half a pixel into the block: 1:1 sprites would not be the nearest upscale and the last block would read past the
//     sprite. Texels are read in order (end codes as natively; HSS decided natively). A mapping of one step per pixel
//     keeps every block constant; shrinks show the steps natively skipped.
//     Rows and pixels running towards smaller coordinates map sub-step 0 to sub-texel 0 too: sub-texel i > 0 of a block
//     lies between the previous native pixel (or row) and this one, so it takes sub-step N - i of the previous one
//     (the first pixel or row repeats itself there). At sub-texel (0, 0) every value is native. One slot per N x pixel.
//   - Quad (other polygons and distorted sprites): N x vertices v_N = N * v + c per axis (VertexRule; R1: c =
//     floor((N - 1) / 2)), then a 32-bit QuadStepper / Edge / LineStepper over them (no 13-bit wrap), antialiased as
//     natively; texture and Gouraud steppers over the N x lengths; HSS from the native-equivalent length dmaj_N / N.
//     Clipping, mesh, double interlace, the alternate field and the FB address come from the native coordinates
//     (floor(X / N), floor(Y / N)) of every N x pixel. Lines are rejected as VDP1IsLineSystemClipped does, at native
//     coordinates; the N x quad termination (a box with a margin) and LineStepper::SystemClip over the N x extent of the
//     clip area drop no pixel the native clip would keep. Two slots per N x line step: key = base + 2 * step + isAA.
//     Degenerate quads (all four vertices collinear) follow ThinRule (T2: N copies offset 0..N-1 on the minor axis). A
//     quad command is generated whole in the first native batch that holds one of its spans (a command the front half
//     split at a batch capacity is skipped in the next batch: CommandCounts::Continuation).
//   - Replicate: each native fragment of a native span becomes its N x N block, key = base + N^2 * nativeKey + j * N + i.
//     For the commands whose native geometry wraps the 13-bit counters, decided from the command alone (so a command
//     split over batches keeps one kind): an edge delta outside [-4096, 4095], a quad major delta of 2048 or more, or
//     any native line longer than 1364 pixels (LineStepper's counters, vdp1_clip.hpp: every quad step's line of the
//     native QuadStepper, rect rows, polyline segments); lines and polylines with LineRule::NativeThick; every non-rect
//     quad without the quad path.
//   Lines and polylines with LineRule::Parallel (L2) become N parallel N x lines (offset 0..N-1 on the minor axis, the
//     major axis area-mapped to cover both end pixels' blocks), quad-kind spans without antialiasing.
//
// Sub-batches: a native batch's N x spans are cut into sub-batches at a slot (and span) capacity, between spans; each
// sub-batch has its own slot range from 0 and resolves on its own, in order. The order inside a sub-batch is the
// command order, then quad step / row, sub-row, pixel, sub-step, main before antialias: VDP1's drawing order at N x.
//
// Resolve: the native per-word ordered semantics (vdp1_raster_reference.hpp part 3) per N x word.
// Erase and CPU framebuffer writes: the native word's change applied to its whole N x N block.

#include "vdp1_raster_reference.hpp"

#include <gpu/vdp1/vdp1_bindings.hpp>
#include <gpu/vdp1/vdp1_front.hpp>

#include <array>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace brimir::test::vdp1nx {

namespace ref = brimir::test::vdp1ref;
using ymir::vdp::Color555;
using ymir::vdp::CoordS32;

// Candidate rules (Task 1 rendered them; the developer chose R1 + T2 + L2)
enum class VertexRule : uint8 {
    FloorHalfDown, // R1: c = floor((N - 1) / 2)
    FloorHalf,     // R2: c = floor(N / 2)
    CentroidSign,  // R3: c = N - 1 above the vertices' centroid, 0 below, floor((N - 1) / 2) on it, per axis
};
enum class ThinRule : uint8 {
    Hairline, // degenerate quads stay 1 N x pixel wide (no special case)
    Parallel, // N copies offset 0..N-1 on the minor axis (native thickness)
};
enum class LineRule : uint8 {
    NativeThick, // lines and polylines replicate the native fragments
    Parallel,    // N parallel N x lines
};

// The default is the rule set the developer chose (stage 3 Ruling 3): R1 + T2 + L2
struct Rules {
    VertexRule vertex = VertexRule::FloorHalfDown;
    ThinRule thin = ThinRule::Parallel;
    LineRule line = LineRule::Parallel;
};

const char *Name(VertexRule rule);
const char *Name(ThinRule rule);
const char *Name(LineRule rule);

struct Options {
    uint32 scale = 2; // N, 1 to 4
    Rules rules;
    bool quadPath = true;  // false: non-rect quads replicate (rect + replicate only: every sample point is native)
    bool rectPath = true;  // false: rect commands replicate too (tests)
    uint32 subBatchSlots = brimir::gpu::vdp1::kMaxFragmentsPerBatch; // sub-batch capacities
    uint32 subBatchSpans = brimir::gpu::vdp1::kMaxSpansPerBatch;
    // Resolve each sub-batch by applying the fragments in emission order instead of by key (shows that the keys encode
    // the emission order)
    bool resolveInEmissionOrder = false;
    uint64 shuffleSeed = 0; // non-zero: shuffle each sub-batch's fragments before the keyed resolve
    bool resolveDescendingForTesting = false; // keys in descending order (shows that a test is order-sensitive)
};

// -----------------------------------------------------------------------------
// N x surface

struct NxFbram {
    uint32 scale = 1;
    // Slices in Vdp1MainSlice / Vdp1AltSlice / Vdp1MeshSlice order (main fb0, fb1, alt fb0, fb1, mesh[0] fb0, fb1,
    // mesh[1] fb0, fb1); index nativeWord * N^2 + j * N + i
    std::array<std::vector<uint16>, 8> slices;

    explicit NxFbram(uint32 n = 1);
    uint32 Sub() const {
        return scale * scale;
    }
    uint16 Word(uint32 slice, uint32 nativeWord, uint32 i, uint32 j) const {
        return slices[slice][nativeWord * Sub() + j * scale + i];
    }
    // Every N x word becomes its native word
    void ReplicateFrom(const ref::Fbram &fb);
};

// The native word of a slice (big-endian bytes)
uint16 NativeWord(const ref::Fbram &fb, uint32 slice, uint32 word);

// First difference of the sample points (sub-texel (0, 0)) from native FBRAM, or "" (and the count in `mismatches`)
std::string CompareSamplePoints(const ref::Fbram &native, const NxFbram &nx, uint64 *mismatches = nullptr);
// First difference of two N x surfaces, or ""
std::string CompareNx(const NxFbram &a, const NxFbram &b);

// -----------------------------------------------------------------------------
// Generator

enum class SpanKind : uint8 { Rect, Quad, Replicate };

struct NxSpan {
    SpanKind kind = SpanKind::Replicate;
    bool antiAlias = false; // Quad
    uint32 cmdIndex = 0;    // into the native batch's commands
    CoordS32 a{}, b{};      // Rect, Replicate: native end points; Quad: N x end points
    uint32 subRow = 0;      // Rect: the sub-row in stepping order (0..N-1)
    sint32 texV = 0;        // texel row: Rect of the sub-row, Quad of the N x quad step, Replicate native
    Color555 gouraudL{.u16 = 0}, gouraudR{.u16 = 0};
    uint32 slots = 0;    // Rect: N * native Length(); Quad: 2 * N x Length(); Replicate: N^2 * native slots
    uint32 slotBase = 0; // set by the sub-batch split
};

// How the generator classified the commands (each command once per native batch it has spans in; continuations of
// a split quad-path command counted as Continuation)
struct CommandCounts {
    enum Class : uint32 {
        Rect, Quad, Thin, ThinParallel, LineParallel, ReplicateWrap, ReplicateLine, ReplicateNoQuadPath, Continuation,
        Count
    };
    std::array<uint64, Count> counts{};
    static const char *Name(uint32 c);
};

struct BatchInput {
    std::span<const ref::CommandParams> commands;
    std::span<const ref::RasterState> states;
    std::span<const ref::Span> spans;    // the native spans (fragBase as recorded)
    std::span<const uint32> spanSlots;   // their native slot counts
    bool firstCommandContinues = false;  // command 0 continues the previous batch's last command (capacity split)
};

// The batch's N x spans, in order (slotBase unset)
std::vector<NxSpan> GenerateBatch(const Options &options, const BatchInput &input, CommandCounts *counts = nullptr);

// Cuts spans into sub-batches (sets slotBase); returns the sub-batch boundaries [begin, end) as indices into spans
std::vector<std::pair<uint32, uint32>> SplitSubBatches(const Options &options, std::vector<NxSpan> &spans);

// -----------------------------------------------------------------------------
// Raster and resolve

struct NxFragment {
    uint32 key = 0;    // slot in the sub-batch
    uint32 target = 0; // ref::kTarget* layout of the native word (word, alt, odd byte, mode bits)
    uint32 sub = 0;    // j * N + i
    uint32 color = 0;
};

// Appends the span's fragments (keys in [slotBase, slotBase + slots))
void RasterSpan(uint32 n, const ref::RasterState &state, const ref::CommandParams &cmd, const NxSpan &span,
                const uint8 *vram, std::vector<NxFragment> &out);

struct NxResolveStats {
    uint64 fragments = 0;
    uint64 units = 0;
    uint32 maxPerUnit = 0;
    uint64 multiHalfTransparent = 0; // units given two or more half-transparency fragments (double blending)
    uint64 halfTransparent = 0;      // half-transparency fragments
    uint64 overwrite = 0;            // overwrite-class fragments (replace, half-luminance, Gouraud, 8 bpp; not MSB-on)
    uint64 mesh = 0;                 // fragments written to the mesh plane (transparent meshes)
};
// Applies fragments sorted by (unit, key) (or in emission order) to the draw framebuffer of the N x surface
enum class ResolveOrder : uint8 { Key, Emission, KeyDescendingForTesting };
NxResolveStats Resolve(uint32 drawFB, std::vector<NxFragment> fragments, NxFbram &nx,
                       ResolveOrder order = ResolveOrder::Key);

uint64 Erase(const brimir::gpu::vdp1::Vdp1ErasePushConstants &params, NxFbram &nx);
void ApplyFbWrites(std::span<const brimir::gpu::vdp1::Vdp1FbWrite> writes, NxFbram &nx);

// -----------------------------------------------------------------------------
// Driver

struct Stats {
    uint64 batches = 0, subBatches = 0;
    uint64 nativeSpans = 0, nativeSlots = 0, nativeFragments = 0;
    std::array<uint64, 3> spans{}, slots{}, fragments{}; // by SpanKind
    uint32 maxSubBatchSlots = 0;
    uint32 maxPerUnit = 0;
    uint64 multiHalfTransparent = 0; // N x units given two or more half-transparency fragments in one sub-batch
    uint64 halfTransparent = 0;
    uint64 overwrite = 0; // overwrite-class fragments (research 1.6 fast path)
    uint64 mesh = 0;      // fragments written to the mesh plane
    CommandCounts commands;
    uint64 nativeLongLines = 0; // native spans with a major delta above 1364 (their native counters wrap)
    double generateSeconds = 0, rasterSeconds = 0, resolveSeconds = 0, nativeSeconds = 0;

    uint64 Spans() const {
        return spans[0] + spans[1] + spans[2];
    }
    uint64 Slots() const {
        return slots[0] + slots[1] + slots[2];
    }
    uint64 Fragments() const {
        return fragments[0] + fragments[1] + fragments[2];
    }
};

// Diagnostics (the confinement metric): every N x step the raster takes on a watched N x word, drawn or not
struct ProbeEvent {
    enum Status : uint8 { Drawn, Transparent, EndCode, Clipped, Mesh, OtherLine };
    uint64 batch = 0;   // ReferenceNx's batch count when it was drawn (1-based)
    uint32 drawFB = 0;
    uint32 cmdIndex = 0;
    SpanKind kind = SpanKind::Replicate;
    uint32 unit = 0;    // word | alt
    uint32 sub = 0;     // j * N + i
    Status status = Drawn;
    uint32 color = 0;   // the fragment colour (after Gouraud); Drawn only
    uint32 mode = 0;    // Drawn: the fragment's ref::kTarget* mode bits (colour calculation, mesh, MSB-on, 8 bpp)
    bool texelKnown = false; // textured quad-kind steps: the texel read for the step
    sint32 u = 0;
    sint32 v = 0;
    uint16 texel = 0; // the raw texel value
};
struct RasterProbe {
    std::function<bool(uint32 drawFB, uint32 unit, uint32 sub)> want;
    std::vector<ProbeEvent> events;
};
const char *Name(ProbeEvent::Status status);
const char *Name(SpanKind kind);

// What one native draw batch was (ReferenceNx::SetBatchObserver): its decoded commands and native spans, the native
// fragments before the resolve and the N x spans the generator made of it
struct BatchObservation {
    uint32 drawFB = 0;
    std::span<const ref::CommandParams> commands;
    std::span<const ref::RasterState> states;
    std::span<const ref::Span> spans;
    std::span<const ref::Fragment> nativeFragments;
    std::span<const NxSpan> nxSpans;
};

class ReferenceNx {
public:
    // Called for every draw batch, after the native resolve and before the N x raster (tests: the confinement metric)
    void SetBatchObserver(std::function<void(const BatchObservation &)> observer) {
        m_observer = std::move(observer);
    }
    // Records the N x raster's steps on the words `probe->want` selects (null: none)
    void SetRasterProbe(RasterProbe *probe) {
        m_probe = probe;
    }

    // `native` and `nx` hold the start state (nx usually ReplicateFrom(native)); VDP1 VRAM starts as `vram` (zero when
    // empty) and follows the batches' uploads
    ReferenceNx(ref::Fbram &native, NxFbram &nx, Options options, std::span<const uint8> vram = {});

    void Replay(const brimir::gpu::vdp1::Vdp1OpList &ops);

    const Stats &GetStats() const {
        return m_stats;
    }
    // Native draw framebuffer of the last batch (or erase)
    uint32 LastDrawFB() const {
        return m_lastDrawFB;
    }
    // VDP1 VRAM as the batches' uploads left it
    std::span<const uint8> Vram() const {
        return m_vram;
    }

private:
    void DrawBatch(const brimir::gpu::vdp1::Vdp1OpList &ops, const brimir::gpu::vdp1::Vdp1DrawBatch &batch);

    ref::Fbram &m_native;
    NxFbram &m_nx;
    Options m_options;
    std::vector<uint8> m_vram;
    Stats m_stats;
    uint32 m_lastDrawFB = 0;
    uint64 m_shuffle = 0;
    std::function<void(const BatchObservation &)> m_observer;
    std::vector<ref::Fragment> m_observedFragments;
    RasterProbe *m_probe = nullptr;
    // The previous batch's end, its last command and last span's quad step (capacity-split continuations)
    bool m_prevCapacity = false;
    brimir::gpu::vdp1::Vdp1CommandParams m_prevCommand{};
    uint32 m_prevQuadStep = 0;
};

} // namespace brimir::test::vdp1nx
