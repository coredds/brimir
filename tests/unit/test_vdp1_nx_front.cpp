// The N x VDP1 span generator of the front half (src/gpu/vdp1/vdp1_nx_front.hpp) against its specification, the CPU
// reference's generator (vdp1_nx_reference.hpp: GenerateBatch + SplitSubBatches with the default rules R1 + T2 + L2),
// both fed with the same native op lists of the real front half: seeded random tables (vdp1_scenario.hpp), targeted
// scenes and the captured frames (BRIMIR_VDP1_CAPTURES). Every batch must give the same spans (kind, command, end
// points, sub-row, texel row, Gouraud colours, slots), the same slot ranges and the same sub-batches. Also: slot ranges
// contiguous and in order, capacity splits only between spans.

#include "catch_amalgamated.hpp"

#include "vdp1_nx_reference.hpp"
#include "vdp1_scenario.hpp"

#include <gpu/vdp1/vdp1_nx_front.hpp>
#include <gpu/vdp1/vdp1_op_dump.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace brimir::test::vdp1scene;
namespace nx = brimir::test::vdp1nx;

// The reference generator's command classes the production classes correspond to (the others need non-default rules)
constexpr nx::CommandCounts::Class kRefClass[] = {
    nx::CommandCounts::Rect,         nx::CommandCounts::Quad,          nx::CommandCounts::ThinParallel,
    nx::CommandCounts::LineParallel, nx::CommandCounts::ReplicateWrap, nx::CommandCounts::Continuation};
static_assert(std::size(kRefClass) == static_cast<size_t>(gpu::Vdp1NxCommandClass::Count));

struct Totals {
    nx::CommandCounts ref;
    gpu::Vdp1NxStats prod;
    uint64 batches = 0, splitBatches = 0, emptyBatches = 0;
    double refSeconds = 0;

    void Merge(const Totals &o) {
        for (uint32 c = 0; c < nx::CommandCounts::Count; ++c) {
            ref.counts[c] += o.ref.counts[c];
        }
        for (uint32 k = 0; k < 3; ++k) {
            prod.spans[k] += o.prod.spans[k];
            prod.slots[k] += o.prod.slots[k];
        }
        for (size_t c = 0; c < prod.commands.size(); ++c) {
            prod.commands[c] += o.prod.commands[c];
        }
        prod.subBatches += o.prod.subBatches;
        prod.batches += o.prod.batches;
        prod.nanoseconds += o.prod.nanoseconds;
        batches += o.batches;
        splitBatches += o.splitBatches;
        emptyBatches += o.emptyBatches;
        refSeconds += o.refSeconds;
    }
    uint64 Classes(gpu::Vdp1NxCommandClass c) const {
        return prod.commands[static_cast<size_t>(c)];
    }
};

// ReferenceNx::DrawBatch's state between batches (capacity-split continuations)
struct RefState {
    bool prevCapacity = false;
    gpu::Vdp1CommandParams prevCommand{};
    uint32 prevQuadStep = 0;
};

uint32 KindOf(nx::SpanKind k) {
    switch (k) {
    case nx::SpanKind::Rect: return gpu::kNxSpanRect;
    case nx::SpanKind::Quad: return gpu::kNxSpanQuad;
    case nx::SpanKind::Replicate: return gpu::kNxSpanReplicate;
    }
    return 3;
}

std::string Where(uint32 batch, uint32 span) {
    return "batch " + std::to_string(batch) + " span " + std::to_string(span) + ": ";
}

// The production output's own invariants: sub-batches cover the batch's spans in order; in each, the slot ranges are
// contiguous from 0 in emission order; capacities hold (a lone span may exceed the slot capacity)
std::string CheckSubBatches(const gpu::Vdp1NxArena &arena, std::span<const gpu::Vdp1NxSubBatch> subs, size_t spanBase,
                            const gpu::Vdp1NxLimits &limits) {
    size_t next = spanBase;
    for (const gpu::Vdp1NxSubBatch &sb : subs) {
        if (sb.spanOffset != next || sb.spanCount == 0) {
            return "sub-batch spans not contiguous";
        }
        if (sb.spanCount > limits.spans || (sb.spanCount > 1 && sb.slotCount > limits.slots)) {
            return "sub-batch over capacity";
        }
        uint32 expected = 0;
        for (uint32 i = 0; i < sb.spanCount; ++i) {
            const gpu::Vdp1NxSpan &s = arena.spans[sb.spanOffset + i];
            if (s.slotBase != expected) {
                return "slot ranges not contiguous";
            }
            const uint32 end = i + 1 < sb.spanCount ? arena.spans[sb.spanOffset + i + 1].slotBase : sb.slotCount;
            if (end <= s.slotBase) {
                return "empty or reversed slot range";
            }
            expected = end;
        }
        if (expected != sb.slotCount) {
            return "slot count mismatch";
        }
        next += sb.spanCount;
    }
    return next == arena.spans.size() ? "" : "spans outside the sub-batches";
}

// Both generators over one op list (state carried over lists as ReferenceNx does); "" when every batch matches
std::string CompareList(const gpu::Vdp1OpList &ops, uint32 n, const gpu::Vdp1NxLimits &limits, RefState &st,
                        Totals &t, bool timeReference = false) {
    gpu::Vdp1NxArena arena;
    arena.limits = limits;
    nx::Options o;
    o.scale = n;
    o.subBatchSlots = limits.slots;
    o.subBatchSpans = limits.spans;
    std::vector<ref::CommandParams> commands;
    std::vector<ref::RasterState> states;
    std::vector<ref::Span> spans;
    std::vector<uint32> slots;
    std::string error;
    for (const gpu::Vdp1Op &op : ops.ops) {
        if (op.kind != gpu::Vdp1OpKind::Draw) {
            st.prevCapacity = false;
            continue;
        }
        const gpu::Vdp1DrawBatch &batch = ops.batches[op.index];
        commands.resize(batch.cmdCount);
        states.resize(batch.cmdCount);
        for (uint32 i = 0; i < batch.cmdCount; ++i) {
            const gpu::Vdp1CommandParams &c = ops.commands[batch.cmdOffset + i];
            commands[i] = ref::DecodeCommand(c);
            states[i] = ref::DecodeRasterState(c.rasterFlags, batch.drawFB);
        }
        spans.resize(batch.spanCount);
        slots.resize(batch.spanCount);
        for (uint32 s = 0; s < batch.spanCount; ++s) {
            spans[s] = ref::DecodeSpan(ops.spans[batch.spanOffset + s]);
            const uint32 end =
                s + 1 < batch.spanCount ? ops.spans[batch.spanOffset + s + 1].fragBase : batch.fragmentCount;
            slots[s] = end - spans[s].fragBase;
        }
        nx::BatchInput in;
        in.commands = commands;
        in.states = states;
        in.spans = spans;
        in.spanSlots = slots;
        const gpu::Vdp1CommandParams &firstCommand = ops.commands[batch.cmdOffset];
        in.firstCommandContinues = st.prevCapacity &&
                                   std::memcmp(&firstCommand, &st.prevCommand, sizeof(gpu::Vdp1CommandParams)) == 0 &&
                                   spans[0].quadStep > st.prevQuadStep;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<nx::NxSpan> refSpans = nx::GenerateBatch(o, in, &t.ref);
        const auto bounds = nx::SplitSubBatches(o, refSpans);
        if (timeReference) {
            t.refSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }

        const size_t spanBase = arena.spans.size();
        const auto subs = gpu::GenerateNx(ops, op.index, n, arena);
        ++t.batches;
        t.splitBatches += subs.size() > 1;
        t.emptyBatches += subs.empty();

        st.prevCapacity = batch.end == gpu::Vdp1BatchEnd::Capacity;
        st.prevCommand = ops.commands[batch.cmdOffset + batch.cmdCount - 1];
        st.prevQuadStep = spans.back().quadStep;

        if (!error.empty()) {
            continue;
        }
        error = CheckSubBatches(arena, subs, spanBase, limits);
        if (!error.empty()) {
            error = Where(op.index, 0) + error;
            continue;
        }
        if (subs.size() != bounds.size() || arena.spans.size() - spanBase != refSpans.size()) {
            error = Where(op.index, 0) + "sub-batches " + std::to_string(subs.size()) + " vs " +
                    std::to_string(bounds.size()) + ", spans " + std::to_string(arena.spans.size() - spanBase) +
                    " vs " + std::to_string(refSpans.size());
            continue;
        }
        for (size_t b = 0; b < subs.size() && error.empty(); ++b) {
            const gpu::Vdp1NxSubBatch &sb = subs[b];
            if (sb.spanOffset - spanBase != bounds[b].first || sb.spanCount != bounds[b].second - bounds[b].first) {
                error = Where(op.index, 0) + "sub-batch " + std::to_string(b) + " boundaries differ";
                break;
            }
            for (uint32 i = 0; i < sb.spanCount; ++i) {
                const gpu::Vdp1NxSpan &p = arena.spans[sb.spanOffset + i];
                const nx::NxSpan &r = refSpans[bounds[b].first + i];
                const uint32 pSlots =
                    (i + 1 < sb.spanCount ? arena.spans[sb.spanOffset + i + 1].slotBase : sb.slotCount) - p.slotBase;
                const uint32 kind = gpu::Vdp1NxSpanKind(p.flags);
                const char *field = kind != KindOf(r.kind)                                         ? "kind"
                                    : gpu::Vdp1NxSpanAntiAlias(p.flags) != r.antiAlias             ? "antiAlias"
                                    : p.cmdIndex != r.cmdIndex                                     ? "cmdIndex"
                                    : static_cast<sint32>(p.x0) != r.a.x()                         ? "x0"
                                    : static_cast<sint32>(p.y0) != r.a.y()                         ? "y0"
                                    : static_cast<sint32>(p.x1) != r.b.x()                         ? "x1"
                                    : static_cast<sint32>(p.y1) != r.b.y()                         ? "y1"
                                    : gpu::Vdp1NxSpanSubRow(p.flags) != r.subRow                   ? "subRow"
                                    : static_cast<sint32>(gpu::Vdp1NxSpanTexV(p.flags)) != r.texV  ? "texV"
                                    : gpu::Vdp1Lo16(p.gouraudLR) != r.gouraudL.u16                 ? "gouraudL"
                                    : gpu::Vdp1Hi16(p.gouraudLR) != r.gouraudR.u16                 ? "gouraudR"
                                    : p.slotBase != r.slotBase                                     ? "slotBase"
                                    : pSlots != r.slots                                            ? "slots"
                                                                                                   : nullptr;
                if (field != nullptr) {
                    error = Where(op.index, bounds[b].first + i) + field + " differs (kind " +
                            std::to_string(kind) + ", cmd " + std::to_string(r.cmdIndex) + ", type " +
                            std::to_string(gpu::Vdp1CmdType(ops.commands[batch.cmdOffset + r.cmdIndex].drawFlags)) +
                            ")";
                    break;
                }
            }
        }
    }
    Totals add;
    add.prod = arena.TakeStats();
    t.Merge(add);
    return error;
}

uint32 Workers() {
    return std::clamp<uint32>(std::thread::hardware_concurrency() / 2, 1, 8);
}

struct SeedOutcome {
    std::string error;
    Totals totals;
};

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

// The scenario's op lists through both generators at scale n (with limits)
std::string CompareScenario(const Scenario &s, uint32 n, const gpu::Vdp1NxLimits &limits, Totals &t) {
    std::vector<gpu::Vdp1OpList> lists;
    RunReference(s, {}, 0, [&](const gpu::Vdp1OpList &ops) { lists.push_back(ops); });
    RefState st;
    for (size_t i = 0; i < lists.size(); ++i) {
        const std::string e = CompareList(lists[i], n, limits, st, t);
        if (!e.empty()) {
            return "list " + std::to_string(i) + " " + e;
        }
    }
    return "";
}

gpu::Vdp1NxLimits SmallLimits(uint64 seed) {
    return {97 + static_cast<uint32>(seed % 500), 3 + static_cast<uint32>(seed % 7)};
}

void InfoTotals(const Totals &t) {
    static constexpr const char *kNames[] = {"rect", "quad", "thin (N-parallel)", "line (N-parallel)",
                                             "replicate (13-bit wrap)", "continuation"};
    for (size_t c = 0; c < std::size(kNames); ++c) {
        UNSCOPED_INFO("commands " << kNames[c] << ": " << t.prod.commands[c]);
    }
    UNSCOPED_INFO("spans rect " << t.prod.spans[0] << " quad " << t.prod.spans[1] << " replicate " << t.prod.spans[2]
                                << "; batches " << t.batches << " (split " << t.splitBatches << ", empty "
                                << t.emptyBatches << "), sub-batches " << t.prod.subBatches);
}

void CheckClassCounts(const Totals &t) {
    for (size_t c = 0; c < std::size(kRefClass); ++c) {
        CHECK(t.prod.commands[c] == t.ref.counts[kRefClass[c]]);
    }
    CHECK(t.ref.counts[nx::CommandCounts::Thin] == 0);
    CHECK(t.ref.counts[nx::CommandCounts::ReplicateLine] == 0);
    CHECK(t.ref.counts[nx::CommandCounts::ReplicateNoQuadPath] == 0);
}

} // namespace

TEST_CASE("VDP1 N x generator equals the reference generator on seeded tables", "[vdp1][nx][front]") {
    // 10,000 random tables (every command type and colour mode, Gouraud, mesh, MSB-on, 8 bpp, DIE/DIL, deinterlace,
    // end codes, HSS, clipping, raw large clips, 13-bit wraps, erases, CPU writes, swaps, front-half batch splits,
    // takes) at N = 1 to 4; every third with small sub-batch capacities
    constexpr uint64 kFirst = 1, kCount = 10'000;
    const auto outcomes = ForSeeds(kFirst, kCount, [](uint64 seed) {
        Coverage cov;
        const Scenario s = RandomScenario(seed, cov);
        SeedOutcome out;
        const uint32 n = 1 + static_cast<uint32>(seed % 4);
        const gpu::Vdp1NxLimits limits = seed % 3 == 0 ? SmallLimits(seed) : gpu::Vdp1NxLimits{};
        out.error = CompareScenario(s, n, limits, out.totals);
        if (!out.error.empty()) {
            out.error = "N=" + std::to_string(n) + ": " + out.error;
        }
        return out;
    });
    Totals total;
    uint64 failures = 0;
    for (size_t i = 0; i < outcomes.size(); ++i) {
        if (!outcomes[i].error.empty()) {
            if (failures++ < 5) {
                UNSCOPED_INFO("seed " << kFirst + i << ": " << outcomes[i].error);
            }
            continue;
        }
        total.Merge(outcomes[i].totals);
    }
    InfoTotals(total);
    CHECK(failures == 0);
    CheckClassCounts(total);
    for (size_t c = 0; c < std::size(kRefClass); ++c) {
        CHECK(total.prod.commands[c] > 0);
    }
    CHECK(total.splitBatches > 0);
    CHECK(total.prod.spans[gpu::kNxSpanRect] > 0);
    CHECK(total.prod.spans[gpu::kNxSpanQuad] > 0);
    CHECK(total.prod.spans[gpu::kNxSpanReplicate] > 0);
}

TEST_CASE("VDP1 N x generator equals the reference generator (long)", "[vdp1][nx][front][.]") {
    constexpr uint64 kFirst = 1'000'001, kCount = 200'000;
    const auto outcomes = ForSeeds(kFirst, kCount, [](uint64 seed) {
        Coverage cov;
        const Scenario s = RandomScenario(seed, cov);
        SeedOutcome out;
        const uint32 n = 1 + static_cast<uint32>(seed % 4);
        out.error = CompareScenario(s, n, seed % 3 == 0 ? SmallLimits(seed) : gpu::Vdp1NxLimits{}, out.totals);
        return out;
    });
    uint64 failures = 0;
    for (size_t i = 0; i < outcomes.size(); ++i) {
        if (!outcomes[i].error.empty() && failures++ < 5) {
            UNSCOPED_INFO("seed " << kFirst + i << ": " << outcomes[i].error);
        }
    }
    CHECK(failures == 0);
}

namespace {

// Targeted scenes: every command type and each case the brief names
std::vector<std::pair<std::string, Scenario>> TargetedScenes() {
    std::vector<std::pair<std::string, Scenario>> out;
    auto base = [](uint64 seed) {
        Scenario s;
        TestRng rng{seed};
        for (uint32 i = Scenario::kTextureBase + 0x8000; i < Scenario::kVRAMSize; ++i) {
            s.vram[i] = static_cast<uint8>(rng.Next());
        }
        return s;
    };

    // Every command type, Gouraud and flips, rect and non-rect, both directions
    {
        Scenario s = base(1);
        const uint16 tex = s.AddTexture(RGBTexture(32, 24, 1));
        const uint16 g = s.AddGouraud({0x001F, 0x03E0, 0x7C00, 0x4210});
        s.Op0(Op::Kind::BeginFrame);
        for (const uint16 ctrl : {uint16{0x0000}, uint16{0x0010}, uint16{0x0020}, uint16{0x0030}}) {
            const uint16 off = static_cast<uint16>(ctrl * 2);
            s.AddCommand({ctrl, 0, 0x00EC, 0, tex, 0x0418, U(10 + off), 20, 0, 0, 0, 0, 0, 0, g, 0}); // normal
            s.AddCommand({static_cast<uint16>(0x0001 | ctrl), 0, 0x00EC, 0, tex, 0x0418, U(20 + off), 60, 50, 30, 0,
                          0, 0, 0, g, 0}); // scaled, zoom point upper left
            s.AddCommand({static_cast<uint16>(0x0001 | ctrl), 0, 0x00C8, 0, tex, 0x0418, U(150), U(150 - off),
                          U(100 + off), U(40), 0, 0, 0, 0, 0, 0}); // scaled (C), reversed corners
        }
        s.Distorted(0, 0x00EC, 0, tex, 0x0418, {40, 120}, {140, 100}, {160, 200}, {30, 180}, g);
        s.Distorted(0x0030, 0x00C8, 0, tex, 0x0418, {200, 100}, {120, 100}, {120, 60}, {200, 60}); // reversed rect
        s.Polygon(0x00C4, 0x4210, {300, 140}, {269, 140}, {269, 109}, {300, 109}, g); // rect, right to left, up
        s.Polygon(0x00C0, 0x7FFF, {250, 10}, {310, 30}, {290, 90}, {240, 60});
        s.Polygon(0x00C4, 0x7FFF, {10, 200}, {80, 210}, {80, 210}, {10, 200}, g); // thin (collinear)
        s.Polygon(0x00C0, 0x03FF, {100, 100}, {100, 100}, {100, 100}, {100, 100}); // point
        s.AddCommand({0x0005, 0, 0x00C4, 0x83E0, 0, 0, U(5), U(5), U(310), U(40), U(200), U(220), U(15), U(150), g, 0});
        s.AddCommand({0x0007, 0, 0x00C0, 0x801F, 0, 0, U(60), U(70), U(160), U(70), U(160), U(120), U(60), U(120), 0,
                      0}); // command 7 (polyline)
        s.AddCommand({0x0006, 0, 0x00C4, 0xFC00, 0, 0, U(300), U(200), U(20), U(30), 0, 0, 0, 0, g, 0}); // line
        s.AddCommand({0x0006, 0, 0x00C0, 0xFC00, 0, 0, U(40), U(150), U(40), U(60), 0, 0, 0, 0, 0, 0}); // line up
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back("command types", s);
    }
    // High-speed shrink and end codes (colour mode 4 with 0xFF texels), shrinking scaled and distorted sprites
    {
        Scenario s = base(2);
        std::vector<uint8> tex(64 * 32);
        for (size_t i = 0; i < tex.size(); ++i) {
            tex[i] = (i % 13 == 5) ? 0xFF : static_cast<uint8>(1 + i % 200);
        }
        const uint16 srca = s.AddTexture(tex);
        s.Op0(Op::Kind::BeginFrame);
        s.AddCommand({0x0001, 0, 0x1020, 0, srca, 0x0820, 10, 10, 20, 9, 0, 0, 0, 0, 0, 0}); // HSS shrink
        s.AddCommand({0x0001, 0, 0x0020, 0, srca, 0x0820, 40, 10, 150, 90, 0, 0, 0, 0, 0, 0}); // end codes, zoom
        s.Distorted(0, 0x1020, 0, srca, 0x0820, {200, 10}, {230, 14}, {226, 30}, {198, 28});
        s.Distorted(0, 0x0020, 0, srca, 0x0820, {20, 120}, {300, 110}, {280, 210}, {30, 200});
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back("HSS and end codes", s);
    }
    // 8 bpp with MSB-on at odd x; mesh on and off with transparent meshes on and off
    for (const bool meshes : {false, true}) {
        Scenario s = base(3);
        s.tvmr = 1;
        s.transparentMeshes = meshes;
        const uint16 tex = s.AddTexture(RGBTexture(16, 16, 3));
        s.Op0(Op::Kind::BeginFrame);
        for (sint32 x = 1; x < 200; x += 37) {
            s.AddCommand({0, 0, 0x80E8, 0, tex, 0x0210, U(x), U(x / 2 + 1), 0, 0, 0, 0, 0, 0, 0, 0}); // MSB on
            s.Polygon(0x01C0, 0x00AB, {x, 120}, {x + 21, 125}, {x + 25, 160}, {x + 3, 150}); // mesh
            s.Polygon(0x00C0, 0x00CD, {x + 1, 170}, {x + 22, 170}, {x + 22, 190}, {x + 1, 190});
        }
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back(meshes ? "8 bpp, MSB-on, mesh (transparent meshes)" : "8 bpp, MSB-on, mesh", s);
    }
    // Double interlace (DIE, DIL 0 and 1) and the deinterlace enhancement in double density (doubleV)
    for (const uint16 fbcr : {uint16{0x0008}, uint16{0x000C}}) {
        for (const bool deinterlace : {false, true}) {
            Scenario s = base(4);
            s.tvmd = 0x80C0;
            s.fbcr = deinterlace ? 0 : fbcr;
            s.deinterlace = deinterlace;
            s.vres = 448;
            const uint16 tex = s.AddTexture(RGBTexture(24, 24, 4));
            s.Op0(Op::Kind::BeginFrame);
            s.AddCommand({0, 0, 0x00E8, 0, tex, 0x0318, 30, 31, 0, 0, 0, 0, 0, 0, 0, 0});
            s.Polygon(0x00C0, 0x7C1F, {100, 21}, {200, 41}, {180, 161}, {90, 141});
            s.Distorted(0, 0x00E8, 0, tex, 0x0318, {20, 100}, {60, 100}, {60, 140}, {20, 140});
            s.AddCommand({0x0006, 0, 0x00C0, 0x83E0, 0, 0, U(10), U(201), U(300), U(211), 0, 0, 0, 0, 0, 0});
            s.Op0(Op::Kind::EndFrame);
            out.emplace_back(std::string("double interlace fbcr ") + std::to_string(fbcr) +
                                 (deinterlace ? " deinterlace" : ""),
                             s);
        }
    }
    // 13-bit wraps and long native lines: replicate (per command, also when the front half splits it)
    {
        Scenario s = base(5);
        s.clip.sysClipH = 0x3FF;
        s.clip.sysClipV = 0x1FF;
        s.Op0(Op::Kind::BeginFrame);
        s.Polygon(0x00C0, 0x7FFF, {-4000, 10}, {300, 20}, {4000, 200}, {20, 100});  // edge delta > 4095
        s.Polygon(0x00C0, 0x7FE0, {10, 10}, {40, 10}, {60, 2100}, {5, 2090});       // major >= 2048
        s.Polygon(0x00C0, 0x03FF, {-800, 30}, {800, 30}, {800, 60}, {-800, 60});    // rect row > 1364
        s.Polygon(0x00C0, 0x001F, {-700, 100}, {700, 90}, {720, 130}, {-690, 140}); // quad lines > 1364
        s.AddCommand({0x0006, 0, 0x00C0, 0x83E0, 0, 0, U(-700), U(10), U(700), U(20), 0, 0, 0, 0, 0, 0}); // line
        s.AddCommand({0x0005, 0, 0x00C0, 0x83E0, 0, 0, U(5), U(5), U(1500), U(40), U(200), U(220), U(15), U(150), 0,
                      0}); // polyline with a long segment
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back("13-bit wraps", s);
        Scenario split = s;
        split.limits.fragments = 700;
        out.emplace_back("13-bit wraps, split", split);
    }
    // Huge system clip (raw 16-bit values): big shapes, the N x clip extent, lines off the area
    {
        Scenario s = base(6);
        s.clip.sysClipH = 0xFFFF;
        s.clip.sysClipV = 0xFFFF;
        s.clip.userClipX1 = 0xFFFF;
        s.clip.userClipY1 = 0xFFFF;
        const uint16 tex = s.AddTexture(RGBTexture(32, 32, 6));
        s.Op0(Op::Kind::BeginFrame);
        s.Polygon(0x00C0, 0x7FFF, {-300, -200}, {1200, -100}, {1100, 900}, {-200, 1000});
        s.Distorted(0, 0x00C8, 0, tex, 0x0420, {1000, 10}, {1300, 40}, {1250, 400}, {990, 380});
        s.AddCommand({0x0006, 0, 0x00C0, 0x83E0, 0, 0, U(-50), U(-50), U(1300), U(1000), 0, 0, 0, 0, 0, 0});
        s.Polygon(0x00C0, 0x03FF, {-1000, 5}, {300, 5}, {300, 90}, {-1000, 90}); // rect, 1300 wide
        s.Op0(Op::Kind::EndFrame);
        out.emplace_back("huge system clip", s);
    }
    // Front-half capacity splits: quads, distorted sprites and polylines repeated over batches (continuations)
    {
        Scenario s = base(7);
        const uint16 tex = s.AddTexture(RGBTexture(32, 32, 3));
        s.Op0(Op::Kind::BeginFrame);
        s.Polygon(0x00C3, 0xFC1F, {10, 20}, {300, 5}, {280, 210}, {30, 190});
        s.Distorted(0, 0x00C7, 0, tex, 0x0420, {40, 30}, {250, 60}, {230, 200}, {20, 170},
                    s.AddGouraud({0x7FFF, 0x0000, 0x4210, 0x7C00}));
        s.AddCommand({0x0005, 0, 0x00C3, 0x83FF, 0, 0, U(5), U(5), U(310), U(40), U(200), U(220), U(15), U(150), 0, 0});
        s.Polygon(0x00C4, 0x4210, {20, 20}, {200, 20}, {200, 180}, {20, 180},
                  s.AddGouraud({0x001F, 0x03E0, 0x7C00, 0x4210})); // rect split over batches
        s.Polygon(0x00C0, 0x7FFF, {10, 200}, {300, 20}, {300, 20}, {10, 200}); // thin, split
        s.Op0(Op::Kind::EndFrame);
        for (const uint32 fragments : {200u, 1'500u, 9'000u}) {
            Scenario split = s;
            split.limits.fragments = fragments;
            out.emplace_back("capacity split " + std::to_string(fragments), split);
        }
        Scenario spans = s;
        spans.limits.spans = 7;
        out.emplace_back("capacity split (spans)", spans);
    }
    return out;
}

} // namespace

TEST_CASE("VDP1 N x generator equals the reference generator on targeted scenes", "[vdp1][nx][front]") {
    Totals total;
    for (const auto &[name, s] : TargetedScenes()) {
        for (uint32 n = 1; n <= 4; ++n) {
            for (const bool small : {false, true}) {
                CAPTURE(name, n, small);
                Totals t;
                CHECK(CompareScenario(s, n, small ? SmallLimits(n * 31) : gpu::Vdp1NxLimits{}, t) == "");
                CHECK(t.batches > 0);
                total.Merge(t);
            }
        }
    }
    InfoTotals(total);
    CheckClassCounts(total);
    for (size_t c = 0; c < std::size(kRefClass); ++c) {
        CHECK(total.prod.commands[c] > 0);
    }
}

TEST_CASE("VDP1 N x generator: capacity splits fall between the unsplit generation's spans", "[vdp1][nx][front]") {
    // The same batch at the full and at small capacities: the concatenated sub-batches are the same span records
    // (only the slot bases restart), each cut between two spans, and the slot ranges restart at 0 per sub-batch
    uint64 cuts = 0;
    for (uint64 seed = 40'001; seed <= 40'300; ++seed) {
        Coverage cov;
        const Scenario s = RandomScenario(seed, cov);
        std::vector<gpu::Vdp1OpList> lists;
        RunReference(s, {}, 0, [&](const gpu::Vdp1OpList &ops) { lists.push_back(ops); });
        const uint32 n = 2 + static_cast<uint32>(seed % 3);
        for (const gpu::Vdp1OpList &ops : lists) {
            for (uint32 b = 0; b < ops.batches.size(); ++b) {
                gpu::Vdp1NxArena whole, parts;
                parts.limits = SmallLimits(seed + b);
                const auto w = gpu::GenerateNx(ops, b, n, whole);
                const auto subs = gpu::GenerateNx(ops, b, n, parts);
                REQUIRE(parts.spans.size() == whole.spans.size());
                const size_t count = whole.spans.size();
                // the full-capacity generation's slots per span (a native batch can exceed one N x sub-batch)
                std::vector<uint32> wholeSlots(count);
                for (const gpu::Vdp1NxSubBatch &sb : w) {
                    for (uint32 i = 0; i < sb.spanCount; ++i) {
                        const uint32 j = sb.spanOffset + i;
                        wholeSlots[j] = (i + 1 < sb.spanCount ? whole.spans[j + 1].slotBase : sb.slotCount) -
                                        whole.spans[j].slotBase;
                    }
                }
                uint32 offset = 0;
                for (const gpu::Vdp1NxSubBatch &sb : subs) {
                    REQUIRE(sb.spanOffset == offset);
                    uint32 slot = 0;
                    for (uint32 i = 0; i < sb.spanCount; ++i) {
                        gpu::Vdp1NxSpan a = whole.spans[offset + i], c = parts.spans[offset + i];
                        REQUIRE(c.slotBase == slot);
                        slot += wholeSlots[offset + i];
                        a.slotBase = c.slotBase = 0;
                        REQUIRE(std::memcmp(&a, &c, sizeof(a)) == 0);
                    }
                    REQUIRE(slot == sb.slotCount);
                    offset += sb.spanCount;
                }
                REQUIRE(offset == count);
                cuts += subs.empty() ? 0 : subs.size() - 1;
            }
        }
    }
    CHECK(cuts > 1'000);
}

TEST_CASE("VDP1 N x generator: an invalid scale or batch gives nothing", "[vdp1][nx][front]") {
    Scenario s;
    s.Op0(Op::Kind::BeginFrame);
    s.Polygon(0x00C0, 0x7FFF, {10, 10}, {50, 12}, {40, 60}, {8, 50});
    s.Op0(Op::Kind::EndFrame);
    std::vector<gpu::Vdp1OpList> lists;
    RunReference(s, {}, 0, [&](const gpu::Vdp1OpList &ops) { lists.push_back(ops); });
    const auto list = std::find_if(lists.begin(), lists.end(), [](const auto &l) { return !l.batches.empty(); });
    REQUIRE(list != lists.end());
    gpu::Vdp1NxArena arena;
    CHECK(gpu::GenerateNx(*list, 0, 0, arena).empty());
    CHECK(gpu::GenerateNx(*list, 0, gpu::kMaxNxScale + 1, arena).empty());
    CHECK(gpu::GenerateNx(*list, static_cast<uint32>(list->batches.size()), 2, arena).empty());
    CHECK(arena.spans.empty());
    CHECK(gpu::GenerateNx(*list, 0, 2, arena).size() == 1);
    CHECK(arena.stats.spans[gpu::kNxSpanQuad] > 0);
    arena.Clear();
    CHECK(arena.spans.empty());
    CHECK(arena.TakeStats().batches == 1);
    CHECK(arena.stats.batches == 0);
}

TEST_CASE("VDP1 N x generator on captured frames (BRIMIR_VDP1_CAPTURES)", "[vdp1][nx][front][.captures]") {
    // Every vdp1ops_*.bin under the directory at N = 2, 3, 4: equal to the reference generator, and the generator's CPU
    // time per frame (best of BRIMIR_NX_REPEAT runs, default 5) next to the reference generator's
    const char *dir = std::getenv("BRIMIR_VDP1_CAPTURES");
    if (dir == nullptr) {
        SKIP("BRIMIR_VDP1_CAPTURES not set");
    }
    const char *repeatText = std::getenv("BRIMIR_NX_REPEAT");
    const uint32 repeat = repeatText != nullptr ? std::max(1, std::atoi(repeatText)) : 5;
    uint32 captures = 0;
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().extension() == ".bin" && entry.path().filename().string().rfind("vdp1ops_", 0) == 0) {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const auto &path : paths) {
        CAPTURE(path.string());
        gpu::Vdp1OpDump d;
        std::string error;
        REQUIRE(gpu::ReadVdp1OpDump(path.string(), d, &error));
        ++captures;
        for (const uint32 n : {2u, 3u, 4u}) {
            CAPTURE(n);
            Totals t;
            RefState st;
            for (const auto &l : d.lists) {
                CHECK(CompareList(l, n, {}, st, t, true) == "");
            }
            // Timing: the production generator alone, the arena reused over the frame's lists as the core will
            double best = 1e9;
            gpu::Vdp1NxArena arena;
            for (uint32 r = 0; r < repeat; ++r) {
                const auto t0 = std::chrono::steady_clock::now();
                for (const auto &l : d.lists) {
                    arena.Clear();
                    for (uint32 b = 0; b < l.batches.size(); ++b) {
                        gpu::GenerateNx(l, b, n, arena);
                    }
                }
                best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
            std::printf("nx-front: %-40s N=%u batches %llu spans %llu (rect %llu quad %llu repl %llu) slots %llu "
                        "sub-batches %llu: generator %.3f ms (best of %u), reference generator %.3f ms\n",
                        path.parent_path().filename().string().c_str(), n,
                        static_cast<unsigned long long>(t.batches),
                        static_cast<unsigned long long>(t.prod.spans[0] + t.prod.spans[1] + t.prod.spans[2]),
                        static_cast<unsigned long long>(t.prod.spans[0]),
                        static_cast<unsigned long long>(t.prod.spans[1]),
                        static_cast<unsigned long long>(t.prod.spans[2]),
                        static_cast<unsigned long long>(t.prod.slots[0] + t.prod.slots[1] + t.prod.slots[2]),
                        static_cast<unsigned long long>(t.prod.subBatches), best * 1e3, repeat, t.refSeconds * 1e3);
            CheckClassCounts(t);
        }
    }
    CHECK(captures > 0);
}
