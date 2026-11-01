// The CPU reference of the VDP2 sub-pixel rules at internal resolution N (vdp2_subpixel_reference.hpp) on its own:
// truncation of negative offsets, sample points equal to the native values, the hi-res and interlace pair rules, and
// monotone, non-trivial sample sequences with negative increments. [vdp2][scale]; builds and runs without Vulkan (the
// coordinate-bitmap scenes in test_gpu_vdp2_scenes.cpp compare the GPU with it).
#include "catch_amalgamated.hpp"

#include "vdp2_subpixel_reference.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <set>

namespace sp = brimir::test::subpixel;

namespace {

// A rotation by `angle` scaled by `scale` (4.10 matrix), screen centre (160, 112), deltaYst = deltaX = 1.
sp::RotParams Rotation(double angle, double scale) {
    sp::RotParams p;
    p.A = std::lround(std::cos(angle) * scale * 1024);
    p.B = std::lround(-std::sin(angle) * scale * 1024);
    p.D = std::lround(std::sin(angle) * scale * 1024);
    p.E = std::lround(std::cos(angle) * scale * 1024);
    p.deltaYst = 1024;
    p.deltaX = 1024;
    p.Px = p.Cx = 160;
    p.Py = p.Cy = 112;
    return p;
}

// The identity matrix with negative Xst and horizontal increments: u follows Xst - 1.7 per line and -2.5 per dot,
// v follows Yst + 0.68 per line.
sp::RotParams NegativeSteps() {
    sp::RotParams p = Rotation(0, 1);
    p.Xst = 300 << 10;
    p.Yst = (40 << 10) + 517;
    p.deltaXst = -1741;
    p.deltaYst = 700;
    p.deltaX = -2560;
    return p;
}

struct Walk {
    bool monotone = true;
    size_t distinct = 0;
    uint32_t subSteps = 0; // changes between two sub-pixels of one native pixel (a nearest upscale has none)
};

// value(k) for k = 0 .. count - 1 (k / n the native pixel): whether it never moves against `sign` (+1 non-decreasing,
// -1 non-increasing), how many distinct values it takes, and how often it changes inside a native pixel.
Walk WalkValues(uint32_t count, uint32_t n, int sign, const std::function<int64_t(uint32_t)> &value) {
    Walk walk;
    std::set<int64_t> values;
    int64_t last = value(0);
    values.insert(last);
    for (uint32_t k = 1; k < count; ++k) {
        const int64_t v = value(k);
        walk.monotone = walk.monotone && (v - last) * sign >= 0;
        walk.subSteps += (v != last && k % n != 0) ? 1 : 0;
        values.insert(v);
        last = v;
    }
    walk.distinct = values.size();
    return walk;
}

} // namespace

TEST_CASE("The sub-pixel reference", "[vdp2][scale]") {
    // Truncation toward zero, as HLSL int division
    CHECK(sp::SubOffset(1, -3, 2) == -1);
    CHECK(sp::SubOffset(1, 3, 2) == 1);
    CHECK(sp::SubOffset(0, -1000, 4) == 0);
    CHECK(sp::SubOffset(3, -1741, 8) == -652);

    // A sample point (i = j = 0) is the native value, wherever the lattice includes it
    sp::RBGFrame rbg;
    rbg.table = Rotation(2.5, 1.2);
    rbg.table.deltaXst = -300;
    sp::NBGFrame nbg;
    nbg.scrollX = 0x12345;
    nbg.scrollY = 0x4580;
    nbg.incH = 0xCD;
    nbg.incV = 0xA0;
    for (const uint32_t n : {2u, 3u, 4u}) {
        CAPTURE(n);
        for (uint32_t y = 0; y < 8; ++y) {
            for (uint32_t x = 0; x < 8; ++x) {
                sp::RBGFrame r1 = rbg, rn = rbg;
                rn.geometry.n = n;
                CHECK(sp::RBGDotAt(r1, x, y).u == sp::RBGDotAt(rn, x * n, y * n).u);
                CHECK(sp::RBGDotAt(r1, x, y).v == sp::RBGDotAt(rn, x * n, y * n).v);
                sp::NBGFrame n1 = nbg, nn = nbg;
                nn.geometry.n = n;
                CHECK(sp::NBGDotAt(n1, x, y).u == sp::NBGDotAt(nn, x * n, y * n).u);
                CHECK(sp::NBGDotAt(n1, x, y).v == sp::NBGDotAt(nn, x * n, y * n).v);
            }
        }
        // Hi-res: the odd column's sample point is half a dot step on (s = N of 2N); interlaced: the odd row's is half
        // a line step on (t = N of 2N), with the line's own (negative) delta, deinterlaced or woven alike.
        sp::RBGFrame hi1 = rbg;
        hi1.geometry = {.n = 1, .hiRes = true};
        CHECK(sp::RBGDotAt(hi1, 1, 0).u == sp::RBGDotAt(hi1, 0, 0).u); // 1x: both columns are the RBG dot
        sp::RBGFrame half = rbg;
        half.table.Xst += sp::SubOffset(n, rbg.table.deltaXst, 2 * n);
        half.table.Yst += sp::SubOffset(n, rbg.table.deltaYst, 2 * n);
        for (const bool deinterlace : {true, false}) {
            CAPTURE(deinterlace);
            sp::RBGFrame de = rbg;
            de.geometry = {.n = n, .interlace = sp::Interlace::DoubleDensity, .deinterlace = deinterlace};
            CHECK(sp::RBGDotAt(de, 0, n).u == sp::RBGDotAt(half, 0, 0).u);
            CHECK(sp::RBGDotAt(de, 0, n).v == sp::RBGDotAt(half, 0, 0).v);
        }
    }
}

TEST_CASE("The sub-pixel reference is monotone with sub-pixel steps", "[vdp2][scale]") {
    // Negative increments: Xst and the horizontal screen increment fall, Yst rises. In every mode the samples down a
    // column and along a row never step back (in particular from row 2l + 1 to row 2l + 2 of an interlaced pair), and
    // they take more distinct values than there are native lines or dots (the sub-pixels move between them).
    for (const uint32_t n : {2u, 3u, 4u}) {
        for (const bool hiRes : {false, true}) {
            for (const sp::Interlace interlace :
                 {sp::Interlace::None, sp::Interlace::SingleDensity, sp::Interlace::DoubleDensity}) {
                for (const bool deinterlace : {false, true}) {
                    CAPTURE(n, hiRes, static_cast<int>(interlace), deinterlace);
                    sp::RBGFrame f;
                    f.table = NegativeSteps();
                    f.geometry = {.n = n, .hiRes = hiRes, .interlace = interlace, .deinterlace = deinterlace};
                    constexpr uint32_t kRows = 32, kColumns = 16;
                    const uint32_t lines = interlace == sp::Interlace::None ? kRows : kRows / 2;
                    const uint32_t dots = hiRes ? kColumns / 2 : kColumns;
                    // u falls 1.7 dots per line and 2.5 per dot: more distinct values than lines or dots
                    const Walk down =
                        WalkValues(kRows * n, n, -1, [&](uint32_t Y) { return sp::RBGDotAt(f, 5, Y).u; });
                    CHECK(down.monotone);
                    CHECK(down.distinct > lines);
                    CHECK(down.subSteps > 0);
                    const Walk downV =
                        WalkValues(kRows * n, n, 1, [&](uint32_t Y) { return sp::RBGDotAt(f, 5, Y).v; });
                    CHECK(downV.monotone);
                    CHECK(downV.subSteps > 0);
                    const Walk along =
                        WalkValues(kColumns * n, n, -1, [&](uint32_t X) { return sp::RBGDotAt(f, X, 3 * n + 1).u; });
                    CHECK(along.monotone);
                    CHECK(along.distinct > dots);
                    CHECK(along.subSteps > 0);
                    // Woven and deinterlaced interlace give every composite pixel the same dot (same bases)
                    if (interlace != sp::Interlace::None) {
                        sp::RBGFrame other = f;
                        other.geometry.deinterlace = !deinterlace;
                        for (uint32_t Y = 0; Y < kRows * n; ++Y) {
                            CHECK(sp::RBGDotAt(f, 7, Y).u == sp::RBGDotAt(other, 7, Y).u);
                            CHECK(sp::RBGDotAt(f, 7, Y).v == sp::RBGDotAt(other, 7, Y).v);
                        }
                    }
                }
            }
        }

        // NBGs: zoomed, fractional scroll; single density's pair rule continues row 2l into 2l + 1 and on into the
        // next line; double density's rows are one increment apart.
        for (const sp::Interlace interlace :
             {sp::Interlace::None, sp::Interlace::SingleDensity, sp::Interlace::DoubleDensity}) {
            CAPTURE(n, static_cast<int>(interlace));
            sp::NBGFrame f;
            f.geometry = {.n = n, .interlace = interlace};
            f.scrollX = 0x12345;
            f.scrollY = 0x4580;
            f.incH = 0x60;
            f.incV = 0x70;
            const Walk down = WalkValues(32 * n, n, 1, [&](uint32_t Y) { return sp::NBGDotAt(f, 3, Y).v; });
            CHECK(down.monotone);
            CHECK(down.distinct >= (interlace == sp::Interlace::SingleDensity ? 7u : 14u)); // 0x70 per line
            CHECK(down.subSteps > 0);
            const Walk along = WalkValues(32 * n, n, 1, [&](uint32_t X) { return sp::NBGDotAt(f, X, n + 1).u; });
            CHECK(along.monotone);
            CHECK(along.distinct >= 12); // 0x60 per dot
            CHECK(along.subSteps > 0);
        }
    }
}

TEST_CASE("The sub-pixel reference interpolates scale coefficients between lines", "[vdp2][scale]") {
    // The guard: same sign, the difference at most half the larger magnitude (2:1 passes, just over fails)
    CHECK(sp::CoefficientsInterpolable(0x10000, 0x20000));
    CHECK(sp::CoefficientsInterpolable(0x20000, 0x10000));
    CHECK_FALSE(sp::CoefficientsInterpolable(0x10000, 0x20001));
    CHECK(sp::CoefficientsInterpolable(-0x10000, -0x14000));
    CHECK_FALSE(sp::CoefficientsInterpolable(0x8000, -0x8000));
    CHECK_FALSE(sp::CoefficientsInterpolable(0, 0x100));
    CHECK(sp::CoefficientsInterpolable(0, 0));

    // The identity rotation, per-line coefficients (one entry per line): line l's coefficient is 2 + l / 4 (8.16),
    // entry 3 is transparent, entry 6 jumps (fails the guard), and u = (k scrX) >> 16 with scrX = 64 dots.
    sp::RBGFrame f;
    f.table = sp::RotParams{};
    f.table.A = f.table.E = 1024;
    f.table.Xst = 64 << 10;
    f.table.deltaX = 1024;
    f.table.deltaYst = 1024;
    f.table.dKAst = 1024;
    f.coefficient = [](uint32_t e) {
        sp::Coefficient c;
        c.value = e == 6 ? 0x80000 : 0x20000 + e * 0x4000;
        c.transparent = e == 3;
        return c;
    };
    const auto u = [&](uint32_t n, uint32_t Y) {
        f.geometry.n = n;
        return sp::RBGDotAt(f, 0, Y);
    };
    // 1x and the sample points: the native coefficient
    CHECK(u(1, 0).u == 128);
    CHECK(u(1, 1).u == 144);
    CHECK(u(4, 4).u == 144);
    // Sub-lines between line 0 (2.0) and line 1 (2.25): 2.0625, 2.125, 2.1875
    CHECK(u(4, 1).u == 132);
    CHECK(u(4, 2).u == 136);
    CHECK(u(4, 3).u == 140);
    CHECK(u(3, 1).u == ((64 * (0x20000 + 0x4000 / 3)) >> 16));
    // Next entry transparent: native; own entry transparent: transparent
    CHECK(u(4, 2 * 4 + 2).u == u(1, 2).u);
    CHECK(u(4, 3 * 4 + 1).transparent);
    CHECK(u(1, 3).transparent);
    // The guard: line 5 -> 6 jumps, so line 5's sub-lines stay native; line 6 -> 7 (8.0 -> 2.0 + 7/4) fails too
    CHECK(u(4, 5 * 4 + 3).u == u(1, 5).u);
    CHECK(u(4, 6 * 4 + 1).u == u(1, 6).u);
    // Viewpoint X: never interpolated
    f.coeffMode = sp::CoeffMode::ViewpointX;
    f.coefficient = [](uint32_t e) { return sp::Coefficient{.value = int64_t{e} * 40 << 8}; };
    f.geometry.n = 4;
    CHECK(sp::RBGDotAt(f, 0, 2).u == sp::RBGDotAt(f, 0, 0).u);
    // A KAst reload at line 10: line 9's sub-lines use line 9's own dKAst, not the reloaded address
    f.coeffMode = sp::CoeffMode::ScaleXY;
    f.coefficient = [](uint32_t e) { return sp::Coefficient{.value = 0x10000 + int64_t{e} * 0x800}; };
    f.reloadLine = 10;
    f.reloaded = f.table;
    f.reloaded.KAst = 100 << 10;
    f.reloadKAst = true;
    f.geometry.n = 2;
    CHECK(sp::RBGDotAt(f, 0, 9 * 2 + 1).u == (64 * (0x10000 + 9 * 0x800 + 0x400)) >> 16);
    CHECK(sp::RBGDotAt(f, 0, 10 * 2).u == (64 * (0x10000 + 100 * 0x800)) >> 16);
}
