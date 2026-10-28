// Vdp1FrontEnd (src/gpu/vdp1/vdp1_front.*), the Vulkan-free front half of the GPU VDP1 renderer, on its own: the op
// stream's structure and order (segments around MarkFrameBoundary, the trailing erase, CPU write records holding the
// value at write time, write runs), the batch rules (op-kind changes, VRAM hazards on the texture, look-up table,
// Gouraud table and command table, framebuffer swaps, capacities; no split on mode changes), the VRAM uploads, the
// command parameters (raw clip values, local coordinates, doubleV) and the erase push constants. FBRAM equality with
// software through the reference is in test_vdp1_raster_reference.cpp.
// GPU-free: tagged [vdp1], so it also runs in BRIMIR_VULKAN=OFF builds.
#include "catch_amalgamated.hpp"

#include <gpu/vdp1/vdp1_front.hpp>

#include <ymir/hw/vdp/renderer/common/vdp1_steppers.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>
#include <ymir/util/data_ops.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace brimir::gpu::vdp1;

namespace {

namespace vdp = ymir::vdp;

// A VDPState with a command table at 0x100 + i * 0x20 and the front half reading it
struct Fixture {
    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
    std::unique_ptr<Vdp1FrontEnd> front;
    uint32_t next = 0x100;

    explicit Fixture(vdp::config::Enhancements enhancements = {}, uint16_t tvmd = 0x8000) {
        state->regs2.WriteTVMD(tvmd);
        state->regs2.LatchTVMD();
        state->state2.layerEnabled[0] = true;
        state->state1.sysClipH = 319;
        state->state1.sysClipV = 223;
        state->state1.userClipX1 = 319;
        state->state1.userClipY1 = 223;
        front = std::make_unique<Vdp1FrontEnd>(*state);
        front->SetEnhancements(enhancements);
    }

    // Writes a command table without the VRAM hook (before the front half runs it) and returns its address
    uint32_t Command(const std::array<uint16_t, 16> &words) {
        const uint32_t address = next;
        for (uint32_t i = 0; i < 16; ++i) {
            ::util::WriteBE<uint16_t>(&state->mem1.VRAM[address + i * 2], words[i]);
        }
        next += 0x20;
        return address;
    }
    void Execute(uint32_t address) {
        front->ExecuteCommand(address,
                              vdp::VDP1Command::Control{.u16 = ::util::ReadBE<uint16_t>(&state->mem1.VRAM[address])});
    }
    uint32_t Polygon(int16_t x, int16_t y, int16_t size, uint16_t pmod = 0x00C0, uint16_t grda = 0) {
        const uint16_t x0 = static_cast<uint16_t>(x), y0 = static_cast<uint16_t>(y);
        const uint16_t x1 = static_cast<uint16_t>(x + size), y1 = static_cast<uint16_t>(y + size);
        return Command({0x0004, 0, pmod, 0x801F, 0, 0, x0, y0, x1, y0, x1, y1, x0, y1, grda, 0});
    }
    // A distorted sprite of 16x16 texels of colour mode `colorMode` at CMDSRCA srca (LUT at CMDCOLR for mode 1)
    uint32_t Sprite(int16_t x, int16_t y, uint16_t srca, uint16_t colorMode, uint16_t cmdcolr = 0, uint16_t extra = 0) {
        const uint16_t x0 = static_cast<uint16_t>(x), y0 = static_cast<uint16_t>(y);
        const uint16_t x1 = static_cast<uint16_t>(x + 15), y1 = static_cast<uint16_t>(y + 15);
        return Command({0x0002, 0, static_cast<uint16_t>(0x00C0 | (colorMode << 3) | extra), cmdcolr, srca, 0x0210,
                        x0, y0, x1, y0, x1, y1, x0, y1, 0, 0});
    }
    // A VDP1 VRAM write as the VDP makes it: VDPState first, then the hook
    void WriteVRAM(uint32_t address, uint16_t value) {
        state->mem1.WriteVRAM<uint16_t>(address, value, [&](uint32_t a, uint16_t) { front->WriteVRAM(a); });
    }
};

std::vector<Vdp1OpKind> Kinds(const Vdp1OpList &ops, const Vdp1Segment &segment) {
    std::vector<Vdp1OpKind> kinds;
    for (uint32_t i = 0; i < segment.opCount; ++i) {
        kinds.push_back(ops.ops[segment.firstOp + i].kind);
    }
    return kinds;
}

using K = Vdp1OpKind;

} // namespace

TEST_CASE("VDP1 front half: segments end at MarkFrameBoundary; ops after the last boundary trail", "[vdp1][front]") {
    Fixture f;
    const uint32_t a = f.Polygon(10, 10, 20);
    const uint32_t b = f.Polygon(40, 10, 20);
    f.front->BeginFrame();
    f.Execute(a);
    f.front->MarkFrameBoundary(); // VDP2EndFrame of frame 0: the draw is replayed before it
    f.Execute(b);
    f.front->Erase(0, 224);       // the BlankingAndSync display erase comes after VDP2EndFrame of frame 1
    f.front->MarkFrameBoundary();
    f.front->Erase(0, 224);       // ... and this one after the last frame: trailing
    REQUIRE(f.front->HasPendingOps());
    const Vdp1OpList ops = f.front->TakeOps();
    REQUIRE(ops.segments.size() == 3);
    CHECK(ops.FrameBoundaries() == 2);
    CHECK(Kinds(ops, ops.segments[0]) == std::vector<K>{K::Draw});
    CHECK(Kinds(ops, ops.segments[1]) == std::vector<K>{K::Draw, K::Erase});
    CHECK(Kinds(ops, ops.segments[2]) == std::vector<K>{K::Erase});
    CHECK(ops.batches[0].end == Vdp1BatchEnd::FrameBoundary);
    CHECK(ops.batches[1].end == Vdp1BatchEnd::OpKind);

    // Nothing pending: one empty (trailing) segment
    CHECK_FALSE(f.front->HasPendingOps());
    const Vdp1OpList empty = f.front->TakeOps();
    REQUIRE(empty.segments.size() == 1);
    CHECK(empty.segments[0].opCount == 0);
    CHECK(empty.Empty());

    // A boundary without ops gives an empty segment; TakeOps closes the open batch
    f.front->MarkFrameBoundary();
    f.Execute(a);
    const Vdp1OpList open = f.front->TakeOps();
    REQUIRE(open.segments.size() == 2);
    CHECK(open.segments[0].opCount == 0);
    CHECK(Kinds(open, open.segments[1]) == std::vector<K>{K::Draw});
    CHECK(open.batches[0].end == Vdp1BatchEnd::Take);
}

TEST_CASE("VDP1 front half: CPU framebuffer write records hold the value and framebuffer at write time",
          "[vdp1][front]") {
    for (const bool deinterlace : {false, true}) {
        for (const uint16_t tvmd : {uint16_t{0x8000}, uint16_t{0x8080}, uint16_t{0x80C0}}) {
            CAPTURE(deinterlace, tvmd);
            Fixture f({.deinterlace = deinterlace}, tvmd);
            auto write16 = [&](uint32_t address, uint16_t value) {
                f.state->VDP1WriteFB<uint16_t>(address, value, [&](uint32_t a, uint16_t v) { f.front->WriteFB16(a, v); });
            };
            auto write8 = [&](uint32_t address, uint8_t value) {
                f.state->VDP1WriteFB<uint8_t>(address, value, [&](uint32_t a, uint8_t v) { f.front->WriteFB8(a, v); });
            };
            write16(0x100, 0x1234);
            write16(0x100, 0x5678); // the same word again: both records, in order
            write8(0x203, 0xAB);
            write8(0x40000 + 0x204, 0xCD); // masked like VDPState masks it
            f.state->fbIndex.Flip();
            write16(0x3FFFE, 0xBEEF);
            const uint32_t poly = f.Polygon(0, 0, 4);
            f.Execute(poly);
            write16(0x300, 0x0001);
            const Vdp1OpList ops = f.front->TakeOps();
            CHECK(Kinds(ops, ops.segments[0]) == std::vector<K>{K::FbWrites, K::Draw, K::FbWrites});
            REQUIRE(ops.fbWrites.size() == 6);
            const bool mirror = deinterlace && tvmd != 0x8000;
            auto check = [&](size_t i, uint32_t word, uint32_t fb, uint32_t value, uint32_t mask) {
                CAPTURE(i);
                const Vdp1FbWrite &w = ops.fbWrites[i];
                CHECK(Vdp1FbWriteWord(w.address) == word);
                CHECK(Vdp1FbWriteFb(w.address) == fb);
                CHECK(Vdp1FbWriteMirrorAlt(w.address) == mirror);
                CHECK(w.value == value);
                CHECK(w.mask == mask);
            };
            const uint32_t fb0 = 0, fb1 = 1; // fbIndex.draw starts at 0
            check(0, 0x80, fb0, 0x1234, 0xFFFF);
            check(1, 0x80, fb0, 0x5678, 0xFFFF);
            check(2, 0x101, fb0, 0x00AB, 0x00FF);
            check(3, 0x102, fb0, 0xCD00, 0xFF00);
            check(4, 0x1FFFF, fb1, 0xBEEF, 0xFFFF);
            check(5, 0x180, fb1, 0x0001, 0xFFFF);
            REQUIRE(ops.fbWriteRuns.size() == 2);
            CHECK(ops.fbWriteRuns[0].offset == 0);
            CHECK(ops.fbWriteRuns[0].count == 5);
            CHECK(ops.fbWriteRuns[1].offset == 5);
            CHECK(ops.fbWriteRuns[1].count == 1);
            CHECK(ops.batches[0].drawFB == 1);
        }
    }

    // Write runs split at the capacity
    Fixture f;
    f.front->SetBatchLimitsForTesting({.fbWrites = 2});
    for (uint32_t i = 0; i < 5; ++i) {
        f.front->WriteFB16(i * 2, static_cast<uint16_t>(i));
    }
    const Vdp1OpList ops = f.front->TakeOps();
    REQUIRE(ops.fbWriteRuns.size() == 3);
    CHECK(ops.fbWriteRuns[2].count == 1);
    CHECK(ops.ops.size() == 3);
}

TEST_CASE("VDP1 front half: VRAM writes into what a pending command reads close the batch", "[vdp1][front]") {
    Fixture f;
    // A 4-bpp look-up-table sprite (colour mode 1): texture at 0x4000 (64 rows x 8 bytes... 16x16 texels = 128 bytes),
    // look-up table at CMDCOLR 0x0C00 * 8 = 0x6000, Gouraud table at 0x7000
    const uint32_t sprite = f.Sprite(10, 10, 0x4000 / 8, 1, 0x6000 / 8, 0x0004);
    ::util::WriteBE<uint16_t>(&f.state->mem1.VRAM[sprite + 0x1C], 0x7000 / 8);
    f.front->BeginFrame();

    struct Case {
        uint32_t address;
        bool splits;
        const char *what;
    };
    // 32-byte granules: the texture is [0x4000, 0x4080) plus 2 bytes of margin
    const Case cases[] = {
        {0x4000 + 0x7E, true, "last texel byte"}, {0x4000 + 0xA0, false, "past the texture"},
        {0x3FDE, false, "before the texture"},    {0x6000, true, "look-up table"},
        {0x601E, true, "look-up table end"},      {0x6020, false, "past the look-up table"},
        {0x7006, true, "Gouraud table"},          {0x7020, false, "past the Gouraud table"},
        {sprite + 0x1E, true, "command table"},   {sprite + 0x20, false, "next command table"},
    };
    for (const Case &c : cases) {
        CAPTURE(c.what);
        f.Execute(sprite); // opens a batch (or adds to the open one)
        const uint64_t batches = f.front->Stats().batches;
        const uint64_t hazards = f.front->Stats().batchEnds[static_cast<size_t>(Vdp1BatchEnd::VramHazard)];
        f.WriteVRAM(c.address, 0x1234);
        CHECK(f.front->Stats().batches == batches + (c.splits ? 1 : 0));
        CHECK(f.front->Stats().batchEnds[static_cast<size_t>(Vdp1BatchEnd::VramHazard)] == hazards + (c.splits ? 1 : 0));
    }

    // With hazard tracking off nothing splits
    Fixture g;
    g.front->SetHazardTrackingForTesting(false);
    const uint32_t s2 = g.Sprite(10, 10, 0x4000 / 8, 1, 0x6000 / 8);
    g.Execute(s2);
    g.WriteVRAM(0x4000, 0x1111);
    CHECK(g.front->Stats().batches == 0);
}

TEST_CASE("VDP1 front half: a batch closed by a VRAM hazard uploads the value its commands read", "[vdp1][front]") {
    Fixture f;
    const uint32_t texture = 0x4000;
    ::util::WriteBE<uint16_t>(&f.state->mem1.VRAM[texture], 0xAAAA);
    f.front->PostLoadStateSync(); // the texture was written without the hook
    const uint32_t sprite = f.Sprite(10, 10, texture / 8, 5);
    f.Execute(sprite);
    f.WriteVRAM(0x9000, 0x7777);   // not read by the batch: stays pending, uploaded with this batch
    f.WriteVRAM(texture, 0xBBBB);  // read by the batch: closes it first
    f.Execute(sprite);             // reads the new texel
    const Vdp1OpList ops = f.front->TakeOps();
    REQUIRE(ops.batches.size() == 2);
    CHECK(ops.batches[0].end == Vdp1BatchEnd::VramHazard);
    CHECK(ops.batches[1].end == Vdp1BatchEnd::Take);

    auto uploaded = [&](const Vdp1DrawBatch &batch, uint32_t address, uint16_t &value) {
        for (uint32_t i = 0; i < batch.copyCount; ++i) {
            const Vdp1VramCopy &c = ops.vramCopies[batch.firstCopy + i];
            if (address >= c.vramOffset && address + 2 <= c.vramOffset + c.size) {
                value = ::util::ReadBE<uint16_t>(&ops.arena[c.arenaOffset + address - c.vramOffset]);
                return true;
            }
        }
        return false;
    };
    uint16_t value = 0;
    // The first batch uploads all of VRAM (first batch after PostLoadStateSync) with the old texel and the other write
    uint64_t firstBytes = 0;
    for (uint32_t i = 0; i < ops.batches[0].copyCount; ++i) {
        firstBytes += ops.vramCopies[ops.batches[0].firstCopy + i].size;
    }
    CHECK(firstBytes == vdp::kVDP1VRAMSize);
    REQUIRE(uploaded(ops.batches[0], texture, value));
    CHECK(value == 0xAAAA);
    REQUIRE(uploaded(ops.batches[0], 0x9000, value));
    CHECK(value == 0x7777);
    // The second only the chunk written since, with the new texel
    REQUIRE(ops.batches[1].copyCount == 1);
    CHECK(ops.vramCopies[ops.batches[1].firstCopy].size == 256);
    REQUIRE(uploaded(ops.batches[1], texture, value));
    CHECK(value == 0xBBBB);
    CHECK_FALSE(uploaded(ops.batches[1], 0x9000, value));
}

TEST_CASE("VDP1 front half: batches split at swaps and capacities, not at mode changes", "[vdp1][front]") {
    SECTION("register and enhancement changes between commands keep one batch") {
        Fixture f;
        const uint32_t a = f.Polygon(10, 10, 20);
        f.Execute(a);
        f.state->regs1.WriteTVMR(1); // 8 bpp
        f.Execute(a);
        f.front->SetEnhancements({.transparentMeshes = true});
        f.state->regs1.WriteFBCR(0x0008); // DIE
        f.Execute(a);
        const Vdp1OpList ops = f.front->TakeOps();
        REQUIRE(ops.batches.size() == 1);
        REQUIRE(ops.commands.size() == 3);
        CHECK_FALSE(Vdp1RasPixel8(ops.commands[0].rasterFlags));
        CHECK(Vdp1RasPixel8(ops.commands[1].rasterFlags));
        CHECK_FALSE(Vdp1RasTransparentMeshes(ops.commands[1].rasterFlags));
        CHECK(Vdp1RasTransparentMeshes(ops.commands[2].rasterFlags));
        CHECK(Vdp1RasDblInterlace(ops.commands[2].rasterFlags));
    }
    SECTION("a swap between commands closes the batch") {
        Fixture f;
        const uint32_t a = f.Polygon(10, 10, 20);
        f.Execute(a);
        f.state->fbIndex.Flip();
        f.Execute(a);
        const Vdp1OpList ops = f.front->TakeOps();
        REQUIRE(ops.batches.size() == 2);
        CHECK(ops.batches[0].end == Vdp1BatchEnd::FbSwap);
        CHECK(ops.batches[0].drawFB == 0);
        CHECK(ops.batches[1].drawFB == 1);
    }
    SECTION("capacities split between spans and repeat the command") {
        Fixture f;
        f.front->SetBatchLimitsForTesting({.spans = 7});
        const uint32_t a = f.Polygon(10, 10, 20); // 21 lines
        f.Execute(a);
        const Vdp1OpList ops = f.front->TakeOps();
        REQUIRE(ops.batches.size() == 3);
        for (const Vdp1DrawBatch &b : ops.batches) {
            CHECK(b.spanCount == 7);
            CHECK(b.cmdCount == 1);
            CHECK(ops.spans[b.spanOffset].fragBase == 0);
            CHECK(std::memcmp(&ops.commands[b.cmdOffset], &ops.commands[0], sizeof(Vdp1CommandParams)) == 0);
        }
        CHECK(ops.batches[0].end == Vdp1BatchEnd::Capacity);
        // quad steps continue across the split
        CHECK(Vdp1SpanQuadStep(ops.spans[7].stepTexV) == 7);

        Fixture g;
        g.front->SetBatchLimitsForTesting({.fragments = 100});
        g.Execute(g.Polygon(10, 10, 20)); // 21 lines x 42 slots
        const Vdp1OpList gops = g.front->TakeOps();
        for (const Vdp1DrawBatch &b : gops.batches) {
            CHECK(b.fragmentCount <= 100);
        }
        CHECK(gops.batches.size() == 11);

        Fixture h;
        h.front->SetBatchLimitsForTesting({.commands = 2});
        for (int i = 0; i < 5; ++i) {
            h.Execute(h.Polygon(static_cast<int16_t>(i * 30), 10, 10));
        }
        const Vdp1OpList hops = h.front->TakeOps();
        REQUIRE(hops.batches.size() == 3);
        CHECK(hops.batches[0].cmdCount == 2);
        CHECK(hops.batches[2].cmdCount == 1);
    }
    SECTION("fragment bases are each batch's prefix sums of 2 x Length()") {
        Fixture f;
        f.Execute(f.Polygon(-5, -5, 20)); // clipped at the left and top
        const Vdp1OpList ops = f.front->TakeOps();
        uint32_t base = 0;
        for (uint32_t i = 0; i < ops.batches[0].spanCount; ++i) {
            const Vdp1Span &s = ops.spans[i];
            CHECK(s.fragBase == base);
            vdp::LineStepper<> line{{static_cast<int32_t>(s.x0), static_cast<int32_t>(s.y0)},
                                    {static_cast<int32_t>(s.x1), static_cast<int32_t>(s.y1)}, true};
            line.SystemClip(319, 223);
            base += 2 * line.Length();
        }
        CHECK(ops.batches[0].fragmentCount == base);
    }
}

TEST_CASE("VDP1 front half: doubleV, clip commands, local coordinates and the sprite layer", "[vdp1][front]") {
    SECTION("doubleV is software's at every frame start") {
        for (const bool deinterlace : {false, true}) {
            for (const uint16_t tvmd : {uint16_t{0x8000}, uint16_t{0x8080}, uint16_t{0x80C0}}) {
                for (const uint16_t fbcr : {uint16_t{0}, uint16_t{0x0008}}) {
                    CAPTURE(deinterlace, tvmd, fbcr);
                    Fixture f({.deinterlace = deinterlace}, tvmd);
                    vdp::config::VDP2DebugRender debug{};
                    vdp::config::VDP2AccessPatternsConfig access{};
                    auto sw = std::make_unique<vdp::SoftwareVDPRenderer>(*f.state, debug, access);
                    sw->ConfigureEnhancements({.deinterlace = deinterlace});
                    f.state->regs1.WriteFBCR(fbcr);
                    sw->VDP1BeginFrame();
                    f.front->BeginFrame();
                    auto saved = std::make_unique<ymir::savestate::VDPSaveState::VDPRendererSaveState>();
                    sw->SaveState(*saved);
                    CHECK(f.front->DoubleV() == (saved->vdp1State.doubleV != 0));
                    CHECK(f.front->DoubleV() == (deinterlace && tvmd == 0x80C0 && fbcr == 0));
                }
            }
        }
        Fixture f;
        f.front->SetDoubleV(true);
        CHECK(f.front->DoubleV());
        // A line's Y is doubled (sw:1944-1946), and the command carries doubleV
        const uint32_t line = f.Command({0x0006, 0, 0x00C0, 0x801F, 0, 0, 3, 7, 40, 50, 0, 0, 0, 0, 0, 0});
        f.Execute(line);
        const Vdp1OpList ops = f.front->TakeOps();
        REQUIRE(ops.commands.size() == 1);
        CHECK(ops.commands[0].vertex[1] == 14);
        CHECK(ops.commands[0].vertex[3] == 100);
        CHECK(Vdp1RasDoubleV(ops.commands[0].rasterFlags) == 1);
        CHECK(ops.spans[0].y1 == 100);
    }
    SECTION("clip commands store raw 16-bit values in VDPState::state1; local coordinates sign-extend 13 bits") {
        Fixture f;
        f.Execute(f.Command({0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFFF0, 0xABCD, 0, 0, 0, 0}));
        f.Execute(f.Command({0x0008, 0, 0, 0, 0, 0, 0x8001, 0x7002, 0, 0, 0x1234, 0xFEDC, 0, 0, 0, 0}));
        f.Execute(f.Command({0x000A, 0, 0, 0, 0, 0, 0x1FFF, 0x0FFF, 0, 0, 0, 0, 0, 0, 0, 0}));
        const auto &ctx = f.state->state1;
        CHECK(ctx.sysClipH == 0xFFF0);
        CHECK(ctx.sysClipV == 0xABCD);
        CHECK(ctx.userClipX0 == 0x8001);
        CHECK(ctx.userClipY0 == 0x7002);
        CHECK(ctx.userClipX1 == 0x1234);
        CHECK(ctx.userClipY1 == 0xFEDC);
        CHECK(ctx.localCoordX == -1);
        CHECK(ctx.localCoordY == 0x0FFF);
        f.Execute(f.Polygon(10, 20, 5));
        const Vdp1OpList ops = f.front->TakeOps();
        REQUIRE(ops.commands.size() == 1);
        CHECK(static_cast<int32_t>(ops.commands[0].vertex[0]) == 9);
        CHECK(static_cast<int32_t>(ops.commands[0].vertex[1]) == 20 + 0x0FFF);
        CHECK(ops.commands[0].sysClip == Vdp1Pack16(0xFFF0, 0xABCD));
        CHECK(ops.commands[0].userClip0 == Vdp1Pack16(0x8001, 0x7002));
        CHECK(ops.commands[0].userClip1 == Vdp1Pack16(0x1234, 0xFEDC));
        CHECK(f.front->Stats().commands == 4);
    }
    SECTION("draw commands draw nothing while the sprite layer is disabled") {
        Fixture f;
        f.state->state2.layerEnabled[0] = false;
        f.Execute(f.Polygon(10, 10, 20));
        CHECK_FALSE(f.front->HasPendingOps());
    }
    SECTION("rejected quads and lines get no command or span") {
        Fixture f;
        f.Execute(f.Polygon(-100, 10, 20)); // all of it left of the clip area
        f.Execute(f.Command({0x0006, 0, 0x00C0, 0, 0, 0, 0xFFF0, 5, 0xFFF8, 9, 0, 0, 0, 0, 0, 0}));
        CHECK_FALSE(f.front->HasPendingOps());
        CHECK(f.front->Stats().drawCommands == 0);
    }
}

TEST_CASE("VDP1 front half: the erase push constants are software's region", "[vdp1][front]") {
    Fixture f({.deinterlace = true, .transparentMeshes = true}, 0x80C1);
    auto &regs1 = f.state->regs1;
    regs1.WriteTVMR(0);
    regs1.eraseWriteValueLatch = 0xBEEF;
    regs1.eraseX1Latch = 8;
    regs1.eraseX3Latch = 1016;
    regs1.eraseY1Latch = 10;
    regs1.eraseY3Latch = 511;
    f.front->Erase(0, 480);
    f.front->Erase(37, 480);
    f.state->fbIndex.Flip();
    f.state->regs2.WriteTVMD(0x8000);
    f.state->regs2.LatchTVMD();
    f.front->Erase(1, 224);
    const Vdp1OpList ops = f.front->TakeOps();
    REQUIRE(ops.erases.size() == 3);
    const Vdp1ErasePushConstants &e = ops.erases[0];
    CHECK(e.x1 == 8);
    CHECK(e.x3 == 428);      // HRES bit 0: 428
    CHECK(e.y1 == 20);       // double density: shifted
    CHECK(e.y3 == 480);      // clamped to 480 >> 1, shifted
    CHECK(e.value == 0xBEEF);
    CHECK(e.offsetShift == 9);
    CHECK(e.writeLimit == 0xFFFFFFFFu);
    CHECK(e.flags == Vdp1MakeEraseFlags(1, true, true)); // the display framebuffer is 1 while drawing to 0
    CHECK(ops.erases[1].writeLimit == 38);
    CHECK(ops.erases[2].x3 == 400);
    CHECK(ops.erases[2].y1 == 10);
    CHECK(ops.erases[2].y3 == 224);
    CHECK(ops.erases[2].writeLimit == 2);
    CHECK(ops.erases[2].flags == Vdp1MakeEraseFlags(0, false, true));
    CHECK(Kinds(ops, ops.segments[0]) == std::vector<K>{K::Erase, K::Erase, K::Erase});
}

namespace {

// A polygon-heavy table, as 3D games draw it: small textured quads and Gouraud polygons over the screen
void WriteHeavyTable(vdp::VDPState &state, std::vector<uint32_t> &commands, uint32_t count, uint32_t seed) {
    uint64_t s = seed;
    auto next = [&] {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    };
    for (uint32_t i = 0; i < 0x10000; ++i) {
        state.mem1.VRAM[0x40000 + i] = static_cast<uint8_t>(next() | 0x80);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t address = 0x100 + i * 0x20;
        const int32_t cx = static_cast<int32_t>(next() % 340) - 10, cy = static_cast<int32_t>(next() % 240) - 8;
        const int32_t r = 4 + static_cast<int32_t>(next() % 28);
        auto u = [](int32_t v) { return static_cast<uint16_t>(v); };
        const int32_t dx = static_cast<int32_t>(next() % 9) - 4, dy = static_cast<int32_t>(next() % 9) - 4;
        const bool textured = next() % 3 == 0;
        const uint16_t ctrl = textured ? 0x0002 : 0x0004;
        const uint16_t pmod = textured ? 0x00EC : 0x00C4; // RGB texels / Gouraud
        const std::array<uint16_t, 16> words{
            ctrl, 0, pmod, static_cast<uint16_t>(0x8000 | (next() & 0x7FFF)), static_cast<uint16_t>(0x40000 / 8), 0x0410,
            u(cx - r), u(cy - r + dy), u(cx + r + dx), u(cy - r), u(cx + r), u(cy + r - dy), u(cx - r - dx), u(cy + r),
            static_cast<uint16_t>(0x7F000 / 8), 0};
        for (uint32_t w = 0; w < 16; ++w) {
            ::util::WriteBE<uint16_t>(&state.mem1.VRAM[address + w * 2], words[w]);
        }
        commands.push_back(address);
    }
}

} // namespace

TEST_CASE("VDP1 front half: CPU time per span on a polygon-heavy table", "[vdp1][front][perf][.]") {
    // Run in a Release build: brimir_tests "[vdp1][front][perf]". Prints the front half's ns per span and the software
    // renderer's time for the same commands (which also rasterizes).
    auto state = std::make_unique<vdp::VDPState>();
    state->state2.layerEnabled[0] = true;
    state->state1.sysClipH = 319;
    state->state1.sysClipV = 223;
    state->state1.userClipX1 = 319;
    state->state1.userClipY1 = 223;
    std::vector<uint32_t> commands;
    WriteHeavyTable(*state, commands, 3000, 7);
    constexpr int kFrames = 60;

    Vdp1FrontEnd front{*state};
    front.TakeOps(); // the first batch's full VRAM upload aside
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    uint64_t spans = 0;
    for (int frame = 0; frame < kFrames; ++frame) {
        front.BeginFrame();
        for (const uint32_t address : commands) {
            front.ExecuteCommand(address, vdp::VDP1Command::Control{
                                              .u16 = ::util::ReadBE<uint16_t>(&state->mem1.VRAM[address])});
        }
        front.MarkFrameBoundary();
        const Vdp1OpList ops = front.TakeOps();
        spans += ops.spans.size();
    }
    const double frontNs = std::chrono::duration<double, std::nano>(clock::now() - t0).count();

    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto sw = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
    const auto t1 = clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        sw->VDP1BeginFrame();
        for (const uint32_t address : commands) {
            sw->VDP1ExecuteCommand(address, vdp::VDP1Command::Control{
                                                .u16 = ::util::ReadBE<uint16_t>(&state->mem1.VRAM[address])});
        }
        sw->VDP1EndFrame();
    }
    const double swNs = std::chrono::duration<double, std::nano>(clock::now() - t1).count();

    std::printf("VDP1 front half: %d frames x %zu commands, %.0f spans/frame, %.1f ns/span, %.3f ms/frame; "
                "software %.3f ms/frame\n",
                kFrames, commands.size(), static_cast<double>(spans) / kFrames, frontNs / static_cast<double>(spans),
                frontNs / kFrames / 1e6, swNs / kFrames / 1e6);
    CHECK(spans > 0);
}
