#pragma once

// A CPU executable specification of the GPU VDP1 rasterizer (GPU stage 2; docs/ymir-hw-renderer-notes/
// stage2-vdp1-research.md sections 4.1-4.3), written from the software renderer (vdp_renderer_sw.cpp,
// vdp1_steppers.hpp) independently of Ymir's shaders. The GPU front half (Task 3) and shaders (Task 4) are checked
// against it part by part; the tests check it against SoftwareVDPRenderer's FBRAM byte for byte.
//
// Three separable parts:
//   1. Span builder: VDP1 command -> command parameters and spans (one span per line software plots). A test-local
//      port of the software command handlers and quad loops (with VDP1QuadTermination); Task 3's front half
//      replaces it.
//   2. RasterSpan(span) -> fragments: a pure function of the span, its command, the batch's raster state and VRAM. It
//      steps the line iteratively with a 32-bit port of LineStepper/TextureStepper/GouraudStepper (no SetStep jumps)
//      and runs the loop body of VDP1PlotLine / VDP1PlotTexturedLine and the read half of VDP1PlotPixel (clipping,
//      mesh skip, double-interlace skip, address). Every pixel VDP1PlotPixel would write becomes a fragment with
//      key = fragBase + 2 * step + isAA, fragBase being the prefix sum of the batch's span slot counts (2 * Length()
//      after LineStepper::SystemClip). Key order is software's emission order.
//   3. Resolve(fragments in any order) -> FBRAM: per FBRAM word (16 bits of the draw framebuffer or of its alternate
//      field), the word's fragments sorted by key are applied with the write half of VDP1PlotPixel to the word's
//      (main, mesh) pair: colour calculation in 16 bpp, bytes in 8 bpp, MSB-on ORing bit 15 of the word.
//
// Batches (a run of commands with the same raster state, cut between spans at a fragment limit) are resolved in order;
// a batch boundary is a barrier.

#include <ymir/core/types.hpp>
#include <ymir/hw/vdp/vdp1_defs.hpp>
#include <ymir/hw/vdp/vdp_common_defs.hpp>
#include <ymir/util/inline.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace brimir::test::vdp1ref {

using ymir::vdp::Color555;
using ymir::vdp::CoordS32;
using ymir::vdp::SpriteFB;
using ymir::vdp::VDP1Command;

inline constexpr uint32 kVRAMMask = 0x7FFFF;
inline constexpr uint32 kWordsPerFB = 0x20000; // 256 KiB / 2

// -----------------------------------------------------------------------------
// 32-bit ports of the steppers (vdp1_steppers.hpp). Arithmetic wraps modulo 2^32 like the GPU's (and like the
// software's in practice); no 64-bit operations.

namespace port {

FORCE_INLINE sint32 Add(sint32 a, sint32 b) {
    return static_cast<sint32>(static_cast<uint32>(a) + static_cast<uint32>(b));
}
FORCE_INLINE sint32 Sub(sint32 a, sint32 b) {
    return static_cast<sint32>(static_cast<uint32>(a) - static_cast<uint32>(b));
}
FORCE_INLINE sint32 Mul(sint32 a, uint32 b) {
    return static_cast<sint32>(static_cast<uint32>(a) * b);
}
FORCE_INLINE sint32 Shl(sint32 a, uint32 s) {
    return static_cast<sint32>(static_cast<uint32>(a) << s);
}

// LineStepper<13>
struct Line {
    static constexpr uint32 kShift = 32 - 13;

    sint32 num = 0, den = 0, accum = 0, accumTarget = 0;
    sint32 xMajInc = 0, yMajInc = 0, xMinInc = 0, yMinInc = 0;
    sint32 x = 0, y = 0, xEnd = 0, yEnd = 0;
    uint32 dmaj = 0, step = 0, length = 0;
    sint32 aaXInc = 0, aaYInc = 0;
    bool antiAlias = false;

    FORCE_INLINE Line(CoordS32 coord1, CoordS32 coord2, bool aa) {
        const sint32 x1 = coord1.x(), y1 = coord1.y(), x2 = coord2.x(), y2 = coord2.y();
        x = x1;
        y = y1;
        xEnd = x2;
        yEnd = y2;
        antiAlias = aa;

        sint32 dx = x2 - x1;
        sint32 dy = y2 - y1;
        sint32 adx = std::abs(dx);
        sint32 ady = std::abs(dy);
        dmaj = static_cast<uint32>(std::max(adx, ady));
        length = dmaj + 1;
        step = 0;

        const bool xMajor = adx >= ady;
        if (xMajor) {
            xMajInc = dx >= 0 ? +1 : -1;
            yMajInc = 0;
            xMinInc = 0;
            yMinInc = dy >= 0 ? +1 : -1;
        } else {
            xMajInc = 0;
            yMajInc = dy >= 0 ? +1 : -1;
            xMinInc = dx >= 0 ? +1 : -1;
            yMinInc = 0;
            std::swap(dx, dy);
            std::swap(adx, ady);
        }
        num = ady << 1;
        den = adx << 1;
        accum = adx + 1;
        accumTarget = 0;
        if (!aa && dx < 0) {
            ++accumTarget;
        }
        accum = Add(accum, num);

        x -= xMajInc;
        y -= yMajInc;

        if (aa) {
            accum = Sub(accum, 1);
            --accumTarget;
            const bool samesign = (x1 > x2) == (y1 > y2);
            if (xMajor) {
                aaXInc = samesign ? 0 : -xMajInc;
                aaYInc = samesign ? -yMinInc : 0;
            } else {
                aaXInc = samesign ? 0 : -xMinInc;
                aaYInc = samesign ? -yMajInc : 0;
            }
        }

        num = Shl(num, kShift);
        den = Shl(den, kShift);
        accum = Shl(accum, kShift);
        accumTarget = Shl(accumTarget, kShift);
    }

    // Set by SystemClip when one of its minor-step loops ("while (accum <= accumTarget) accum += den") never ends with
    // wrapping arithmetic. That happens for lines without antialiasing whose major delta is 2048 (mod 4096): den is
    // (2 * 2048) << 19 = INT_MIN, so the accumulator alternates between two values, and if the non-negative one is
    // <= accumTarget (0 or 1 << 19) the loop never exits. The software renderer has the same loop (with signed
    // overflow, undefined behaviour): it hangs or not depending on the compiled code. See the tests.
    bool undefined = false;

    // Runs a minor-step loop; false (and undefined set) if it would never end. den is a multiple of 2^20, so the
    // accumulator's orbit has at most 4096 values: a loop that has not ended after 4096 adds never ends.
    FORCE_INLINE bool SettleMinor(sint32 &acc, sint32 &px, sint32 &py) {
        if (den == 0) {
            return true;
        }
        for (uint32 n = 0; acc <= accumTarget; ++n) {
            if (n > 4096) {
                undefined = true;
                return false;
            }
            acc = Add(acc, den);
            px += xMinInc;
            py += yMinInc;
        }
        return true;
    }

    // LineStepper::SystemClip: clips to 0..width x 0..height; returns the steps skipped at the start
    FORCE_INLINE uint32 SystemClip(sint32 width, sint32 height) {
        static constexpr sint32 kPadding = 1;
        width += kPadding + 1;
        height += kPadding + 1;

        uint32 len = dmaj;
        if (len == 0u) {
            return 0u;
        }

        const sint32 xs = x, ys = y, xe = xEnd, ye = yEnd;
        if (xs >= -kPadding && xs <= width && ys >= -kPadding && ys <= height && xe >= -kPadding && xe <= width &&
            ye >= -kPadding && ye <= height) {
            return 0u;
        }
        if ((xs < -kPadding && xe < -kPadding) || (xs > width && xe > width) || (ys < -kPadding && ye < -kPadding) ||
            (ys > height && ye > height)) {
            x = xEnd;
            y = yEnd;
            length = 0u;
            return 0u;
        }

        const bool xmajor = xMajInc != 0;
        const sint32 inc = xMajInc + yMajInc;
        const sint32 start = xmajor ? xs : ys;
        const sint32 end = xmajor ? xe : ye;
        const sint32 maxLen = xmajor ? width : height;

        uint32 startClip = 0u;
        if (inc > 0 && start < -kPadding) {
            startClip = static_cast<uint32>(-start - 1 - kPadding);
        } else if (inc < 0 && start > maxLen) {
            startClip = static_cast<uint32>(start - maxLen);
        }
        if (startClip > 0u) {
            startClip = std::min(startClip, len - 1u);
            x = Add(x, Mul(xMajInc, startClip));
            y = Add(y, Mul(yMajInc, startClip));
            for (uint32 i = 0; i < startClip; ++i) {
                accum = Sub(accum, num);
                if (!SettleMinor(accum, x, y)) {
                    return 0u;
                }
            }
            len -= startClip;
        }

        uint32 endClip = 0u;
        if (inc < 0 && end < -kPadding) {
            endClip = static_cast<uint32>(-end - 1 - kPadding);
        } else if (inc > 0 && end > maxLen) {
            endClip = static_cast<uint32>(end - maxLen);
        }
        endClip = std::min(endClip, len);
        if (endClip > 0u) {
            len -= endClip;
            // The new end point is unused afterwards; the loop only matters for whether it ends, which it always does
            // unless den is INT_MIN
            if (den != std::numeric_limits<sint32>::min()) {
                length = len + 1u;
                return startClip;
            }
            sint32 tempAccum = accum;
            sint32 ex = x, ey = y;
            for (uint32 i = 0; i < len; ++i) {
                tempAccum = Sub(tempAccum, num);
                if (!SettleMinor(tempAccum, ex, ey)) {
                    return 0u;
                }
            }
        }
        length = len + 1u;
        return startClip;
    }

    FORCE_INLINE bool CanStep() const {
        return step <= length;
    }

    // Returns true if an antialias pixel is drawn at this step
    FORCE_INLINE bool Step() {
        ++step;
        x += xMajInc;
        y += yMajInc;
        accum = Sub(accum, num);
        if (accum <= accumTarget) {
            accum = Add(accum, den);
            x += xMinInc;
            y += yMinInc;
            return antiAlias;
        }
        return false;
    }

    FORCE_INLINE CoordS32 Coord() const {
        return {x, y};
    }
    FORCE_INLINE CoordS32 AACoord() const {
        return {x + aaXInc, y + aaYInc};
    }
};

// TextureStepper
struct Texture {
    sint32 num = 0, den = 0, accum = 0, value = 0, inc = 0;

    FORCE_INLINE void Setup(uint32 len, sint32 start, sint32 end, bool hss, sint32 hssSelect) {
        if (hss) {
            start >>= 1;
            end >>= 1;
        }
        const sint32 delta = end - start;
        const uint32 absDelta = static_cast<uint32>(std::abs(delta));

        value = start;
        inc = delta >= 0 ? +1 : -1;
        if (hss) {
            value = Shl(value, 1) | hssSelect;
            inc = Shl(inc, 1);
        }

        num = static_cast<sint32>(absDelta);
        den = static_cast<sint32>(len);
        if (len <= absDelta) {
            ++num;
            accum = static_cast<sint32>(absDelta - (len << 1));
            if (delta >= 0) {
                accum = Add(accum, 1);
            }
        } else {
            --den;
            accum = static_cast<sint32>(len - (len << 1));
            if (delta < 0) {
                accum = Add(accum, 1);
            }
        }
        num = Shl(num, 1);
        den = Shl(den, 1);
    }

    FORCE_INLINE uint32 Value() const {
        return static_cast<uint32>(value);
    }
    FORCE_INLINE bool ShouldStepTexel() const {
        return accum >= 0;
    }
    FORCE_INLINE void StepTexel() {
        value = Add(value, inc);
        accum = Sub(accum, den);
    }
    FORCE_INLINE void StepPixel() {
        accum = Add(accum, num);
    }
    FORCE_INLINE void SkipPixels(uint32 count) {
        accum = Add(accum, Mul(num, count));
    }
};

// GouraudChannelStepper
struct GouraudChannel {
    sint32 num = 0, den = 0, accum = 0, value = 0, intInc = 0, fracInc = 0;

    FORCE_INLINE void Setup(uint32 len, uint16 start, uint16 end) {
        const sint32 delta = static_cast<sint32>(end) - static_cast<sint32>(start);
        const uint32 absDelta = static_cast<uint32>(std::abs(delta));

        value = start;
        intInc = 0;
        fracInc = delta >= 0 ? +1 : -1;

        num = static_cast<sint32>(absDelta);
        den = static_cast<sint32>(len);
        if (len <= absDelta) {
            ++num;
            accum = static_cast<sint32>(absDelta - (len << 1));
            if (delta >= 0) {
                accum = Add(accum, 1);
            }
        } else {
            --den;
            accum = static_cast<sint32>(0u - len);
            if (delta < 0) {
                accum = Add(accum, 1);
            }
        }
        num = Shl(num, 1);
        den = Shl(den, 1);

        if (den != 0) {
            while (accum >= 0) {
                value += fracInc;
                accum = Sub(accum, den);
            }
            while (num >= den) {
                intInc += fracInc;
                num = Sub(num, den);
            }
        }
        accum = ~accum;
    }

    FORCE_INLINE void Step() {
        value = Add(value, intInc);
        accum = Sub(accum, num);
        if (accum < 0) {
            value = Add(value, fracInc);
            accum = Add(accum, den);
        }
    }

    FORCE_INLINE void Skip(uint32 steps) {
        value = Add(value, Mul(intInc, steps));
        accum = Sub(accum, Mul(num, steps));
        if (den != 0) {
            while (accum < 0) {
                value = Add(value, fracInc);
                accum = Add(accum, den);
            }
        }
    }

    // GouraudChannelStepper::Blend's table: clamp(value + color - 16, 0, 31)
    FORCE_INLINE uint16 Blend(uint16 color) const {
        return static_cast<uint16>(std::clamp<sint32>(value + static_cast<sint32>(color) - 16, 0, 31));
    }
};

// GouraudStepper
struct Gouraud {
    GouraudChannel r, g, b;

    FORCE_INLINE void Setup(uint32 len, Color555 start, Color555 end) {
        r.Setup(len, start.r, end.r);
        g.Setup(len, start.g, end.g);
        b.Setup(len, start.b, end.b);
    }
    FORCE_INLINE void Step() {
        r.Step();
        g.Step();
        b.Step();
    }
    FORCE_INLINE void Skip(uint32 steps) {
        if (steps > 0) {
            r.Skip(steps);
            g.Skip(steps);
            b.Skip(steps);
        }
    }
    FORCE_INLINE Color555 Blend(Color555 base) const {
        Color555 out{.u16 = 0};
        out.r = r.Blend(base.r);
        out.g = g.Blend(base.g);
        out.b = b.Blend(base.b);
        out.msb = base.msb;
        return out;
    }
};

} // namespace port

// -----------------------------------------------------------------------------
// Data

// Registers and enhancements at draw time. A batch has one raster state; a change splits the batch.
struct RasterState {
    uint32 fbSizeH = 512;           // TVMR
    bool pixel8Bits = false;        // TVMR
    bool dblInterlaceEnable = false; // FBCR.DIE
    bool dblInterlaceDrawLine = false; // FBCR.DIL
    bool evenOddCoordSelect = false; // FBCR.EOS
    bool doubleDensity = false;     // TVMD.LSMD == double density
    bool deinterlace = false;       // enhancements
    bool transparentMeshes = false; // enhancements
    uint32 doubleV = 0;             // latched at VDP1BeginFrame
    uint8 drawFB = 0;               // draw framebuffer index

    bool operator==(const RasterState &) const = default;
};

// System clip, user clip and local coordinates (VDPState::state1), raw as the clip commands store them
struct ClipContext {
    uint16 sysClipH = 319, sysClipV = 223;
    uint16 userClipX0 = 0, userClipY0 = 0, userClipX1 = 319, userClipY1 = 223;
    sint32 localX = 0, localY = 0;
};

// One draw command, as the front half records it
struct CommandParams {
    uint32 address = 0;
    VDP1Command::Control control{.u16 = 0};
    VDP1Command::DrawMode mode{.u16 = 0};
    bool textured = false;
    bool antiAlias = false;  // polygons and sprites; not polylines and lines
    uint16 color = 0;        // CMDCOLR (untextured)
    uint32 colorBank = 0;    // CMDCOLR with the colour mode's mask or shift (textured)
    uint32 charAddr = 0;     // CMDSRCA * 8, 16-byte aligned in colour mode 5
    uint32 charSizeH = 0, charSizeV = 0;
    ClipContext clip;        // at the command
    std::array<CoordS32, 4> vertices{}; // after local coordinates and doubleV (A, B, C, D; lines use A, B)
    std::array<Color555, 4> gouraud{};  // CMDGRDA table (when Gouraud is enabled)
};

// One line software plots: the quad loop's L-R line at quadStep, or a polyline/line segment
struct Span {
    uint32 cmdIndex = 0;     // into the batch's commands
    uint32 quadStep = 0;     // quad step, or polyline segment
    CoordS32 a{}, b{};       // end points
    uint32 texV = 0;         // texel row (textured)
    Color555 gouraudL{.u16 = 0}, gouraudR{.u16 = 0};
    uint32 fragBase = 0;     // first fragment slot (prefix sum of SpanSlots)
};

// target bit layout
inline constexpr uint32 kTargetWordMask = 0x1FFFF; // FBRAM word index (fbOffset >> 1)
inline constexpr uint32 kTargetAltBit = 1u << 17;  // alternate-field framebuffer (and mesh[1])
inline constexpr uint32 kTargetOddBit = 1u << 18;  // 8 bpp: the odd byte of the word
inline constexpr uint32 kTargetCalcShift = 19;     // 2 bits: colour calculation (16 bpp)
inline constexpr uint32 kTargetMeshBit = 1u << 21; // transparent meshes: write to the mesh framebuffer
inline constexpr uint32 kTargetMsbOnBit = 1u << 22;
inline constexpr uint32 kTargetUnitMask = kTargetWordMask | kTargetAltBit; // the resolve unit

struct Fragment {
    uint32 key = 0;    // fragBase + 2 * step + isAA
    uint32 target = 0; // see kTarget*
    uint32 color = 0;  // 16 bpp: source colour after Gouraud; 8 bpp: the byte
};

struct Fbram {
    std::array<SpriteFB, 2> main{};
    std::array<SpriteFB, 2> alt{};
    std::array<std::array<SpriteFB, 2>, 2> mesh{}; // [altField][fbIndex]
};

enum class ResolveOrder { Ascending, DescendingForTesting };

struct RasterOptions {
    // VDP1PlotLine/VDP1PlotTexturedLine's "plotted && clipped && clipping mode 0 -> break". False drops it (research
    // 4.1: a pure optimisation); the tests show both give software's FBRAM.
    bool earlyBreak = true;
    // Descending only to show that the tests are sensitive to the order
    ResolveOrder resolveOrder = ResolveOrder::Ascending;
};

// -----------------------------------------------------------------------------
// Part 1: span builder (test-local port of the software command handlers; Task 3 replaces it)

struct CommandSpans {
    bool draws = false; // a draw command that was not rejected whole
    CommandParams params;
    std::vector<Span> spans; // cmdIndex and fragBase unset
};

// Executes the command at `address` (clip and local coordinate commands update `ctx`)
CommandSpans BuildCommandSpans(const uint8 *vram, uint32 address, ClipContext &ctx, const RasterState &state);

// -----------------------------------------------------------------------------
// Part 2: span rasterization

// Thrown by SpanSlots and RasterSpan for a span whose SystemClip never ends (port::Line::undefined): software has no
// defined result there (it hangs, or not, depending on the compiled code)
struct UndefinedLineError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// 2 * Length() after LineStepper::SystemClip; 0 when VDP1IsLineSystemClipped rejects the line
uint32 SpanSlots(const RasterState &state, const CommandParams &cmd, const Span &span);

// Appends the span's fragments (keys in [fragBase, fragBase + SpanSlots)) to `out`
void RasterSpan(const RasterState &state, const CommandParams &cmd, const Span &span, const uint8 *vram,
                const RasterOptions &options, std::vector<Fragment> &out);

// -----------------------------------------------------------------------------
// Part 3: per-word ordered resolve

struct ResolveStats {
    uint64 fragments = 0;
    uint64 units = 0;       // words written
    uint32 maxPerUnit = 0;  // most fragments on one word
};

// Applies the batch's fragments (any order; keys unique) to the draw framebuffer state.drawFB
ResolveStats Resolve(const RasterState &state, std::vector<Fragment> fragments, Fbram &fbram,
                     ResolveOrder order = ResolveOrder::Ascending);

// -----------------------------------------------------------------------------
// Erase (VDP1DoEraseFramebuffer), per framebuffer word

struct EraseParams {
    uint8 fbIndex = 1;    // the display framebuffer
    uint16 value = 0;     // eraseWriteValueLatch
    uint16 x1 = 0, y1 = 0, x3 = 0, y3 = 0; // erase*Latch
    uint32 offsetShift = 9; // TVMR eraseOffsetShift
    bool doubleDensity = false;
    bool hiResH = false;  // TVMD.HRESOn & 1
    uint32 vres = 224;    // the renderer's vertical resolution (VDP2SetResolution)
    bool deinterlace = false;
    bool transparentMeshes = false;
    uint64 cycles = 0;    // 0 = unlimited; else cycles + 1 writes in row-major order
};

void Erase(const EraseParams &params, Fbram &fbram);

// -----------------------------------------------------------------------------
// Driver: commands -> batches -> raster -> resolve

struct ReferenceStats {
    uint64 commands = 0;
    uint64 batches = 0;
    uint64 spans = 0;
    uint64 slots = 0;
    uint64 fragments = 0;
    uint32 maxPerUnit = 0;
};

class ReferenceVDP1 {
public:
    ReferenceVDP1(const uint8 *vram, Fbram &fbram, RasterOptions options = {},
                  uint32 maxBatchFragments = 0xFFFFFFFFu, uint64 shuffleSeed = 0);

    ClipContext clip;

    // Executes a command with the registers at the time of the command (VDP1ExecuteCommand)
    void Command(uint32 address, const RasterState &state);
    // Rasterizes and resolves the pending batch
    void Flush();
    // Flushes, then erases
    void Erase(const EraseParams &params);

    const ReferenceStats &Stats() const {
        return m_stats;
    }

private:
    void AddSpan(const CommandParams &params, const RasterState &state, Span span, uint32 slots);

    const uint8 *m_vram;
    Fbram &m_fbram;
    RasterOptions m_options;
    uint32 m_maxBatchFragments;
    uint64 m_shuffle;

    bool m_pending = false;
    RasterState m_state;
    std::vector<CommandParams> m_commands;
    bool m_commandInBatch = false;
    std::vector<Span> m_spans;
    uint32 m_slots = 0;
    ReferenceStats m_stats;
};

} // namespace brimir::test::vdp1ref
