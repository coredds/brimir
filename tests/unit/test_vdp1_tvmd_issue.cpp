// VDP1 drawing and erases use the VDP2 screen mode (TVMD: double density, horizontal
// resolution; for the erase also the vertical resolution) as the emulator thread had it when it issued them, which is
// what the unthreaded renderer reads. Upstream read the VDP2 render thread's register copy (threaded VDP2) or the
// emulator thread's registers at processing time (threaded VDP1), so the result depended on the threads' timing
// (BRIMIR_FORK.md, 2026-11-15).
//
// The threaded-VDP2 cases are deterministic: TVMD is changed in VDPState directly, not through the renderer, so the
// VDP2 render thread's copy keeps the old mode for good, and a renderer that reads that copy always draws differently.
// The threaded-VDP1 command and erase cases with unthreaded VDP2 change TVMD right after issuing the command: they
// catch a renderer that reads it at processing time only when the render thread is slower.

#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

using namespace ymir;

constexpr uint16 kTVMDProgressive = 0x8000;    // DISP, non-interlaced, 320 wide
constexpr uint16 kTVMDDoubleDensity = 0x80C0;  // DISP, double-density interlace, 320 wide
constexpr uint16 kTVMDDoubleDensityHi = 0x80C1; // ... 352 wide (HRESOn bit 0: erase width limit 428)
constexpr uint16 kFBCR_DIE = 1u << 3;
constexpr uint32 kFBRowBytes = 512 * sizeof(uint16);

struct Mode {
    bool threadedVDP1;
    bool threadedVDP2;
};

// A software renderer with the deinterlace enhancement on a fresh VDP state. The state is set up before the threads
// start (they copy it).
struct Rig {
    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<vdp::SoftwareVDPRenderer> renderer;

    explicit Rig(uint16 tvmd) {
        state->Reset(true);
        renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
        renderer->ConfigureEnhancements({.deinterlace = true});
        state->regs2.WriteTVMD(tvmd);
        state->regs2.LatchTVMD();
        state->state1.sysClipH = 319;
        state->state1.sysClipV = 255;
        state->state2.layerEnabled[0] = true;
    }

    void Start(Mode mode) {
        renderer->PostLoadStateSync();
        renderer->EnableThreadedVDP2(mode.threadedVDP2);
        renderer->EnableThreadedVDP1(mode.threadedVDP1);
    }

    ~Rig() {
        renderer->EnableThreadedVDP1(false);
        renderer->EnableThreadedVDP2(false);
    }

    // TVMD as the game CPUs see it, without telling the renderer (so the VDP2 render thread's copy does not change)
    void SetTVMD(uint16 tvmd) {
        state->regs2.WriteTVMD(tvmd);
    }

    void Execute(uint32 cmdAddress) {
        const vdp::VDP1Command::Control control{.u16 = state->mem1.ReadVRAM<uint16>(cmdAddress)};
        renderer->VDP1ExecuteCommand(cmdAddress, control);
    }
};

struct Framebuffers {
    std::vector<uint8> main;
    std::vector<uint8> alt;

    bool operator==(const Framebuffers &) const = default;
};

Framebuffers Take(Rig &rig, uint8 fbIndex) {
    rig.renderer->PreSaveStateSync();
    const auto &main = rig.state->mem1.FBRAM[fbIndex];
    const auto &alt = rig.renderer->GetAltFBRAM(fbIndex);
    return {{main.begin(), main.end()}, {alt.begin(), alt.end()}};
}

// Offset of the first differing byte, -1 if equal (Catch2 would print the whole framebuffers)
long FirstDifference(const std::vector<uint8> &a, const std::vector<uint8> &b) {
    if (a.size() != b.size()) {
        return 0;
    }
    const auto it = std::mismatch(a.begin(), a.end(), b.begin()).first;
    return it == a.end() ? -1 : static_cast<long>(it - a.begin());
}

uint16 Word(const std::vector<uint8> &fb, uint32 x, uint32 row) {
    return util::ReadBE<uint16>(&fb[row * kFBRowBytes + x * 2]);
}

void WriteCommand(vdp::VDPState &state, uint32 address, std::initializer_list<uint16> words) {
    uint32 offset = 0;
    for (uint16 word : words) {
        state.mem1.WriteVRAM<uint16>(address + offset, word);
        offset += 2;
    }
}

// One command of each kind that reads TVMD (line, polyline, polygon, normal sprite: VDP1Cmd_DrawLine,
// VDP1Cmd_DrawPolylines, VDP1Cmd_DrawPolygon, VDP1PlotTexturedQuad), each at odd rows: with double density and the
// deinterlace enhancement an odd row goes to the alternate field's framebuffer, without it to the main one. FBCR.DIE
// is set so that VDP1BeginFrame does not double the coordinates and both cases land on row y / 2.
constexpr uint32 kLine = 0x100, kPolyline = 0x120, kPolygon = 0x140, kSprite = 0x160, kTexture = 0x1000;

void SetUpCommands(Rig &rig) {
    auto &s = *rig.state;
    s.regs1.WriteFBCR(kFBCR_DIE);
    //                       CTRL    LINK PMOD    COLR    SRCA            SIZE        XA  YA  XB  YB  XC  YC  XD  YD
    WriteCommand(s, kLine, {0x0006, 0, 0x00C0, 0x83E0, 0, 0, 10, 41, 60, 41});
    WriteCommand(s, kPolyline, {0x0005, 0, 0x00C0, 0xFC00, 0, 0, 70, 43, 90, 43, 90, 47, 70, 47});
    WriteCommand(s, kPolygon, {0x0004, 0, 0x00C0, 0x801F, 0, 0, 100, 51, 120, 51, 120, 55, 100, 55});
    WriteCommand(s, kSprite, {0x0000, 0, 0x00E8, 0x0000, kTexture >> 3, (1 << 8) | 4, 130, 61});
    for (uint32 i = 0; i < 8 * 4; ++i) {
        s.mem1.WriteVRAM<uint16>(kTexture + i * 2, static_cast<uint16>(0x8000 | (i * 0x0421)));
    }
}

// Draws the four commands with double density at issue time. `after` is the TVMD the emulator thread changes to right
// after issuing each command (as a game switching modes would), before the renderer may have drawn it.
Framebuffers DrawCommands(Mode mode, uint16 after) {
    Rig rig{kTVMDProgressive};
    SetUpCommands(rig);
    rig.Start(mode);
    rig.renderer->VDP1BeginFrame();
    for (uint32 cmd : {kLine, kPolyline, kPolygon, kSprite}) {
        rig.SetTVMD(kTVMDDoubleDensity);
        rig.Execute(cmd);
        rig.SetTVMD(after);
    }
    rig.renderer->VDP1EndFrame();
    return Take(rig, rig.state->fbIndex.draw);
}

// Erases the display framebuffer with double density and the 352-wide mode at issue time (height limit 480 / 2 lines
// doubled, width limit 428), then switches to the progressive 320 x 224 mode (height limit 224, width limit 400, no
// alternate-field mirror).
Framebuffers Erase(Mode mode) {
    Rig rig{kTVMDProgressive};
    auto &regs1 = rig.state->regs1;
    regs1.eraseWriteValueLatch = 0x1234;
    regs1.eraseX1Latch = 0;
    regs1.eraseY1Latch = 0;
    regs1.eraseX3Latch = 440;
    regs1.eraseY3Latch = 120;
    rig.renderer->VDP2SetResolution(352, 480, false);
    rig.Start(mode);
    rig.SetTVMD(kTVMDDoubleDensityHi);
    rig.renderer->VDP1EraseFramebuffer(0);
    rig.SetTVMD(kTVMDProgressive);
    rig.renderer->VDP2SetResolution(320, 224, false);
    return Take(rig, rig.state->fbIndex.display);
}

const Mode kModes[] = {
    {.threadedVDP1 = false, .threadedVDP2 = true},
    {.threadedVDP1 = true, .threadedVDP2 = true},
    {.threadedVDP1 = true, .threadedVDP2 = false},
};

} // namespace

TEST_CASE("VDP1 commands draw with the screen mode the emulator thread had when they were issued",
          "[vdp][vdp1][threaded][tvmd][brimir]") {
    const uint16 after = GENERATE(kTVMDProgressive, kTVMDDoubleDensity);
    const auto expected = DrawCommands({.threadedVDP1 = false, .threadedVDP2 = false}, after);
    // Non-vacuous: the odd rows went to the alternate field (row y / 2), not to the main framebuffer
    REQUIRE(Word(expected.alt, 10, 20) == 0x83E0);  // line, y 41
    REQUIRE(Word(expected.main, 10, 20) == 0x0000);
    REQUIRE(Word(expected.alt, 70, 21) == 0xFC00);  // polyline, y 43
    REQUIRE(Word(expected.alt, 110, 25) == 0x801F); // polygon, y 51..55: odd rows to the alternate field
    REQUIRE(Word(expected.main, 110, 25) == 0x0000); // ... y 50 is outside the polygon
    REQUIRE(Word(expected.main, 110, 26) == 0x801F); // ... y 52
    REQUIRE(Word(expected.alt, 130, 30) != 0x0000);  // sprite, y 61..64
    REQUIRE(Word(expected.main, 130, 30) == 0x0000); // ... y 60 is outside the sprite
    REQUIRE(Word(expected.main, 130, 31) != 0x0000); // ... y 62
    for (const Mode mode : kModes) {
        CAPTURE(mode.threadedVDP1, mode.threadedVDP2, after);
        for (int i = 0; i < 10; ++i) {
            CAPTURE(i);
            const auto got = DrawCommands(mode, after);
            CHECK(FirstDifference(got.main, expected.main) == -1);
            CHECK(FirstDifference(got.alt, expected.alt) == -1);
        }
    }
}

TEST_CASE("VDP1 erases use the screen mode and resolution the emulator thread had when they were issued",
          "[vdp][vdp1][threaded][tvmd][brimir]") {
    const auto expected = Erase({.threadedVDP1 = false, .threadedVDP2 = false});
    // Non-vacuous: double density doubles the 120 lines (rows up to 240) and mirrors into the alternate field; the
    // 352-wide mode lets the erase reach x 427
    REQUIRE(Word(expected.main, 0, 200) == 0x1234);
    REQUIRE(Word(expected.alt, 0, 200) == 0x1234);
    REQUIRE(Word(expected.main, 420, 10) == 0x1234);
    REQUIRE(Word(expected.main, 430, 10) == 0x0000);
    for (const Mode mode : kModes) {
        CAPTURE(mode.threadedVDP1, mode.threadedVDP2);
        for (int i = 0; i < 10; ++i) {
            CAPTURE(i);
            const auto got = Erase(mode);
            CHECK(FirstDifference(got.main, expected.main) == -1);
            CHECK(FirstDifference(got.alt, expected.alt) == -1);
        }
    }
}
