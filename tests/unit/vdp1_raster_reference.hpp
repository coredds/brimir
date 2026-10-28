#pragma once

// A CPU executable specification of the GPU VDP1 back half (GPU stage 2; docs/ymir-hw-renderer-notes/
// stage2-vdp1-research.md sections 4.1-4.3), written from the software renderer (vdp_renderer_sw.cpp,
// vdp1_steppers.hpp) independently of Ymir's shaders. It replays the op stream of the real front half
// (src/gpu/vdp1/vdp1_front.hpp: commands, spans, VRAM uploads, erases, CPU framebuffer writes) the way the GPU passes
// will; the shaders (Task 4) are checked against it part by part and the tests check it against SoftwareVDPRenderer's
// FBRAM byte for byte.
//
// Parts:
//   1. Decoding: the front half's GPU structs (Vdp1CommandParams, Vdp1Span) into the reference's view (CommandParams,
//      RasterState per command, Span), through the same accessors the shaders use.
//   2. RasterSpan(span) -> fragments: a pure function of the span, its command, the command's raster state and VRAM. It
//      steps the line iteratively with a 32-bit port of LineStepper/TextureStepper/GouraudStepper (no SetStep jumps)
//      and runs the loop body of VDP1PlotLine / VDP1PlotTexturedLine and the read half of VDP1PlotPixel (clipping,
//      mesh skip, double-interlace skip, address). Every pixel VDP1PlotPixel would write becomes a fragment with
//      key = fragBase + 2 * step + isAA, fragBase being the prefix sum of the batch's span slot counts (2 * Length()
//      after LineStepper::SystemClip). Key order is software's emission order.
//   3. Resolve(fragments in any order) -> FBRAM: per FBRAM word (16 bits of the draw framebuffer or of its alternate
//      field), the word's fragments sorted by key are applied with the write half of VDP1PlotPixel to the word's
//      (main, mesh) pair: colour calculation in 16 bpp, bytes in 8 bpp, MSB-on ORing bit 15 of the word. Each fragment
//      carries its command's 8-bpp and transparent-meshes modes, so a batch may mix them.
//   4. Erase (from Vdp1ErasePushConstants) and CPU framebuffer writes (Vdp1FbWrite records).
//
// Batches are resolved in op order; a batch boundary is a barrier. The reference keeps its own copy of VDP1 VRAM, built
// only from the batches' uploads, as the GPU's.

#include <gpu/vdp1/vdp1_front.hpp>
#include <gpu/vdp1/vdp1_hw_params.hpp>

#include <ymir/core/types.hpp>
#include <ymir/hw/vdp/vdp1_defs.hpp>
#include <ymir/hw/vdp/vdp_common_defs.hpp>
#include <ymir/util/inline.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace brimir::test::vdp1ref {

using ymir::vdp::Color555;
using ymir::vdp::CoordS32;
using ymir::vdp::SpriteFB;
using ymir::vdp::VDP1Command;

inline constexpr uint32 kVRAMMask = 0x7FFFF;
inline constexpr uint32 kWordsPerFB = 0x20000; // 256 KiB / 2

// splitmix64 (the fragment shuffle and the tests' random generator); deterministic on every platform
inline uint64 SplitMix64(uint64 &state) {
    uint64 z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

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

    // Set by SystemClip when it took the stepping fallback below (for tests)
    bool clippedByStepping = false;

    // A minor-step loop of SystemClip ("while (accum <= accumTarget) accum += den") never ends when den is 2^31 (major
    // delta 2048 mod 4096: the accumulator alternates between two values) and both values are <= accumTarget; that
    // needs accumTarget = 1 << 19, i.e. no antialiasing and a line running towards negative coordinates on its major
    // axis. Checked on every pass through the loop (inside it accum <= accumTarget already holds).
    FORCE_INLINE bool MinorLoopNeverEnds(sint32 acc) const {
        return den == std::numeric_limits<sint32>::min() && Add(acc, den) <= accumTarget;
    }

    // The rule master adopted for those lines (vdp1_steppers.hpp, 1a769b1): from the line's start, step a copy with
    // the plain Step() rule over all dmaj + 1 pixels. No pixel (main, or AA when drawn) inside the padded clip area
    // [lo, maxX] x [lo, maxY]: the line is fully clipped. Otherwise skip to the first such step (returned as the skip
    // count) and end at the last one.
    FORCE_INLINE uint32 ClipByStepping(sint32 lo, sint32 maxX, sint32 maxY) {
        clippedByStepping = true;
        auto inside = [&](CoordS32 c) { return c.x() >= lo && c.x() <= maxX && c.y() >= lo && c.y() <= maxY; };
        Line probe = *this;
        bool found = false;
        uint32 first = 0, last = 0;
        for (uint32 i = 0; i <= dmaj; ++i) {
            const bool aa = probe.Step();
            if (inside(probe.Coord()) || (aa && inside(probe.AACoord()))) {
                first = found ? first : i;
                found = true;
                last = i;
            }
        }
        if (!found) {
            x = xEnd;
            y = yEnd;
            length = 0u;
            return 0u;
        }
        for (uint32 i = 0; i < first; ++i) {
            Step();
        }
        step = 0;
        length = last - first + 1u;
        return first;
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
        const sint32 startAccum = accum;
        auto fallback = [&] {
            x = xs;
            y = ys;
            accum = startAccum;
            return ClipByStepping(-kPadding, width, height);
        };

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
                if (den != 0) {
                    while (accum <= accumTarget) {
                        if (MinorLoopNeverEnds(accum)) {
                            return fallback();
                        }
                        accum = Add(accum, den);
                        x += xMinInc;
                        y += yMinInc;
                    }
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
            // The end loop only moves the (unused) end point; it matters only when it never ends, which needs den = 2^31
            if (den == std::numeric_limits<sint32>::min()) {
                sint32 tempAccum = accum;
                for (uint32 i = 0; i < len; ++i) {
                    tempAccum = Sub(tempAccum, num);
                    while (tempAccum <= accumTarget) {
                        if (MinorLoopNeverEnds(tempAccum)) {
                            return fallback();
                        }
                        tempAccum = Add(tempAccum, den);
                    }
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

// Registers and enhancements at draw time: per command (Vdp1CommandParams::rasterFlags), except the draw framebuffer,
// which is per batch
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

// System clip, user clip and local coordinates (VDPState::state1), raw as the clip commands store them (the local
// coordinates are already applied to a command's vertices)
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
inline constexpr uint32 kTargetPixel8Bit = 1u << 23;            // the command drew in 8 bpp (TVMR)
inline constexpr uint32 kTargetTransparentMeshesBit = 1u << 24; // ... with transparent meshes on
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
// Part 1: decoding the front half's GPU structs (brimir::gpu::vdp1, vdp1_hw_params.hpp)

CommandParams DecodeCommand(const brimir::gpu::vdp1::Vdp1CommandParams &command);
RasterState DecodeRasterState(uint32 rasterFlags, uint32 drawFB);
Span DecodeSpan(const brimir::gpu::vdp1::Vdp1Span &span);

// -----------------------------------------------------------------------------
// Part 2: span rasterization

// 2 * Length() after LineStepper::SystemClip; 0 when VDP1IsLineSystemClipped rejects the line
uint32 SpanSlots(const RasterState &state, const CommandParams &cmd, const Span &span);

// What a span did, for the tests' drawn-coverage counters
struct SpanInfo {
    bool highSpeedShrink = false;   // textured line with high-speed shrink in effect
    bool endCodeTerminated = false; // the second end code ended the line
};

// Appends the span's fragments (keys in [fragBase, fragBase + SpanSlots)) to `out`
void RasterSpan(const RasterState &state, const CommandParams &cmd, const Span &span, const uint8 *vram,
                const RasterOptions &options, std::vector<Fragment> &out, SpanInfo *info = nullptr);

// -----------------------------------------------------------------------------
// Part 3: per-word ordered resolve

struct ResolveStats {
    uint64 fragments = 0;
    uint64 units = 0;       // words written
    uint32 maxPerUnit = 0;  // most fragments on one word
};

// Applies the batch's fragments (any order; keys unique) to the draw framebuffer `drawFB`. 8 bpp and transparent
// meshes come from each fragment (kTargetPixel8Bit, kTargetTransparentMeshesBit).
ResolveStats Resolve(uint32 drawFB, std::vector<Fragment> fragments, Fbram &fbram,
                     ResolveOrder order = ResolveOrder::Ascending);

// -----------------------------------------------------------------------------
// Part 4: erase (VDP1DoEraseFramebuffer, per framebuffer word) and CPU framebuffer writes

// Returns the number of words written to the main framebuffer
uint64 Erase(const brimir::gpu::vdp1::Vdp1ErasePushConstants &params, Fbram &fbram);

// Applies the records in order
void ApplyFbWrites(std::span<const brimir::gpu::vdp1::Vdp1FbWrite> writes, Fbram &fbram);

// -----------------------------------------------------------------------------
// Driver: op lists -> VRAM uploads, raster and resolve per batch, erases, writes

// Drawn coverage: fragments (and lines, erase writes) actually produced, by category
struct DrawnCoverage {
    enum Counter : uint32 {
        CommandType0 = 0, // + CMDCTRL command 0-7: fragments
        ColorMode0 = 8,   // + colour mode 0-7: fragments of textured commands
        ColorCalc0 = 16,  // + mode 0-3: 16 bpp fragments that are not MSB-on
        Gouraud = 20,     // 16 bpp fragments with Gouraud (not MSB-on)
        Mesh,             // fragments written to the mesh framebuffer
        Alt,              // fragments in the alternate field
        MsbOn,
        ByteEven,         // 8 bpp byte writes
        ByteOdd,
        DoubleInterlace,  // fragments drawn with FBCR.DIE
        DieDoubleDensity, // ... in double density without deinterlacing (the DIL line select)
        DoubleV,          // fragments drawn with doubleV
        HssLines,         // spans with high-speed shrink that produced fragments
        EndCodeLines,     // spans ended by end codes that produced fragments
        EraseWrites,      // words an erase wrote
        MixedModeBatches, // batches whose fragments mix 8 and 16 bpp or transparent meshes on and off
        Count
    };
    std::array<uint64, Count> counts{};

    uint64 &operator[](uint32 counter) {
        return counts[counter];
    }
    uint64 operator[](uint32 counter) const {
        return counts[counter];
    }
    void Merge(const DrawnCoverage &other) {
        for (uint32 i = 0; i < Count; ++i) {
            counts[i] += other.counts[i];
        }
    }
    static const char *Name(uint32 counter);
};
struct ReferenceStats {
    uint64 opLists = 0;
    uint64 commands = 0; // command parameters in batches
    uint64 batches = 0;
    uint64 spans = 0;
    uint64 slots = 0;
    uint64 fragments = 0;
    uint64 vramUploadBytes = 0;
    uint64 fbWrites = 0;
    uint64 erases = 0;
    uint32 maxPerUnit = 0;
    DrawnCoverage drawn;
};

class ReferenceVDP1 {
public:
    // VRAM starts zeroed: only the front half's uploads fill it, as on the GPU
    explicit ReferenceVDP1(Fbram &fbram, RasterOptions options = {}, uint64 shuffleSeed = 0);

    // Replays the ops in order. Checks the list's structure (segments partition the ops; every span's slots equal
    // SpanSlots of its decoded command; fragment keys stay inside their span's slots) and throws std::logic_error if it
    // is broken.
    void Replay(const brimir::gpu::vdp1::Vdp1OpList &ops);

    const ReferenceStats &Stats() const {
        return m_stats;
    }
    const std::vector<uint8> &VRAM() const {
        return m_vram;
    }

private:
    void DrawBatch(const brimir::gpu::vdp1::Vdp1OpList &ops, const brimir::gpu::vdp1::Vdp1DrawBatch &batch);

    Fbram &m_fbram;
    RasterOptions m_options;
    uint64 m_shuffle;
    std::vector<uint8> m_vram;
    ReferenceStats m_stats;
};

} // namespace brimir::test::vdp1ref
