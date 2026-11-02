#include "gpu/vdp1/vdp1_nx_front.hpp"

#include <ymir/hw/vdp/renderer/vdp1_probe.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace brimir::gpu::vdp1 {

namespace {

// Wrapping 32-bit arithmetic, as the steppers' (the GPU's and the reference's)
inline int32_t Add(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t Sub(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline int32_t Shl(int32_t a, uint32_t s) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) << s);
}

struct Point {
    int32_t x, y;
};

inline uint16_t Pack555(int32_t r, int32_t g, int32_t b) {
    return static_cast<uint16_t>((static_cast<uint32_t>(r) & 31u) | ((static_cast<uint32_t>(g) & 31u) << 5) |
                                 ((static_cast<uint32_t>(b) & 31u) << 10));
}

// ---------------------------------------------------------------------------------------------------------------------
// Steppers (vdp1_steppers.hpp in 32 bits; the reference's port::Texture and port::GouraudChannel)

// TextureStepper without high-speed shrink (the generator steps texel rows only)
struct TexStepper {
    int32_t num = 0, den = 0, accum = 0, value = 0, inc = 0;

    void Setup(uint32_t len, int32_t start, int32_t end) {
        const int32_t delta = end - start;
        const uint32_t absDelta = static_cast<uint32_t>(std::abs(delta));
        value = start;
        inc = delta >= 0 ? +1 : -1;
        num = static_cast<int32_t>(absDelta);
        den = static_cast<int32_t>(len);
        if (len <= absDelta) {
            ++num;
            accum = static_cast<int32_t>(absDelta - (len << 1));
            if (delta >= 0) {
                accum = Add(accum, 1);
            }
        } else {
            --den;
            accum = static_cast<int32_t>(len - (len << 1));
            if (delta < 0) {
                accum = Add(accum, 1);
            }
        }
        num = Shl(num, 1);
        den = Shl(den, 1);
    }
    bool ShouldStepTexel() const {
        return accum >= 0;
    }
    void StepTexel() {
        value = Add(value, inc);
        accum = Sub(accum, den);
    }
    void StepPixel() {
        accum = Add(accum, num);
    }
};

struct GouraudChannel {
    int32_t num = 0, den = 0, accum = 0, value = 0, intInc = 0, fracInc = 0;

    void Setup(uint32_t len, uint32_t start, uint32_t end) {
        const int32_t delta = static_cast<int32_t>(end) - static_cast<int32_t>(start);
        const uint32_t absDelta = static_cast<uint32_t>(std::abs(delta));
        value = static_cast<int32_t>(start);
        intInc = 0;
        fracInc = delta >= 0 ? +1 : -1;
        num = static_cast<int32_t>(absDelta);
        den = static_cast<int32_t>(len);
        if (len <= absDelta) {
            ++num;
            accum = static_cast<int32_t>(absDelta - (len << 1));
            if (delta >= 0) {
                accum = Add(accum, 1);
            }
        } else {
            --den;
            accum = static_cast<int32_t>(0u - len);
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
    void Step() {
        value = Add(value, intInc);
        accum = Sub(accum, num);
        if (accum < 0) {
            value = Add(value, fracInc);
            accum = Add(accum, den);
        }
    }
};

struct Gouraud {
    std::array<GouraudChannel, 3> ch; // r, g, b

    void Setup(uint32_t len, uint16_t start, uint16_t end) {
        for (uint32_t c = 0; c < 3; ++c) {
            ch[c].Setup(len, (start >> (5 * c)) & 31u, (end >> (5 * c)) & 31u);
        }
    }
    void Step() {
        for (GouraudChannel &c : ch) {
            c.Step();
        }
    }
    uint16_t Value() const {
        return Pack555(ch[0].value, ch[1].value, ch[2].value);
    }
};

// Rect sub-steps: K = clamp(phase((N * P + s) * num / (N * den)), c(P), c(P + 1)), phase floor towards larger
// coordinates and ceil towards smaller ones (vdp1_nx_reference.cpp, PhaseCount)
inline uint32_t PhaseCount(uint32_t num, uint32_t den, uint32_t n, uint32_t P, uint32_t s, bool ceil) {
    if (den == 0) {
        return 0;
    }
    const uint32_t numerator = (n * P + s) * num;
    const uint32_t denominator = n * den;
    return (numerator + (ceil ? denominator - 1 : 0)) / denominator;
}

// The texel count the native stepper reaches at the next pixel
inline uint32_t NextTexelCount(TexStepper t, uint32_t count) {
    t.StepPixel();
    while (t.ShouldStepTexel()) {
        t.StepTexel();
        ++count;
    }
    return count;
}

// Gouraud sub-steps per channel by the same rule, counted in 5-bit steps from the value after Setup (the reference's
// SubGouraud with no skipped pixels)
class SubGouraud {
public:
    SubGouraud(const Gouraud &setup, uint32_t n, bool ceil)
        : m_n(n)
        , m_ceil(ceil)
        , m_cur(setup)
        , m_next(setup) {
        for (uint32_t c = 0; c < 3; ++c) {
            const GouraudChannel &ch = setup.ch[c];
            m_v0[c] = ch.value;
            m_f[c] = ch.fracInc;
            m_rateNum[c] = static_cast<uint32_t>(std::abs(ch.intInc) * ch.den + ch.num);
            m_rateDen[c] = static_cast<uint32_t>(ch.den);
        }
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
    uint16_t Value() const {
        return m_value;
    }

private:
    void Update() {
        int32_t v[3];
        for (uint32_t c = 0; c < 3; ++c) {
            if (m_s == 0 || m_rateDen[c] == 0) {
                v[c] = m_cur.ch[c].value;
                continue;
            }
            const int32_t u0 = (m_cur.ch[c].value - m_v0[c]) * m_f[c];
            const int32_t u1 = std::max(u0, (m_next.ch[c].value - m_v0[c]) * m_f[c]);
            const int32_t fl = static_cast<int32_t>(PhaseCount(m_rateNum[c], m_rateDen[c], m_n, m_P, m_s, m_ceil));
            v[c] = m_v0[c] + m_f[c] * std::clamp(fl, u0, u1);
        }
        m_value = Pack555(v[0], v[1], v[2]);
    }

    uint32_t m_n, m_P = 0, m_s = 0;
    bool m_ceil;
    Gouraud m_cur, m_next;
    int32_t m_v0[3]{}, m_f[3]{};
    uint32_t m_rateNum[3]{}, m_rateDen[3]{};
    uint16_t m_value = 0;
};

// Edge stepper over N x (or native) coordinates in 32 bits (EdgeStepper without the 13-bit counters)
struct Edge {
    Gouraud gouraud;
    bool gouraudEnable = false;
    uint32_t dmaj = 0;
    int32_t x = 0, y = 0, xInc = 0, yInc = 0;
    int32_t xNum = 0, yNum = 0, xDen = 0, yDen = 0, xAccum = 0, yAccum = 0, xAccumTarget = 0, yAccumTarget = 0;
    int32_t num = 0, den = 0, accum = 0, accumTarget = 0;

    void Setup(Point c1, Point c2, uint32_t delta) {
        const int32_t dx = c2.x - c1.x;
        const int32_t dy = c2.y - c1.y;
        const uint32_t adx = static_cast<uint32_t>(std::abs(dx));
        const uint32_t ady = static_cast<uint32_t>(std::abs(dy));
        dmaj = std::max(adx, ady);
        x = c1.x;
        y = c1.y;
        xInc = dx >= 0 ? +1 : -1;
        yInc = dy >= 0 ? +1 : -1;
        xNum = static_cast<int32_t>(adx << 1);
        yNum = static_cast<int32_t>(ady << 1);
        xDen = yDen = static_cast<int32_t>(dmaj << 1);
        xAccum = yAccum = ~static_cast<int32_t>(dmaj);
        xAccumTarget = dy < 0 ? -1 : 0;
        yAccumTarget = dx < 0 ? -1 : 0;
        num = static_cast<int32_t>(dmaj << 1);
        den = static_cast<int32_t>(delta << 1);
        accum = ~static_cast<int32_t>(delta);
        accumTarget = adx >= ady ? yAccumTarget : xAccumTarget;
        gouraudEnable = false;
    }
    void SetupGouraud(uint16_t start, uint16_t end) {
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
    Point Coord() const {
        return {x, y};
    }
};

struct Quad {
    Edge left, right; // A-D, B-C
    uint32_t dmaj = 0, step = 0;

    Quad(Point a, Point b, Point c, Point d) {
        const uint32_t dlx = static_cast<uint32_t>(std::abs(d.x - a.x));
        const uint32_t dly = static_cast<uint32_t>(std::abs(d.y - a.y));
        const uint32_t drx = static_cast<uint32_t>(std::abs(c.x - b.x));
        const uint32_t dry = static_cast<uint32_t>(std::abs(c.y - b.y));
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

// The N x extent of the command's system clip area (native pixel x covers N x columns N * x .. N * x + N - 1)
struct Extent {
    int32_t maxX, maxY;
};

Extent NxExtent(const Vdp1CommandParams &cmd, int32_t n) {
    const int32_t dV = static_cast<int32_t>(Vdp1RasDoubleV(cmd.rasterFlags));
    const int32_t sysH = static_cast<int32_t>(Vdp1Lo16(cmd.sysClip));
    const int32_t sysV = (static_cast<int32_t>(Vdp1Hi16(cmd.sysClip)) << dV) | dV;
    return {n * (sysH + 1) - 1, n * (sysV + 1) - 1};
}

// VDP1IsLineSystemClipped at N x
inline bool LineRejected(const Extent &e, Point a, Point b) {
    return (a.x < 0 && b.x < 0) || (a.y < 0 && b.y < 0) || (a.x > e.maxX && b.x > e.maxX) ||
           (a.y > e.maxY && b.y > e.maxY);
}

// LineStepper's Length() after SystemClip(maxX, maxY) with 32-bit counters. The clip's lengths depend only on the end
// points and the major axis (its minor-axis catch-up moves the start point, not the length), so it is computed
// directly.
uint32_t ClippedLength(Point a, Point b, int32_t maxX, int32_t maxY) {
    constexpr int32_t kPadding = 1;
    const int32_t dx = b.x - a.x, dy = b.y - a.y;
    const int32_t adx = std::abs(dx), ady = std::abs(dy);
    const uint32_t dmaj = static_cast<uint32_t>(std::max(adx, ady));
    if (dmaj == 0u) {
        return 1u;
    }
    const bool xMajor = adx >= ady;
    const int32_t inc = xMajor ? (dx >= 0 ? +1 : -1) : (dy >= 0 ? +1 : -1);
    const int32_t width = maxX + kPadding + 1;
    const int32_t height = maxY + kPadding + 1;
    // The stepper starts one major step before the first pixel
    const int32_t xs = xMajor ? a.x - inc : a.x;
    const int32_t ys = xMajor ? a.y : a.y - inc;
    const int32_t xe = b.x, ye = b.y;
    if (xs >= -kPadding && xs <= width && ys >= -kPadding && ys <= height && xe >= -kPadding && xe <= width &&
        ye >= -kPadding && ye <= height) {
        return dmaj + 1u;
    }
    if ((xs < -kPadding && xe < -kPadding) || (xs > width && xe > width) || (ys < -kPadding && ye < -kPadding) ||
        (ys > height && ye > height)) {
        return 0u;
    }
    const int32_t start = xMajor ? xs : ys;
    const int32_t end = xMajor ? xe : ye;
    const int32_t maxLen = xMajor ? width : height;
    uint32_t len = dmaj;
    uint32_t startClip = 0u;
    if (inc > 0 && start < -kPadding) {
        startClip = static_cast<uint32_t>(-start - 1 - kPadding);
    } else if (inc < 0 && start > maxLen) {
        startClip = static_cast<uint32_t>(start - maxLen);
    }
    if (startClip > 0u) {
        len -= std::min(startClip, len - 1u);
    }
    uint32_t endClip = 0u;
    if (inc < 0 && end < -kPadding) {
        endClip = static_cast<uint32_t>(-end - 1 - kPadding);
    } else if (inc > 0 && end > maxLen) {
        endClip = static_cast<uint32_t>(end - maxLen);
    }
    len -= std::min(endClip, len);
    return len + 1u;
}

// ---------------------------------------------------------------------------------------------------------------------
// Classification (vdp1_nx_reference.cpp, Classify, with the default rules)

enum class Kind : uint8_t { Rect, Quad, Thin, LineParallel, Replicate };

constexpr int32_t kMaxNativeLineMajor = 1364; // longer native lines wrap LineStepper's 13-bit counters (vdp1_clip.hpp)

inline Point Vertex(const Vdp1CommandParams &cmd, uint32_t i) {
    return {static_cast<int32_t>(cmd.vertex[2 * i]), static_cast<int32_t>(cmd.vertex[2 * i + 1])};
}

inline uint16_t GouraudCorner(const Vdp1CommandParams &cmd, uint32_t i) {
    const uint32_t word = cmd.gouraud[i >> 1];
    return static_cast<uint16_t>((i & 1) ? Vdp1Hi16(word) : Vdp1Lo16(word));
}

inline bool IsLineCommand(uint32_t type) {
    return type == 5 || type == 6 || type == 7;
}

inline bool LongLine(Point a, Point b) {
    return std::max(std::abs(b.x - a.x), std::abs(b.y - a.y)) > kMaxNativeLineMajor;
}

// The native QuadStepper's 13-bit counters wrap
bool QuadWraps(const std::array<Point, 4> &v) {
    const int32_t deltas[4] = {v[3].x - v[0].x, v[3].y - v[0].y, v[2].x - v[1].x, v[2].y - v[1].y};
    int32_t dmaj = 0;
    for (const int32_t d : deltas) {
        if (d < -4096 || d > 4095) {
            return true;
        }
        dmaj = std::max(dmaj, std::abs(d));
    }
    return dmaj >= 2048;
}

bool IsRect(const std::array<Point, 4> &v) {
    return v[0].x == v[3].x && v[1].x == v[2].x && v[0].y == v[1].y && v[2].y == v[3].y;
}

bool IsCollinear(const std::array<Point, 4> &v) {
    uint32_t q = 0;
    for (uint32_t i = 1; i < 4; ++i) {
        if (v[i].x != v[0].x || v[i].y != v[0].y) {
            q = i;
            break;
        }
    }
    if (q == 0) {
        return true;
    }
    const int64_t dx = v[q].x - v[0].x, dy = v[q].y - v[0].y;
    for (uint32_t i = 0; i < 4; ++i) {
        const int64_t ex = v[i].x - v[0].x, ey = v[i].y - v[0].y;
        if (dx * ey - dy * ex != 0) {
            return false;
        }
    }
    return true;
}

// Whether any native line of the command can be longer than kMaxNativeLineMajor (polyline segments, or every native
// quad step's line)
bool NativeLinesWrap(uint32_t type, const std::array<Point, 4> &v) {
    if (IsLineCommand(type)) {
        if (type == 6) {
            return LongLine(v[0], v[1]);
        }
        return LongLine(v[0], v[1]) || LongLine(v[1], v[2]) || LongLine(v[2], v[3]) || LongLine(v[3], v[0]);
    }
    // Every quad step's end points lie on the edges, inside the vertices' bounding box
    const auto [minX, maxX] = std::minmax({v[0].x, v[1].x, v[2].x, v[3].x});
    const auto [minY, maxY] = std::minmax({v[0].y, v[1].y, v[2].y, v[3].y});
    if (maxX - minX <= kMaxNativeLineMajor && maxY - minY <= kMaxNativeLineMajor) {
        return false;
    }
    Quad quad{v[0], v[1], v[2], v[3]};
    for (; quad.CanStep(); quad.StepCoordinates()) {
        if (LongLine(quad.left.Coord(), quad.right.Coord())) {
            return true;
        }
    }
    return false;
}

Kind Classify(const Vdp1CommandParams &cmd, const std::array<Point, 4> &v, Vdp1NxCommandClass &cls) {
    const uint32_t type = Vdp1CmdType(cmd.drawFlags);
    if (IsLineCommand(type)) {
        if (NativeLinesWrap(type, v)) {
            cls = Vdp1NxCommandClass::ReplicateWrap;
            return Kind::Replicate;
        }
        cls = Vdp1NxCommandClass::LineParallel;
        return Kind::LineParallel;
    }
    if (QuadWraps(v)) {
        cls = Vdp1NxCommandClass::ReplicateWrap;
        return Kind::Replicate;
    }
    if (IsRect(v)) {
        // the rows are the native lines
        if (std::abs(v[1].x - v[0].x) > kMaxNativeLineMajor) {
            cls = Vdp1NxCommandClass::ReplicateWrap;
            return Kind::Replicate;
        }
        cls = Vdp1NxCommandClass::Rect;
        return Kind::Rect;
    }
    if (NativeLinesWrap(type, v)) {
        cls = Vdp1NxCommandClass::ReplicateWrap;
        return Kind::Replicate;
    }
    if (IsCollinear(v)) {
        cls = Vdp1NxCommandClass::ThinParallel;
        return Kind::Thin;
    }
    cls = Vdp1NxCommandClass::Quad;
    return Kind::Quad;
}

// ---------------------------------------------------------------------------------------------------------------------
// Generation (the span's slot count is kept in slotBase until the sub-batch split)

class Generator {
public:
    Generator(const Vdp1OpList &ops, const Vdp1DrawBatch &batch, uint32_t n, Vdp1NxArena &arena)
        : m_ops(ops)
        , m_batch(batch)
        , m_n(n)
        , m_sn(static_cast<int32_t>(n))
        , m_arena(arena) {}

    const Vdp1CommandParams &Command(uint32_t cmdIndex) const {
        return m_ops.commands[m_batch.cmdOffset + cmdIndex];
    }
    const Vdp1Span &NativeSpan(uint32_t s) const {
        return m_ops.spans[m_batch.spanOffset + s];
    }
    uint32_t NativeSlots(uint32_t s) const {
        const uint32_t end = s + 1 < m_batch.spanCount ? NativeSpan(s + 1).fragBase : m_batch.fragmentCount;
        return end >= NativeSpan(s).fragBase ? end - NativeSpan(s).fragBase : 0; // (a malformed batch: none)
    }

    void Push(uint32_t kind, bool aa, uint32_t cmdIndex, Point a, Point b, uint32_t subRow, int32_t texV,
              uint32_t gouraudLR, uint32_t slots) {
        m_arena.spans.push_back({static_cast<uint32_t>(a.x), static_cast<uint32_t>(a.y), static_cast<uint32_t>(b.x),
                                 static_cast<uint32_t>(b.y), cmdIndex, slots, gouraudLR,
                                 Vdp1MakeNxSpanFlags(kind, aa, subRow, texV)});
    }

    void Replicate(uint32_t cmdIndex, uint32_t s) {
        const Vdp1Span &span = NativeSpan(s);
        Push(kNxSpanReplicate, false, cmdIndex, {static_cast<int32_t>(span.x0), static_cast<int32_t>(span.y0)},
             {static_cast<int32_t>(span.x1), static_cast<int32_t>(span.y1)}, 0,
             static_cast<int32_t>(Vdp1SpanTexV(span.stepTexV)), span.gouraudLR, NativeSlots(s) * m_n * m_n);
    }

    void Rect(uint32_t cmdIndex, uint32_t first, uint32_t end) {
        const Vdp1CommandParams &cmd = Command(cmdIndex);
        const uint32_t f = cmd.drawFlags;
        const int32_t dy = Vertex(cmd, 3).y - Vertex(cmd, 0).y;
        const bool rowsDown = dy >= 0;
        const uint32_t rows = static_cast<uint32_t>(std::abs(dy)) + 1;
        const uint32_t subRows = rows * m_n;
        const bool textured = Vdp1CmdTextured(f);
        const bool gouraud = Vdp1CmdGouraud(f);

        // Per sub-row in stepping order: the texel row and the edge Gouraud colours
        std::vector<int32_t> &texV = m_arena.texV;
        std::vector<uint16_t> &gl = m_arena.gouraudL;
        std::vector<uint16_t> &gr = m_arena.gouraudR;
        if (textured) {
            texV.resize(subRows);
            const uint32_t charSizeH = Vdp1Lo16(cmd.charSize);
            const int32_t charSizeV = charSizeH == 0 ? 1 : static_cast<int32_t>(Vdp1Hi16(cmd.charSize));
            int32_t start = 0, stop = charSizeV - 1;
            if (Vdp1CmdFlipV(f)) {
                std::swap(start, stop);
            }
            TexStepper native;
            native.Setup(rows, start, stop);
            const int32_t v0 = native.value;
            uint32_t count = 0;
            for (uint32_t k = 0; k < rows; ++k) {
                while (native.ShouldStepTexel()) {
                    native.StepTexel();
                    ++count;
                }
                const uint32_t next = NextTexelCount(native, count);
                native.StepPixel();
                texV[k * m_n] = v0 + native.inc * static_cast<int32_t>(count);
                for (uint32_t j = 1; j < m_n; ++j) {
                    const uint32_t K = std::clamp(PhaseCount(static_cast<uint32_t>(native.num),
                                                             static_cast<uint32_t>(native.den), m_n, k, j, !rowsDown),
                                                  count, std::max(count, next));
                    texV[k * m_n + j] = v0 + native.inc * static_cast<int32_t>(K);
                }
            }
        }
        if (gouraud) {
            gl.resize(subRows);
            gr.resize(subRows);
            Gouraud nl, nr;
            nl.Setup(rows, GouraudCorner(cmd, 0), GouraudCorner(cmd, 3));
            nr.Setup(rows, GouraudCorner(cmd, 1), GouraudCorner(cmd, 2));
            SubGouraud sl{nl, m_n, !rowsDown}, sr{nr, m_n, !rowsDown};
            for (uint32_t r = 0; r < subRows; ++r) {
                gl[r] = sl.Value();
                gr[r] = sr.Value();
                sl.Step();
                sr.Step();
            }
        }

        for (uint32_t s = first; s < end; ++s) {
            const Vdp1Span &span = NativeSpan(s);
            const uint32_t quadStep = Vdp1SpanQuadStep(span.stepTexV);
            if (quadStep >= rows || span.y0 != span.y1) {
                // Not one of the command's rows (the front half never makes one): skipped, counted
                ++m_arena.stats.malformedSpans;
                continue;
            }
            const uint32_t r0 = quadStep * m_n;
            const uint32_t slots = (NativeSlots(s) / 2) * m_n;
            const Point a{static_cast<int32_t>(span.x0), static_cast<int32_t>(span.y0)};
            const Point b{static_cast<int32_t>(span.x1), static_cast<int32_t>(span.y1)};
            for (uint32_t j = 0; j < m_n; ++j) {
                // Rows stepping up: sub-row j > 0 lies between rows k - 1 and k, sub-step N - j of row k - 1 (row 0
                // repeats itself there)
                const uint32_t r = rowsDown || j == 0 ? r0 + j : quadStep == 0 ? 0 : r0 - m_n + (m_n - j);
                Push(kNxSpanRect, false, cmdIndex, a, b, j, textured ? texV[r] : 0,
                     gouraud ? Vdp1Pack16(gl[r], gr[r]) : 0u, slots);
            }
        }
    }

    void QuadCommand(uint32_t cmdIndex, Kind kind) {
        const Vdp1CommandParams &cmd = Command(cmdIndex);
        std::array<Point, 4> v, base;
        const int32_t c = (m_sn - 1) / 2; // R1
        for (uint32_t i = 0; i < 4; ++i) {
            v[i] = Vertex(cmd, i);
            base[i] = {m_sn * v[i].x + c, m_sn * v[i].y + c};
        }
        if (kind == Kind::Thin) {
            // T2: N copies offset 0..N-1 on the minor axis of the vertices' bounding box
            int32_t minX = v[0].x, maxX = minX, minY = v[0].y, maxY = minY;
            for (const Point &p : v) {
                minX = std::min(minX, p.x);
                maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
            const bool xMajor = maxX - minX >= maxY - minY;
            for (int32_t o = 0; o < m_sn; ++o) {
                std::array<Point, 4> copy;
                for (uint32_t i = 0; i < 4; ++i) {
                    copy[i] = xMajor ? Point{base[i].x, m_sn * v[i].y + o} : Point{m_sn * v[i].x + o, base[i].y};
                }
                QuadSpans(cmdIndex, copy);
            }
            return;
        }
        QuadSpans(cmdIndex, base);
    }

    // L2: N parallel N x lines per segment, the major axis covering both end pixels' blocks
    void ParallelLines(uint32_t cmdIndex) {
        const Vdp1CommandParams &cmd = Command(cmdIndex);
        const uint32_t segments = Vdp1CmdType(cmd.drawFlags) != 6 ? 4 : 1;
        const bool gouraud = Vdp1CmdGouraud(cmd.drawFlags);
        const Extent e = NxExtent(cmd, m_sn);
        const int32_t n = m_sn;
        for (uint32_t i = 0; i < segments; ++i) {
            const uint32_t j = (i + 1) & 3;
            const Point a = Vertex(cmd, i), b = Vertex(cmd, j);
            const int32_t dx = b.x - a.x, dy = b.y - a.y;
            const bool xMajor = std::abs(dx) >= std::abs(dy);
            const uint32_t gouraudLR = gouraud ? Vdp1Pack16(GouraudCorner(cmd, i), GouraudCorner(cmd, j)) : 0u;
            for (int32_t o = 0; o < n; ++o) {
                Point na, nb;
                if (xMajor) {
                    na = {n * a.x + (dx >= 0 ? 0 : n - 1), n * a.y + o};
                    nb = {n * b.x + (dx >= 0 ? n - 1 : 0), n * b.y + o};
                } else {
                    na = {n * a.x + o, n * a.y + (dy >= 0 ? 0 : n - 1)};
                    nb = {n * b.x + o, n * b.y + (dy >= 0 ? n - 1 : 0)};
                }
                AddQuadSpan(cmdIndex, e, na, nb, false, 0, gouraudLR);
            }
        }
    }

private:
    void QuadSpans(uint32_t cmdIndex, const std::array<Point, 4> &vn) {
        const Vdp1CommandParams &cmd = Command(cmdIndex);
        const uint32_t f = cmd.drawFlags;
        const bool gouraud = Vdp1CmdGouraud(f);
        const bool textured = Vdp1CmdTextured(f);
        const bool aa = Vdp1CmdAntiAlias(f);
        const Extent e = NxExtent(cmd, m_sn);
        Quad quad{vn[0], vn[1], vn[2], vn[3]};
        if (gouraud) {
            quad.left.SetupGouraud(GouraudCorner(cmd, 0), GouraudCorner(cmd, 3));
            quad.right.SetupGouraud(GouraudCorner(cmd, 1), GouraudCorner(cmd, 2));
        }
        TexStepper texV;
        if (textured) {
            const uint32_t charSizeH = Vdp1Lo16(cmd.charSize);
            const int32_t charSizeV = charSizeH == 0 ? 1 : static_cast<int32_t>(Vdp1Hi16(cmd.charSize));
            int32_t start = 0, stop = charSizeV - 1;
            if (Vdp1CmdFlipV(f)) {
                std::swap(start, stop);
            }
            texV.Setup(quad.dmaj + 1, start, stop);
        }

        // VDP1QuadTermination at N x (a box with a margin around the current and the final lines). The edges stay in
        // the vertices' bounding box, so a box inside the clip extent never terminates and needs no final lines.
        const auto [boxMinX, boxMaxX] = std::minmax({vn[0].x, vn[1].x, vn[2].x, vn[3].x});
        const auto [boxMinY, boxMaxY] = std::minmax({vn[0].y, vn[1].y, vn[2].y, vn[3].y});
        const bool terminates = boxMinX < 0 || boxMaxX > e.maxX || boxMinY < 0 || boxMaxY > e.maxY;
        Point finalL = quad.left.Coord(), finalR = quad.right.Coord();
        if (terminates) {
            Quad copy = quad;
            for (copy.StepCoordinates(); copy.CanStep(); copy.StepCoordinates()) {
                finalL = copy.left.Coord();
                finalR = copy.right.Coord();
            }
        }
        for (; quad.CanStep(); quad.Step()) {
            const Point l = quad.left.Coord(), r = quad.right.Coord();
            if (terminates) {
                const int32_t minX = std::min({l.x, r.x, finalL.x, finalR.x});
                const int32_t maxX = std::max({l.x, r.x, finalL.x, finalR.x});
                const int32_t minY = std::min({l.y, r.y, finalL.y, finalR.y});
                const int32_t maxY = std::max({l.y, r.y, finalL.y, finalR.y});
                if (maxX + 1 < 0 || minX - 1 > e.maxX || maxY + 1 < 0 || minY - 1 > e.maxY) {
                    break;
                }
            }
            int32_t v = 0;
            if (textured) {
                while (texV.ShouldStepTexel()) {
                    texV.StepTexel();
                }
                texV.StepPixel();
                v = texV.value;
            }
            const uint32_t gouraudLR =
                gouraud ? Vdp1Pack16(quad.left.gouraud.Value(), quad.right.gouraud.Value()) : 0u;
            AddQuadSpan(cmdIndex, e, l, r, aa, v, gouraudLR);
        }
    }

    void AddQuadSpan(uint32_t cmdIndex, const Extent &e, Point a, Point b, bool aa, int32_t texV, uint32_t gouraudLR) {
        if (LineRejected(e, a, b)) {
            return;
        }
        const uint32_t length = ClippedLength(a, b, e.maxX, e.maxY);
        if (length == 0) {
            return;
        }
        Push(kNxSpanQuad, aa, cmdIndex, a, b, 0, texV, gouraudLR, 2 * length);
    }

    const Vdp1OpList &m_ops;
    const Vdp1DrawBatch &m_batch;
    uint32_t m_n;
    int32_t m_sn;
    Vdp1NxArena &m_arena;
};

// Whether the batch's first command continues the previous batch's last one: the front half split it at a capacity
// (the continuing batch follows the split one directly) and repeated its parameters; the quad step moves on
bool FirstCommandContinues(const Vdp1OpList &ops, uint32_t batchIndex) {
    if (batchIndex == 0) {
        return false;
    }
    const Vdp1DrawBatch &prev = ops.batches[batchIndex - 1];
    const Vdp1DrawBatch &batch = ops.batches[batchIndex];
    if (prev.end != Vdp1BatchEnd::Capacity || prev.cmdCount == 0 || prev.spanCount == 0) {
        return false;
    }
    const Vdp1CommandParams &last = ops.commands[prev.cmdOffset + prev.cmdCount - 1];
    const Vdp1CommandParams &first = ops.commands[batch.cmdOffset];
    return std::memcmp(&first, &last, sizeof(Vdp1CommandParams)) == 0 &&
           Vdp1SpanQuadStep(ops.spans[batch.spanOffset].stepTexV) >
               Vdp1SpanQuadStep(ops.spans[prev.spanOffset + prev.spanCount - 1].stepTexV);
}

} // namespace

std::span<const Vdp1NxSubBatch> GenerateNx(const Vdp1OpList &ops, uint32_t batchIndex, uint32_t scale,
                                           Vdp1NxArena &arena) {
    if (scale < 1 || scale > kMaxNxScale || batchIndex >= ops.batches.size()) {
        return {};
    }
    const Vdp1DrawBatch &batch = ops.batches[batchIndex];
    if (batch.spanCount == 0 || batch.cmdCount == 0 ||
        static_cast<size_t>(batch.spanOffset) + batch.spanCount > ops.spans.size() ||
        static_cast<size_t>(batch.cmdOffset) + batch.cmdCount > ops.commands.size()) {
        return {};
    }
    const auto t0 = std::chrono::steady_clock::now();
    const size_t firstSpan = arena.spans.size();
    const size_t firstSubBatch = arena.subBatches.size();
    Generator gen{ops, batch, scale, arena};
    const bool continues = FirstCommandContinues(ops, batchIndex);

    for (uint32_t first = 0; first < batch.spanCount;) {
        const uint32_t cmdIndex = gen.NativeSpan(first).cmdIndex;
        uint32_t end = first + 1;
        while (end < batch.spanCount && gen.NativeSpan(end).cmdIndex == cmdIndex) {
            ++end;
        }
        if (cmdIndex >= batch.cmdCount) {
            arena.stats.malformedSpans += end - first;
            first = end;
            continue;
        }
        const Vdp1CommandParams &cmd = gen.Command(cmdIndex);
        std::array<Point, 4> v;
        for (uint32_t i = 0; i < 4; ++i) {
            v[i] = Vertex(cmd, i);
        }
        Vdp1NxCommandClass cls{};
        const Kind kind = Classify(cmd, v, cls);
        switch (kind) {
        case Kind::Replicate:
            for (uint32_t s = first; s < end; ++s) {
                gen.Replicate(cmdIndex, s);
            }
            break;
        case Kind::Rect: gen.Rect(cmdIndex, first, end); break;
        case Kind::Quad:
        case Kind::Thin:
        case Kind::LineParallel:
            // generated whole where the command's first span is
            if (cmdIndex == 0 && continues) {
                cls = Vdp1NxCommandClass::Continuation;
            } else if (kind == Kind::LineParallel) {
                gen.ParallelLines(cmdIndex);
            } else {
                gen.QuadCommand(cmdIndex, kind);
            }
            break;
        }
        ++arena.stats.commands[static_cast<size_t>(cls)];
        first = end;
    }

    // Sub-batches: cut between spans at the capacities; slot ranges from 0 in emission order
    const uint32_t maxSlots = std::clamp(arena.limits.slots, 1u, kMaxNxSlotsPerSubBatch);
    const uint32_t maxSpans = std::clamp(arena.limits.spans, 1u, kMaxNxSpansPerSubBatch);
    const uint32_t spanEnd = static_cast<uint32_t>(arena.spans.size());
    std::array<uint64_t, 3> kindSpans{}, kindSlots{};
    uint32_t begin = static_cast<uint32_t>(firstSpan), slots = 0;
    for (uint32_t i = begin; i < spanEnd; ++i) {
        Vdp1NxSpan &span = arena.spans[i];
        const uint32_t s = span.slotBase;
        // A span larger than a whole sub-batch cannot be split (unreachable at the real capacity, Vdp1NxStats)
        assert(s <= kMaxNxSlotsPerSubBatch);
        if (s > maxSlots) {
            ++arena.stats.oversizedSpans;
        }
        if (i > begin && (i - begin >= maxSpans || uint64_t{slots} + s > maxSlots)) {
            arena.subBatches.push_back({begin, i - begin, slots});
            begin = i;
            slots = 0;
        }
        span.slotBase = slots;
        slots += s;
        const uint32_t k = Vdp1NxSpanKind(span.flags);
        ++kindSpans[k];
        kindSlots[k] += s;
    }
    if (begin < spanEnd) {
        arena.subBatches.push_back({begin, spanEnd - begin, slots});
    }

    const uint64_t subBatchCount = arena.subBatches.size() - firstSubBatch;
    const uint64_t nanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    for (uint32_t k = 0; k < 3; ++k) {
        arena.stats.spans[k] += kindSpans[k];
        arena.stats.slots[k] += kindSlots[k];
        BRIMIR_VDP1_PROBE_ADD(nxSpans[k], kindSpans[k]);
    }
    ++arena.stats.batches;
    arena.stats.subBatches += subBatchCount;
    arena.stats.nanoseconds += nanoseconds;
    BRIMIR_VDP1_PROBE_ADD(nxSlots, kindSlots[0] + kindSlots[1] + kindSlots[2]);
    BRIMIR_VDP1_PROBE_ADD(nxSubBatches, subBatchCount);
    BRIMIR_VDP1_PROBE_ADD(nxGenerateNanos, nanoseconds);
    return std::span<const Vdp1NxSubBatch>(arena.subBatches).subspan(firstSubBatch);
}

} // namespace brimir::gpu::vdp1
