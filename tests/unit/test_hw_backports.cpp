#include "catch_amalgamated.hpp"

#include <ymir/hw/scsp/scsp_slot.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/saturn.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace {

using namespace ymir;

constexpr uint32 kSMPCCOMREG = 0x010'001F;
constexpr uint32 kSMPCSF = 0x010'0063;
constexpr uint32 kVDP1PTMR = 0x5D0'0004;
constexpr uint32 kVDP1COPR = 0x5D0'0014;

constexpr uint8 kCmdSSHON = 0x02;

// Advances the system (via the master SH-2 step loop, which also ticks the scheduler) until the SMPC command event
// has had enough time to fire.
void RunSMPCCommand(Saturn &saturn, uint8 command) {
    saturn.mainBus.Write<uint8>(kSMPCCOMREG, command);
    uint64 cycles = 0;
    while (cycles < 20000) {
        cycles += saturn.StepMasterSH2();
    }
}

// Parks the slave SH-2 in a `bra $; nop` loop in high WRAM so its PC stays within [kLoopAddr, kLoopAddr + 4) while it
// runs. A reset reloads the PC from the (empty) IPL vector table, moving it out of that range.
constexpr uint32 kLoopAddr = 0x0600'4000;

void ParkSlaveSH2(Saturn &saturn) {
    saturn.mainBus.Write<uint16>(kLoopAddr + 0, 0xAFFE); // bra $
    saturn.mainBus.Write<uint16>(kLoopAddr + 2, 0x0009); // nop (delay slot)
    saturn.slaveSH2.GetProbe().PC() = kLoopAddr;
}

bool SlaveSH2Parked(Saturn &saturn) {
    const uint32 pc = saturn.slaveSH2.GetProbe().PC();
    return pc >= kLoopAddr && pc < kLoopAddr + 8;
}

} // namespace

TEST_CASE("SMPC SSHON resets the slave SH-2 when it is disabled", "[smpc][backport]") {
    auto saturn = std::make_unique<Saturn>();
    saturn->slaveSH2Enabled = false;
    ParkSlaveSH2(*saturn);

    RunSMPCCommand(*saturn, kCmdSSHON);

    CHECK(saturn->slaveSH2Enabled);
    CHECK_FALSE(SlaveSH2Parked(*saturn));
}

TEST_CASE("SMPC SSHON is ignored when the slave SH-2 is already enabled", "[smpc][backport]") {
    // Ymir 184d2fbc: fixes Guardian Heroes level transitions, Madden NFL 97 (EU), Ten Pin Alley, etc.
    auto saturn = std::make_unique<Saturn>();
    saturn->slaveSH2Enabled = true;
    ParkSlaveSH2(*saturn);

    RunSMPCCommand(*saturn, kCmdSSHON);

    CHECK(saturn->slaveSH2Enabled);
    CHECK(SlaveSH2Parked(*saturn));
}

TEST_CASE("VDP1 frame start preserves COPR", "[vdp][vdp1][backport]") {
    // Ymir 12ef24fb: fixes lockups in Alone in the Dark - One-Eyed Jack's Revenge.
    auto saturn = std::make_unique<Saturn>();

    auto state = std::make_unique<savestate::SaveState>();
    saturn->SaveState(*state);
    state->vdp.regs1.COPR = 0x1234;
    REQUIRE(saturn->LoadState(*state, true));
    REQUIRE(saturn->mainBus.Peek<uint16>(kVDP1COPR) == 0x1234);

    // PTM=01: start drawing immediately
    saturn->mainBus.Write<uint16>(kVDP1PTMR, 0x0001);

    CHECK(saturn->mainBus.Peek<uint16>(kVDP1COPR) == 0x1234);
}

TEST_CASE("VDP2 VCNT is restored from save states", "[vdp][vdp2][savestate][backport]") {
    // Ymir 49ee054d: VCNT was loaded into the latch instead of the counter.
    auto saturn = std::make_unique<Saturn>();

    auto state = std::make_unique<savestate::SaveState>();
    saturn->SaveState(*state);
    state->vdp.regs2.VCNT = 123;
    state->vdp.regs2.VCNTLatch = 0x3FF;
    REQUIRE(saturn->LoadState(*state, true));

    const auto &regs2 = saturn->VDP.GetProbe().GetVDP2Regs();
    CHECK(regs2.VCNT == 123);
    CHECK(regs2.VCNTLatch == 0x3FF);
}

namespace {

struct LineCapture {
    std::array<uint32, vdp::kMaxResH> line{};
    uint32 width = 0;

    void Bind(vdp::SoftwareVDPRenderer &renderer) {
        renderer.SwCallbacks.FrameComplete.Rebind(this, [](uint32 *fb, uint32 w, uint32 h, void *context) {
            auto &out = *static_cast<LineCapture *>(context);
            std::copy_n(fb, w, out.line.begin());
            out.width = w;
        });
    }
};

// Writes an identity rotation parameter table: screen coordinate (x, y) maps to bitmap coordinate (x, y).
void WriteIdentityRotParamTable(vdp::VDPState &state, uint32 address) {
    auto write32 = [&](uint32 offset, uint32 value) { state.mem2.WriteVRAM<uint32>(address + offset, value); };
    constexpr uint32 kOne10 = (1u << 10) << 6; // 1.0 in x.10 fixed point, stored from bit 6
    write32(0x10, kOne10);                     // deltaYst
    write32(0x14, kOne10);                     // deltaX
    write32(0x1C, kOne10);                     // A
    write32(0x2C, kOne10);                     // E
    write32(0x4C, 1u << 16);                   // kx = 1.0 (8.16)
    write32(0x50, 1u << 16);                   // ky = 1.0 (8.16)
}

} // namespace

TEST_CASE("RBG0 honors the renderer resolution when TVMD changes mid-frame", "[vdp][vdp2][rbg][backport]") {
    // Ymir 3d504d53: the RBG line buffers are sized for normal horizontal resolution. Deriving the double-resolution
    // flag from TVMD instead of the active renderer resolution overran them when the resolution changed mid-frame.
    using namespace ymir::vdp;

    auto state = std::make_unique<VDPState>();
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<SoftwareVDPRenderer>(*state, debug, access);

    auto &regs = state->regs2;
    regs.WriteTVMD(0x8000); // DISP on, 320 pixels wide
    regs.LatchTVMD();
    regs.WriteRAMCTL(0x010C);   // Partition VRAM-A; A1 holds RBG0 character (bitmap) data
    regs.WriteBGON(0x0010);     // RBG0 on
    regs.WriteCHCTLB(0x3200);   // RBG0 bitmap, 512x256, RGB555
    regs.WriteMPOFR(0x0001);    // RBG0 bitmap at 0x20000
    regs.WritePRIR(0x0007);     // RBG0 priority 7
    regs.WriteRPTAU(0x0000);    // Rotation parameter table at 0x0
    regs.WriteRPTAL(0x0000);
    WriteIdentityRotParamTable(*state, 0x0);

    for (uint32 x = 0; x < 512; ++x) {
        state->mem2.WriteVRAM<uint16>(0x20000 + x * 2, static_cast<uint16>(0x8000 | (x & 0x7FFF)));
    }

    LineCapture capture;
    renderer->PostLoadStateSync();
    capture.Bind(*renderer);

    // The display switched to a high resolution mode, but TVMD still reads as normal resolution.
    renderer->VDP2SetResolution(640, 224, false);
    renderer->VDP2BeginFrame();
    renderer->VDP2RenderLine(0);
    renderer->VDP2EndFrame();
    REQUIRE(capture.width == 640);

    // Each RBG0 dot must be doubled horizontally to fill the 640-pixel line.
    for (uint32 x = 0; x < 640; x += 2) {
        CAPTURE(x);
        REQUIRE(capture.line[x] == capture.line[x + 1]);
        if (x + 2 < 640) {
            REQUIRE(capture.line[x] != capture.line[x + 2]);
        }
    }
}

TEST_CASE("Threaded VDP1 keeps CPU framebuffer writes made after the draw ends", "[vdp][vdp1][threaded][backport]") {
    // Ymir bcb0f526: with threaded VDP1 rendering, the render thread copies its framebuffer over the main state on
    // EndDraw. CPU writes made after EndDraw was queued (but before the thread processed it) were overwritten and only
    // applied to the render thread's copy. Fixes the glitched title screen in Waialae no Kiseki - Extra 36 Holes.
    using namespace ymir::vdp;

    auto state = std::make_unique<VDPState>();
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<SoftwareVDPRenderer>(*state, debug, access);
    renderer->EnableThreadedVDP1(true);

    constexpr uint32 kAddress = 0x100;
    constexpr uint16 kValue = 0xABCD;

    for (uint32 i = 0; i < 8; ++i) {
        CAPTURE(i);
        const uint16 value = static_cast<uint16>(kValue + i);
        renderer->VDP1EndFrame();
        state->VDP1WriteFB<uint16>(kAddress, value,
                                   [&](uint32 address, uint16 v) { renderer->VDP1WriteFB(address, v); });
        renderer->PreSaveStateSync();
        REQUIRE(state->VDP1ReadFB<uint16>(kAddress) == value);

        // Byte writes take the same path
        const uint8 byteValue = static_cast<uint8>(0x40 + i);
        renderer->VDP1EndFrame();
        state->VDP1WriteFB<uint8>(kAddress + 3, byteValue,
                                  [&](uint32 address, uint8 v) { renderer->VDP1WriteFB(address, v); });
        renderer->PreSaveStateSync();
        REQUIRE(state->VDP1ReadFB<uint8>(kAddress + 3) == byteValue);
    }

    renderer->EnableThreadedVDP1(false);
}

namespace {

struct ThreadedVDP1EraseFixture {
    static constexpr uint32 kTVMR = 0x00;
    static constexpr uint32 kEWDR = 0x06;
    static constexpr uint32 kEWLR = 0x08;
    static constexpr uint32 kEWRR = 0x0A;

    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<vdp::SoftwareVDPRenderer> renderer;

    ThreadedVDP1EraseFixture() {
        renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
        renderer->EnableThreadedVDP1(true);
    }

    ~ThreadedVDP1EraseFixture() {
        renderer->EnableThreadedVDP1(false);
    }

    // Sets pixel 0 of the display framebuffer in both the render thread and the main state.
    void FillDisplayFBPixel0(uint16 value) {
        state->displayFB = 1; // CPU writes go to the draw framebuffer, which is FB 0 while FB 1 is displayed
        renderer->VDP1WriteFB(0, value);
        renderer->PreSaveStateSync();
        state->displayFB = 0;
    }

    // Erase window covering X = 0..7 on row y
    void WriteEraseWindow(uint16 y) {
        renderer->VDP1WriteReg(kEWLR, y);
        renderer->VDP1WriteReg(kEWRR, (1 << 9) | y);
    }

    // Latches the render thread's erase parameters and erases the display framebuffer.
    void SwapAndErase() {
        renderer->VDP1SwapFramebuffer();
        renderer->VDP1EraseFramebuffer(0);
        renderer->PreSaveStateSync();
    }

    uint16 ReadDisplayFB(uint32 address) const {
        return util::ReadBE<uint16>(&state->spriteFB[state->displayFB][address]);
    }
};

} // namespace

TEST_CASE("Threaded VDP1 erases with the render thread's latched parameters", "[vdp][vdp1][threaded][backport]") {
    // Ymir ce270c04: the render thread must latch its own erase parameters on SwapBuffers and erase using its own
    // register state, not the main state's (which is only updated by the emulator thread).
    ThreadedVDP1EraseFixture f;

    f.FillDisplayFBPixel0(0xFFFF);
    f.renderer->VDP1WriteReg(ThreadedVDP1EraseFixture::kEWDR, 0x1234);
    f.WriteEraseWindow(0);

    auto &regs1 = f.state->regs1;
    regs1.eraseWriteValueLatch = 0x5555;
    regs1.eraseX1Latch = 0;
    regs1.eraseY1Latch = 0;
    regs1.eraseX3Latch = 8;
    regs1.eraseY3Latch = 1;

    f.SwapAndErase();
    CHECK(f.ReadDisplayFB(0) == 0x1234);
}

TEST_CASE("Threaded VDP1 reset clears the render thread's registers", "[vdp][vdp1][threaded][backport]") {
    // Ymir 44bd8ea4: SoftwareVDPRenderer::Reset must reset the render thread's register copy too. The erase row offset
    // depends on TVMR, which is reset (the erase parameters themselves are not touched by VDP1Regs::Reset).
    ThreadedVDP1EraseFixture f;

    // TVM=3 (8-bit rotation mode) erases rows at 256-pixel strides instead of 512
    f.renderer->VDP1WriteReg(ThreadedVDP1EraseFixture::kTVMR, 0x0003);
    f.renderer->Reset(true);
    f.renderer->VDP1WriteReg(ThreadedVDP1EraseFixture::kEWDR, 0x1234);
    f.WriteEraseWindow(1);

    f.SwapAndErase();
    CHECK(f.ReadDisplayFB(512 * sizeof(uint16)) == 0x1234); // row 1 with the default 512-pixel stride
    CHECK(f.ReadDisplayFB(256 * sizeof(uint16)) == 0x0000);
}

TEST_CASE("VDP soft reset keeps the VDP1 command address", "[vdp][vdp1][backport]") {
    // Ymir 44bd8ea4: only a hard reset clears the VDP1 command addresses.
    auto state = std::make_unique<vdp::VDPState>();

    state->regs1.currCommandAddress = 0x100;
    state->regs1.prevCommandAddress = 0x80;
    state->Reset(false);
    CHECK(state->regs1.currCommandAddress == 0x100);
    CHECK(state->regs1.prevCommandAddress == 0x80);

    state->Reset(true);
    CHECK(state->regs1.currCommandAddress == 0);
    CHECK(state->regs1.prevCommandAddress == 0);
}

namespace {

struct FrameCapture {
    std::array<uint32, vdp::kMaxResH * 4> pixels{};
    uint32 width = 0;

    void Bind(vdp::SoftwareVDPRenderer &renderer) {
        renderer.SwCallbacks.FrameComplete.Rebind(this, [](uint32 *fb, uint32 w, uint32 h, void *context) {
            auto &out = *static_cast<FrameCapture *>(context);
            std::copy_n(fb, w * 4, out.pixels.begin());
            out.width = w;
        });
    }

    uint32 At(uint32 x, uint32 y) const {
        return pixels[y * width + x];
    }
};

uint32 RGB555to888(uint16 color) {
    uint32 out = 0xFF000000u;
    for (uint32 channel = 0; channel < 3; ++channel) {
        out |= (((color >> (channel * 5)) & 31u) << 3u) << (channel * 8);
    }
    return out;
}

struct BackScreenFixture {
    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<vdp::SoftwareVDPRenderer> renderer =
        std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
    FrameCapture capture;

    BackScreenFixture(uint16 tvmd) {
        auto &regs = state->regs2;
        regs.WriteTVMD(tvmd);
        regs.LatchTVMD();
        regs.WriteBGON(0);
        renderer->PostLoadStateSync();
        capture.Bind(*renderer);
        renderer->VDP2SetResolution(320, 224, false);
        renderer->VDP2BeginFrame();
    }

    void EndFrame() {
        renderer->VDP2EndFrame();
        REQUIRE(capture.width == 320);
    }
};

} // namespace

TEST_CASE("VDP2 back screen table address changes apply mid-frame", "[vdp][vdp2][lncl-back][backport]") {
    // Ymir f9f47595: single-color back/line screens were fetched on line 0 only, ignoring table address changes made
    // later in the frame.
    BackScreenFixture fx{0x8000};
    auto &regs = fx.state->regs2;
    fx.state->mem2.WriteVRAM<uint16>(0x100, 0x001F);
    fx.state->mem2.WriteVRAM<uint16>(0x200, 0x7C00);

    regs.WriteBKTAU(0x0000); // single color
    regs.WriteBKTAL(0x0080); // 0x100
    fx.renderer->VDP2RenderLine(0);
    regs.WriteBKTAL(0x0100); // 0x200
    fx.renderer->VDP2RenderLine(1);
    fx.EndFrame();

    CHECK(fx.capture.At(0, 0) == RGB555to888(0x001F));
    CHECK(fx.capture.At(0, 1) == RGB555to888(0x7C00));
}

TEST_CASE("VDP2 per-line back screen advances through the back screen table", "[vdp][vdp2][lncl-back][backport]") {
    // Guards the intermediate regression in Ymir f9f47595 (fixed in 4ec1e59d) where the per-line offset was applied to
    // the line color address instead of the back screen address.
    BackScreenFixture fx{0x8000};
    auto &regs = fx.state->regs2;
    constexpr std::array<uint16, 3> colors{0x001F, 0x03E0, 0x7C00};
    for (uint32 y = 0; y < colors.size(); ++y) {
        fx.state->mem2.WriteVRAM<uint16>(0x100 + y * 2, colors[y]);
    }

    regs.WriteBKTAU(0x8000); // per-line
    regs.WriteBKTAL(0x0080); // 0x100
    for (uint32 y = 0; y < colors.size(); ++y) {
        fx.renderer->VDP2RenderLine(y);
    }
    fx.EndFrame();

    for (uint32 y = 0; y < colors.size(); ++y) {
        CAPTURE(y);
        CHECK(fx.capture.At(0, y) == RGB555to888(colors[y]));
    }
}

TEST_CASE("VDP2 stops updating the back screen once DISP is cleared mid-frame", "[vdp][vdp2][lncl-back][backport]") {
    // Ymir 49df0c2e: once the display is disabled, the border shows the last fetched back screen color for the rest of
    // the frame.
    BackScreenFixture fx{0x8100}; // DISP on, BDCLMD = back screen color
    auto &regs = fx.state->regs2;
    constexpr std::array<uint16, 3> colors{0x001F, 0x03E0, 0x7C00};
    for (uint32 y = 0; y < colors.size(); ++y) {
        fx.state->mem2.WriteVRAM<uint16>(0x100 + y * 2, colors[y]);
    }

    regs.WriteBKTAU(0x8000); // per-line
    regs.WriteBKTAL(0x0080); // 0x100
    fx.renderer->VDP2RenderLine(0);
    regs.WriteTVMD(0x0100); // DISP off, keep BDCLMD
    fx.renderer->VDP2RenderLine(1);
    fx.renderer->VDP2RenderLine(2);
    fx.EndFrame();

    CHECK(fx.capture.At(0, 0) == RGB555to888(colors[0]));
    CHECK(fx.capture.At(0, 1) == RGB555to888(colors[0]));
    CHECK(fx.capture.At(0, 2) == RGB555to888(colors[0]));
}

namespace {

// The threaded VDP2 equivalence tests loop because the races they cover are timing-dependent.
constexpr int kThreadedEquivalenceIterations = 50;

// Builds a VDP state with setup(state), renders a frame with script(state, renderer) with or without the threaded VDP2
// renderer, and returns the captured pixels.
std::vector<uint32> RenderScript(bool threaded, auto setup, auto script) {
    auto state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
    FrameCapture capture;

    setup(*state);
    renderer->PostLoadStateSync();
    renderer->EnableThreadedVDP2(threaded);
    capture.Bind(*renderer);
    script(*state, *renderer);
    renderer->EnableThreadedVDP2(false);

    return {capture.pixels.begin(), capture.pixels.begin() + capture.width * 4};
}

// Renders the script with the threaded VDP2 renderer repeatedly and counts the frames that differ from expected.
int CountThreadedMismatches(auto setup, auto script, const std::vector<uint32> &expected) {
    int mismatches = 0;
    for (int i = 0; i < kThreadedEquivalenceIterations; ++i) {
        if (RenderScript(true, setup, script) != expected) {
            ++mismatches;
        }
    }
    return mismatches;
}

// Single-color back screen.
void SetupBackScreen(vdp::VDPState &state) {
    auto &regs = state.regs2;
    regs.WriteTVMD(0x8000); // DISP on, 320 pixels wide
    regs.LatchTVMD();
    regs.WriteBGON(0);
    state.mem2.WriteVRAM<uint16>(0x100, 0x7C1F);
    regs.WriteBKTAU(0x0000); // single color
    regs.WriteBKTAL(0x0080); // 0x100
}

// Sprite layer (type 0, all priorities 7, palette data) over a display framebuffer filled with distinct 16-bit words
// and a CRAM filled with distinct colors.
void SetupSpriteLayer(vdp::VDPState &state) {
    auto &regs = state.regs2;
    regs.WriteTVMD(0x8000); // DISP on, 320 pixels wide
    regs.LatchTVMD();
    regs.WriteBGON(0);
    regs.WriteSPCTL(0x0000);
    regs.WritePRISA(0x0707);
    regs.WritePRISB(0x0707);
    regs.WritePRISC(0x0707);
    regs.WritePRISD(0x0707);
    for (uint32 i = 0; i < 1024; ++i) {
        state.mem2.WriteCRAM<uint16>(i * sizeof(uint16), static_cast<uint16>(i * 31 + 1) & 0x7FFF);
    }
    auto &fb = state.spriteFB[state.displayFB];
    for (uint32 i = 0; i < fb.size() / sizeof(uint16); ++i) {
        util::WriteBE<uint16>(&fb[i * sizeof(uint16)], static_cast<uint16>(0x400 | (i & 0x3FF)));
    }
}

} // namespace

TEST_CASE("Threaded VDP2 applies a resolution change after earlier lines", "[vdp][vdp2][threaded][backport]") {
    // Ymir 5fab3c3d: the resolution change must reach the VDP2 render thread in order with the lines it renders,
    // instead of changing the renderer's resolution from the emulator thread while earlier lines are still pending.
    auto script = [](vdp::VDPState &, vdp::SoftwareVDPRenderer &renderer) {
        renderer.VDP2SetResolution(320, 224, false);
        renderer.VDP2BeginFrame();
        renderer.VDP2RenderLine(0);
        renderer.VDP2SetResolution(640, 224, false);
        renderer.VDP2RenderLine(1);
        renderer.VDP2EndFrame();
    };

    const auto expected = RenderScript(false, SetupBackScreen, script);
    REQUIRE(expected.size() == 640 * 4);
    CHECK(CountThreadedMismatches(SetupBackScreen, script, expected) == 0);
}

TEST_CASE("Threaded VDP2 sees VDP1 TVMR changes in order", "[vdp][vdp2][threaded][backport]") {
    // Ymir 23b3dedd: the VDP2 render thread read TVMR from the VDP1 registers as they were at render time instead of
    // when the line was queued. Fixes a single-frame glitch in Virtua Fighter 2 after selecting characters.
    constexpr uint32 kWidth = 320;

    auto renderTwoLines = [](uint16 tvmr0, uint16 tvmr1) {
        return [=](vdp::VDPState &state, vdp::SoftwareVDPRenderer &renderer) {
            state.regs1.WriteTVMR(tvmr0);
            renderer.VDP1WriteReg(0x00, tvmr0);
            renderer.VDP2SetResolution(kWidth, 224, false);
            renderer.VDP2BeginFrame();
            renderer.VDP2RenderLine(0);
            state.regs1.WriteTVMR(tvmr1);
            renderer.VDP1WriteReg(0x00, tvmr1);
            renderer.VDP2RenderLine(1);
            renderer.VDP2EndFrame();
        };
    };
    auto line = [&](const std::vector<uint32> &pixels, uint32 y) {
        return std::vector<uint32>(pixels.begin() + y * kWidth, pixels.begin() + (y + 1) * kWidth);
    };

    const auto readout16 = RenderScript(false, SetupSpriteLayer, renderTwoLines(0, 0));
    const auto readout8 = RenderScript(false, SetupSpriteLayer, renderTwoLines(1, 1));
    const auto script = renderTwoLines(0, 1);
    const auto expected = RenderScript(false, SetupSpriteLayer, script);
    REQUIRE(expected.size() == kWidth * 4);
    REQUIRE(line(readout16, 1) != line(readout8, 1));
    CHECK(line(expected, 0) == line(readout16, 0));
    CHECK(line(expected, 1) == line(readout8, 1));

    CHECK(CountThreadedMismatches(SetupSpriteLayer, script, expected) == 0);
}

TEST_CASE("VDP2 EXTEN read latches HCNT when external latch is disabled", "[vdp][vdp2][hcnt][backport]") {
    // Ymir f9f47595: reading EXTEN with EXLTEN=0 latches the H/V counters. HCNT is approximated near the end of the
    // active display area.
    using namespace ymir::vdp;
    VDP2Regs regs{};
    regs.Reset();

    constexpr std::array<uint16, 4> hRes{320, 352, 640, 704};
    for (uint16 mode = 0; mode < 4; ++mode) {
        CAPTURE(mode);
        regs.WriteTVMD(0x8000 | mode);
        regs.HCNT = 0;
        (void)regs.ReadEXTEN<false>();
        CHECK(regs.HCNT == hRes[mode] - 50);

        // Peeking must not latch
        regs.HCNT = 0;
        (void)regs.ReadEXTEN<true>();
        CHECK(regs.HCNT == 0);
    }
}

TEST_CASE("SMPC INTBACK status report sets the reserved port mode bits", "[smpc][backport]") {
    // Ymir b3e6fc64: the unused port mode bits in the INTBACK status report read back as 11 on hardware.
    auto saturn = std::make_unique<Saturn>();
    saturn->mainBus.Write<uint8>(0x010'0001, 0x01); // IREG0: status report
    saturn->mainBus.Write<uint8>(0x010'0003, 0x00); // IREG1: no peripheral data
    saturn->mainBus.Write<uint8>(0x010'0005, 0xF0); // IREG2

    RunSMPCCommand(*saturn, 0x10); // INTBACK

    CHECK((saturn->mainBus.Peek<uint8>(0x010'0061) & 0x0F) == 0x0F);
}

namespace {

uint64 CyclesUntilSFClears(Saturn &saturn, uint8 command) {
    saturn.mainBus.Write<uint8>(kSMPCSF, 1);
    saturn.mainBus.Write<uint8>(kSMPCCOMREG, command);
    uint64 cycles = 0;
    while ((saturn.mainBus.Peek<uint8>(kSMPCSF) & 1) != 0 && cycles < 1'000'000) {
        cycles += saturn.StepMasterSH2();
    }
    return cycles;
}

} // namespace

TEST_CASE("SMPC INTBACK takes longer than other commands", "[smpc][backport]") {
    // Ymir 775ba762, 839aef3b: INTBACK needs a longer delay than other commands (Assault Rigs), which in turn need
    // neither too short nor too long a delay (Fighters Megamix, Virtua Cop, Quake, Destruction Derby, etc.)
    auto saturn = std::make_unique<Saturn>();
    const uint64 resenab = CyclesUntilSFClears(*saturn, 0x19);
    saturn = std::make_unique<Saturn>();
    const uint64 intback = CyclesUntilSFClears(*saturn, 0x10);

    CAPTURE(resenab, intback);
    CHECK(intback > 3 * resenab);
}

TEST_CASE("SCU DMA ignores writes to VDP1 registers", "[scu][vdp][backport]") {
    // Ymir d400f3d2: GunBlaze-S DMAs a huge block over the VDP1 register area, which hardware ignores.
    constexpr uint32 kSCU = 0x5FE'0000;
    constexpr uint32 kSrc = 0x0601'0000;

    auto runDMA = [&](Saturn &saturn, uint32 dst) {
        saturn.mainBus.Write<uint16>(kSrc, 0x1234);
        saturn.mainBus.Write<uint32>(kSCU + 0x00, kSrc);
        saturn.mainBus.Write<uint32>(kSCU + 0x04, dst);
        saturn.mainBus.Write<uint32>(kSCU + 0x08, 2);
        saturn.mainBus.Write<uint32>(kSCU + 0x0C, 0x101);
        saturn.mainBus.Write<uint32>(kSCU + 0x14, 7);
        saturn.mainBus.Write<uint32>(kSCU + 0x10, 0x101);
        uint64 cycles = 0;
        while (cycles < 2000) {
            cycles += saturn.StepMasterSH2();
        }
    };

    SECTION("VDP1 registers are not written") {
        auto saturn = std::make_unique<Saturn>();
        runDMA(*saturn, 0x5D0'0006); // EWDR
        CHECK(saturn->VDP.GetProbe().GetVDP1Regs().eraseWriteValue != 0x1234);
    }

    SECTION("VDP1 VRAM is written") {
        auto saturn = std::make_unique<Saturn>();
        runDMA(*saturn, 0x5C0'0000);
        CHECK(saturn->mainBus.Peek<uint16>(0x5C0'0000) == 0x1234);
    }
}

TEST_CASE("VDP2 registers accept 8-bit reads and writes", "[vdp][vdp2][backport]") {
    // Ymir acde106c, d57f1260
    auto saturn = std::make_unique<Saturn>();
    auto &bus = saturn->mainBus;
    constexpr uint32 kRAMCTL = 0x5F8'000E;

    bus.Write<uint16>(kRAMCTL, 0x130F);
    const uint16 v = bus.Read<uint16>(kRAMCTL);
    REQUIRE((v >> 8) != 0);
    REQUIRE((v & 0xFF) != 0);

    CHECK(bus.Read<uint8>(kRAMCTL + 0) == (v >> 8));
    CHECK(bus.Read<uint8>(kRAMCTL + 1) == (v & 0xFF));

    bus.Write<uint8>(kRAMCTL + 1, 0x00);
    const uint16 w = bus.Read<uint16>(kRAMCTL);
    CHECK((w >> 8) == (v >> 8));
    CHECK((w & 0xFF) == 0);
}

TEST_CASE("SCSP slot reset clears pitch LFO sensitivity", "[scsp][backport]") {
    // Ymir a1442182
    scsp::Slot slot;
    slot.pitchLFOSens = 5;
    slot.Reset();
    CHECK(slot.pitchLFOSens == 0);
}

TEST_CASE("SCSP slot after a loop-start crossing keeps playing past a one-shot end", "[scsp][backport]") {
    // Ymir aae1dbef: slot N+1 with LPCTL=Off doesn't stop at its loop end if slot N crossed a loop point on the
    // same sample.
    auto makeSlot = [](uint16 currSample, bool crossedLoopStart) {
        scsp::Slot slot;
        slot.active = true;
        slot.loopControl = scsp::Slot::LoopControl::Off;
        slot.loopStartAddress = 4;
        slot.loopEndAddress = 8;
        slot.currPhase = 1 << 14;
        slot.currSample = currSample;
        slot.crossedLoopStart = crossedLoopStart;
        return slot;
    };

    SECTION("loop-start crossing in the previous slot suppresses the one-shot end") {
        auto s0 = makeSlot(3, false); // 3 -> 4; next sample 5 > loop start: crosses loop start
        auto s1 = makeSlot(8, true);  // 8 -> 9; next sample 10 > loop end: would stop
        bool suppress = false;

        s0.IncrementSampleCounter(suppress);
        CHECK(s0.crossedLoopStart);
        CHECK(s0.active);
        CHECK(suppress);

        s1.IncrementSampleCounter(suppress);
        CHECK(s1.active);
        CHECK(s1.crossedLoopStart);
    }

    SECTION("without a previous crossing the slot stops") {
        auto s1 = makeSlot(8, true);
        bool suppress = false;

        s1.IncrementSampleCounter(suppress);
        CHECK_FALSE(s1.active);
        CHECK_FALSE(s1.crossedLoopStart);
    }

    SECTION("suppression only lasts one slot") {
        auto s0 = makeSlot(3, false);
        auto s1 = makeSlot(8, true);
        auto s2 = makeSlot(8, true);
        bool suppress = false;

        s0.IncrementSampleCounter(suppress);
        s1.IncrementSampleCounter(suppress);
        s2.IncrementSampleCounter(suppress);
        CHECK_FALSE(s2.active);
    }

    SECTION("inactive slots clear the suppression flag") {
        auto s0 = makeSlot(3, false);
        auto s1 = makeSlot(0, false);
        s1.active = false;
        auto s2 = makeSlot(8, true);
        bool suppress = false;

        s0.IncrementSampleCounter(suppress);
        s1.IncrementSampleCounter(suppress);
        CHECK_FALSE(suppress);
        s2.IncrementSampleCounter(suppress);
        CHECK_FALSE(s2.active);
    }
}

TEST_CASE("SCSP one-shot PCM suppression flag survives save states", "[scsp][savestate][backport]") {
    auto saturn = std::make_unique<Saturn>();

    auto state = std::make_unique<savestate::SaveState>();
    saturn->SaveState(*state);
    CHECK_FALSE(state->scsp.suppressSlotEnd);

    state->scsp.suppressSlotEnd = true;
    REQUIRE(saturn->LoadState(*state, true));

    auto state2 = std::make_unique<savestate::SaveState>();
    saturn->SaveState(*state2);
    CHECK(state2->scsp.suppressSlotEnd);
}
