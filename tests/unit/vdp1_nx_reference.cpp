#include "vdp1_nx_reference.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace brimir::test::vdp1nx {

namespace gpu = brimir::gpu::vdp1;
using ymir::vdp::SpriteFB;
using ymir::vdp::VDP1Command;

namespace {

using Clock = std::chrono::steady_clock;

double Seconds(Clock::time_point since) {
    return std::chrono::duration<double>(Clock::now() - since).count();
}

FORCE_INLINE sint32 FloorDiv(sint32 v, sint32 n) {
    return v >= 0 ? v / n : -((-v + n - 1) / n);
}

FORCE_INLINE uint8 Read8(const uint8 *vram, uint32 address) {
    return vram[address & ref::kVRAMMask];
}

FORCE_INLINE uint16 Read16(const uint8 *vram, uint32 address) {
    address &= ref::kVRAMMask & ~1u;
    return static_cast<uint16>((vram[address] << 8) | vram[address + 1]);
}

// -----------------------------------------------------------------------------
// Native clipping (VDP1IsPixelClipped, VDP1IsLineSystemClipped; clip values with the doubleV doubling)

struct ClipRect {
    sint32 sysH, sysV;
    sint32 userX0, userY0, userX1, userY1;
};

ClipRect EffectiveClip(const ref::ClipContext &c, uint32 doubleV) {
    const sint32 dV = static_cast<sint32>(doubleV);
    return {c.sysClipH, (c.sysClipV << dV) | dV, c.userClipX0, (c.userClipY0 << dV) | dV, c.userClipX1,
            (c.userClipY1 << dV) | dV};
}

FORCE_INLINE bool PixelClipped(const ClipRect &clip, sint32 x, sint32 y, bool userClippingEnable, bool clippingMode) {
    if (x < 0 || x > clip.sysH || y < 0 || y > clip.sysV) {
        return true;
    }
    const bool userClipped = x < clip.userX0 || x > clip.userX1 || y < clip.userY0 || y > clip.userY1;
    return userClippingEnable && userClipped != clippingMode;
}

bool NativeLineSystemClipped(const ClipRect &clip, CoordS32 a, CoordS32 b) {
    return (a.x() < 0 && b.x() < 0) || (a.y() < 0 && b.y() < 0) || (a.x() > clip.sysH && b.x() > clip.sysH) ||
           (a.y() > clip.sysV && b.y() > clip.sysV);
}

// The N x extents of the system clip area: native pixel x covers N x columns N * x .. N * x + N - 1
struct NxExtent {
    sint32 maxX, maxY;
};

NxExtent Extent(const ClipRect &clip, sint32 n) {
    return {n * (clip.sysH + 1) - 1, n * (clip.sysV + 1) - 1};
}

// VDP1IsLineSystemClipped at N x: both end points' native coordinates beyond the same side of the clip area (it drops
// the antialias pixels such a line may put inside, as natively)
bool NxLineRejected(const NxExtent &e, CoordS32 a, CoordS32 b) {
    return (a.x() < 0 && b.x() < 0) || (a.y() < 0 && b.y() < 0) || (a.x() > e.maxX && b.x() > e.maxX) ||
           (a.y() > e.maxY && b.y() > e.maxY);
}

// -----------------------------------------------------------------------------
// 32-bit steppers at N x (vdp1_steppers.hpp without the 13-bit counter shift and sign extension)

struct NxLine {
    sint32 num = 0, den = 0, accum = 0, accumTarget = 0;
    sint32 xMajInc = 0, yMajInc = 0, xMinInc = 0, yMinInc = 0;
    sint32 x = 0, y = 0, xEnd = 0, yEnd = 0;
    uint32 dmaj = 0, step = 0, length = 0;
    sint32 aaXInc = 0, aaYInc = 0;
    bool antiAlias = false;

    NxLine(CoordS32 c1, CoordS32 c2, bool aa) {
        const sint32 x1 = c1.x(), y1 = c1.y(), x2 = c2.x(), y2 = c2.y();
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
        const bool xMajor = adx >= ady;
        if (xMajor) {
            xMajInc = dx >= 0 ? +1 : -1;
            yMinInc = dy >= 0 ? +1 : -1;
        } else {
            yMajInc = dy >= 0 ? +1 : -1;
            xMinInc = dx >= 0 ? +1 : -1;
            std::swap(dx, dy);
            std::swap(adx, ady);
        }
        num = ady << 1;
        den = adx << 1;
        accum = adx + 1 + num;
        accumTarget = (!aa && dx < 0) ? 1 : 0;
        x -= xMajInc;
        y -= yMajInc;
        if (aa) {
            --accum;
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
    }

    // LineStepper::SystemClip with 32-bit counters (its non-terminating case needs den = 2^31: impossible here)
    uint32 SystemClip(sint32 width, sint32 height) {
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
            x += xMajInc * static_cast<sint32>(startClip);
            y += yMajInc * static_cast<sint32>(startClip);
            for (uint32 i = 0; i < startClip; ++i) {
                accum -= num;
                if (den != 0) {
                    while (accum <= accumTarget) {
                        accum += den;
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
        len -= std::min(endClip, len);
        length = len + 1u;
        return startClip;
    }

    bool CanStep() const {
        return step <= length;
    }
    bool Step() {
        ++step;
        x += xMajInc;
        y += yMajInc;
        accum -= num;
        if (accum <= accumTarget) {
            accum += den;
            x += xMinInc;
            y += yMinInc;
            return antiAlias;
        }
        return false;
    }
};

struct NxEdge {
    ref::port::Gouraud gouraud;
    bool gouraudEnable = false;
    uint32 dmaj = 0;
    sint32 x = 0, y = 0, xInc = 0, yInc = 0;
    sint32 xNum = 0, yNum = 0, xDen = 0, yDen = 0, xAccum = 0, yAccum = 0, xAccumTarget = 0, yAccumTarget = 0;
    sint32 num = 0, den = 0, accum = 0, accumTarget = 0;

    void Setup(CoordS32 c1, CoordS32 c2, uint32 delta) {
        const sint32 dx = c2.x() - c1.x();
        const sint32 dy = c2.y() - c1.y();
        const uint32 adx = static_cast<uint32>(std::abs(dx));
        const uint32 ady = static_cast<uint32>(std::abs(dy));
        dmaj = std::max(adx, ady);
        x = c1.x();
        y = c1.y();
        xInc = dx >= 0 ? +1 : -1;
        yInc = dy >= 0 ? +1 : -1;
        xNum = static_cast<sint32>(adx << 1);
        yNum = static_cast<sint32>(ady << 1);
        xDen = yDen = static_cast<sint32>(dmaj << 1);
        xAccum = yAccum = ~static_cast<sint32>(dmaj);
        xAccumTarget = dy < 0 ? -1 : 0;
        yAccumTarget = dx < 0 ? -1 : 0;
        num = static_cast<sint32>(dmaj << 1);
        den = static_cast<sint32>(delta << 1);
        accum = ~static_cast<sint32>(delta);
        accumTarget = adx >= ady ? yAccumTarget : xAccumTarget;
        gouraudEnable = false;
    }
    void SetupGouraud(Color555 start, Color555 end) {
        gouraud.Setup(dmaj + 1, start, end);
        gouraudEnable = true;
    }
    bool StepCoordinates() {
        accum += num;
        if (accum >= accumTarget) {
            accum -= den;
            xAccum += xNum;
            if (xAccum >= xAccumTarget) {
                xAccum -= xDen;
                x += xInc;
            }
            yAccum += yNum;
            if (yAccum >= yAccumTarget) {
                yAccum -= yDen;
                y += yInc;
            }
            return true;
        }
        return false;
    }
    void Step() {
        if (StepCoordinates() && gouraudEnable) {
            gouraud.Step();
        }
    }
    CoordS32 Coord() const {
        return {x, y};
    }
    Color555 GouraudValue() const {
        Color555 c{.u16 = 0};
        c.r = static_cast<uint16>(gouraud.r.value);
        c.g = static_cast<uint16>(gouraud.g.value);
        c.b = static_cast<uint16>(gouraud.b.value);
        return c;
    }
};

struct NxQuad {
    NxEdge left, right; // A-D, B-C
    uint32 dmaj = 0, step = 0;

    NxQuad(CoordS32 a, CoordS32 b, CoordS32 c, CoordS32 d) {
        const uint32 dlx = static_cast<uint32>(std::abs(d.x() - a.x()));
        const uint32 dly = static_cast<uint32>(std::abs(d.y() - a.y()));
        const uint32 drx = static_cast<uint32>(std::abs(c.x() - b.x()));
        const uint32 dry = static_cast<uint32>(std::abs(c.y() - b.y()));
        dmaj = std::max({dlx, dly, drx, dry});
        left.Setup(a, d, dmaj);
        right.Setup(b, c, dmaj);
    }
    bool CanStep() const {
        return step <= dmaj;
    }
    void Step() {
        ++step;
        left.Step();
        right.Step();
    }
    void StepCoordinates() {
        ++step;
        left.StepCoordinates();
        right.StepCoordinates();
    }
};

// VDP1QuadTermination at N x without the line-wrap limit (32-bit line counters do not wrap): an optimisation only
class NxTermination {
public:
    NxTermination(const NxQuad &quad, const NxExtent &e)
        : m_e(e) {
        NxQuad copy = quad;
        m_finalL = copy.left.Coord();
        m_finalR = copy.right.Coord();
        for (copy.StepCoordinates(); copy.CanStep(); copy.StepCoordinates()) {
            m_finalL = copy.left.Coord();
            m_finalR = copy.right.Coord();
        }
    }
    bool Done(const NxQuad &quad) const {
        const CoordS32 l = quad.left.Coord();
        const CoordS32 r = quad.right.Coord();
        const sint32 minX = std::min({l.x(), r.x(), m_finalL.x(), m_finalR.x()});
        const sint32 maxX = std::max({l.x(), r.x(), m_finalL.x(), m_finalR.x()});
        const sint32 minY = std::min({l.y(), r.y(), m_finalL.y(), m_finalR.y()});
        const sint32 maxY = std::max({l.y(), r.y(), m_finalL.y(), m_finalR.y()});
        return maxX + 1 < 0 || minX - 1 > m_e.maxX || maxY + 1 < 0 || minY - 1 > m_e.maxY;
    }

private:
    NxExtent m_e;
    CoordS32 m_finalL{}, m_finalR{};
};

// Rect-kind sub-steps (textures and Gouraud). Sub-step s of native pixel (or row) P is the position x = P + s / N
// along the line from its start. The native steppers round (texel = round(u) of the end-point mapping), so reading
// their accumulators at sub-positions would move every texel or colour step about half a pixel into the block (1:1
// sprites would not be the nearest upscale, and the last block would read past the sprite). Instead the step count at
// sub-step s is the same linear mapping, num / den per native pixel, rounded towards the native pixel that owns the
// block, and clamped between the native counts:
//   K(P, s) = clamp(phase((N * P + s) * num / (N * den)), c(P), c(P + 1))
// phase = floor when the line (or the rows) runs towards larger coordinates: the block of pixel P holds positions
// [P, P + 1); ceil when it runs towards smaller ones: the block holds (P - 1, P] (the previous-pixel mapping in
// RasterRect). K = c(P) at s = 0, so the sample points are native; a mapping of one step per pixel keeps every block
// constant (1:1 sprites and one-step-per-pixel Gouraud are the nearest upscale); shrinks show the steps natively
// skipped.
FORCE_INLINE uint32 PhaseCount(uint32 num, uint32 den, uint32 n, uint32 P, uint32 s, bool ceil) {
    if (den == 0) {
        return 0;
    }
    const uint32 numerator = (n * P + s) * num;
    const uint32 denominator = n * den;
    return (numerator + (ceil ? denominator - 1 : 0)) / denominator;
}

// The native texture stepper's texel count at the next pixel (the catch-up loop of P + 1), without changing `t`
FORCE_INLINE uint32 NextTexelCount(ref::port::Texture t, uint32 count) {
    t.StepPixel();
    while (t.ShouldStepTexel()) {
        t.StepTexel();
        ++count;
    }
    return count;
}

// Rect-kind Gouraud sub-steps by the same rule per channel: v(P) from the native stepper, U(P) = (v(P) - v0) /
// fracInc the steps from the value after Setup, and num / den = (|intInc| * den + num) / den of the reduced stepper
//   U = clamp(phase((N * P + s) * num / (N * den)), U(P), U(P + 1))
class SubGouraud {
public:
    SubGouraud() = default;
    // `setup`: the native stepper right after Setup; `skip`: the native pixels skipped before the first one; `ceil`:
    // the line runs towards smaller coordinates
    SubGouraud(const ref::port::Gouraud &setup, uint32 n, uint32 skip, bool ceil)
        : m_n(n)
        , m_P(skip)
        , m_ceil(ceil)
        , m_cur(setup)
        , m_next(setup) {
        const ref::port::GouraudChannel *chs[3] = {&setup.r, &setup.g, &setup.b};
        for (uint32 c = 0; c < 3; ++c) {
            m_v0[c] = chs[c]->value;
            m_f[c] = chs[c]->fracInc;
            m_rateNum[c] = static_cast<uint32>(std::abs(chs[c]->intInc) * chs[c]->den + chs[c]->num);
            m_rateDen[c] = static_cast<uint32>(chs[c]->den);
        }
        m_cur.Skip(skip);
        m_next = m_cur;
        m_next.Step();
        Update();
    }
    void Step() {
        if (++m_s == m_n) {
            m_s = 0;
            ++m_P;
            m_cur = m_next;
            m_next.Step();
        }
        Update();
    }
    Color555 Value() const {
        Color555 c{.u16 = 0};
        c.r = static_cast<uint16>(m_value[0]);
        c.g = static_cast<uint16>(m_value[1]);
        c.b = static_cast<uint16>(m_value[2]);
        return c;
    }
    Color555 Blend(Color555 base) const {
        auto blend = [](sint32 v, uint16 color) {
            return static_cast<uint16>(std::clamp<sint32>(v + static_cast<sint32>(color) - 16, 0, 31));
        };
        Color555 out{.u16 = 0};
        out.r = blend(m_value[0], base.r);
        out.g = blend(m_value[1], base.g);
        out.b = blend(m_value[2], base.b);
        out.msb = base.msb;
        return out;
    }

private:
    void Update() {
        const ref::port::GouraudChannel *cur[3] = {&m_cur.r, &m_cur.g, &m_cur.b};
        const ref::port::GouraudChannel *next[3] = {&m_next.r, &m_next.g, &m_next.b};
        for (uint32 c = 0; c < 3; ++c) {
            if (m_s == 0 || m_rateDen[c] == 0) {
                m_value[c] = cur[c]->value;
                continue;
            }
            const sint32 u0 = (cur[c]->value - m_v0[c]) * m_f[c];
            const sint32 u1 = std::max(u0, (next[c]->value - m_v0[c]) * m_f[c]);
            const sint32 fl = static_cast<sint32>(PhaseCount(m_rateNum[c], m_rateDen[c], m_n, m_P, m_s, m_ceil));
            m_value[c] = m_v0[c] + m_f[c] * std::clamp(fl, u0, u1);
        }
    }

    uint32 m_n = 1, m_P = 0, m_s = 0;
    bool m_ceil = false;
    ref::port::Gouraud m_cur{}, m_next{};
    sint32 m_v0[3]{}, m_f[3]{}, m_value[3]{};
    uint32 m_rateNum[3]{}, m_rateDen[3]{};
};

// -----------------------------------------------------------------------------
// Texel reads and pixel writes (the loop bodies of VDP1PlotTexturedLine and VDP1PlotPixel)

struct TexelReader {
    const ref::CommandParams &cmd;
    const uint8 *vram;
    uint32 v;
    uint32 charSizeH;
    uint16 color = 0;
    bool transparent = true;
    bool hasEndCode = false;
    sint32 endCodeCount = 0;

    void Read(uint32 u) {
        const VDP1Command::DrawMode mode = cmd.mode;
        const uint32 charIndex = u + v * charSizeH;
        auto processEndCode = [&](bool endCode) {
            if (endCode && !mode.endCodeDisable) {
                hasEndCode = true;
                ++endCodeCount;
            } else {
                hasEndCode = false;
            }
        };
        switch (mode.colorMode) {
        case 0:
            color = (Read8(vram, cmd.charAddr + (charIndex >> 1)) >> ((~u & 1) * 4)) & 0xF;
            processEndCode(color == 0xF);
            transparent = color == 0x0;
            color |= static_cast<uint16>(cmd.colorBank);
            break;
        case 1:
            color = (Read8(vram, cmd.charAddr + (charIndex >> 1)) >> ((~u & 1) * 4)) & 0xF;
            processEndCode(color == 0xF);
            transparent = color == 0x0;
            color = Read16(vram, color * 2u + cmd.colorBank);
            break;
        case 2:
            color = Read8(vram, cmd.charAddr + charIndex);
            processEndCode(color == 0xFF);
            transparent = color == 0x00;
            color = static_cast<uint16>((color & 0x3F) | cmd.colorBank);
            break;
        case 3:
            color = Read8(vram, cmd.charAddr + charIndex);
            processEndCode(color == 0xFF);
            transparent = color == 0x00;
            color = static_cast<uint16>((color & 0x7F) | cmd.colorBank);
            break;
        case 4:
            color = Read8(vram, cmd.charAddr + charIndex);
            processEndCode(color == 0xFF);
            transparent = color == 0x00;
            color = static_cast<uint16>(color | cmd.colorBank);
            break;
        case 5:
            color = Read16(vram, cmd.charAddr + charIndex * 2u);
            processEndCode(color == 0x7FFF);
            transparent = (color & 0x8000) == 0;
            break;
        default: break; // colour modes 6 and 7 keep the previous texel
        }
    }
};

class PixelWriter {
public:
    PixelWriter(uint32 n, const ref::RasterState &state, const ref::CommandParams &cmd, std::vector<NxFragment> &out)
        : m_n(static_cast<sint32>(n))
        , m_state(state)
        , m_mode(cmd.mode)
        , m_clip(EffectiveClip(cmd.clip, state.doubleV))
        , m_out(out) {
        const bool meshToMeshFB = state.transparentMeshes && m_mode.meshEnable;
        m_targetMode = (static_cast<uint32>(m_mode.colorCalcBits) << ref::kTargetCalcShift) |
                       (meshToMeshFB ? ref::kTargetMeshBit : 0u) | (m_mode.msbOn ? ref::kTargetMsbOnBit : 0u) |
                       (state.pixel8Bits ? ref::kTargetPixel8Bit : 0u) |
                       (state.transparentMeshes ? ref::kTargetTransparentMeshesBit : 0u);
    }

    const ClipRect &Clip() const {
        return m_clip;
    }

    // VDP1PlotPixel up to the write at native coordinates (floor(X / N), floor(Y / N)); false only when clipped
    bool Plot(sint32 X, sint32 Y, uint32 key, uint32 color) {
        const sint32 x = FloorDiv(X, m_n);
        sint32 y = FloorDiv(Y, m_n);
        const uint32 i = static_cast<uint32>(X - x * m_n);
        const uint32 j = static_cast<uint32>(Y - y * m_n);
        if (PixelClipped(m_clip, x, y, m_mode.userClippingEnable, m_mode.clippingMode)) {
            return false;
        }
        const ref::RasterState &s = m_state;
        if (!s.transparentMeshes && m_mode.meshEnable && ((x ^ y) & 1)) {
            return true;
        }
        const bool altFB = s.deinterlace && s.doubleDensity && (y & 1);
        if (s.doubleDensity && !s.deinterlace && s.dblInterlaceEnable &&
            (y & 1) != static_cast<sint32>(s.dblInterlaceDrawLine)) {
            return true;
        }
        if ((s.deinterlace && s.doubleDensity) || s.dblInterlaceEnable) {
            y >>= 1;
        }
        uint32 fbOffset = static_cast<uint32>(y) * s.fbSizeH + static_cast<uint32>(x);
        if (!s.pixel8Bits) {
            fbOffset *= 2u;
        }
        fbOffset &= 0x3FFFF;
        NxFragment f;
        f.key = key;
        f.target = (fbOffset >> 1) | (altFB ? ref::kTargetAltBit : 0u) | ((fbOffset & 1) ? ref::kTargetOddBit : 0u) |
                   m_targetMode;
        f.sub = j * static_cast<uint32>(m_n) + i;
        f.color = color;
        m_out.push_back(f);
        return true;
    }

    // The fragment colour of VDP1PlotPixel
    template <typename G>
    uint32 Color(uint16 color, const G *gouraud) const {
        if (m_mode.msbOn) {
            return 0;
        }
        if (m_state.pixel8Bits) {
            return color & 0xFFu;
        }
        if (m_mode.gouraudEnable && gouraud != nullptr) {
            return gouraud->Blend(Color555{.u16 = color}).u16;
        }
        return color;
    }

private:
    sint32 m_n;
    const ref::RasterState &m_state;
    VDP1Command::DrawMode m_mode;
    ClipRect m_clip;
    uint32 m_targetMode = 0;
    std::vector<NxFragment> &m_out;
};

// -----------------------------------------------------------------------------
// Classification

enum class Kind : uint8 { Rect, Quad, Thin, LineParallel, Replicate };

bool IsLineCommand(const ref::CommandParams &cmd) {
    const uint32 type = static_cast<uint32>(cmd.control.command) & 0xF;
    return type == 5 || type == 6 || type == 7;
}

// The native QuadStepper's 13-bit counters wrap: an edge delta outside [-4096, 4095] (sign_extend<13> changes it) or a
// major delta of 2048 or more (dmaj & 0xFFF, and the edge accumulators leave 13 signed bits)
bool QuadWraps(const std::array<CoordS32, 4> &v) {
    const sint32 deltas[4] = {v[3].x() - v[0].x(), v[3].y() - v[0].y(), v[2].x() - v[1].x(), v[2].y() - v[1].y()};
    sint32 dmaj = 0;
    for (const sint32 d : deltas) {
        if (d < -4096 || d > 4095) {
            return true;
        }
        dmaj = std::max(dmaj, std::abs(d));
    }
    return dmaj >= 2048;
}

bool IsRect(const std::array<CoordS32, 4> &v) {
    return v[0].x() == v[3].x() && v[1].x() == v[2].x() && v[0].y() == v[1].y() && v[2].y() == v[3].y();
}

// All four vertices on one line (a point included)
bool IsCollinear(const std::array<CoordS32, 4> &v) {
    uint32 p = 0, q = 0;
    for (uint32 i = 1; i < 4; ++i) {
        if (v[i].x() != v[p].x() || v[i].y() != v[p].y()) {
            q = i;
            break;
        }
    }
    if (q == 0) {
        return true;
    }
    const sint64 dx = v[q].x() - v[p].x(), dy = v[q].y() - v[p].y();
    for (uint32 i = 0; i < 4; ++i) {
        const sint64 ex = v[i].x() - v[p].x(), ey = v[i].y() - v[p].y();
        if (dx * ey - dy * ex != 0) {
            return false;
        }
    }
    return true;
}

constexpr sint32 kMaxNativeLineMajor = 1364; // longer native lines wrap LineStepper's 13-bit counters (vdp1_clip.hpp)

FORCE_INLINE bool LongLine(CoordS32 a, CoordS32 b) {
    return std::max(std::abs(b.x() - a.x()), std::abs(b.y() - a.y())) > kMaxNativeLineMajor;
}

// Whether any native line of the command can be longer than kMaxNativeLineMajor: every polyline segment, or every quad
// step's line (the native QuadStepper's edges, which the 32-bit stepper reproduces at N = 1 for quads that do not
// wrap). Decided from the command alone, so a command split over batches gets one kind; conservative (lines the
// native termination or line clip drops count too).
bool NativeLinesWrap(const ref::CommandParams &cmd) {
    const auto &v = cmd.vertices;
    if (IsLineCommand(cmd)) {
        if ((static_cast<uint32>(cmd.control.command) & 0xF) == 6) {
            return LongLine(v[0], v[1]);
        }
        return LongLine(v[0], v[1]) || LongLine(v[1], v[2]) || LongLine(v[2], v[3]) || LongLine(v[3], v[0]);
    }
    NxQuad quad{v[0], v[1], v[2], v[3]};
    for (; quad.CanStep(); quad.StepCoordinates()) {
        if (LongLine(quad.left.Coord(), quad.right.Coord())) {
            return true;
        }
    }
    return false;
}

Kind Classify(const Options &o, const ref::CommandParams &cmd, CommandCounts::Class &cls) {
    if (IsLineCommand(cmd)) {
        if (NativeLinesWrap(cmd)) {
            cls = CommandCounts::ReplicateWrap;
            return Kind::Replicate;
        }
        if (o.rules.line == LineRule::Parallel && o.quadPath) {
            cls = CommandCounts::LineParallel;
            return Kind::LineParallel;
        }
        cls = CommandCounts::ReplicateLine;
        return Kind::Replicate;
    }
    if (QuadWraps(cmd.vertices)) {
        cls = CommandCounts::ReplicateWrap;
        return Kind::Replicate;
    }
    if (IsRect(cmd.vertices)) {
        // The rows are the native lines
        if (std::abs(cmd.vertices[1].x() - cmd.vertices[0].x()) > kMaxNativeLineMajor) {
            cls = CommandCounts::ReplicateWrap;
            return Kind::Replicate;
        }
        if (!o.rectPath) {
            cls = CommandCounts::ReplicateNoQuadPath;
            return Kind::Replicate;
        }
        cls = CommandCounts::Rect;
        return Kind::Rect;
    }
    if (!o.quadPath) {
        cls = CommandCounts::ReplicateNoQuadPath;
        return Kind::Replicate;
    }
    if (NativeLinesWrap(cmd)) {
        cls = CommandCounts::ReplicateWrap;
        return Kind::Replicate;
    }
    if (IsCollinear(cmd.vertices)) {
        cls = o.rules.thin == ThinRule::Parallel ? CommandCounts::ThinParallel : CommandCounts::Thin;
        return Kind::Thin;
    }
    cls = CommandCounts::Quad;
    return Kind::Quad;
}

// v_N per axis (VertexRule)
std::array<CoordS32, 4> NxVertices(const std::array<CoordS32, 4> &v, sint32 n, VertexRule rule) {
    const sint32 half = (n - 1) / 2;
    std::array<CoordS32, 4> out{};
    if (rule != VertexRule::CentroidSign) {
        const sint32 c = rule == VertexRule::FloorHalf ? n / 2 : half;
        for (uint32 i = 0; i < 4; ++i) {
            out[i] = {n * v[i].x() + c, n * v[i].y() + c};
        }
        return out;
    }
    const sint64 sx = sint64{v[0].x()} + v[1].x() + v[2].x() + v[3].x();
    const sint64 sy = sint64{v[0].y()} + v[1].y() + v[2].y() + v[3].y();
    auto c = [&](sint32 coord, sint64 sum) { return 4 * sint64{coord} > sum ? n - 1 : 4 * sint64{coord} < sum ? 0 : half; };
    for (uint32 i = 0; i < 4; ++i) {
        out[i] = {n * v[i].x() + c(v[i].x(), sx), n * v[i].y() + c(v[i].y(), sy)};
    }
    return out;
}

// -----------------------------------------------------------------------------
// Generation per kind

class Generator {
public:
    Generator(const Options &o, const BatchInput &in, std::vector<NxSpan> &out)
        : m_o(o)
        , m_n(static_cast<sint32>(o.scale))
        , m_in(in)
        , m_out(out) {}

    void Replicate(uint32 cmdIndex, uint32 s) {
        const ref::Span &span = m_in.spans[s];
        NxSpan nx;
        nx.kind = SpanKind::Replicate;
        nx.cmdIndex = cmdIndex;
        nx.a = span.a;
        nx.b = span.b;
        nx.texV = static_cast<sint32>(span.texV);
        nx.gouraudL = span.gouraudL;
        nx.gouraudR = span.gouraudR;
        nx.slots = m_in.spanSlots[s] * static_cast<uint32>(m_n * m_n);
        m_out.push_back(nx);
    }

    void Rect(uint32 cmdIndex, uint32 first, uint32 end) {
        const ref::CommandParams &cmd = m_in.commands[cmdIndex];
        const auto &v = cmd.vertices;
        const sint32 dy = v[3].y() - v[0].y();
        const bool rowsDown = dy >= 0;
        const uint32 dmaj = static_cast<uint32>(std::abs(dy)); // the quad's major delta (vertical edges)
        const uint32 rows = dmaj + 1;
        const uint32 subRows = rows * static_cast<uint32>(m_n);

        // Per sub-row (stepping order): the texel row and the edge Gouraud colours, "native DDA + sub-step"
        m_texV.assign(subRows, 0);
        m_gouraudL.assign(subRows, Color555{.u16 = 0});
        m_gouraudR.assign(subRows, Color555{.u16 = 0});
        if (cmd.textured) {
            const sint32 charSizeV = cmd.charSizeH == 0 ? 1 : static_cast<sint32>(cmd.charSizeV);
            sint32 start = 0, stop = charSizeV - 1;
            if (cmd.control.flipV) {
                std::swap(start, stop);
            }
            ref::port::Texture native;
            native.Setup(rows, start, stop, false, 0);
            const sint32 v0 = native.value;
            uint32 count = 0;
            for (uint32 k = 0; k < rows; ++k) {
                while (native.ShouldStepTexel()) {
                    native.StepTexel();
                    ++count;
                }
                const uint32 next = NextTexelCount(native, count);
                native.StepPixel();
                for (uint32 j = 0; j < static_cast<uint32>(m_n); ++j) {
                    const uint32 K =
                        j == 0 ? count
                               : std::clamp(PhaseCount(static_cast<uint32>(native.num), static_cast<uint32>(native.den),
                                                       static_cast<uint32>(m_n), k, j, !rowsDown),
                                            count, std::max(count, next));
                    m_texV[k * m_n + j] = v0 + native.inc * static_cast<sint32>(K);
                }
            }
        }
        if (cmd.mode.gouraudEnable) {
            ref::port::Gouraud nl, nr;
            nl.Setup(rows, cmd.gouraud[0], cmd.gouraud[3]);
            nr.Setup(rows, cmd.gouraud[1], cmd.gouraud[2]);
            SubGouraud gl{nl, static_cast<uint32>(m_n), 0, !rowsDown}, gr{nr, static_cast<uint32>(m_n), 0, !rowsDown};
            for (uint32 r = 0; r < subRows; ++r) {
                m_gouraudL[r] = gl.Value();
                m_gouraudR[r] = gr.Value();
                gl.Step();
                gr.Step();
            }
        }

        for (uint32 s = first; s < end; ++s) {
            const ref::Span &span = m_in.spans[s];
            if (span.quadStep >= rows || span.a.y() != span.b.y()) {
                throw std::logic_error("vdp1nx: a rect command's span is not one of its rows");
            }
            const uint32 r0 = span.quadStep * static_cast<uint32>(m_n);
            if ((cmd.textured && m_texV[r0] != static_cast<sint32>(span.texV)) ||
                (cmd.mode.gouraudEnable &&
                 (m_gouraudL[r0].u16 != span.gouraudL.u16 || m_gouraudR[r0].u16 != span.gouraudR.u16))) {
                throw std::logic_error("vdp1nx: sub-row 0 of a rect row differs from the native span");
            }
            for (uint32 j = 0; j < static_cast<uint32>(m_n); ++j) {
                // Sub-row j is N x row N * y + j. Rows stepping down: sub-step j of row k. Rows stepping up: sub-row 0
                // is row k itself and sub-row j > 0 lies between rows k - 1 and k, sub-step N - j of row k - 1 (row
                // 0 repeats itself there)
                const uint32 r = rowsDown || j == 0 ? r0 + j
                                 : span.quadStep == 0 ? 0
                                                      : r0 - m_n + (m_n - j);
                NxSpan nx;
                nx.kind = SpanKind::Rect;
                nx.cmdIndex = cmdIndex;
                nx.a = span.a;
                nx.b = span.b;
                nx.subRow = j;
                nx.texV = m_texV[r];
                nx.gouraudL = m_gouraudL[r];
                nx.gouraudR = m_gouraudR[r];
                nx.slots = (m_in.spanSlots[s] / 2) * static_cast<uint32>(m_n);
                m_out.push_back(nx);
            }
        }
    }

    void Quad(uint32 cmdIndex, Kind kind) {
        const ref::CommandParams &cmd = m_in.commands[cmdIndex];
        const auto &v = cmd.vertices;
        const std::array<CoordS32, 4> base = NxVertices(v, m_n, m_o.rules.vertex);
        if (kind == Kind::Thin && m_o.rules.thin == ThinRule::Parallel) {
            sint32 minX = v[0].x(), maxX = minX, minY = v[0].y(), maxY = minY;
            for (const CoordS32 &c : v) {
                minX = std::min(minX, c.x());
                maxX = std::max(maxX, c.x());
                minY = std::min(minY, c.y());
                maxY = std::max(maxY, c.y());
            }
            const bool xMajor = maxX - minX >= maxY - minY;
            for (sint32 o = 0; o < m_n; ++o) {
                std::array<CoordS32, 4> copy = base;
                for (uint32 i = 0; i < 4; ++i) {
                    copy[i] = xMajor ? CoordS32{base[i].x(), m_n * v[i].y() + o}
                                     : CoordS32{m_n * v[i].x() + o, base[i].y()};
                }
                QuadSpans(cmdIndex, copy);
            }
            return;
        }
        QuadSpans(cmdIndex, base);
    }

    void ParallelLines(uint32 cmdIndex) {
        const ref::CommandParams &cmd = m_in.commands[cmdIndex];
        const bool polyline = (static_cast<uint32>(cmd.control.command) & 0xF) != 6;
        const uint32 segments = polyline ? 4 : 1;
        const bool gouraud = cmd.mode.gouraudEnable;
        const NxExtent e = Extent(EffectiveClip(cmd.clip, m_in.states[cmdIndex].doubleV), m_n);
        for (uint32 i = 0; i < segments; ++i) {
            const uint32 j = (i + 1) & 3;
            const CoordS32 a = cmd.vertices[i], b = cmd.vertices[j];
            const sint32 dx = b.x() - a.x(), dy = b.y() - a.y();
            const bool xMajor = std::abs(dx) >= std::abs(dy);
            for (sint32 o = 0; o < m_n; ++o) {
                CoordS32 na, nb;
                if (xMajor) {
                    na = {m_n * a.x() + (dx >= 0 ? 0 : m_n - 1), m_n * a.y() + o};
                    nb = {m_n * b.x() + (dx >= 0 ? m_n - 1 : 0), m_n * b.y() + o};
                } else {
                    na = {m_n * a.x() + o, m_n * a.y() + (dy >= 0 ? 0 : m_n - 1)};
                    nb = {m_n * b.x() + o, m_n * b.y() + (dy >= 0 ? m_n - 1 : 0)};
                }
                AddQuadSpan(cmdIndex, e, na, nb, false, 0, gouraud ? cmd.gouraud[i] : Color555{.u16 = 0},
                            gouraud ? cmd.gouraud[j] : Color555{.u16 = 0});
            }
        }
    }

private:
    void QuadSpans(uint32 cmdIndex, const std::array<CoordS32, 4> &vn) {
        const ref::CommandParams &cmd = m_in.commands[cmdIndex];
        const NxExtent e = Extent(EffectiveClip(cmd.clip, m_in.states[cmdIndex].doubleV), m_n);
        NxQuad quad{vn[0], vn[1], vn[2], vn[3]};
        if (cmd.mode.gouraudEnable) {
            quad.left.SetupGouraud(cmd.gouraud[0], cmd.gouraud[3]);
            quad.right.SetupGouraud(cmd.gouraud[1], cmd.gouraud[2]);
        }
        ref::port::Texture texV;
        if (cmd.textured) {
            const sint32 charSizeV = cmd.charSizeH == 0 ? 1 : static_cast<sint32>(cmd.charSizeV);
            sint32 start = 0, stop = charSizeV - 1;
            if (cmd.control.flipV) {
                std::swap(start, stop);
            }
            texV.Setup(quad.dmaj + 1, start, stop, false, 0);
        }
        const NxTermination termination{quad, e};
        for (; quad.CanStep(); quad.Step()) {
            if (termination.Done(quad)) {
                break;
            }
            sint32 v = 0;
            if (cmd.textured) {
                while (texV.ShouldStepTexel()) {
                    texV.StepTexel();
                }
                texV.StepPixel();
                v = static_cast<sint32>(texV.Value());
            }
            Color555 gl{.u16 = 0}, gr{.u16 = 0};
            if (cmd.mode.gouraudEnable) {
                gl = quad.left.GouraudValue();
                gr = quad.right.GouraudValue();
            }
            AddQuadSpan(cmdIndex, e, quad.left.Coord(), quad.right.Coord(), cmd.antiAlias, v, gl, gr);
        }
    }

    void AddQuadSpan(uint32 cmdIndex, const NxExtent &e, CoordS32 a, CoordS32 b, bool aa, sint32 texV, Color555 gl,
                     Color555 gr) {
        if (NxLineRejected(e, a, b)) {
            return;
        }
        NxLine line{a, b, aa};
        line.SystemClip(e.maxX, e.maxY);
        if (line.length == 0) {
            return;
        }
        NxSpan nx;
        nx.kind = SpanKind::Quad;
        nx.antiAlias = aa;
        nx.cmdIndex = cmdIndex;
        nx.a = a;
        nx.b = b;
        nx.texV = texV;
        nx.gouraudL = gl;
        nx.gouraudR = gr;
        nx.slots = 2 * line.length;
        m_out.push_back(nx);
    }

    const Options &m_o;
    sint32 m_n;
    const BatchInput &m_in;
    std::vector<NxSpan> &m_out;
    std::vector<sint32> m_texV;
    std::vector<Color555> m_gouraudL, m_gouraudR;
};

// -----------------------------------------------------------------------------
// Raster per kind

void RasterReplicate(uint32 n, const ref::RasterState &state, const ref::CommandParams &cmd, const NxSpan &span,
                     const uint8 *vram, std::vector<NxFragment> &out) {
    ref::Span native;
    native.cmdIndex = span.cmdIndex;
    native.a = span.a;
    native.b = span.b;
    native.texV = static_cast<uint32>(span.texV);
    native.gouraudL = span.gouraudL;
    native.gouraudR = span.gouraudR;
    native.fragBase = 0;
    thread_local std::vector<ref::Fragment> frags;
    frags.clear();
    ref::RasterSpan(state, cmd, native, vram, ref::RasterOptions{}, frags);
    const uint32 sub = n * n;
    for (const ref::Fragment &f : frags) {
        for (uint32 s = 0; s < sub; ++s) {
            out.push_back({span.slotBase + f.key * sub + s, f.target, s, f.color});
        }
    }
}

void RasterRect(uint32 n, const ref::RasterState &state, const ref::CommandParams &cmd, const NxSpan &span,
                const uint8 *vram, std::vector<NxFragment> &out) {
    PixelWriter writer{n, state, cmd, out};
    const ClipRect &clip = writer.Clip();
    if (NativeLineSystemClipped(clip, span.a, span.b)) {
        return;
    }
    const sint32 sn = static_cast<sint32>(n);
    const VDP1Command::DrawMode mode = cmd.mode;
    ref::port::Line line{span.a, span.b, cmd.antiAlias};
    const uint32 skip = line.SystemClip(clip.sysH, clip.sysV);
    if (line.xMajInc == 0) {
        throw std::logic_error("vdp1nx: a rect row is not horizontal");
    }
    const bool forward = line.xMajInc > 0;
    const sint32 Y = sn * span.a.y() + static_cast<sint32>(span.subRow);

    // The states of sub-step s of native pixel p (stepping order): whether drawn, the colour
    struct State {
        bool drawn;
        uint32 color;
    };
    thread_local std::vector<State> states;
    thread_local std::vector<sint32> xs;
    states.clear();
    xs.clear();
    states.reserve(size_t{line.length} * n);

    ref::port::Gouraud nativeGouraud;
    SubGouraud gouraud{};
    if (mode.gouraudEnable) {
        nativeGouraud.Setup(line.dmaj + 1, span.gouraudL, span.gouraudR);
        gouraud = SubGouraud{nativeGouraud, n, skip, !forward};
    }

    if (!cmd.textured) {
        for (line.Step(); line.CanStep(); line.Step()) {
            xs.push_back(line.x);
            for (uint32 s = 0; s < n; ++s) {
                states.push_back({true, writer.Color(cmd.color, &gouraud)});
                if (mode.gouraudEnable) {
                    gouraud.Step();
                }
            }
        }
    } else {
        const uint32 charSizeH = std::max<uint32>(cmd.charSizeH, 1u);
        sint32 uStart = 0, uEnd = static_cast<sint32>(charSizeH) - 1;
        if (cmd.control.flipH) {
            std::swap(uStart, uEnd);
        }
        const bool hss = mode.highSpeedShrink && line.dmaj < charSizeH - 1; // decided natively
        ref::port::Texture u;
        u.Setup(line.dmaj + 1, uStart, uEnd, hss, state.evenOddCoordSelect ? 1 : 0);
        u.SkipPixels(skip);
        const sint32 v0 = u.value;
        TexelReader texel{cmd, vram, static_cast<uint32>(span.texV), charSizeH};
        texel.endCodeCount = hss ? std::numeric_limits<sint32>::min() : 0;
        texel.Read(static_cast<uint32>(v0));
        uint32 count = 0; // native texel steps (catch-up loops)
        uint32 read = 0;  // texels read
        bool ended = false;
        for (line.Step(); line.CanStep() && !ended; line.Step()) {
            const uint32 P = skip + line.step - 1;
            xs.push_back(line.x);
            while (u.ShouldStepTexel()) {
                u.StepTexel();
                ++count;
            }
            const uint32 next = NextTexelCount(u, count);
            for (uint32 s = 0; s < n; ++s) {
                const uint32 K = s == 0 ? count
                                        : std::clamp(PhaseCount(static_cast<uint32>(u.num), static_cast<uint32>(u.den), n, P,
                                                                s, !forward),
                                                     count, std::max(count, next));
                while (read < K) {
                    ++read;
                    texel.Read(static_cast<uint32>(v0 + u.inc * static_cast<sint32>(read)));
                    if (texel.endCodeCount == 2) {
                        break;
                    }
                }
                if (texel.endCodeCount == 2) {
                    ended = true; // the line ends before this sub-step
                    break;
                }
                const bool drawn = !(texel.hasEndCode || (texel.transparent && !mode.transparentPixelDisable));
                states.push_back({drawn, writer.Color(texel.color, &gouraud)});
                if (mode.gouraudEnable) {
                    gouraud.Step();
                }
            }
            u.StepPixel();
        }
    }

    // Sub-texel i of native pixel p's block. Forward: sub-step i of p. Backward (right to left): sub-texel 0 is p
    // itself and i > 0 lies between pixels p - 1 and p, sub-step N - i of p - 1 (pixel 0 repeats itself there).
    for (uint32 p = 0; p < xs.size(); ++p) {
        for (uint32 i = 0; i < n; ++i) {
            const size_t index = forward || i == 0 ? size_t{p} * n + i : p == 0 ? 0 : size_t{p - 1} * n + (n - i);
            if (index >= states.size() || !states[index].drawn) {
                continue;
            }
            const uint32 slot = forward || i == 0 ? i : n - i;
            writer.Plot(sn * xs[p] + static_cast<sint32>(i), Y, span.slotBase + n * p + slot, states[index].color);
        }
    }
}

void RasterQuad(uint32 n, const ref::RasterState &state, const ref::CommandParams &cmd, const NxSpan &span,
                const uint8 *vram, std::vector<NxFragment> &out) {
    PixelWriter writer{n, state, cmd, out};
    const NxExtent e = Extent(writer.Clip(), static_cast<sint32>(n));
    const VDP1Command::DrawMode mode = cmd.mode;
    NxLine line{span.a, span.b, span.antiAlias};
    const uint32 skip = line.SystemClip(e.maxX, e.maxY);
    ref::port::Gouraud gouraud;
    if (mode.gouraudEnable) {
        gouraud.Setup(line.dmaj + 1, span.gouraudL, span.gouraudR);
        gouraud.Skip(skip);
    }
    auto plotStep = [&](uint32 step, bool aa, uint16 color) {
        const uint32 c = writer.Color(color, &gouraud);
        writer.Plot(line.x, line.y, span.slotBase + 2 * step, c);
        if (aa) {
            writer.Plot(line.x + line.aaXInc, line.y + line.aaYInc, span.slotBase + 2 * step + 1, c);
        }
    };

    if (!cmd.textured) {
        bool aa = false;
        for (line.Step(); line.CanStep(); aa = line.Step()) {
            plotStep(line.step - 1, aa, cmd.color);
            if (mode.gouraudEnable) {
                gouraud.Step();
            }
        }
        return;
    }

    const uint32 charSizeH = std::max<uint32>(cmd.charSizeH, 1u);
    sint32 uStart = 0, uEnd = static_cast<sint32>(charSizeH) - 1;
    if (cmd.control.flipH) {
        std::swap(uStart, uEnd);
    }
    const bool hss = mode.highSpeedShrink && line.dmaj / n < charSizeH - 1; // the native-equivalent length
    ref::port::Texture u;
    u.Setup(line.dmaj + 1, uStart, uEnd, hss, state.evenOddCoordSelect ? 1 : 0);
    u.SkipPixels(skip);
    TexelReader texel{cmd, vram, static_cast<uint32>(span.texV), charSizeH};
    texel.endCodeCount = hss ? std::numeric_limits<sint32>::min() : 0;
    texel.Read(u.Value());

    bool aa = false;
    for (line.Step(); line.CanStep(); aa = line.Step()) {
        while (u.ShouldStepTexel()) {
            u.StepTexel();
            texel.Read(u.Value());
            if (texel.endCodeCount == 2) {
                break;
            }
        }
        if (texel.endCodeCount == 2) {
            return;
        }
        u.StepPixel();
        if (!(texel.hasEndCode || (texel.transparent && !mode.transparentPixelDisable))) {
            plotStep(line.step - 1, aa, texel.color);
        }
        if (mode.gouraudEnable) {
            gouraud.Step();
        }
    }
}

uint16 &NxWord(NxFbram &nx, uint32 slice, uint32 index) {
    return nx.slices[slice][index];
}

} // namespace

// =====================================================================================================================
// Names

const char *Name(VertexRule rule) {
    switch (rule) {
    case VertexRule::FloorHalfDown: return "R1 c=floor((N-1)/2)";
    case VertexRule::FloorHalf: return "R2 c=floor(N/2)";
    case VertexRule::CentroidSign: return "R3 centroid-sign";
    }
    return "?";
}

const char *Name(ThinRule rule) {
    return rule == ThinRule::Hairline ? "thin hairline" : "thin N-parallel";
}

const char *Name(LineRule rule) {
    return rule == LineRule::NativeThick ? "lines native-thick" : "lines N-parallel";
}

const char *CommandCounts::Name(uint32 c) {
    static constexpr const char *kNames[Count] = {"rect",           "quad",          "thin (hairline)",
                                                  "thin (N-parallel)", "line (N-parallel)", "replicate (13-bit wrap)",
                                                  "replicate (line)", "replicate (no quad path)", "continuation"};
    return c < Count ? kNames[c] : "?";
}

// =====================================================================================================================
// N x surface

NxFbram::NxFbram(uint32 n)
    : scale(n) {
    if (n < 1 || n > 4) {
        throw std::invalid_argument("vdp1nx: scale out of range");
    }
    for (auto &s : slices) {
        s.assign(size_t{ref::kWordsPerFB} * n * n, 0);
    }
}

namespace {

const SpriteFB &Slice(const ref::Fbram &fb, uint32 slice) {
    switch (slice) {
    case 0: return fb.main[0];
    case 1: return fb.main[1];
    case 2: return fb.alt[0];
    case 3: return fb.alt[1];
    case 4: return fb.mesh[0][0];
    case 5: return fb.mesh[0][1];
    case 6: return fb.mesh[1][0];
    default: return fb.mesh[1][1];
    }
}

FORCE_INLINE uint16 WordOf(const SpriteFB &s, uint32 word) {
    const uint32 a = (word & (ref::kWordsPerFB - 1)) * 2;
    return static_cast<uint16>((s[a] << 8) | s[a + 1]);
}

} // namespace

uint16 NativeWord(const ref::Fbram &fb, uint32 slice, uint32 word) {
    return WordOf(Slice(fb, slice), word);
}

void NxFbram::ReplicateFrom(const ref::Fbram &fb) {
    const uint32 sub = Sub();
    for (uint32 slice = 0; slice < 8; ++slice) {
        const SpriteFB &s = Slice(fb, slice);
        uint16 *out = slices[slice].data();
        for (uint32 w = 0; w < ref::kWordsPerFB; ++w) {
            const uint16 v = WordOf(s, w);
            for (uint32 i = 0; i < sub; ++i) {
                *out++ = v;
            }
        }
    }
}

std::string CompareSamplePoints(const ref::Fbram &native, const NxFbram &nx, uint64 *mismatches) {
    static constexpr const char *kSlices[8] = {"main fb0",    "main fb1",    "alt fb0",     "alt fb1",
                                               "mesh[0] fb0", "mesh[0] fb1", "mesh[1] fb0", "mesh[1] fb1"};
    std::string first;
    uint64 count = 0;
    for (uint32 slice = 0; slice < 8; ++slice) {
        const SpriteFB &ns = Slice(native, slice);
        for (uint32 w = 0; w < ref::kWordsPerFB; ++w) {
            const uint16 a = WordOf(ns, w);
            const uint16 b = nx.Word(slice, w, 0, 0);
            if (a != b) {
                if (count++ == 0) {
                    char text[160];
                    std::snprintf(text, sizeof(text), "%s word 0x%05X (x %u, y %u at 512 wide): native %04X, N x %04X",
                                  kSlices[slice], w, w % 512, w / 512, a, b);
                    first = text;
                }
            }
        }
    }
    if (mismatches != nullptr) {
        *mismatches = count;
    }
    return count == 0 ? std::string{} : first + " (" + std::to_string(count) + " sample points differ)";
}

std::string CompareNx(const NxFbram &a, const NxFbram &b) {
    if (a.scale != b.scale) {
        return "scales differ";
    }
    for (uint32 slice = 0; slice < 8; ++slice) {
        if (a.slices[slice] != b.slices[slice]) {
            const auto m = std::mismatch(a.slices[slice].begin(), a.slices[slice].end(), b.slices[slice].begin());
            const size_t index = static_cast<size_t>(m.first - a.slices[slice].begin());
            return "slice " + std::to_string(slice) + " index " + std::to_string(index) + " (native word " +
                   std::to_string(index / a.Sub()) + ", sub " + std::to_string(index % a.Sub()) + ") differs";
        }
    }
    return {};
}

// =====================================================================================================================
// Generator

std::vector<NxSpan> GenerateBatch(const Options &o, const BatchInput &in, CommandCounts *counts) {
    std::vector<NxSpan> out;
    out.reserve(in.spans.size() * o.scale);
    Generator gen{o, in, out};
    const uint32 spanCount = static_cast<uint32>(in.spans.size());
    for (uint32 first = 0; first < spanCount;) {
        const uint32 cmdIndex = in.spans[first].cmdIndex;
        uint32 end = first + 1;
        while (end < spanCount && in.spans[end].cmdIndex == cmdIndex) {
            ++end;
        }
        const ref::CommandParams &cmd = in.commands[cmdIndex];
        CommandCounts::Class cls{};
        const Kind kind = Classify(o, cmd, cls);
        const bool continuation = cmdIndex == 0 && in.firstCommandContinues;
        switch (kind) {
        case Kind::Replicate:
            for (uint32 s = first; s < end; ++s) {
                gen.Replicate(cmdIndex, s);
            }
            break;
        case Kind::Rect: gen.Rect(cmdIndex, first, end); break;
        case Kind::Quad:
        case Kind::Thin:
        case Kind::LineParallel:
            // generated whole where the command's first span is
            if (continuation) {
                cls = CommandCounts::Continuation;
            } else if (kind == Kind::LineParallel) {
                gen.ParallelLines(cmdIndex);
            } else {
                gen.Quad(cmdIndex, kind);
            }
            break;
        }
        if (counts != nullptr) {
            ++counts->counts[cls];
        }
        first = end;
    }
    return out;
}

std::vector<std::pair<uint32, uint32>> SplitSubBatches(const Options &o, std::vector<NxSpan> &spans) {
    std::vector<std::pair<uint32, uint32>> out;
    uint32 begin = 0, slots = 0;
    for (uint32 i = 0; i < spans.size(); ++i) {
        const uint32 s = spans[i].slots;
        if (i > begin && (i - begin >= o.subBatchSpans || uint64{slots} + s > o.subBatchSlots)) {
            out.emplace_back(begin, i);
            begin = i;
            slots = 0;
        }
        spans[i].slotBase = slots;
        slots += s;
    }
    if (begin < spans.size()) {
        out.emplace_back(begin, static_cast<uint32>(spans.size()));
    }
    return out;
}

// =====================================================================================================================
// Raster and resolve

void RasterSpan(uint32 n, const ref::RasterState &state, const ref::CommandParams &cmd, const NxSpan &span,
                const uint8 *vram, std::vector<NxFragment> &out) {
    switch (span.kind) {
    case SpanKind::Rect: RasterRect(n, state, cmd, span, vram, out); break;
    case SpanKind::Quad: RasterQuad(n, state, cmd, span, vram, out); break;
    case SpanKind::Replicate: RasterReplicate(n, state, cmd, span, vram, out); break;
    }
}

NxResolveStats Resolve(uint32 drawFB, std::vector<NxFragment> fragments, NxFbram &nx, ResolveOrder order) {
    const uint32 sub = nx.Sub();
    const bool emissionOrder = order == ResolveOrder::Emission;
    const bool descending = order == ResolveOrder::KeyDescendingForTesting;
    auto unitOf = [sub](const NxFragment &f) { return (f.target & ref::kTargetUnitMask) * sub + f.sub; };
    auto keyOrder = [&](const NxFragment &l, const NxFragment &r) { return descending ? l.key > r.key : l.key < r.key; };
    // Bucket by unit with a stable radix sort (units < 2^22: two 11-bit passes), which keeps the emission order inside
    // a unit; then each unit's run is sorted by key unless resolving in emission order
    {
        std::vector<NxFragment> tmp(fragments.size());
        for (const uint32 shift : {0u, 11u}) {
            std::vector<uint32> start(2049, 0);
            for (const NxFragment &f : fragments) {
                ++start[((unitOf(f) >> shift) & 2047) + 1];
            }
            for (size_t i = 1; i < start.size(); ++i) {
                start[i] += start[i - 1];
            }
            for (const NxFragment &f : fragments) {
                tmp[start[(unitOf(f) >> shift) & 2047]++] = f;
            }
            fragments.swap(tmp);
        }
    }
    if (!emissionOrder) {
        for (size_t begin = 0; begin < fragments.size();) {
            size_t end = begin + 1;
            while (end < fragments.size() && unitOf(fragments[end]) == unitOf(fragments[begin])) {
                ++end;
            }
            if (!std::is_sorted(fragments.begin() + begin, fragments.begin() + end, keyOrder)) {
                std::sort(fragments.begin() + begin, fragments.begin() + end, keyOrder);
            }
            begin = end;
        }
    }

    NxResolveStats stats;
    stats.fragments = fragments.size();
    const uint32 fb = drawFB & 1;
    for (size_t begin = 0; begin < fragments.size();) {
        const uint32 unit = unitOf(fragments[begin]);
        size_t end = begin + 1;
        while (end < fragments.size() && unitOf(fragments[end]) == unit) {
            if (!emissionOrder && fragments[end].key == fragments[end - 1].key) {
                throw std::logic_error("vdp1nx::Resolve: duplicate fragment key");
            }
            ++end;
        }
        const uint32 target = fragments[begin].target;
        const bool alt = (target & ref::kTargetAltBit) != 0;
        const uint32 index = (target & ref::kTargetWordMask) * sub + fragments[begin].sub;
        uint16 &mainWord = NxWord(nx, alt ? gpu::Vdp1AltSlice(fb) : gpu::Vdp1MainSlice(fb), index);
        uint16 &meshWord = NxWord(nx, gpu::Vdp1MeshSlice(alt ? 1 : 0, fb), index);
        uint16 m = mainWord;
        uint16 s = meshWord;
        uint32 halfTransparent = 0;
        for (size_t i = begin; i < end; ++i) {
            const NxFragment &f = fragments[i];
            halfTransparent += (f.target & (ref::kTargetMsbOnBit | ref::kTargetPixel8Bit)) == 0 &&
                               ((f.target >> ref::kTargetCalcShift) & 3) == 3;
            const uint32 calc = (f.target >> ref::kTargetCalcShift) & 3;
            stats.overwrite += (f.target & ref::kTargetMsbOnBit) == 0 &&
                               ((f.target & ref::kTargetPixel8Bit) != 0 || calc == 0 || calc == 2);
            if (f.target & ref::kTargetMsbOnBit) {
                m |= 0x8000;
                continue;
            }
            const bool toMesh = (f.target & ref::kTargetMeshBit) != 0;
            stats.mesh += toMesh;
            const bool transparentMeshes = (f.target & ref::kTargetTransparentMeshesBit) != 0;
            if (f.target & ref::kTargetPixel8Bit) {
                const uint32 shift = (f.target & ref::kTargetOddBit) ? 0 : 8;
                const uint16 keep = static_cast<uint16>(~(0xFFu << shift));
                const uint16 byte = static_cast<uint16>((f.color & 0xFF) << shift);
                if (toMesh) {
                    s = (s & keep) | byte;
                } else {
                    m = (m & keep) | byte;
                    if (transparentMeshes) {
                        s &= keep;
                    }
                }
                continue;
            }
            const Color555 src{.u16 = static_cast<uint16>(f.color)};
            Color555 dst{.u16 = m};
            switch ((f.target >> ref::kTargetCalcShift) & 3) {
            case 0: dst = src; break;
            case 1:
                if (dst.msb) {
                    dst.r >>= 1u;
                    dst.g >>= 1u;
                    dst.b >>= 1u;
                }
                break;
            case 2:
                dst.r = src.r >> 1u;
                dst.g = src.g >> 1u;
                dst.b = src.b >> 1u;
                dst.msb = src.msb;
                break;
            case 3:
                if (dst.msb) {
                    dst.r = (src.r + dst.r) >> 1u;
                    dst.g = (src.g + dst.g) >> 1u;
                    dst.b = (src.b + dst.b) >> 1u;
                } else {
                    dst = src;
                }
                break;
            }
            if (toMesh) {
                s = dst.u16;
            } else {
                m = dst.u16;
                if (transparentMeshes) {
                    s = 0;
                }
            }
        }
        mainWord = m;
        meshWord = s;
        stats.multiHalfTransparent += halfTransparent >= 2;
        stats.halfTransparent += halfTransparent;
        ++stats.units;
        stats.maxPerUnit = std::max(stats.maxPerUnit, static_cast<uint32>(end - begin));
        begin = end;
    }
    return stats;
}

uint64 Erase(const gpu::Vdp1ErasePushConstants &params, NxFbram &nx) {
    const bool mirror = gpu::Vdp1EraseMirrorAlt(params.flags);
    const bool clearMesh = gpu::Vdp1EraseClearMesh(params.flags);
    const uint32 fb = gpu::Vdp1EraseFb(params.flags) & 1;
    const uint32 width = params.x3 > params.x1 ? params.x3 - params.x1 : 0;
    const uint16 value = static_cast<uint16>(params.value);
    const uint32 sub = nx.Sub();
    uint64 writes = 0;
    auto fill = [&](uint32 slice, uint32 word, uint16 v) {
        std::fill_n(nx.slices[slice].begin() + size_t{word} * sub, sub, v);
    };
    for (uint32 y = params.y1; y <= params.y3; y++) {
        for (uint32 x = params.x1; x < params.x3; x++) {
            const uint64 i = static_cast<uint64>(y - params.y1) * width + (x - params.x1);
            if (i >= params.writeLimit) {
                continue;
            }
            const uint32 word = ((((y << params.offsetShift) + x) * 2u) & 0x3FFFE) >> 1;
            fill(gpu::Vdp1MainSlice(fb), word, value);
            ++writes;
            if (mirror) {
                fill(gpu::Vdp1AltSlice(fb), word, value);
            }
            if (clearMesh) {
                fill(gpu::Vdp1MeshSlice(0, fb), word, 0);
                if (mirror) {
                    fill(gpu::Vdp1MeshSlice(1, fb), word, 0);
                }
            }
        }
    }
    return writes;
}

void ApplyFbWrites(std::span<const gpu::Vdp1FbWrite> writes, NxFbram &nx) {
    const uint32 sub = nx.Sub();
    for (const gpu::Vdp1FbWrite &w : writes) {
        const uint32 word = gpu::Vdp1FbWriteWord(w.address);
        const uint32 fb = gpu::Vdp1FbWriteFb(w.address) & 1;
        const uint16 mask = static_cast<uint16>(w.mask);
        auto apply = [&](uint32 slice) {
            for (uint32 s = 0; s < sub; ++s) {
                uint16 &v = nx.slices[slice][size_t{word} * sub + s];
                v = static_cast<uint16>((v & ~mask) | (w.value & mask));
            }
        };
        apply(gpu::Vdp1MainSlice(fb));
        if (gpu::Vdp1FbWriteMirrorAlt(w.address)) {
            apply(gpu::Vdp1AltSlice(fb));
        }
    }
}

// =====================================================================================================================
// Driver

ReferenceNx::ReferenceNx(ref::Fbram &native, NxFbram &nx, Options options, std::span<const uint8> vram)
    : m_native(native)
    , m_nx(nx)
    , m_options(options)
    , m_vram(ymir::vdp::kVDP1VRAMSize, 0)
    , m_shuffle(options.shuffleSeed) {
    if (nx.scale != options.scale) {
        throw std::invalid_argument("vdp1nx: the N x surface has another scale");
    }
    std::copy_n(vram.begin(), std::min(vram.size(), m_vram.size()), m_vram.begin());
}

void ReferenceNx::Replay(const gpu::Vdp1OpList &ops) {
    for (const gpu::Vdp1Op &op : ops.ops) {
        switch (op.kind) {
        case gpu::Vdp1OpKind::Draw: DrawBatch(ops, ops.batches.at(op.index)); break;
        case gpu::Vdp1OpKind::Erase: {
            const gpu::Vdp1ErasePushConstants &e = ops.erases.at(op.index);
            ref::Erase(e, m_native);
            Erase(e, m_nx);
            m_prevCapacity = false;
            break;
        }
        case gpu::Vdp1OpKind::FbWrites: {
            const gpu::Vdp1FbWritePushConstants &run = ops.fbWriteRuns.at(op.index);
            if (run.count == 0 || static_cast<size_t>(run.offset) + run.count > ops.fbWrites.size()) {
                throw std::logic_error("vdp1nx::Replay: bad framebuffer write run");
            }
            const auto writes = std::span(ops.fbWrites).subspan(run.offset, run.count);
            ref::ApplyFbWrites(writes, m_native);
            ApplyFbWrites(writes, m_nx);
            m_prevCapacity = false;
            break;
        }
        }
    }
}

void ReferenceNx::DrawBatch(const gpu::Vdp1OpList &ops, const gpu::Vdp1DrawBatch &batch) {
    for (uint32 i = 0; i < batch.copyCount; ++i) {
        const gpu::Vdp1VramCopy &copy = ops.vramCopies.at(batch.firstCopy + i);
        if (static_cast<size_t>(copy.vramOffset) + copy.size > m_vram.size() ||
            static_cast<size_t>(copy.arenaOffset) + copy.size > ops.arena.size()) {
            throw std::logic_error("vdp1nx::DrawBatch: VRAM copy out of range");
        }
        std::copy_n(ops.arena.begin() + copy.arenaOffset, copy.size, m_vram.begin() + copy.vramOffset);
    }
    if (batch.spanCount == 0 || batch.cmdCount == 0 ||
        static_cast<size_t>(batch.spanOffset) + batch.spanCount > ops.spans.size() ||
        static_cast<size_t>(batch.cmdOffset) + batch.cmdCount > ops.commands.size()) {
        throw std::logic_error("vdp1nx::DrawBatch: bad batch");
    }
    m_lastDrawFB = batch.drawFB & 1;
    ++m_stats.batches;

    std::vector<ref::CommandParams> commands(batch.cmdCount);
    std::vector<ref::RasterState> states(batch.cmdCount);
    for (uint32 i = 0; i < batch.cmdCount; ++i) {
        const gpu::Vdp1CommandParams &c = ops.commands[batch.cmdOffset + i];
        commands[i] = ref::DecodeCommand(c);
        states[i] = ref::DecodeRasterState(c.rasterFlags, batch.drawFB);
    }
    std::vector<ref::Span> spans(batch.spanCount);
    std::vector<uint32> slots(batch.spanCount);
    for (uint32 s = 0; s < batch.spanCount; ++s) {
        spans[s] = ref::DecodeSpan(ops.spans[batch.spanOffset + s]);
        const uint32 end = s + 1 < batch.spanCount ? ops.spans[batch.spanOffset + s + 1].fragBase : batch.fragmentCount;
        if (spans[s].cmdIndex >= batch.cmdCount || end <= spans[s].fragBase) {
            throw std::logic_error("vdp1nx::DrawBatch: bad span");
        }
        slots[s] = end - spans[s].fragBase;
        const sint32 major = std::max(std::abs(spans[s].b.x() - spans[s].a.x()), std::abs(spans[s].b.y() - spans[s].a.y()));
        m_stats.nativeLongLines += major > 1364;
    }

    // Native: the stage 2 reference's raster and resolve
    auto t0 = Clock::now();
    {
        std::vector<ref::Fragment> fragments;
        fragments.reserve(batch.fragmentCount);
        for (uint32 s = 0; s < batch.spanCount; ++s) {
            ref::RasterSpan(states[spans[s].cmdIndex], commands[spans[s].cmdIndex], spans[s], m_vram.data(),
                            ref::RasterOptions{}, fragments);
        }
        m_stats.nativeSpans += batch.spanCount;
        m_stats.nativeSlots += batch.fragmentCount;
        m_stats.nativeFragments += fragments.size();
        if (m_observer) {
            m_observedFragments = fragments;
        }
        ref::Resolve(batch.drawFB, std::move(fragments), m_native);
    }
    m_stats.nativeSeconds += Seconds(t0);

    // N x
    const gpu::Vdp1CommandParams &firstCommand = ops.commands[batch.cmdOffset];
    BatchInput in;
    in.commands = commands;
    in.states = states;
    in.spans = spans;
    in.spanSlots = slots;
    in.firstCommandContinues = m_prevCapacity &&
                               std::memcmp(&firstCommand, &m_prevCommand, sizeof(gpu::Vdp1CommandParams)) == 0 &&
                               spans[0].quadStep > m_prevQuadStep;
    t0 = Clock::now();
    std::vector<NxSpan> nxSpans = GenerateBatch(m_options, in, &m_stats.commands);
    const auto subBatches = SplitSubBatches(m_options, nxSpans);
    m_stats.generateSeconds += Seconds(t0);
    if (m_observer) {
        m_observer(BatchObservation{batch.drawFB & 1u, commands, states, spans, m_observedFragments, nxSpans});
        m_observedFragments.clear();
    }

    std::vector<NxFragment> fragments;
    for (const auto &[begin, end] : subBatches) {
        t0 = Clock::now();
        fragments.clear();
        uint32 subSlots = 0;
        for (uint32 i = begin; i < end; ++i) {
            const NxSpan &span = nxSpans[i];
            const size_t before = fragments.size();
            RasterSpan(m_options.scale, states[span.cmdIndex], commands[span.cmdIndex], span, m_vram.data(), fragments);
            for (size_t f = before; f < fragments.size(); ++f) {
                if (fragments[f].key < span.slotBase || fragments[f].key >= span.slotBase + span.slots) {
                    throw std::logic_error("vdp1nx::DrawBatch: fragment key outside its span's slots");
                }
            }
            const auto k = static_cast<size_t>(span.kind);
            ++m_stats.spans[k];
            m_stats.slots[k] += span.slots;
            m_stats.fragments[k] += fragments.size() - before;
            subSlots += span.slots;
        }
        m_stats.maxSubBatchSlots = std::max(m_stats.maxSubBatchSlots, subSlots);
        ++m_stats.subBatches;
        if (m_shuffle != 0) {
            for (size_t i = fragments.size(); i > 1; --i) {
                std::swap(fragments[i - 1], fragments[ref::SplitMix64(m_shuffle) % i]);
            }
        }
        m_stats.rasterSeconds += Seconds(t0);
        t0 = Clock::now();
        const ResolveOrder order = m_options.resolveInEmissionOrder        ? ResolveOrder::Emission
                                   : m_options.resolveDescendingForTesting ? ResolveOrder::KeyDescendingForTesting
                                                                           : ResolveOrder::Key;
        const NxResolveStats rs = Resolve(batch.drawFB, std::move(fragments), m_nx, order);
        fragments = {};
        m_stats.maxPerUnit = std::max(m_stats.maxPerUnit, rs.maxPerUnit);
        m_stats.multiHalfTransparent += rs.multiHalfTransparent;
        m_stats.halfTransparent += rs.halfTransparent;
        m_stats.overwrite += rs.overwrite;
        m_stats.mesh += rs.mesh;
        m_stats.resolveSeconds += Seconds(t0);
    }

    m_prevCapacity = batch.end == gpu::Vdp1BatchEnd::Capacity;
    m_prevCommand = ops.commands[batch.cmdOffset + batch.cmdCount - 1];
    m_prevQuadStep = spans.back().quadStep;
}

} // namespace brimir::test::vdp1nx
