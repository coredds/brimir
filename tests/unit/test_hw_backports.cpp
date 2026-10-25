#include "catch_amalgamated.hpp"

#include <ymir/hw/scsp/scsp_slot.hpp>
#include <ymir/hw/vdp/renderer/common/vdp1_steppers.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/saturn.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
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

TEST_CASE("VDP1 FBRAM follows VDP1 VRAM in the save state", "[vdp][vdp1][savestate][backport]") {
    // Ymir 2220b167: the sprite framebuffers moved into VDP1 memory as FBRAM, saved right after VRAM1.
    STATIC_REQUIRE(offsetof(savestate::VDPSaveState, FBRAM) == vdp::kVDP1VRAMSize);
    CHECK(offsetof(savestate::VDPSaveState, FBRAM) == vdp::kVDP1VRAMSize);
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
        // CPU writes go to the draw framebuffer: flip so the display framebuffer is drawn into, then flip back
        state->fbIndex.Flip();
        renderer->VDP1WriteFB(0, value);
        renderer->PreSaveStateSync();
        state->fbIndex.Flip();
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
        return util::ReadBE<uint16>(&state->mem1.FBRAM[state->fbIndex.display][address]);
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

TEST_CASE("VDP1 framebuffer index after reset", "[vdp][vdp1][backport]") {
    // Ymir 9a64f38e: after a hard reset FB 1 is displayed and FB 0 is drawn into. A soft reset keeps the index.
    auto state = std::make_unique<vdp::VDPState>();
    CHECK(state->fbIndex.display == 1);
    CHECK(state->fbIndex.draw == 0);

    state->fbIndex.Flip();
    CHECK(state->fbIndex.display == 0);
    CHECK(state->fbIndex.draw == 1);

    state->Reset(false);
    CHECK(state->fbIndex.display == 0);
    CHECK(state->fbIndex.draw == 1);

    state->Reset(true);
    CHECK(state->fbIndex.display == 1);
    CHECK(state->fbIndex.draw == 0);
}

TEST_CASE("CPU framebuffer write after reset lands in FBRAM 0", "[vdp][vdp1][backport]") {
    // Ymir 9a64f38e: the draw framebuffer, which the CPU sees on the bus, is FB 0 after a reset.
    auto state = std::make_unique<vdp::VDPState>();
    state->VDP1WriteFB<uint16>(0x100, 0xABCD);
    CHECK(util::ReadBE<uint16>(&state->mem1.FBRAM[0][0x100]) == 0xABCD);
    CHECK(util::ReadBE<uint16>(&state->mem1.FBRAM[1][0x100]) == 0x0000);
}

TEST_CASE("Framebuffer index survives a save state", "[vdp][vdp1][savestate][backport]") {
    // Ymir 9a64f38e, 4769bb03: the display index is saved once, in VDPSaveState::displayFB.
    auto state = std::make_unique<vdp::VDPState>();
    state->fbIndex.Flip();
    auto saved = std::make_unique<savestate::VDPSaveState>();
    state->SaveState(*saved);
    CHECK(saved->displayFB == 0);

    auto loaded = std::make_unique<vdp::VDPState>();
    loaded->LoadState(*saved);
    CHECK(loaded->fbIndex.display == 0);
    CHECK(loaded->fbIndex.draw == 1);
}

TEST_CASE("Out-of-range saved framebuffer index is sanitised", "[vdp][vdp1][savestate][backport]") {
    // Ymir 9a64f38e: any nonzero saved display index loads as display 1, draw 0, so a crafted state cannot index past
    // the two framebuffers.
    auto state = std::make_unique<vdp::VDPState>();
    auto saved = std::make_unique<savestate::VDPSaveState>();
    state->SaveState(*saved);
    saved->FBRAM[0][0x100] = 0x5A;
    saved->displayFB = 2;

    auto loaded = std::make_unique<vdp::VDPState>();
    loaded->fbIndex.Flip();
    loaded->LoadState(*saved);
    CHECK(loaded->fbIndex.display == 1);
    CHECK(loaded->fbIndex.draw == 0);
    CHECK(loaded->VDP1ReadFB<uint8>(0x100) == 0x5A);
}

TEST_CASE("Hard reset clears both framebuffers in the threaded VDP1 renderer", "[vdp][vdp1][threaded][backport]") {
    // Ymir 2220b167, 9a64f38e: FBRAM lives in VDP1Memory, so a hard reset must clear the render thread's copy too, or
    // the next EndDraw/SwapBuffers copies the stale framebuffer back over the main state.
    using namespace ymir::vdp;

    auto state = std::make_unique<VDPState>();
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<SoftwareVDPRenderer>(*state, debug, access);
    renderer->EnableThreadedVDP1(true);

    auto writeFB = [&](uint16 value) {
        state->VDP1WriteFB<uint16>(0x100, value, [&](uint32 address, uint16 v) { renderer->VDP1WriteFB(address, v); });
    };
    auto fbIsClear = [&](uint8 index) {
        const auto &fb = state->mem1.FBRAM[index];
        return std::all_of(fb.begin(), fb.end(), [](uint8 b) { return b == 0; });
    };

    writeFB(0x1111);
    renderer->PreSaveStateSync(); // the render thread reads the draw index when it processes the write
    state->fbIndex.Flip();
    writeFB(0x2222);
    renderer->PreSaveStateSync();
    REQUIRE(util::ReadBE<uint16>(&state->mem1.FBRAM[0][0x100]) == 0x1111);
    REQUIRE(util::ReadBE<uint16>(&state->mem1.FBRAM[1][0x100]) == 0x2222);

    state->Reset(true);
    renderer->Reset(true);
    renderer->PreSaveStateSync();
    CHECK(fbIsClear(0));
    CHECK(fbIsClear(1));

    // An empty VDP1 frame copies the render thread's framebuffers back to the main state
    renderer->VDP1BeginFrame();
    renderer->VDP1EndFrame();
    renderer->VDP1SwapFramebuffer();
    state->fbIndex.Flip();
    renderer->VDP1BeginFrame();
    renderer->VDP1EndFrame();
    renderer->PreSaveStateSync();
    CHECK(fbIsClear(0));
    CHECK(fbIsClear(1));

    renderer->EnableThreadedVDP1(false);
}

namespace {

constexpr uint32 kVDP1FBCR = 0x02;
constexpr uint16 kFBCR_DIE = 1u << 3;
constexpr uint32 kFBRowBytes = 512 * sizeof(uint16);

// Writes a VDP1 Draw Line command from (0, y) to (7, y) at the given VRAM address.
void WriteDrawLineCommand(vdp::VDPState &state, uint32 address, uint16 y, uint16 color) {
    state.mem1.WriteVRAM<uint16>(address + 0x00, 0x0006); // CMDCTRL: draw line, end of table not set
    state.mem1.WriteVRAM<uint16>(address + 0x04, 0x0000); // CMDPMOD
    state.mem1.WriteVRAM<uint16>(address + 0x06, color);  // CMDCOLR
    state.mem1.WriteVRAM<uint16>(address + 0x0C, 0);      // XA
    state.mem1.WriteVRAM<uint16>(address + 0x0E, y);      // YA
    state.mem1.WriteVRAM<uint16>(address + 0x10, 7);      // XB
    state.mem1.WriteVRAM<uint16>(address + 0x12, y);      // YB
}

// Draws two VDP1 frames in double-density interlace with the deinterlace enhancement: the first with FBCR.DIE set
// (no vertical doubling), the second with it cleared (vertical doubling), and returns the draw framebuffer.
std::vector<uint8> DrawDoubleDensityFrames(bool threaded) {
    auto state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
    renderer->ConfigureEnhancements({.deinterlace = true});

    state->regs2.WriteTVMD(0x80C0); // DISP on, double-density interlace
    state->regs2.LatchTVMD();
    state->state1.sysClipH = 319;
    state->state1.sysClipV = 255;
    state->state2.layerEnabled[0] = true;
    state->regs1.WriteFBCR(kFBCR_DIE);
    WriteDrawLineCommand(*state, 0x100, 40, 0x83E0);
    WriteDrawLineCommand(*state, 0x200, 60, 0xFC00);

    renderer->EnableThreadedVDP1(threaded);
    renderer->PostLoadStateSync();

    const vdp::VDP1Command::Control drawLine{.u16 = 0x0006};
    renderer->VDP1BeginFrame();
    renderer->VDP1ExecuteCommand(0x100, drawLine);
    renderer->VDP1EndFrame();

    state->regs1.WriteFBCR(0x0000);
    renderer->VDP1WriteReg(kVDP1FBCR, 0x0000);
    renderer->VDP1BeginFrame();
    renderer->VDP1ExecuteCommand(0x200, drawLine);
    renderer->VDP1EndFrame();
    renderer->PreSaveStateSync();

    const auto &fb = state->mem1.FBRAM[state->fbIndex.draw];
    std::vector<uint8> out(fb.begin(), fb.end());
    renderer->EnableThreadedVDP1(false);
    return out;
}

} // namespace

TEST_CASE("Threaded VDP1 applies the double-density setting of each frame in order", "[vdp][vdp1][threaded][brimir]") {
    // Brimir fix (same in upstream Ymir): VDP1BeginFrame ran on the emulator thread but read the VDP1 render thread's
    // register copy, and wrote the vertical doubling flag while the render thread could still be drawing the previous
    // frame's commands. Each frame must use the FBCR.DIE value written before it began.
    const auto expected = DrawDoubleDensityFrames(false);
    // Frame 1: no doubling, line Y=40 lands on framebuffer row 20. Frame 2: doubled to Y=120, row 60.
    REQUIRE(util::ReadBE<uint16>(&expected[20 * kFBRowBytes]) == 0x83E0);
    REQUIRE(util::ReadBE<uint16>(&expected[60 * kFBRowBytes]) == 0xFC00);

    int mismatches = 0;
    for (int i = 0; i < 50; ++i) {
        if (DrawDoubleDensityFrames(true) != expected) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
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
    auto &fb = state.mem1.FBRAM[state.fbIndex.display];
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

TEST_CASE("VDP2 CRAM address unmapping inverts the mapping", "[vdp][vdp2][cram][backport]") {
    // Ymir ebc7da8d replaces its commented-out static_assert with this round trip.
    vdp::VDP2Regs regs;
    vdp::VDP2Memory mem{regs};

    for (uint8 mode = 0; mode < 4; mode++) {
        regs.vramControl.colorRAMMode = mode;
        CAPTURE(mode);

        for (uint32 addr = 0; addr <= 0xFFF; addr++) {
            CAPTURE(addr);
            CHECK(mem.UnmapCRAMAddress<uint8>(mem.MapCRAMAddress<uint8>(addr)) == addr);
            CHECK(mem.MapCRAMAddress<uint8>(mem.UnmapCRAMAddress<uint8>(addr)) == addr);
        }
        for (uint32 addr = 0; addr <= 0xFFF; addr += 2) {
            CAPTURE(addr);
            CHECK(mem.UnmapCRAMAddress<uint16>(mem.MapCRAMAddress<uint16>(addr)) == addr);
            CHECK(mem.MapCRAMAddress<uint16>(mem.UnmapCRAMAddress<uint16>(addr)) == addr);
        }
    }
}

namespace {

constexpr uint16 kFBCR_EOS = 1u << 4;

// Drives single VDP1 commands through the unthreaded software renderer and reads back the draw framebuffer.
struct VDP1StepperFixture {
    static constexpr uint32 kCommandAddr = 0x100;
    static constexpr uint32 kGouraudTableAddr = 0x800; // CMDGRDA = 0x100 (address / 8)
    static constexpr uint32 kTextureAddr = 0x2000;     // CMDSRCA = 0x400 (address / 8)

    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<vdp::SoftwareVDPRenderer> renderer;

    VDP1StepperFixture() {
        renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
        state->regs2.WriteTVMD(0x8000); // DISP on, 320x224
        state->regs2.LatchTVMD();
        state->state1.sysClipH = 319;
        state->state1.sysClipV = 255;
        state->state2.layerEnabled[0] = true;
    }

    void Write16(uint32 offset, uint16 value) {
        state->mem1.WriteVRAM<uint16>(kCommandAddr + offset, value);
    }

    void Draw(uint16 control) {
        renderer->PostLoadStateSync();
        renderer->VDP1BeginFrame();
        renderer->VDP1ExecuteCommand(kCommandAddr, vdp::VDP1Command::Control{.u16 = control});
        renderer->VDP1EndFrame();
    }

    uint16 ReadPixel(uint32 x, uint32 y) const {
        const auto &fb = state->mem1.FBRAM[state->fbIndex.draw];
        return util::ReadBE<uint16>(&fb[(y * 512 + x) * sizeof(uint16)]);
    }

    // Draw Line (x0, y0) -> (x1, y1) with Gouraud shading from table entry A to table entry B
    void DrawGouraudLine(sint32 x0, sint32 y0, sint32 x1, sint32 y1, uint16 gouraudA, uint16 gouraudB) {
        Write16(0x04, 0x0004);                                  // CMDPMOD: Gouraud shading enable
        Write16(0x06, 0x8000 | (16u << 10) | (16u << 5) | 16u); // CMDCOLR: mid-grey, opaque (Gouraud offset 0)
        Write16(0x0C, x0);
        Write16(0x0E, y0);
        Write16(0x10, x1);
        Write16(0x12, y1);
        Write16(0x1C, kGouraudTableAddr / 8);
        state->mem1.WriteVRAM<uint16>(kGouraudTableAddr + 0, gouraudA);
        state->mem1.WriteVRAM<uint16>(kGouraudTableAddr + 2, gouraudB);
        state->mem1.WriteVRAM<uint16>(kGouraudTableAddr + 4, gouraudA);
        state->mem1.WriteVRAM<uint16>(kGouraudTableAddr + 6, gouraudB);
        Draw(0x0006);
    }

    // Draws a one-pixel-high distorted sprite spanning x = 0..width-1 on row y, textured with a 16x1 RGB texture whose
    // texel i is 0x8000 | i
    void DrawTexturedSpan(uint32 width, uint16 pmod, uint32 y) {
        for (uint32 i = 0; i < 16; ++i) {
            state->mem1.WriteVRAM<uint16>(kTextureAddr + i * sizeof(uint16), 0x8000 | i);
        }
        Write16(0x04, pmod);
        Write16(0x06, 0x0000);           // CMDCOLR (unused in RGB mode)
        Write16(0x08, kTextureAddr / 8); // CMDSRCA
        Write16(0x0A, 0x0201);           // CMDSIZE: 16x1
        Write16(0x0C, 0);                // A
        Write16(0x0E, y);
        Write16(0x10, width - 1); // B
        Write16(0x12, y);
        Write16(0x14, width - 1); // C
        Write16(0x16, y);
        Write16(0x18, 0); // D
        Write16(0x1A, y);
        Draw(0x0002);
    }

    std::vector<uint16> ReadRow(uint32 y, uint32 count) const {
        std::vector<uint16> row;
        for (uint32 x = 0; x < count; ++x) {
            row.push_back(ReadPixel(x, y));
        }
        return row;
    }

    // Returns the single plotted pixel of every column x = 0..count-1 within rows [y0, y1]
    std::vector<uint16> ReadColumns(uint32 count, uint32 y0, uint32 y1) const {
        std::vector<uint16> out;
        for (uint32 x = 0; x < count; ++x) {
            uint32 plotted = 0;
            uint16 value = 0;
            for (uint32 y = y0; y <= y1; ++y) {
                if (const uint16 px = ReadPixel(x, y); px != 0) {
                    ++plotted;
                    value = px;
                }
            }
            CAPTURE(x, plotted);
            REQUIRE(plotted == 1);
            out.push_back(value);
        }
        return out;
    }
};

// CMDPMOD for a 16 bpp RGB texture with transparent pixels and end codes disabled, optionally with high-speed shrink
constexpr uint16 kRGBTexturePMOD = 0x0028 | 0x0040 | 0x0080;
constexpr uint16 kHighSpeedShrink = 0x1000;

constexpr uint16 Gray555(uint32 v) {
    return static_cast<uint16>(0x8000u | (v << 10) | (v << 5) | v);
}

} // namespace

TEST_CASE("VDP1 Gouraud line spans the major axis", "[vdp][vdp1][steppers]") {
    // Ymir a5482816: LineStepper::Length() became DMajor() (and Length() now means dmaj+1). The Gouraud stepper must
    // still be set up with dmaj+1 pixels so the last pixel reaches the end colour exactly.
    SECTION("a ramp with one value per pixel hits both end colours") {
        // dmaj = 31 gives 32 pixels for 32 Gouraud values (0..31), so pixel x carries value x in every channel
        VDP1StepperFixture f;
        f.DrawGouraudLine(0, 40, 31, 43, 0x0000, 0x7FFF);

        const auto line = f.ReadColumns(32, 40, 43);
        CHECK(line.front() == Gray555(0));
        CHECK(line.back() == Gray555(31));
        for (uint32 x = 0; x < 32; ++x) {
            CAPTURE(x);
            CHECK(line[x] == Gray555(x));
        }
    }

    SECTION("a steep ramp over a short line matches the golden") {
        // 0..31 over 11 pixels (dmaj = 10): the stepper skips values, so the first pixel is already 1 and the last
        // is 30. Golden captured by running this test on the pre-sync code (LineStepper::Length() == dmaj).
        VDP1StepperFixture f;
        f.DrawGouraudLine(0, 40, 10, 43, 0x0000, 0x7FFF);

        const auto line = f.ReadColumns(11, 40, 43);
        constexpr std::array<uint16, 11> kGolden{0x8421, 0x9084, 0x9CE7, 0xA94A, 0xB5AD, 0xC210,
                                                 0xCA52, 0xD6B5, 0xE318, 0xEF7B, 0xFBDE};
        CHECK(std::equal(line.begin(), line.end(), kGolden.begin(), kGolden.end()));
    }
}

TEST_CASE("VDP1 high-speed shrink threshold uses the major axis", "[vdp][vdp1][steppers]") {
    // Ymir a5482816: the check is `DMajor() < charSizeH - 1`. With the new Length() (dmaj+1) it would switch off for
    // a 15 pixel wide span of a 16 pixel texture.
    SECTION("15 pixels wide with EOS = 0 samples even texels") {
        VDP1StepperFixture f;
        f.state->regs1.WriteFBCR(0x0000);
        f.DrawTexturedSpan(15, kRGBTexturePMOD | kHighSpeedShrink, 50);
        const auto row = f.ReadRow(50, 16);
        CHECK(f.ReadPixel(15, 50) == 0);
        // Captured by running this test on the pre-sync code.
        constexpr std::array<uint16, 15> kGolden{0x8000, 0x8000, 0x8002, 0x8002, 0x8004, 0x8004, 0x8006, 0x8006,
                                                 0x8008, 0x8008, 0x800A, 0x800A, 0x800C, 0x800C, 0x800E};
        CHECK(std::equal(row.begin(), row.begin() + 15, kGolden.begin(), kGolden.end()));
        for (uint32 x = 0; x < 15; ++x) {
            CAPTURE(x);
            CHECK((row[x] & 0x8000) != 0);
            CHECK((row[x] & 1) == 0);
        }
    }

    SECTION("15 pixels wide with EOS = 1 samples odd texels") {
        VDP1StepperFixture f;
        f.state->regs1.WriteFBCR(kFBCR_EOS);
        f.DrawTexturedSpan(15, kRGBTexturePMOD | kHighSpeedShrink, 50);
        const auto row = f.ReadRow(50, 16);
        CHECK(f.ReadPixel(15, 50) == 0);
        constexpr std::array<uint16, 15> kGolden{0x8001, 0x8001, 0x8003, 0x8003, 0x8005, 0x8005, 0x8007, 0x8007,
                                                 0x8009, 0x8009, 0x800B, 0x800B, 0x800D, 0x800D, 0x800F};
        CHECK(std::equal(row.begin(), row.begin() + 15, kGolden.begin(), kGolden.end()));
        for (uint32 x = 0; x < 15; ++x) {
            CAPTURE(x);
            CHECK((row[x] & 0x8000) != 0);
            CHECK((row[x] & 1) == 1);
        }
    }

    SECTION("16 pixels wide keeps every texel in order") {
        for (uint16 eos : {uint16{0}, kFBCR_EOS}) {
            CAPTURE(eos);
            VDP1StepperFixture f;
            f.state->regs1.WriteFBCR(eos);
            f.DrawTexturedSpan(16, kRGBTexturePMOD | kHighSpeedShrink, 50);
            const auto row = f.ReadRow(50, 17);
            for (uint32 x = 0; x < 16; ++x) {
                CAPTURE(x);
                CHECK(row[x] == (0x8000 | x));
            }
            CHECK(row[16] == 0);
        }
    }
}

TEST_CASE("VDP1 textured line U-stepping", "[vdp][vdp1][steppers]") {
    // Ymir a5482816: the U stepper is set up with DMajor() + 1 pixels. 16 texels squeezed into 8 pixels, no
    // high-speed shrink.
    VDP1StepperFixture f;
    f.DrawTexturedSpan(8, kRGBTexturePMOD, 50);
    const auto row = f.ReadRow(50, 9);
    CHECK(row[8] == 0);

    // Captured by running this test on the pre-sync code.
    constexpr std::array<uint16, 8> kGolden{0x8001, 0x8003, 0x8005, 0x8007, 0x8009, 0x800B, 0x800D, 0x800F};
    CHECK(std::equal(row.begin(), row.begin() + 8, kGolden.begin(), kGolden.end()));
}

namespace {

// Walks a LineStepper the way the renderer does and returns every visited coordinate
template <typename Stepper>
std::vector<std::pair<sint32, sint32>> WalkLine(Stepper &line) {
    std::vector<std::pair<sint32, sint32>> coords;
    for (line.Step(); line.CanStep(); line.Step()) {
        coords.emplace_back(line.X(), line.Y());
    }
    return coords;
}

} // namespace

TEST_CASE("VDP1 LineStepper reports the major axis delta and the step count", "[vdp][vdp1][steppers]") {
    // Ymir a5482816: Length() used to return the major axis delta; it is now DMajor() and Length() is dmaj + 1.
    vdp::LineStepper l{{0, 0}, {10, 3}};
    CHECK(l.DMajor() == 10);
    CHECK(l.Length() == 11);

    const auto coords = WalkLine(l);
    REQUIRE(coords.size() == 11);
    CHECK(coords.front().first == 0);
    CHECK(coords.back() == std::pair<sint32, sint32>{10, 3});
}

TEST_CASE("VDP1 LineStepper clipped away entirely has no steps", "[vdp][vdp1][steppers]") {
    // Ymir a5482816: SystemClip() updates Length() while DMajor() keeps describing the unclipped line.
    vdp::LineStepper l{{400, 0}, {500, 10}};
    REQUIRE(l.DMajor() == 100);
    REQUIRE(l.Length() == 101);

    l.SystemClip(319, 255);
    CHECK(l.DMajor() == 100);
    CHECK(l.Length() == 0);
    CHECK(WalkLine(l).empty());
}

TEST_CASE("VDP1 LineStepper precision parameter keeps in-range lines unchanged", "[vdp][vdp1][steppers]") {
    // Ymir c6bfbb18: the stepper precision is a template parameter that defaults to the hardware's 13 bits.
    static_assert(vdp::LineStepper<>::kCoordMask == 0x7FF);
    static_assert(vdp::LineStepper<13>::kCoordMask == 0x7FF);
    static_assert(vdp::LineStepper<16>::kCoordMask == 0x3FFF);

    const std::pair<vdp::CoordS32, vdp::CoordS32> lines[] = {
        {{0, 0}, {10, 3}},   {{10, 3}, {0, 0}},     {{0, 0}, {3, 10}},   {{5, 40}, {300, 200}},
        {{319, 0}, {0, 223}}, {{7, 7}, {7, 7}},      {{0, 40}, {31, 43}}, {{100, 100}, {20, 90}},
    };
    for (const auto &[a, b] : lines) {
        for (bool antiAlias : {false, true}) {
            CAPTURE(a.x(), a.y(), b.x(), b.y(), antiAlias);
            vdp::LineStepper<> reference{a, b, antiAlias};
            vdp::LineStepper<16> wide{a, b, antiAlias};
            CHECK(wide.DMajor() == reference.DMajor());
            CHECK(wide.Length() == reference.Length());
            CHECK(WalkLine(wide) == WalkLine(reference));
        }
    }
}

namespace {

// Draws transparent-mesh VDP1 lines at the given Y coordinates and renders VDP2 line 0 over a black back screen. The sprite
// layer uses the mixed format so the mesh pixels are RGB data.
std::array<uint32, 4> RenderMeshLines(uint16 tvmd, uint16 fbcr, std::initializer_list<uint16> ys) {
    auto state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
    renderer->ConfigureEnhancements({.deinterlace = true, .transparentMeshes = true});
    FrameCapture capture;

    auto &regs = state->regs2;
    regs.WriteTVMD(tvmd);
    regs.LatchTVMD();
    regs.WriteBGON(0);
    regs.WriteSPCTL(0x0020); // sprite type 0, mixed format (MSB = RGB data)
    regs.WritePRISA(0x0007);
    regs.WriteBKTAU(0x0000);
    regs.WriteBKTAL(0x0080);
    state->mem2.WriteVRAM<uint16>(0x100, 0x0000);
    state->state1.sysClipH = 319;
    state->state1.sysClipV = 255;
    state->state2.layerEnabled[0] = true;

    state->regs1.WriteFBCR(fbcr);
    uint32 cmd = 0x100;
    for (uint16 y : ys) {
        state->mem1.WriteVRAM<uint16>(cmd + 0x00, 0x0006); // draw line
        state->mem1.WriteVRAM<uint16>(cmd + 0x04, 0x0100); // CMDPMOD: mesh
        state->mem1.WriteVRAM<uint16>(cmd + 0x06, 0x83E0); // CMDCOLR: RGB green
        state->mem1.WriteVRAM<uint16>(cmd + 0x0C, 0);      // XA
        state->mem1.WriteVRAM<uint16>(cmd + 0x0E, y);      // YA
        state->mem1.WriteVRAM<uint16>(cmd + 0x10, 7);      // XB
        state->mem1.WriteVRAM<uint16>(cmd + 0x12, y);      // YB
        cmd += 0x20;
    }

    renderer->PostLoadStateSync();
    capture.Bind(*renderer);
    renderer->VDP1BeginFrame();
    for (uint32 i = 0; i < ys.size(); ++i) {
        renderer->VDP1ExecuteCommand(0x100 + i * 0x20, vdp::VDP1Command::Control{.u16 = 0x0006});
    }
    renderer->VDP1EndFrame();
    state->fbIndex.Flip(); // display the framebuffer that was just drawn

    renderer->VDP2SetResolution(320, 448, false);
    renderer->VDP2BeginFrame();
    renderer->VDP2RenderLine(0);
    renderer->VDP2EndFrame();
    REQUIRE(capture.width == 320);
    return {capture.At(0, 0), capture.At(0, 1), capture.At(0, 2), capture.At(0, 3)};
}

} // namespace

TEST_CASE("Transparent meshes are drawn on both deinterlaced fields in single-density interlace",
          "[vdp][vdp2][mesh][brimir]") {
    // Brimir fix (same in upstream Ymir): the alternate field read the mesh framebuffer that VDP1 only fills in double
    // density, so meshes vanished on every other output row.
    constexpr uint32 kBack = 0xFF000000;
    const auto rows = RenderMeshLines(0x8080, 0x0000, {0}); // single-density interlace

    // The mesh is blended with the back screen, so it is neither the back color nor the pure mesh color
    CHECK(rows[0] != kBack);
    CHECK(rows[0] != RGB555to888(0x83E0));
    CHECK(rows[1] == rows[0]);
    // Line 1 has no mesh
    CHECK(rows[2] == kBack);
    CHECK(rows[3] == kBack);
}

TEST_CASE("Transparent meshes keep both fields in double-density interlace", "[vdp][vdp2][mesh][brimir]") {
    constexpr uint32 kBack = 0xFF000000;
    const auto rows = RenderMeshLines(0x80C0, kFBCR_DIE, {0, 1}); // double-density interlace, VDP1 rows 0 and 1

    CHECK(rows[0] != kBack);
    CHECK(rows[1] == rows[0]);
    CHECK(rows[2] == kBack);
    CHECK(rows[3] == kBack);
}
