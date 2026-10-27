// Brimir: LineStepper::SystemClip termination and the steppers' explicit wrapping (see the "Brimir-side fixes of
// upstream bugs" rows of src/core/BRIMIR_FORK.md dated 2026-10-27).

#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/common/vdp1_steppers.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace brimir_test_old_stepper {

using ymir::vdp::CoordS32;

// The LineStepper of master 1a1a2ea (the same as upstream Ymir 8cdf0a53,
// libs/ymir-core/include/ymir/hw/vdp/renderer/common/vdp1_steppers.hpp lines 234-489), verbatim except for:
// - the name (OldLineStepper);
// - the two minor-step loops of SystemClip stop after kHangCap iterations and set `hung` (a terminating loop takes
//   at most 4096 iterations with 13 bits: the accumulator visits each of its at most 4096 cycle values once).
template <sint32 t_bits = 13>
struct OldLineStepper {
    static constexpr sint32 kCoordMask = std::max(0x7FF, (1 << (t_bits - 2)) - 1);
    static constexpr uint32 kHangCap = 1u << 14; // test-only
    bool hung = false;                           // test-only

    FORCE_INLINE OldLineStepper(CoordS32 coord1, CoordS32 coord2, bool antiAlias = false) {
        auto [x1, y1] = coord1;
        auto [x2, y2] = coord2;

        m_x = x1;
        m_y = y1;
        m_xEnd = x2;
        m_yEnd = y2;
        m_antiAlias = antiAlias;

        sint32 dx = x2 - x1;
        sint32 dy = y2 - y1;
        sint32 adx = abs(dx);
        sint32 ady = abs(dy);
        m_dmaj = std::max(adx, ady);
        m_length = m_dmaj + 1;
        m_step = 0;

        const bool xMajor = adx >= ady;
        if (xMajor) {
            m_xMajInc = dx >= 0 ? +1 : -1;
            m_yMajInc = 0;
            m_xMinInc = 0;
            m_yMinInc = dy >= 0 ? +1 : -1;
        } else {
            m_xMajInc = 0;
            m_yMajInc = dy >= 0 ? +1 : -1;
            m_xMinInc = dx >= 0 ? +1 : -1;
            m_yMinInc = 0;
            std::swap(dx, dy);
            std::swap(adx, ady);
        }
        m_num = ady << 1;
        m_den = adx << 1;
        m_accum = adx + 1;
        m_accumTarget = 0;
        if (!antiAlias && dx < 0) {
            ++m_accumTarget;
        }
        m_accum += m_num;

        m_x -= m_xMajInc;
        m_y -= m_yMajInc;

        if (antiAlias) {
            --m_accum;
            --m_accumTarget;
            const bool samesign = (x1 > x2) == (y1 > y2);
            if (xMajor) {
                m_aaXInc = samesign ? 0 : -m_xMajInc;
                m_aaYInc = samesign ? -m_yMinInc : 0;
            } else {
                m_aaXInc = samesign ? 0 : -m_xMinInc;
                m_aaYInc = samesign ? -m_yMajInc : 0;
            }
        }

        // NOTE: Shifting counters by this amount forces them to have 13 bits without the need for masking
        static constexpr sint32 kShift = 32 - t_bits;

        m_num <<= kShift;
        m_den <<= kShift;
        m_accum <<= kShift;
        m_accumTarget <<= kShift;
    }

    // Clips the slope to the area 0x0..width x height.
    // Returns the number of increments skipped.
    FORCE_INLINE uint32 SystemClip(sint32 width, sint32 height) {
        static constexpr sint32 kPadding = 1;

        // Add padding to compensate for minor inaccuracies
        width += kPadding + 1;
        height += kPadding + 1;

        // Bail out early if the line length is zero
        uint32 length = m_dmaj;
        if (length == 0u) {
            return 0u;
        }

        const sint32 xs = m_x;
        const sint32 ys = m_y;
        const sint32 xe = m_xEnd;
        const sint32 ye = m_yEnd;

        // Bail out early if the line is entirely in-bounds
        if (xs >= -kPadding && xs <= width && ys >= -kPadding && ys <= height && xe >= -kPadding && xe <= width &&
            ye >= -kPadding && ye <= height) {
            return 0u;
        }

        // Fully clip line if it is entirely out of bounds
        if ((xs < -kPadding && xe < -kPadding) || (xs > width && xe > width) || (ys < -kPadding && ye < -kPadding) ||
            (ys > height && ye > height)) {
            m_x = m_xEnd;
            m_y = m_yEnd;
            m_length = 0u;
            return 0u;
        }

        // ---------------------------------------------------------------------

        const bool xmajor = m_xMajInc != 0;
        const sint32 inc = m_xMajInc + m_yMajInc; // only one of the two is non-zero
        const sint32 start = xmajor ? xs : ys;
        const sint32 end = xmajor ? xe : ye;
        const sint32 maxLen = xmajor ? width : height;

        // Clip from the start
        uint32 startClip = 0u;
        if (inc > 0 && start < -kPadding) {
            startClip = -start - 1 - kPadding;
        } else if (inc < 0 && start > maxLen) {
            startClip = start - maxLen;
        }
        if (startClip > 0u) {
            startClip = std::min(startClip, length - 1u);
            m_x += m_xMajInc * startClip;
            m_y += m_yMajInc * startClip;
            for (uint32 i = 0; i < startClip; ++i) {
                m_accum -= m_num;
                if (m_den != 0) {
                    uint32 iterations = 0; // test-only
                    while (m_accum <= m_accumTarget) {
                        if (++iterations > kHangCap) { // test-only
                            hung = true;
                            return 0u;
                        }
                        m_accum += m_den;
                        m_x += m_xMinInc;
                        m_y += m_yMinInc;
                    }
                }
            }
            length -= startClip;
        }

        // ---------------------------------------------------------------------

        // Clip from the end
        uint32 endClip = 0u;
        if (inc < 0 && end < -kPadding) {
            endClip = -end - 1 - kPadding;
        } else if (inc > 0 && end > maxLen) {
            endClip = end - maxLen;
        }
        endClip = std::min(endClip, length);
        if (endClip > 0u) {
            endClip = length - endClip;
            length = endClip;
            m_xEnd = m_x;
            m_yEnd = m_y;
            sint32 tempAccum = m_accum;
            for (uint32 i = 0; i < endClip; ++i) {
                tempAccum -= m_num;
                if (m_den != 0) {
                    uint32 iterations = 0; // test-only
                    while (tempAccum <= m_accumTarget) {
                        if (++iterations > kHangCap) { // test-only
                            hung = true;
                            return 0u;
                        }
                        tempAccum += m_den;
                        m_xEnd += m_xMinInc;
                        m_yEnd += m_yMinInc;
                    }
                }
            }
        }
        m_length = length + 1u;

        return startClip;
    }

    // Determines if the slope can be stepped.
    FORCE_INLINE bool CanStep() const {
        return m_step <= m_length;
    }

    // Steps the slope to the next coordinate.
    // Should not be invoked when CanStep() returns false.
    // Returns true if an antialias pixel should be drawn.
    FORCE_INLINE bool Step() {
        ++m_step;
        m_x += m_xMajInc;
        m_y += m_yMajInc;
        m_accum -= m_num;
        if (m_accum <= m_accumTarget) {
            m_accum += m_den;
            m_x += m_xMinInc;
            m_y += m_yMinInc;
            return m_antiAlias;
        }
        return false;
    }

    // Retrieves the current X coordinate.
    FORCE_INLINE sint32 X() const {
        return m_x & kCoordMask;
    }

    // Retrieves the current Y coordinate.
    FORCE_INLINE sint32 Y() const {
        return m_y & kCoordMask;
    }

    // Retrieves the current X and Y coordinates.
    FORCE_INLINE CoordS32 Coord() const {
        return {m_x, m_y};
    }

    // Returns the X coordinate of the antialiased pixel.
    FORCE_INLINE sint32 AAX() const {
        return m_x + m_aaXInc;
    }

    // Returns the Y coordinate of the antialiased pixel.
    FORCE_INLINE sint32 AAY() const {
        return m_y + m_aaYInc;
    }

    // Returns the X and Y coordinates of the antialiased pixel.
    FORCE_INLINE CoordS32 AACoord() const {
        return {AAX(), AAY()};
    }

    // Retrieves the length of the major axis of the slope, that is, the longest of the vertical and horizontal spans.
    FORCE_INLINE uint32 DMajor() const {
        return m_dmaj;
    }

    // Retrieves the total number of steps in the slope.
    FORCE_INLINE uint32 Length() const {
        return m_length;
    }

private:
    sint32 m_num;
    sint32 m_den;
    sint32 m_accum;
    sint32 m_accumTarget;

    sint32 m_xMajInc;
    sint32 m_yMajInc;
    sint32 m_xMinInc;
    sint32 m_yMinInc;

    sint32 m_x;
    sint32 m_y;
    sint32 m_xEnd;
    sint32 m_yEnd;

    uint32 m_dmaj;
    uint32 m_step;
    uint32 m_length;

    sint32 m_aaXInc;
    sint32 m_aaYInc;
    bool m_antiAlias;
};

} // namespace brimir_test_old_stepper

namespace {

using namespace ymir;
using brimir_test_old_stepper::OldLineStepper;
using vdp::CoordS32;

struct Rng {
    uint64 state;
    uint64 Next() {
        uint64 z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    sint32 Range(sint32 lo, sint32 hi) { // inclusive
        return lo + static_cast<sint32>(Next() % static_cast<uint64>(static_cast<sint64>(hi) - lo + 1));
    }
    bool Chance(uint32 percent) {
        return Next() % 100 < percent;
    }
};

// Whether the hanging-line condition holds: no antialiasing and a major delta of -2048 (mod 4096)
bool InHangClass(CoordS32 a, CoordS32 b, bool antiAlias) {
    const sint32 dx = b.x() - a.x();
    const sint32 dy = b.y() - a.y();
    const sint32 major = std::abs(dx) >= std::abs(dy) ? dx : dy;
    return !antiAlias && major < 0 && -major % 4096 == 2048;
}

// Steps both steppers the way the plotters do and returns true if they visit the same pixels (main and AA)
template <typename A, typename B>
bool SameSteps(A &a, B &b) {
    if (a.Step() != b.Step()) {
        return false;
    }
    bool aaA = false; // the plotters ignore the first step's AA flag
    bool aaB = false;
    while (true) {
        const bool canA = a.CanStep();
        if (canA != b.CanStep()) {
            return false;
        }
        if (!canA) {
            return true;
        }
        if (a.Coord().x() != b.Coord().x() || a.Coord().y() != b.Coord().y() || a.X() != b.X() || a.Y() != b.Y() ||
            aaA != aaB) {
            return false;
        }
        if (aaA && (a.AAX() != b.AAX() || a.AAY() != b.AAY())) {
            return false;
        }
        aaA = a.Step();
        aaB = b.Step();
    }
}

struct Pixel {
    sint32 x, y;
    bool aa;
    sint32 aaX, aaY;
    bool operator==(const Pixel &) const = default;
};

// Every pixel of the line as the plotters step it (no SystemClip)
std::vector<Pixel> UnclippedPixels(CoordS32 a, CoordS32 b, bool antiAlias) {
    vdp::LineStepper line{a, b, antiAlias};
    std::vector<Pixel> pixels;
    bool aa = false;
    for (line.Step(); line.CanStep(); aa = line.Step()) {
        pixels.push_back({line.Coord().x(), line.Coord().y(), aa, aa ? line.AAX() : 0, aa ? line.AAY() : 0});
    }
    return pixels;
}

// Checks SystemClip on a line whose old clip loops never ended against the documented rule: the plotted pixels are
// those of the unclipped line from the first to the last pixel (main or AA) in the padded clip area, or none.
bool MatchesSteppingRule(CoordS32 a, CoordS32 b, bool antiAlias, sint32 w, sint32 h) {
    const std::vector<Pixel> all = UnclippedPixels(a, b, antiAlias);
    auto inside = [&](sint32 x, sint32 y) { return x >= -1 && x <= w + 2 && y >= -1 && y <= h + 2; };
    sint64 first = -1, last = -1;
    for (size_t i = 0; i < all.size(); ++i) {
        if (inside(all[i].x, all[i].y) || (all[i].aa && inside(all[i].aaX, all[i].aaY))) {
            first = first < 0 ? static_cast<sint64>(i) : first;
            last = static_cast<sint64>(i);
        }
    }

    vdp::LineStepper line{a, b, antiAlias};
    const uint32 skipped = line.SystemClip(w, h);
    std::vector<Pixel> got;
    bool aa = false;
    for (line.Step(); line.CanStep(); aa = line.Step()) {
        got.push_back({line.Coord().x(), line.Coord().y(), aa, aa ? line.AAX() : 0, aa ? line.AAY() : 0});
    }
    if (first < 0) {
        return skipped == 0 && line.Length() == 0 && got.empty();
    }
    if (!got.empty()) {
        got.front().aa = all[first].aa; // the first step's AA flag is not plotted; compare what the stepper had
        got.front().aaX = all[first].aaX;
        got.front().aaY = all[first].aaY;
    }
    const std::vector<Pixel> expected(all.begin() + first, all.begin() + last + 1);
    return skipped == first && line.Length() == last - first + 1 && got == expected;
}

sint32 RandomCoord(Rng &rng) {
    const uint32 kind = static_cast<uint32>(rng.Next() % 100);
    if (kind < 70) {
        return rng.Range(-1100, 1500);
    }
    if (kind < 85) {
        return rng.Range(-4096, 4095);
    }
    if (kind < 95) {
        // 13-bit sign-extended coordinate plus a 13-bit local coordinate, as the command handlers produce
        return rng.Range(-4096, 4095) + rng.Range(-4096, 4095);
    }
    return static_cast<sint16>(rng.Next()); // raw 16 bits
}

sint32 RandomClip(Rng &rng) {
    const uint32 kind = static_cast<uint32>(rng.Next() % 100);
    if (kind < 70) {
        return rng.Range(0, 1023);
    }
    if (kind < 80) {
        return rng.Range(0, 15);
    }
    if (kind < 90) {
        return static_cast<uint16>(rng.Next()); // raw 16-bit clip value
    }
    return (static_cast<uint16>(rng.Next()) << 1) | 1; // raw value doubled for double density
}

} // namespace

TEST_CASE("VDP1 line clipping terminates on the reported hanging line", "[vdp][vdp1][steppers][brimir]") {
    // Draw Line (207, 2426)-(1684, 378) with system clip 565 x 383 froze the emulator (y major, |dy| = 2048)
    const CoordS32 a{207, 2426};
    const CoordS32 b{1684, 378};
    OldLineStepper old{a, b, false};
    old.SystemClip(565, 383);
    CHECK(old.hung);

    vdp::LineStepper line{a, b, false};
    line.SystemClip(565, 383);
    CHECK(MatchesSteppingRule(a, b, false, 565, 383));
    // No pixel of this line comes near the clip area, so it is fully clipped
    CHECK(line.Length() == 0);
}

TEST_CASE("VDP1 line clipping terminates on lines with a major delta of 2048 mod 4096", "[vdp][vdp1][steppers][brimir]") {
    Rng rng{0x2048};
    uint64 hung[2][2][2] = {}; // [xMajor][negative][antiAlias]
    uint64 visibleHung = 0;
    uint64 mismatches = 0;
    for (int i = 0; i < 40000; ++i) {
        const bool xMajor = rng.Chance(50);
        const bool negative = rng.Chance(50);
        const bool antiAlias = rng.Chance(50);
        const sint32 major = (2048 + 4096 * rng.Range(0, 7)) * (negative ? -1 : 1);
        const sint32 minor = rng.Range(-std::abs(major) + 1, std::abs(major) - 1);
        // Start near the clip area or anywhere, so the line crosses the area, grazes it or misses it
        const CoordS32 a = rng.Chance(50) ? CoordS32{rng.Range(-300, 900), rng.Range(-300, 800)}
                                          : CoordS32{rng.Range(-20000, 20000), rng.Range(-20000, 20000)};
        const CoordS32 b = xMajor ? CoordS32{a.x() + major, a.y() + minor} : CoordS32{a.x() + minor, a.y() + major};
        const sint32 w = rng.Chance(80) ? rng.Range(0, 704) : RandomClip(rng);
        const sint32 h = rng.Chance(80) ? rng.Range(0, 512) : RandomClip(rng);

        OldLineStepper old{a, b, antiAlias};
        const uint32 oldSkip = old.SystemClip(w, h);
        vdp::LineStepper line{a, b, antiAlias};
        const uint32 newSkip = line.SystemClip(w, h);
        bool ok;
        if (old.hung) {
            ++hung[xMajor][negative][antiAlias];
            ok = MatchesSteppingRule(a, b, antiAlias, w, h);
            visibleHung += line.Length() > 0;
        } else {
            ok = oldSkip == newSkip && old.Length() == line.Length() && SameSteps(old, line);
        }
        if (!ok) {
            CAPTURE(a.x(), a.y(), b.x(), b.y(), antiAlias, w, h, old.hung);
            ++mismatches;
            CHECK(ok);
        }
    }
    CHECK(mismatches == 0);
    CHECK(visibleHung > 0);
    for (int x = 0; x < 2; ++x) {
        for (int n = 0; n < 2; ++n) {
            CAPTURE(x, n);
            // without antialiasing, lines towards negative coordinates hang sometimes on both axes; lines towards
            // positive coordinates and antialiased lines never hung
            CHECK((hung[x][n][0] > 0) == (n == 1));
            CHECK(hung[x][n][1] == 0);
        }
    }
}

TEST_CASE("VDP1 line stepper matches the previous stepper wherever it terminated", "[vdp][vdp1][steppers][brimir]") {
    // A verbatim copy of the old stepper (with an iteration cap to detect its hangs) against the current one over
    // 1,000,000 random lines: same skipped prefix, same length, same pixels (main and AA) on every line where the old
    // one terminates. The lines where it does not are the 2048 (mod 4096) class and follow the stepping rule.
    Rng rng{0x5EED1E};
    uint64 compared = 0, hungLines = 0, mismatches = 0, notHangClass = 0;
    for (int i = 0; i < 1000000; ++i) {
        CoordS32 a{RandomCoord(rng), RandomCoord(rng)};
        CoordS32 b{RandomCoord(rng), RandomCoord(rng)};
        if (rng.Chance(3)) {
            // force a 2048 (mod 4096) major delta to reach the hanging class often enough
            const sint32 major = (2048 + 4096 * rng.Range(0, 3)) * (rng.Chance(50) ? -1 : 1);
            const sint32 minor = rng.Range(-std::abs(major) + 1, std::abs(major) - 1);
            b = rng.Chance(50) ? CoordS32{a.x() + major, a.y() + minor} : CoordS32{a.x() + minor, a.y() + major};
        }
        const bool antiAlias = rng.Chance(50);
        const bool clip = !rng.Chance(10);
        const sint32 w = RandomClip(rng);
        const sint32 h = RandomClip(rng);

        OldLineStepper old{a, b, antiAlias};
        vdp::LineStepper line{a, b, antiAlias};
        uint32 oldSkip = 0, newSkip = 0;
        if (clip) {
            oldSkip = old.SystemClip(w, h);
            newSkip = line.SystemClip(w, h);
        }
        bool ok;
        if (old.hung) {
            ++hungLines;
            if (!InHangClass(a, b, antiAlias)) {
                ++notHangClass;
            }
            ok = MatchesSteppingRule(a, b, antiAlias, w, h);
        } else {
            ++compared;
            ok = old.DMajor() == line.DMajor() && oldSkip == newSkip && old.Length() == line.Length() &&
                 SameSteps(old, line);
        }
        if (!ok) {
            CAPTURE(a.x(), a.y(), b.x(), b.y(), antiAlias, clip, w, h, old.hung);
            ++mismatches;
            CHECK(ok);
        }
    }
    CAPTURE(compared, hungLines);
    CHECK(mismatches == 0);
    CHECK(notHangClass == 0);
    CHECK(hungLines > 0);
    CHECK(compared + hungLines == 1000000);
}

TEST_CASE("VDP1 Draw Line with a hanging line draws the unclipped line's visible pixels", "[vdp][vdp1][steppers][brimir]") {
    // Through the software renderer: the reported line (fully clipped) and a visible line of the hanging class. The
    // framebuffer must hold exactly the pixels of the unclipped line inside the system clip area.
    const sint32 clipW = 319, clipH = 255;
    std::vector<std::pair<CoordS32, CoordS32>> lines{{{207, 2426}, {1684, 378}}};
    for (sint32 x0 = 0; x0 < 400 && lines.size() < 2; ++x0) {
        const CoordS32 a{x0, 2100};
        const CoordS32 b{x0 + 150, 52}; // y major, dy = -2048
        OldLineStepper old{a, b, false};
        old.SystemClip(clipW, clipH);
        vdp::LineStepper line{a, b, false};
        line.SystemClip(clipW, clipH);
        if (old.hung && line.Length() > 0) {
            lines.push_back({a, b});
        }
    }
    REQUIRE(lines.size() == 2);

    for (const auto &[a, b] : lines) {
        CAPTURE(a.x(), a.y(), b.x(), b.y());
        auto state = std::make_unique<vdp::VDPState>();
        vdp::config::VDP2DebugRender debug{};
        vdp::config::VDP2AccessPatternsConfig access{};
        auto renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
        state->regs2.WriteTVMD(0x8000);
        state->regs2.LatchTVMD();
        state->state1.sysClipH = clipW;
        state->state1.sysClipV = clipH;
        const uint16 words[16] = {0x0006, 0, 0x00C0, 0x83E0, 0, 0, static_cast<uint16>(a.x()), static_cast<uint16>(a.y()),
                                  static_cast<uint16>(b.x()), static_cast<uint16>(b.y()), 0, 0, 0, 0, 0, 0};
        for (uint32 i = 0; i < 16; ++i) {
            state->mem1.WriteVRAM<uint16>(0x100 + i * 2, words[i]);
        }
        renderer->PostLoadStateSync();
        renderer->VDP1BeginFrame();
        renderer->VDP1ExecuteCommand(0x100, vdp::VDP1Command::Control{.u16 = 0x0006});
        renderer->VDP1EndFrame();

        std::set<std::pair<sint32, sint32>> expected;
        for (const Pixel &p : UnclippedPixels(a, b, false)) {
            if (p.x >= 0 && p.x <= clipW && p.y >= 0 && p.y <= clipH) {
                expected.insert({p.x, p.y});
            }
        }
        const auto &fb = state->mem1.FBRAM[state->fbIndex.draw];
        uint64 wrong = 0;
        for (sint32 y = 0; y <= clipH; ++y) {
            for (sint32 x = 0; x <= clipW; ++x) {
                const uint16 pixel = util::ReadBE<uint16>(&fb[(y * 512 + x) * 2]);
                wrong += pixel != (expected.contains({x, y}) ? 0x83E0 : 0x0000);
            }
        }
        CHECK(wrong == 0);
        CHECK(expected.empty() == (a.x() == 207));
    }
}
