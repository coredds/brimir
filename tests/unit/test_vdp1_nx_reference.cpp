// The CPU reference of the N x VDP1 rasteriser (vdp1_nx_reference.hpp), fed with the real front half's op lists of
// the stage 2 scenarios (vdp1_scenario.hpp: seeded random tables and targeted scenes): at N = 1 it is the native
// reference, rect-path and replicated content is native at every sample point, native FBRAM is untouched, overlapping
// stacks resolve in VDP1 order and the sub-batch split changes nothing.

#include "catch_amalgamated.hpp"

#include "vdp1_nx_reference.hpp"
#include "vdp1_scenario.hpp"

#include <gpu/vdp1/vdp1_op_dump.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace brimir::test::vdp1scene;
namespace nx = brimir::test::vdp1nx;

struct NxRun {
    ReferenceRun native;                    // the stage 2 reference (ReferenceVDP1)
    std::unique_ptr<ref::Fbram> nxNative;   // native FBRAM as the N x reference left it
    std::unique_ptr<nx::NxFbram> surface;
    nx::Stats stats;
};

// The scenario through the front half into the stage 2 reference, and its op lists into the N x reference
NxRun RunNx(const Scenario &s, const nx::Options &o) {
    std::vector<gpu::Vdp1OpList> lists;
    NxRun run;
    run.native = RunReference(s, {}, 0, [&](const gpu::Vdp1OpList &ops) { lists.push_back(ops); });
    run.nxNative = std::make_unique<ref::Fbram>();
    FillMain(s, run.nxNative->main);
    run.surface = std::make_unique<nx::NxFbram>(o.scale);
    run.surface->ReplicateFrom(*run.nxNative);
    nx::ReferenceNx reference{*run.nxNative, *run.surface, o};
    for (const auto &l : lists) {
        reference.Replay(l);
    }
    run.stats = reference.GetStats();
    return run;
}

nx::Rules RulesOf(uint64 i) {
    static constexpr nx::VertexRule kVertex[] = {nx::VertexRule::FloorHalfDown, nx::VertexRule::FloorHalf,
                                                 nx::VertexRule::CentroidSign};
    return {kVertex[i % 3], (i / 3) % 2 ? nx::ThinRule::Parallel : nx::ThinRule::Hairline,
            (i / 6) % 2 ? nx::LineRule::Parallel : nx::LineRule::NativeThick};
}

uint32 Workers() {
    return std::clamp<uint32>(std::thread::hardware_concurrency() / 2, 1, 8);
}

struct SeedOutcome {
    std::string error; // a failure (empty: passed)
    bool skipped = false;
    nx::Stats stats;
};

// Runs fn(seed) for seeds [first, first + count) on worker threads; Catch assertions stay on this thread
template <typename Fn>
std::vector<SeedOutcome> ForSeeds(uint64 first, uint64 count, Fn &&fn) {
    std::vector<SeedOutcome> out(count);
    std::atomic<uint64> next{0};
    std::vector<std::thread> threads;
    for (uint32 w = 0; w < Workers(); ++w) {
        threads.emplace_back([&] {
            for (uint64 i = next++; i < count; i = next++) {
                try {
                    out[i] = fn(first + i);
                } catch (const std::exception &e) {
                    out[i].error = std::string("exception: ") + e.what();
                }
            }
        });
    }
    for (auto &t : threads) {
        t.join();
    }
    return out;
}

struct Summary {
    uint64 failures = 0, skipped = 0, passed = 0;
    nx::CommandCounts commands;
    uint64 fragments = 0, subBatches = 0, batches = 0;
};

Summary Summarise(uint64 first, const std::vector<SeedOutcome> &outcomes) {
    Summary s;
    uint32 reported = 0;
    for (size_t i = 0; i < outcomes.size(); ++i) {
        const SeedOutcome &o = outcomes[i];
        if (!o.error.empty()) {
            ++s.failures;
            if (reported++ < 5) {
                UNSCOPED_INFO("seed " << first + i << ": " << o.error);
            }
            continue;
        }
        if (o.skipped) {
            ++s.skipped;
            continue;
        }
        ++s.passed;
        for (uint32 c = 0; c < nx::CommandCounts::Count; ++c) {
            s.commands.counts[c] += o.stats.commands.counts[c];
        }
        s.fragments += o.stats.Fragments();
        s.subBatches += o.stats.subBatches;
        s.batches += o.stats.batches;
    }
    return s;
}

void InfoCommands(const Summary &s) {
    for (uint32 c = 0; c < nx::CommandCounts::Count; ++c) {
        UNSCOPED_INFO("commands " << nx::CommandCounts::Name(c) << ": " << s.commands.counts[c]);
    }
}

// The rect + replicate subset (no quad path): every sample point is native, and native FBRAM is the stage 2 one
SeedOutcome SamplePointSeed(uint64 seed) {
    Coverage cov;
    const Scenario s = RandomScenario(seed, cov);
    nx::Options o;
    static constexpr uint32 kScales[6] = {2, 2, 2, 3, 3, 4}; // the 4x surface costs most
    o.scale = kScales[seed % 6];
    o.quadPath = false;
    o.shuffleSeed = seed % 4 == 0 ? seed * 2654435761ull + 1 : 0;
    const NxRun r = RunNx(s, o);
    SeedOutcome out;
    out.stats = r.stats;
    out.error = CompareFbram(*r.native.fb, *r.nxNative, "stage 2 reference", "N x reference native");
    if (out.error.empty()) {
        out.error = nx::CompareSamplePoints(*r.nxNative, *r.surface);
    }
    if (!out.error.empty()) {
        out.error = "N=" + std::to_string(o.scale) + ": " + out.error;
    }
    return out;
}

void CheckSamplePointRun(uint64 first, uint64 count) {
    const auto outcomes = ForSeeds(first, count, SamplePointSeed);
    const Summary s = Summarise(first, outcomes);
    InfoCommands(s);
    CHECK(s.failures == 0);
    CHECK(s.passed == count);
    CHECK(s.commands.counts[nx::CommandCounts::Rect] > 0);
    CHECK(s.commands.counts[nx::CommandCounts::ReplicateNoQuadPath] > 0);
    CHECK(s.commands.counts[nx::CommandCounts::ReplicateLine] > 0);
    CHECK(s.commands.counts[nx::CommandCounts::ReplicateWrap] > 0);
}

// N = 1 with the quad path: the native reference exactly (commands whose native lines wrap their 13-bit counters use
// the replicate kind)
SeedOutcome UnitScaleSeed(uint64 seed) {
    Coverage cov;
    const Scenario s = RandomScenario(seed, cov);
    nx::Options o;
    o.scale = 1;
    o.rules = RulesOf(seed);
    const NxRun r = RunNx(s, o);
    SeedOutcome out;
    out.stats = r.stats;
    out.error = CompareFbram(*r.native.fb, *r.nxNative, "stage 2 reference", "N x reference native");
    if (out.error.empty()) {
        out.error = nx::CompareSamplePoints(*r.nxNative, *r.surface);
    }
    return out;
}

} // namespace

TEST_CASE("VDP1 N x reference at N = 1 is the native reference with the quad path", "[vdp1][nx][reference]") {
    const auto outcomes = ForSeeds(1, 2'000, UnitScaleSeed);
    const Summary s = Summarise(1, outcomes);
    InfoCommands(s);
    CHECK(s.failures == 0);
    CHECK(s.passed == 2'000);
    CHECK(s.commands.counts[nx::CommandCounts::ReplicateWrap] > 0);
    CHECK(s.commands.counts[nx::CommandCounts::Continuation] > 0);
    CHECK(s.commands.counts[nx::CommandCounts::Quad] > 0);
    CHECK(s.commands.counts[nx::CommandCounts::Thin] + s.commands.counts[nx::CommandCounts::ThinParallel] > 0);
    CHECK(s.commands.counts[nx::CommandCounts::LineParallel] > 0);
    CHECK(s.commands.counts[nx::CommandCounts::Rect] > 0);
}

TEST_CASE("VDP1 N x reference at N = 1 is the native reference (long)", "[vdp1][nx][reference][.]") {
    const auto outcomes = ForSeeds(100'001, 50'000, UnitScaleSeed);
    const Summary s = Summarise(100'001, outcomes);
    InfoCommands(s);
    CHECK(s.failures == 0);
    CHECK(s.passed == 50'000);
}

TEST_CASE("VDP1 N x reference: rect-path and replicated content is native at every sample point",
          "[vdp1][nx][reference]") {
    // 2,000 seeded tables (200,000 in the long variant; every command type and colour mode, Gouraud, mesh, MSB-on, 8 bpp, DIE, end codes, HSS,
    // clipping, 13-bit wraps, erases, CPU writes, swaps, batch splits) at N = 2, 3 and 4, without the quad path
    CheckSamplePointRun(1, 2'000);
}

TEST_CASE("VDP1 N x reference: rect-path sample points (long)", "[vdp1][nx][reference][.]") {
    CheckSamplePointRun(1'000'001, 200'000);
}

TEST_CASE("VDP1 N x reference leaves native FBRAM untouched", "[vdp1][nx][reference]") {
    // The quad path on, every rule combination, N = 2 to 4: native FBRAM is the stage 2 reference's and software's
    const auto outcomes = ForSeeds(20'001, 300, [](uint64 seed) {
        Coverage cov;
        const Scenario s = RandomScenario(seed, cov);
        nx::Options o;
        o.scale = 2 + static_cast<uint32>(seed % 3);
        o.rules = RulesOf(seed / 3);
        const NxRun r = RunNx(s, o);
        SeedOutcome out;
        out.stats = r.stats;
        out.error = CompareFbram(*r.native.fb, *r.nxNative, "stage 2 reference", "N x reference native");
        if (out.error.empty() && seed % 10 == 0) {
            out.error = CompareFbram(*RunSoftware(s), *r.nxNative);
        }
        return out;
    });
    const Summary s = Summarise(20'001, outcomes);
    InfoCommands(s);
    CHECK(s.failures == 0);
    CHECK(s.fragments > 0);
    CHECK(s.commands.counts[nx::CommandCounts::Quad] > 0);
}

namespace {

// Overlapping half-transparent, shadow, half-luminance and Gouraud shapes in one batch (as the stage 2 chain scenes);
// rectOnly: normal sprites and regular-rect polygons only
Scenario StackScene(uint64 seed, bool rectOnly) {
    TestRng rng{seed * 977};
    Scenario s;
    s.transparentMeshes = rng.Chance(50);
    s.tvmr = rng.Chance(25) ? 1 : 0;
    s.fbFill.resize(1000);
    for (auto &b : s.fbFill) {
        b = static_cast<uint8>(rng.Next() | 0x80);
    }
    const uint16 srca = s.AddTexture(RGBTexture(32, 32, seed));
    s.Op0(Op::Kind::BeginFrame);
    for (int n = 0; n < 40; ++n) {
        static constexpr uint16 kCalc[] = {3, 3, 3, 1, 2, 0, 7, 7};
        uint16 pmod = static_cast<uint16>(0x00C0 | rng.Pick(kCalc));
        if (rng.Chance(20)) {
            pmod |= 0x0100; // mesh
        }
        const uint16 grda = s.AddGouraud({static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next()),
                                          static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next())});
        const CoordS32 c{rng.Range(60, 260), rng.Range(40, 180)};
        const sint32 r = rng.Range(10, 60);
        if (rectOnly) {
            const CoordS32 a{c.x() - r, c.y() - rng.Range(5, r)}, cc{c.x() + rng.Range(5, r), c.y() + r};
            if (rng.Chance(40)) {
                // normal sprite (32 x 32 texture), flips from the control word
                s.AddCommand({static_cast<uint16>(rng.Next() & 0x0030), 0, static_cast<uint16>(pmod | 0x0028), 0, srca,
                              0x0420, U(a.x()), U(a.y()), 0, 0, 0, 0, 0, 0, grda, 0});
            } else {
                s.Polygon(pmod, static_cast<uint16>(rng.Next() | 0x8000), a, {cc.x(), a.y()}, cc, {a.x(), cc.y()},
                          grda);
            }
            continue;
        }
        const CoordS32 a{c.x() - r, c.y() - rng.Range(5, r)}, b{c.x() + rng.Range(5, r), c.y() - r};
        const CoordS32 cc{c.x() + r, c.y() + rng.Range(5, r)}, d{c.x() - rng.Range(5, r), c.y() + r};
        if (rng.Chance(30)) {
            s.Distorted(0, static_cast<uint16>(pmod | 0x0028), 0, srca, 0x0420, a, b, cc, d, grda);
        } else {
            s.Polygon(pmod, static_cast<uint16>(rng.Next() | 0x8000), a, b, cc, d, grda);
        }
    }
    s.Op0(Op::Kind::EndFrame);
    return s;
}

} // namespace

TEST_CASE("VDP1 N x reference: overlapping half-transparent and shadow stacks resolve in VDP1 order",
          "[vdp1][nx][reference]") {
    uint64 orderSensitive = 0, halfTransparent = 0;
    for (uint64 seed = 1; seed <= 6; ++seed) {
        for (const uint32 n : {2u, 4u}) {
            CAPTURE(seed, n);
            nx::Options o;
            o.scale = n;
            o.rules = RulesOf(seed);
            // Rect-only stacks: native at every sample point (the stacking order is native's)
            const Scenario rects = StackScene(seed, true);
            const NxRun rr = RunNx(rects, o);
            CHECK(nx::CompareSamplePoints(*rr.nxNative, *rr.surface) == "");
            CHECK(CompareFbram(*rr.native.fb, *rr.nxNative) == "");

            // General stacks (quad path, within-quad overdraw): resolving by key equals applying the fragments in
            // the order the rasteriser emits them (VDP1's drawing order at N x) and resolving them from a shuffled
            // (scatter) order; resolving by descending key differs
            const Scenario quads = StackScene(seed, false);
            const NxRun keyed = RunNx(quads, o);
            nx::Options emission = o;
            emission.resolveInEmissionOrder = true;
            nx::Options shuffled = o;
            shuffled.shuffleSeed = seed * 31 + n;
            nx::Options descending = o;
            descending.resolveDescendingForTesting = true;
            CHECK(nx::CompareNx(*keyed.surface, *RunNx(quads, emission).surface) == "");
            CHECK(nx::CompareNx(*keyed.surface, *RunNx(quads, shuffled).surface) == "");
            const bool differs = nx::CompareNx(*keyed.surface, *RunNx(quads, descending).surface) != "";
            CHECK(differs);
            orderSensitive += differs;
            CHECK(keyed.stats.maxPerUnit > 1);
            halfTransparent += keyed.stats.halfTransparent;
        }
    }
    CHECK(orderSensitive == 12);
    CHECK(halfTransparent > 0); // the counter the capture scan relies on
}

TEST_CASE("VDP1 N x reference: a sub-batch split at a small capacity gives the same surface", "[vdp1][nx][reference]") {
    const auto outcomes = ForSeeds(30'001, 150, [](uint64 seed) {
        Coverage cov;
        const Scenario s = RandomScenario(seed, cov);
        nx::Options o;
        o.scale = 2 + static_cast<uint32>(seed % 2);
        o.rules = RulesOf(seed / 2);
        nx::Options small = o;
        small.subBatchSlots = 97 + static_cast<uint32>(seed % 500);
        small.subBatchSpans = 3 + static_cast<uint32>(seed % 7);
        const NxRun a = RunNx(s, o);
        const NxRun b = RunNx(s, small);
        SeedOutcome out;
        out.stats = b.stats;
        out.error = nx::CompareNx(*a.surface, *b.surface);
        if (out.error.empty() && b.stats.Spans() != a.stats.Spans()) {
            out.error = "span counts differ";
        }
        return out;
    });
    const Summary s = Summarise(30'001, outcomes);
    CAPTURE(s.batches, s.subBatches);
    CHECK(s.failures == 0);
    CHECK(s.subBatches > 4 * s.batches);
}

TEST_CASE("VDP1 N x reference: 1:1 normal sprites are the nearest upscale", "[vdp1][nx][reference]") {
    // Normal sprites (flips, colour modes 1 and 5, transparency, end codes, half-transparency and shadow overlaps,
    // mesh, clipping at the edges) without Gouraud: every N x N block is its native word
    for (uint64 seed = 1; seed <= 20; ++seed) {
        TestRng rng{seed * 7919};
        Scenario s;
        s.transparentMeshes = rng.Chance(50);
        s.clip.sysClipH = 300;
        s.clip.sysClipV = 200;
        const uint16 rgb = s.AddTexture(RGBTexture(24, 16, seed));
        std::vector<uint8> pal(16 * 12);
        for (auto &b : pal) {
            b = static_cast<uint8>(rng.Next());
        }
        pal[5] = 0xFF; // end codes
        pal[40] = 0xF0;
        const uint16 lut = s.AddTexture(std::vector<uint8>(32, 0x55));
        const uint16 idx = s.AddTexture(pal);
        s.Op0(Op::Kind::BeginFrame);
        for (int k = 0; k < 50; ++k) {
            static constexpr uint16 kCalc[] = {0, 0, 3, 3, 1, 2};
            uint16 pmod = rng.Pick(kCalc); // end codes enabled
            if (rng.Chance(15)) {
                pmod |= 0x0100; // mesh
            }
            if (rng.Chance(20)) {
                pmod |= 0x0040; // transparent pixels drawn
            }
            const uint16 ctrl = static_cast<uint16>(rng.Next() & 0x0030); // flips
            const sint16 x = static_cast<sint16>(rng.Range(-20, 300)), y = static_cast<sint16>(rng.Range(-20, 200));
            if (rng.Chance(50)) {
                s.AddCommand({ctrl, 0, static_cast<uint16>(pmod | 0x0028), 0, rgb, 0x0310, U(x), U(y), 0, 0, 0, 0, 0, 0,
                              0, 0});
            } else {
                // colour mode 1 (look-up table), 16 x 12 4-bpp texels
                s.AddCommand({ctrl, 0, static_cast<uint16>(pmod | 0x0008), static_cast<uint16>(lut), idx, 0x020C,
                              U(x), U(y), 0, 0, 0, 0, 0, 0, 0, 0});
            }
        }
        s.Op0(Op::Kind::EndFrame);
        for (const uint32 n : {2u, 3u, 4u}) {
            CAPTURE(seed, n);
            nx::Options o;
            o.scale = n;
            const NxRun r = RunNx(s, o);
            REQUIRE(r.stats.commands.counts[nx::CommandCounts::Rect] > 0);
            uint64 differing = 0;
            for (uint32 slice = 0; slice < 8; ++slice) {
                for (uint32 w = 0; w < ref::kWordsPerFB; ++w) {
                    const uint16 native = nx::NativeWord(*r.nxNative, slice, w);
                    for (uint32 sub = 0; sub < n * n; ++sub) {
                        differing += r.surface->slices[slice][size_t{w} * n * n + sub] != native;
                    }
                }
            }
            CHECK(differing == 0);
        }
    }
}

TEST_CASE("VDP1 N x reference: a quad split over batches at the front half's capacity is generated once",
          "[vdp1][nx][reference]") {
    // Large quads, distorted sprites and polylines with a small fragment capacity: the front half splits the commands
    // over several batches (repeating the command). The N x surface equals the unsplit scene's, and at N = 1 native.
    Scenario s;
    const uint16 tex = s.AddTexture(RGBTexture(32, 32, 3));
    s.Op0(Op::Kind::BeginFrame);
    s.Polygon(0x00C3, 0xFC1F, {10, 20}, {300, 5}, {280, 210}, {30, 190});
    s.Distorted(0, 0x00C7, 0, tex, 0x0420, {40, 30}, {250, 60}, {230, 200}, {20, 170},
                s.AddGouraud({0x7FFF, 0x0000, 0x4210, 0x7C00}));
    s.AddCommand({0x0005, 0, 0x00C3, 0x83FF, 0, 0, U(5), U(5), U(310), U(40), U(200), U(220), U(15), U(150), 0, 0});
    s.Op0(Op::Kind::EndFrame);
    Scenario split = s;
    split.limits.fragments = 1'500;
    for (const uint32 n : {1u, 2u, 4u}) {
        CAPTURE(n);
        nx::Options o;
        o.scale = n;
        const NxRun whole = RunNx(s, o);
        const NxRun parts = RunNx(split, o);
        CHECK(parts.stats.batches > 5);
        CHECK(parts.stats.commands.counts[nx::CommandCounts::Continuation] > 0);
        CHECK(whole.stats.commands.counts[nx::CommandCounts::Continuation] == 0);
        CHECK(nx::CompareNx(*whole.surface, *parts.surface) == "");
        CHECK(CompareFbram(*parts.native.fb, *parts.nxNative) == "");
        if (n == 1) {
            CHECK(nx::CompareSamplePoints(*parts.nxNative, *parts.surface) == "");
        }
    }
}

TEST_CASE("VDP1 N x reference: 1:1 Gouraud sprites with one step per pixel are the nearest upscale",
          "[vdp1][nx][reference]") {
    // Gouraud along the rows and down the edges at one 5-bit step per pixel (and constant): the floor-phase sub-steps
    // keep each N x N block constant; the research's "native DDA + sub-step" would move every step into the block
    for (const uint32 n : {2u, 3u, 4u}) {
        Scenario s;
        const uint16 tex = s.AddTexture(RGBTexture(32, 32, 11));
        s.Op0(Op::Kind::BeginFrame);
        const uint16 g1 = s.AddGouraud({0x0000, 0x001F, 0x03FF, 0x03E0}); // r 0->31 along rows, g 0->31 down
        const uint16 g2 = s.AddGouraud({0x7FFF, 0x7FE0, 0x7C00, 0x7C1F}); // decreasing
        const uint16 g3 = s.AddGouraud({0x4210, 0x4210, 0x4210, 0x4210}); // constant
        uint16 x = 10;
        for (const uint16 g : {g1, g2, g3}) {
            for (const uint16 ctrl : {uint16{0x0000}, uint16{0x0030}}) { // flips
                s.AddCommand({ctrl, 0, 0x00EC, 0, tex, 0x0420, x, 20, 0, 0, 0, 0, 0, 0, g, 0});
                x = static_cast<uint16>(x + 40);
            }
        }
        // the same as a regular-rect polygon drawn right to left and bottom to top
        s.Polygon(0x00C4, 0x4210, {300, 140}, {269, 140}, {269, 109}, {300, 109}, g1);
        s.Distorted(0, 0x00EC, 0, tex, 0x0420, {300, 100}, {269, 100}, {269, 69}, {300, 69}, g2); // textured, reversed
        s.Op0(Op::Kind::EndFrame);
        CAPTURE(n);
        nx::Options o;
        o.scale = n;
        const NxRun r = RunNx(s, o);
        uint64 differing = 0;
        uint32 first = ~0u;
        for (uint32 w = 0; w < ref::kWordsPerFB; ++w) {
            const uint16 native = nx::NativeWord(*r.nxNative, 0, w);
            for (uint32 sub = 0; sub < n * n; ++sub) {
                const bool d = r.surface->slices[0][size_t{w} * n * n + sub] != native;
                differing += d;
                first = d && first == ~0u ? w * 16 + sub : first;
            }
        }
        CAPTURE(first / 16 % 512, first / 16 / 512, first % 16);
        CHECK(r.stats.commands.counts[nx::CommandCounts::Rect] == 8);
        CHECK(differing == 0);
    }
}

TEST_CASE("VDP1 N x reference: parallel lines keep native thickness", "[vdp1][nx][reference]") {
    // Horizontal and vertical lines and polylines with LineRule::Parallel cover whole N x N blocks: every N x word of a
    // native pixel they draw equals the native word (the nearest upscale), as LineRule::NativeThick does by
    // construction. A thin diagonal polygon draws about N times the hairline's fragments with ThinRule::Parallel.
    Scenario s;
    s.Op0(Op::Kind::BeginFrame);
    s.AddCommand({0x0006, 0, 0x00C0, 0x83E0, 0, 0, U(20), U(30), U(200), U(30), 0, 0, 0, 0, 0, 0}); // line
    s.AddCommand({0x0006, 0, 0x00C0, 0xFC00, 0, 0, U(40), U(150), U(40), U(60), 0, 0, 0, 0, 0, 0}); // line, up
    s.AddCommand({0x0005, 0, 0x00C0, 0x801F, 0, 0, U(60), U(70), U(160), U(70), U(160), U(120), U(60), U(120), 0, 0});
    s.Op0(Op::Kind::EndFrame);
    for (const uint32 n : {2u, 3u, 4u}) {
        CAPTURE(n);
        nx::Options o;
        o.scale = n;
        o.rules.line = nx::LineRule::Parallel;
        const NxRun r = RunNx(s, o);
        CHECK(r.stats.commands.counts[nx::CommandCounts::LineParallel] == 3);
        for (uint32 w = 0; w < ref::kWordsPerFB; ++w) {
            const uint16 native = nx::NativeWord(*r.nxNative, 0, w);
            for (uint32 j = 0; j < n; ++j) {
                for (uint32 i = 0; i < n; ++i) {
                    if (r.surface->Word(0, w, i, j) != native) {
                        FAIL("word " << w << " sub " << i << "," << j);
                    }
                }
            }
        }
    }

    Scenario thin;
    thin.Op0(Op::Kind::BeginFrame);
    thin.Polygon(0x00C0, 0x83FF, {30, 20}, {230, 150}, {230, 150}, {30, 20});
    thin.Op0(Op::Kind::EndFrame);
    for (const uint32 n : {2u, 4u}) {
        nx::Options o;
        o.scale = n;
        o.rules.thin = nx::ThinRule::Hairline;
        const NxRun hair = RunNx(thin, o);
        o.rules.thin = nx::ThinRule::Parallel;
        const NxRun par = RunNx(thin, o);
        CAPTURE(n, hair.stats.Fragments(), par.stats.Fragments());
        CHECK(hair.stats.commands.counts[nx::CommandCounts::Thin] == 1);
        CHECK(par.stats.commands.counts[nx::CommandCounts::ThinParallel] == 1);
        CHECK(par.stats.Fragments() > (n - 1) * hair.stats.Fragments());
    }
}

TEST_CASE("VDP1 N x reference: one random table (BRIMIR_NX_SEED, BRIMIR_NX_N, BRIMIR_NX_FLAGS)",
          "[vdp1][nx][reference][.debug]") {
    // BRIMIR_NX_FLAGS: q = quad path on, r = rect path off
    const char *seedText = std::getenv("BRIMIR_NX_SEED");
    const char *nText = std::getenv("BRIMIR_NX_N");
    const char *flagsText = std::getenv("BRIMIR_NX_FLAGS");
    const uint64 seed = seedText != nullptr ? std::strtoull(seedText, nullptr, 10) : 1;
    const std::string flags = flagsText != nullptr ? flagsText : "";
    Coverage cov;
    const Scenario s = RandomScenario(seed, cov);
    nx::Options o;
    o.scale = nText != nullptr ? static_cast<uint32>(std::atoi(nText)) : 2;
    o.quadPath = flags.find('q') != std::string::npos;
    o.rectPath = flags.find('r') == std::string::npos;
    const auto t0 = std::chrono::steady_clock::now();
    const NxRun r = RunNx(s, o);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("total %.1f ms: native %.1f, generate %.1f, raster %.1f, resolve %.1f ms; fragments %llu (native %llu)\n",
                seconds * 1e3, r.stats.nativeSeconds * 1e3, r.stats.generateSeconds * 1e3, r.stats.rasterSeconds * 1e3,
                r.stats.resolveSeconds * 1e3, static_cast<unsigned long long>(r.stats.Fragments()),
                static_cast<unsigned long long>(r.stats.nativeFragments));
    CAPTURE(seed, o.scale, flags, s.tvmr, s.tvmd, s.fbcr, s.deinterlace, s.transparentMeshes);
    CHECK(CompareFbram(*r.native.fb, *r.nxNative) == "");
    CHECK(nx::CompareSamplePoints(*r.nxNative, *r.surface) == "");
}

TEST_CASE("VDP1 N x reference on captured frames (BRIMIR_VDP1_CAPTURES)", "[vdp1][nx][reference][.captures]") {
    // Every vdp1ops_*.bin under the directory: the native replay ends with the captured FBRAM, rect-path and
    // replicated content is native at the sample points at N = 2, 3, 4, and the quad path runs.
    const char *dir = std::getenv("BRIMIR_VDP1_CAPTURES");
    if (dir == nullptr) {
        SKIP("BRIMIR_VDP1_CAPTURES not set");
    }
    uint32 captures = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().extension() != ".bin" || entry.path().filename().string().rfind("vdp1ops_", 0) != 0) {
            continue;
        }
        CAPTURE(entry.path().string());
        gpu::Vdp1OpDump d;
        std::string error;
        REQUIRE(gpu::ReadVdp1OpDump(entry.path().string(), d, &error));
        ++captures;
        auto start = std::make_unique<ref::Fbram>();
        auto toFb = [](const std::vector<uint8> &bytes, ref::Fbram &fb) {
            vdp::SpriteFB *slices[8] = {&fb.main[0],    &fb.main[1],    &fb.alt[0],     &fb.alt[1],
                                        &fb.mesh[0][0], &fb.mesh[0][1], &fb.mesh[1][0], &fb.mesh[1][1]};
            for (uint32 i = 0; i < 8; ++i) {
                std::copy_n(bytes.begin() + i * gpu::kFbramSliceSize, gpu::kFbramSliceSize, slices[i]->begin());
            }
        };
        toFb(d.fbramStart, *start);
        auto end = std::make_unique<ref::Fbram>();
        toFb(d.fbramEnd, *end);
        for (const uint32 n : {2u, 3u, 4u}) {
            for (const bool quadPath : {false, true}) {
                CAPTURE(n, quadPath);
                auto native = std::make_unique<ref::Fbram>(*start);
                auto surface = std::make_unique<nx::NxFbram>(n);
                surface->ReplicateFrom(*native);
                nx::Options o;
                o.scale = n;
                o.quadPath = quadPath;
                nx::ReferenceNx r{*native, *surface, o, d.vramStart};
                for (const auto &l : d.lists) {
                    r.Replay(l);
                }
                CHECK(CompareFbram(*end, *native, "captured", "replayed") == "");
                if (!quadPath) {
                    CHECK(nx::CompareSamplePoints(*native, *surface) == "");
                }
            }
        }
    }
    CHECK(captures > 0);
}
