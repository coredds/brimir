#include "catch_amalgamated.hpp"

#include <ymir/hw/scsp/scsp_slot.hpp>
#include <ymir/hw/vdp/renderer/common/vdp1_clip.hpp>
#include <ymir/hw/vdp/renderer/common/vdp1_steppers.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/saturn.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
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

namespace {

// One rendered frame of a reset scenario: the first line's pixels and the line colour and back screen state.
struct ResetFrame {
    std::vector<uint32> pixels;
    uint32 lineColor;
    uint32 backColor;

    bool operator==(const ResetFrame &) const = default;
};

// Back screen colour 0x7C1F at VRAM 0x100, line colour from CRAM word 0 (LCTA = 0) = 0x83E0.
void SetupResetScene(vdp::VDPState &state) {
    SetupBackScreen(state);
    state.mem2.WriteCRAM<uint16>(0, 0x83E0);
}

// Builds the reset scene, starts with or without the threaded VDP2 renderer and runs script(state, renderer, frame),
// where frame() renders one frame and records it. Returns the recorded frames.
std::vector<ResetFrame> RunResetScript(bool threaded, auto script) {
    auto state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
    FrameCapture capture;
    std::vector<ResetFrame> frames;

    SetupResetScene(*state);
    renderer->PostLoadStateSync();
    renderer->EnableThreadedVDP2(threaded);
    capture.Bind(*renderer);
    const auto frame = [&] {
        renderer->VDP2SetResolution(320, 224, false);
        renderer->VDP2BeginFrame();
        renderer->VDP2RenderLine(0);
        renderer->VDP2EndFrame();
        const auto &lineBack = state->state2.lineBackLayerState;
        frames.push_back({{capture.pixels.begin(), capture.pixels.begin() + capture.width},
                          lineBack.lineColor.u32,
                          lineBack.backColor.u32});
    };
    script(*state, *renderer, frame);
    renderer->EnableThreadedVDP2(false);
    return frames;
}

// The reset as VDP::Reset does it, then the registers programmed again (the reset cleared them) as the game does.
void ResetAndReprogram(vdp::VDPState &state, vdp::SoftwareVDPRenderer &renderer, bool hard) {
    state.Reset(hard);
    renderer.Reset(hard);
    const auto write = [&](uint32 address, uint16 value) {
        state.regs2.Write(address, value);
        renderer.VDP2WriteReg(address, value);
    };
    write(0x000, 0x8000); // TVMD: DISP on, 320 pixels wide
    state.regs2.LatchTVMD();
    renderer.VDP2LatchTVMD();
    write(0x020, 0x0000); // BGON
    write(0x0AC, 0x0000); // BKTAU: single colour
    write(0x0AE, 0x0080); // BKTAL: 0x100
}

// CRAM word 0 written again with the value it holds.
void RewriteCRAMWord0(vdp::VDPState &state, vdp::SoftwareVDPRenderer &renderer) {
    const uint16 value = state.mem2.ReadCRAM<uint16>(0);
    state.mem2.WriteCRAM<uint16>(0, value);
    renderer.VDP2WriteCRAM(0, value);
}

int CountThreadedResetMismatches(auto script, const std::vector<ResetFrame> &expected) {
    int mismatches = 0;
    for (int i = 0; i < kThreadedEquivalenceIterations; ++i) {
        if (RunResetScript(true, script) != expected) {
            ++mismatches;
        }
    }
    return mismatches;
}

} // namespace

TEST_CASE("Threaded VDP2 keeps VRAM and CRAM across a soft reset", "[vdp][vdp2][threaded][reset][brimir]") {
    // Brimir: VDPState::Reset(false) and the unthreaded renderer keep VDP2 VRAM, CRAM and the RGB555 colour cache, but
    // the VDP2 render thread's Reset event cleared its copies of all three on every reset (as upstream Ymir does). The
    // threaded renderer then drew from zeroed memory, and CRAM words rewritten with their old value stayed zero in its
    // colour cache. Found in Daytona USA CE, which soft-resets the VDPs at boot.
    auto script = [](vdp::VDPState &state, vdp::SoftwareVDPRenderer &renderer, auto frame) {
        frame();
        ResetAndReprogram(state, renderer, false);
        frame();
        RewriteCRAMWord0(state, renderer);
        frame();
    };

    const auto expected = RunResetScript(false, script);
    REQUIRE(expected.size() == 3);
    for (const auto &frame : expected) {
        REQUIRE(frame.pixels.size() == 320);
        REQUIRE(frame.pixels[0] == RGB555to888(0x7C1F));
        REQUIRE(frame.backColor == vdp::ConvertRGB555to888(vdp::Color555{.u16 = 0x7C1F}).u32);
        REQUIRE(frame.lineColor == vdp::ConvertRGB555to888(vdp::Color555{.u16 = 0x83E0}).u32);
    }
    CHECK(CountThreadedResetMismatches(script, expected) == 0);
}

TEST_CASE("Threaded VDP2 matches unthreaded after a hard reset", "[vdp][vdp2][threaded][reset][brimir]") {
    auto script = [](vdp::VDPState &state, vdp::SoftwareVDPRenderer &renderer, auto frame) {
        frame();
        ResetAndReprogram(state, renderer, true);
        frame();
        state.mem2.WriteVRAM<uint16>(0x100, 0x001F);
        renderer.VDP2WriteVRAM(0x100, uint16{0x001F});
        state.mem2.WriteCRAM<uint16>(0, 0x801F);
        renderer.VDP2WriteCRAM(0, uint16{0x801F});
        frame();
    };

    const auto expected = RunResetScript(false, script);
    REQUIRE(expected.size() == 3);
    REQUIRE(expected[1].pixels[0] == RGB555to888(0x0000));
    REQUIRE(expected[2].pixels[0] == RGB555to888(0x001F));
    CHECK(CountThreadedResetMismatches(script, expected) == 0);
}

TEST_CASE("Toggling threaded VDP2 after a reset keeps the frames identical", "[vdp][vdp2][threaded][reset][brimir]") {
    // Runs threaded from the start (or unthreaded for the reference) and toggles threading after each reset: every
    // frame must match a run that never toggles.
    const auto makeScript = [](bool toggle) {
        return [=](vdp::VDPState &state, vdp::SoftwareVDPRenderer &renderer, auto frame) {
            ResetAndReprogram(state, renderer, false);
            frame();
            if (toggle) {
                renderer.EnableThreadedVDP2(false);
            }
            frame();
            ResetAndReprogram(state, renderer, false);
            frame();
            if (toggle) {
                renderer.EnableThreadedVDP2(true);
            }
            RewriteCRAMWord0(state, renderer);
            frame();
            ResetAndReprogram(state, renderer, false);
            frame();
        };
    };

    const auto expected = RunResetScript(false, makeScript(false));
    REQUIRE(expected.size() == 5);
    REQUIRE(expected[0].pixels[0] == RGB555to888(0x7C1F));
    CHECK(CountThreadedResetMismatches(makeScript(true), expected) == 0);
    CHECK(CountThreadedResetMismatches(makeScript(false), expected) == 0);
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

namespace {

// Renders VDP2 line 0 over a black back screen with a transparent-mesh pixel (palette data) whose CRAM colour has the given
// MSB. The normal sprite pixel is transparent palette data, but the sprite layer still takes the colour of CRAM entry 0,
// whose MSB is set to normalMsb. Returns the first pixel of the line.
uint32 RenderMeshMsbPixel(uint16 spctl, uint16 ccctl, bool normalMsb, bool meshMsb) {
    auto state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
    renderer->ConfigureEnhancements({.transparentMeshes = true});
    FrameCapture capture;

    auto &regs = state->regs2;
    regs.WriteTVMD(0x8000);
    regs.LatchTVMD();
    regs.WriteBGON(0);
    regs.WriteSPCTL(spctl);
    regs.WritePRISA(0x0007);
    regs.WriteCCCTL(ccctl);
    regs.WriteCCRSA(0x0010);
    regs.WriteBKTAU(0x0000);
    regs.WriteBKTAL(0x0080);
    state->mem2.WriteVRAM<uint16>(0x100, 0x0000);
    state->mem2.WriteCRAM<uint16>(0 * sizeof(uint16), normalMsb ? 0x8000 : 0x0000);
    state->mem2.WriteCRAM<uint16>(1 * sizeof(uint16), meshMsb ? 0x83E0 : 0x03E0);
    state->state1.sysClipH = 319;
    state->state1.sysClipV = 255;
    state->state2.layerEnabled[0] = true;

    state->regs1.WriteFBCR(0x0000);
    state->mem1.WriteVRAM<uint16>(0x100 + 0x00, 0x0006); // draw line
    state->mem1.WriteVRAM<uint16>(0x100 + 0x04, 0x0100); // CMDPMOD: mesh
    state->mem1.WriteVRAM<uint16>(0x100 + 0x06, 0x0001); // CMDCOLR: palette data, colour index 1
    state->mem1.WriteVRAM<uint16>(0x100 + 0x0C, 0);      // XA
    state->mem1.WriteVRAM<uint16>(0x100 + 0x0E, 0);      // YA
    state->mem1.WriteVRAM<uint16>(0x100 + 0x10, 7);      // XB
    state->mem1.WriteVRAM<uint16>(0x100 + 0x12, 0);      // YB

    renderer->PostLoadStateSync();
    capture.Bind(*renderer);
    renderer->VDP1BeginFrame();
    renderer->VDP1ExecuteCommand(0x100, vdp::VDP1Command::Control{.u16 = 0x0006});
    renderer->VDP1EndFrame();
    state->fbIndex.Flip();

    renderer->VDP2SetResolution(320, 224, false);
    renderer->VDP2BeginFrame();
    renderer->VDP2RenderLine(0);
    renderer->VDP2EndFrame();
    REQUIRE(capture.width == 320);
    return capture.At(0, 0);
}

} // namespace

TEST_CASE("Transparent mesh colour calculation in colour MSB mode uses the mesh pixel's MSB",
          "[vdp][vdp2][mesh][brimir]") {
    // Brimir fix (same in upstream Ymir): the MSB condition read the normal sprite layer, which holds an unrelated colour
    // wherever the normal sprite pixel is transparent, instead of the mesh pixel.
    constexpr uint16 kSpriteMixed = 0x0020;
    constexpr uint16 kCondMsb = 0x3000;
    constexpr uint16 kCondAlways = 0x0700; // priority <= 7: always true for the mesh priority used here
    constexpr uint16 kSpriteCCEnable = 0x0040;

    const uint32 withCC = RenderMeshMsbPixel(kSpriteMixed | kCondAlways, kSpriteCCEnable, false, true);
    const uint32 withoutCC = RenderMeshMsbPixel(kSpriteMixed | kCondAlways, 0, false, true);
    REQUIRE(withCC != withoutCC);

    // Mesh MSB = 1 over a normal sprite layer MSB = 0: colour calculation applies
    CHECK(RenderMeshMsbPixel(kSpriteMixed | kCondMsb, kSpriteCCEnable, false, true) == withCC);
    // Mesh MSB = 0 over a normal sprite layer MSB = 1: colour calculation does not apply
    CHECK(RenderMeshMsbPixel(kSpriteMixed | kCondMsb, kSpriteCCEnable, true, false) == withoutCC);
}

// -----------------------------------------------------------------------------
// VDP1 software renderer characterization (GPU stage 2: behaviours the GPU VDP1 must reproduce exactly)

namespace {

// The unthreaded software renderer with a VDP1 command list in VRAM (commands at 0x100 + i * 0x20).
struct VDP1CharFixture {
    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<vdp::SoftwareVDPRenderer> renderer;
    uint32 commandCount = 0;

    explicit VDP1CharFixture(vdp::config::Enhancements enhancements = {}, uint16 tvmd = 0x8000) {
        renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
        renderer->ConfigureEnhancements(enhancements);
        state->regs2.WriteTVMD(tvmd);
        state->regs2.LatchTVMD();
        state->state1.sysClipH = 319;
        state->state1.sysClipV = 255;
        state->state2.layerEnabled[0] = true;
        renderer->VDP2SetResolution(320, (tvmd & 0xC0) == 0xC0 ? 448 : 224, false);
    }

    // CMDCTRL, CMDLINK, CMDPMOD, CMDCOLR, CMDSRCA, CMDSIZE, XA, YA, XB, YB, XC, YC, XD, YD, CMDGRDA, unused
    void Command(const std::array<uint16, 16> &words) {
        const uint32 address = 0x100 + commandCount++ * 0x20;
        for (uint32 i = 0; i < 16; ++i) {
            state->mem1.WriteVRAM<uint16>(address + i * 2, words[i]);
        }
    }

    void Line(uint16 pmod, uint16 color, sint32 x0, sint32 y0, sint32 x1, sint32 y1) {
        Command({0x0006, 0, pmod, color, 0, 0, U(x0), U(y0), U(x1), U(y1), 0, 0, 0, 0, 0, 0});
    }

    // Executes the queued commands as one VDP1 frame and clears the list
    void Draw() {
        renderer->PostLoadStateSync();
        renderer->VDP1BeginFrame();
        for (uint32 i = 0; i < commandCount; ++i) {
            const uint32 address = 0x100 + i * 0x20;
            renderer->VDP1ExecuteCommand(address, vdp::VDP1Command::Control{.u16 = state->mem1.ReadVRAM<uint16>(address)});
        }
        renderer->VDP1EndFrame();
        commandCount = 0;
    }

    vdp::SpriteFB &DrawFB() {
        return state->mem1.FBRAM[state->fbIndex.draw];
    }

    uint16 Pixel(uint32 x, uint32 y) {
        return util::ReadBE<uint16>(&DrawFB()[(y * 512 + x) * 2]);
    }

    // DumpExtraVDP1Framebuffers: alt[draw], alt[display] (deinterlace), mesh[0][draw], mesh[0][display], mesh[1][draw],
    // mesh[1][display] (transparent meshes)
    std::string Extra() const {
        std::ostringstream out;
        renderer->DumpExtraVDP1Framebuffers(out);
        return out.str();
    }

    static uint16 U(sint32 v) {
        return static_cast<uint16>(v);
    }
};

uint16 Read16(const std::string &bytes, size_t offset) {
    return static_cast<uint16>((static_cast<uint8>(bytes[offset]) << 8) | static_cast<uint8>(bytes[offset + 1]));
}

} // namespace

TEST_CASE("VDP1 erase covers x1 <= x < x3 and y1 <= y <= y3", "[vdp][vdp1][characterization]") {
    // Software: x3 is exclusive, y3 inclusive, so y1 == y3 erases one row and x1 == x3 erases nothing.
    // (Ymir's D3D12 renderer skips y1 == y3 and treats x3 as inclusive.)
    for (const auto [x1, x3, y1, y3] : {std::array<uint16, 4>{4, 12, 10, 10}, std::array<uint16, 4>{4, 12, 10, 13},
                                        std::array<uint16, 4>{4, 4, 10, 12}}) {
        CAPTURE(x1, x3, y1, y3);
        VDP1CharFixture f;
        auto &fb = f.state->mem1.FBRAM[f.state->fbIndex.display];
        for (uint32 i = 0; i < fb.size(); i += 2) {
            util::WriteBE<uint16>(&fb[i], 0x1111);
        }
        auto &regs1 = f.state->regs1;
        regs1.eraseWriteValueLatch = 0xABCD;
        regs1.eraseX1Latch = x1;
        regs1.eraseX3Latch = x3;
        regs1.eraseY1Latch = y1;
        regs1.eraseY3Latch = y3;
        f.renderer->VDP1EraseFramebuffer(0);
        for (uint32 y = 8; y < 16; ++y) {
            for (uint32 x = 0; x < 16; ++x) {
                CAPTURE(x, y);
                const bool inside = x >= x1 && x < x3 && y >= y1 && y <= y3;
                CHECK(util::ReadBE<uint16>(&fb[(y * 512 + x) * 2]) == (inside ? 0xABCD : 0x1111));
            }
        }
    }
}

TEST_CASE("VDP1 clipping commands use the raw 16-bit coordinates", "[vdp][vdp1][characterization]") {
    // Software stores CMDXC/CMDYC (system clip) and CMDXA..CMDYC (user clip) unmasked. (Ymir's D3D12 renderer masks
    // them to 10/9 bits.)
    SECTION("system clip H = 0x1140 and V = 0x1020 are not masked to 10 and 9 bits") {
        // masked, V would be 0x20 and H 0x140: the line at y = 100, x up to 340 would be clipped
        VDP1CharFixture f;
        f.Command({0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1140, 0x1020, 0, 0, 0, 0});
        f.Line(0, 0x83E0, 310, 100, 340, 100);
        f.Draw();
        CHECK(f.state->state1.sysClipH == 0x1140);
        CHECK(f.state->state1.sysClipV == 0x1020);
        CHECK(f.Pixel(330, 100) == 0x83E0);
        CHECK(f.Pixel(340, 100) == 0x83E0);
    }
    SECTION("user clip X1 = 0x1140 and Y1 = 0x1020 are not masked") {
        // inside mode with the whole screen as the user area; the system clip is widened the same way
        VDP1CharFixture f;
        f.Command({0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1140, 0x1020, 0, 0, 0, 0});
        f.Command({0x0008, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1140, 0x1020, 0, 0, 0, 0});
        f.Line(0x0400, 0x83E0, 310, 100, 340, 100); // user clipping on, draw inside
        f.Draw();
        CHECK(f.state->state1.userClipX0 == 0);
        CHECK(f.state->state1.userClipY0 == 0);
        CHECK(f.state->state1.userClipX1 == 0x1140);
        CHECK(f.state->state1.userClipY1 == 0x1020);
        CHECK(f.Pixel(330, 100) == 0x83E0);
        CHECK(f.Pixel(340, 100) == 0x83E0);
    }
    SECTION("user clip X0 = 0xFC00 rejects everything inside mode") {
        VDP1CharFixture f;
        f.Command({0x0008, 0, 0, 0, 0, 0, 0xFC00, 0, 0, 0, 300, 200, 0, 0, 0, 0});
        f.Line(0x0400, 0x83E0, 10, 20, 40, 20); // user clipping on, draw inside
        f.Draw();
        CHECK(f.state->state1.userClipX0 == 0xFC00);
        for (uint32 x = 10; x <= 40; ++x) {
            CAPTURE(x);
            CHECK(f.Pixel(x, 20) == 0x0000);
        }
    }
    SECTION("user clip Y0 = 0x1000 rejects everything inside mode") {
        // masked to 9 bits it would be 0 and the line would be drawn
        VDP1CharFixture f;
        f.Command({0x0008, 0, 0, 0, 0, 0, 0, 0x1000, 0, 0, 300, 200, 0, 0, 0, 0});
        f.Line(0x0400, 0x83E0, 10, 20, 40, 20);
        f.Draw();
        CHECK(f.state->state1.userClipY0 == 0x1000);
        for (uint32 x = 10; x <= 40; ++x) {
            CAPTURE(x);
            CHECK(f.Pixel(x, 20) == 0x0000);
        }
    }
}
TEST_CASE("VDP1 Draw Line is vertically doubled without the field offset", "[vdp][vdp1][characterization]") {
    // Deinterlaced double density (FBCR.DIE clear): lines use y << 1 and land on the even field only, while a sprite's
    // bottom edge gets the +1 so one sprite row covers both fields. (Ymir's D3D12 renderer does not double lines.)
    VDP1CharFixture f({.deinterlace = true}, 0x80C0);
    f.Line(0, 0x83E0, 0, 40, 7, 40);
    // one-row normal sprite at y = 50: 8x1 RGB texture, transparent pixels and end codes disabled
    for (uint32 i = 0; i < 8; ++i) {
        f.state->mem1.WriteVRAM<uint16>(0x2000 + i * 2, 0xFC00);
    }
    f.Command({0x0000, 0, 0x00E8, 0, 0x2000 / 8, 0x0101, 0, 50, 0, 0, 0, 0, 0, 0, 0, 0});
    f.Draw();

    const std::string extra = f.Extra(); // alt[draw] first
    const auto alt = [&](uint32 x, uint32 y) { return Read16(extra, (y * 512 + x) * 2); };
    for (uint32 x = 0; x < 8; ++x) {
        CAPTURE(x);
        CHECK(f.Pixel(x, 40) == 0x83E0); // y = 80, even field, row 40
        CHECK(alt(x, 40) == 0x0000);
        CHECK(f.Pixel(x, 20) == 0x0000); // not undoubled
        CHECK(f.Pixel(x, 50) == 0xFC00); // sprite rows 100 and 101
        CHECK(alt(x, 50) == 0xFC00);
    }
}

TEST_CASE("VDP1 CPU framebuffer writes mirror to the alternate field when interlaced and leave meshes alone",
          "[vdp][vdp1][characterization]") {
    // Software: with deinterlacing, a CPU write to the draw framebuffer is mirrored to the alternate-field framebuffer
    // whenever TVMD is interlaced (single or double density), and never touches the mesh framebuffers. (Ymir's D3D12
    // renderer mirrors only in double density and clears the mesh.)
    for (const uint16 tvmd : {uint16{0x8080}, uint16{0x80C0}, uint16{0x8000}}) {
        CAPTURE(tvmd);
        VDP1CharFixture f({.deinterlace = true, .transparentMeshes = true}, tvmd);
        // a mesh pixel at (4, 2) of the draw framebuffer
        f.Line(0x0100, 0x83E0, 4, 2, 4, 2);
        f.Draw();
        const size_t kFB = vdp::kVDP1FBRAMSize;
        const std::string before = f.Extra();
        const uint32 address = (2 * 512 + 4) * 2;
        const size_t meshOffset = 2 * kFB; // mesh[0][draw]
        REQUIRE(Read16(before, meshOffset + address) == 0x83E0);

        f.state->VDP1WriteFB<uint16>(address, 0x1234, [&](uint32 a, uint16 v) { f.renderer->VDP1WriteFB(a, v); });
        f.state->VDP1WriteFB<uint8>(address + 3, 0x56, [&](uint32 a, uint8 v) { f.renderer->VDP1WriteFB(a, v); });
        const std::string after = f.Extra();
        CHECK(f.Pixel(4, 2) == 0x1234);
        const bool interlaced = tvmd != 0x8000;
        CHECK(Read16(after, address) == (interlaced ? 0x1234 : 0x0000)); // alt[draw]
        CHECK(static_cast<uint8>(after[address + 3]) == (interlaced ? 0x56 : 0x00));
        CHECK(after.substr(2 * kFB) == before.substr(2 * kFB)); // meshes untouched
        CHECK(after.substr(kFB, kFB) == before.substr(kFB, kFB)); // alt[display] untouched
    }
}

TEST_CASE("VDP1 MSB-on sets bit 7 of the even byte in 8-bpp framebuffers", "[vdp][vdp1][characterization]") {
    // Software: MSB-on ORs 0x80 into the byte at fbOffset & ~1, so in 8-bpp mode both pixels of a 16-bit word share
    // it and the odd pixel's own byte is untouched.
    for (const uint32 x : {2u, 3u}) {
        CAPTURE(x);
        VDP1CharFixture f;
        f.state->regs1.WriteTVMR(0x0001); // 8 bpp, 1024 wide
        f.DrawFB().fill(0x11);
        f.Line(0x8000, 0x00FF, x, 10, x, 10);
        f.Draw();
        const uint32 offset = 10 * 1024 + x;
        CHECK(f.DrawFB()[offset & ~1u] == 0x91);
        CHECK(f.DrawFB()[offset | 1u] == 0x11);
        CHECK(f.DrawFB()[offset - 2] == 0x11);
    }
}

TEST_CASE("VDP1 end codes in a system-clipped prefix still count", "[vdp][vdp1][characterization]") {
    // Software reads every texel from u = start through the catch-up loop after LineStepper::SystemClip, so end codes
    // left of the screen count towards the two that end the line. A 16x3 RGB sprite at x = -8: row 0 has end codes at
    // texels 1 and 12 (the line ends at x = 4), row 1 at texels 0 and 3 (nothing is drawn), row 2 has none.
    VDP1CharFixture f;
    for (uint32 y = 0; y < 3; ++y) {
        for (uint32 i = 0; i < 16; ++i) {
            uint16 texel = static_cast<uint16>(0x8000 | (y << 8) | i);
            if ((y == 0 && (i == 1 || i == 12)) || (y == 1 && (i == 0 || i == 3))) {
                texel = 0x7FFF;
            }
            f.state->mem1.WriteVRAM<uint16>(0x2000 + (y * 16 + i) * 2, texel);
        }
    }
    f.Command({0x0000, 0, 0x0028, 0, 0x2000 / 8, 0x0203, f.U(-8), 30, 0, 0, 0, 0, 0, 0, 0, 0});
    f.Draw();
    for (uint32 x = 0; x < 8; ++x) {
        CAPTURE(x);
        CHECK(f.Pixel(x, 30) == (x < 4 ? static_cast<uint16>(0x8000 | (8 + x)) : 0x0000));
        CHECK(f.Pixel(x, 31) == 0x0000);
        CHECK(f.Pixel(x, 32) == static_cast<uint16>(0x8000 | (2 << 8) | (8 + x)));
    }
}

// -----------------------------------------------------------------------------
// VDP1 quad termination (Brimir: stop only when no remaining line can reach the system clip area, vdp1_clip.hpp)

namespace {

// splitmix64; deterministic on every platform (the std distributions are not)
struct TestRng {
    uint64 s;
    explicit TestRng(uint64 seed)
        : s(seed) {}
    uint64 Next() {
        uint64 z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    sint32 Range(sint32 lo, sint32 hi) {
        return lo + static_cast<sint32>(Next() % static_cast<uint64>(hi - lo + 1));
    }
    bool Chance(uint32 percent) {
        return Next() % 100 < percent;
    }
};

// A VDP1 frame for the unthreaded software renderer: registers, the first 128 KiB of VDP1 VRAM and the commands to
// execute (addresses into that VRAM; command 0x00..0x7FF, Gouraud tables 0x800..0xFFF, textures from 0x2000).
struct VDP1Scene {
    static constexpr uint32 kVRAMSize = 0x20000;
    static constexpr uint32 kGouraudBase = 0x800;
    static constexpr uint32 kTextureBase = 0x2000;

    uint16 tvmd = 0x8000;
    uint16 tvmr = 0x0000;
    uint16 fbcr = 0x0000;
    bool deinterlace = false;
    bool transparentMeshes = false;
    uint16 sysClipH = 319;
    uint16 sysClipV = 223;
    uint16 userClipX0 = 0, userClipY0 = 0, userClipX1 = 319, userClipY1 = 223;
    sint32 localX = 0, localY = 0;
    std::vector<uint8> vram = std::vector<uint8>(kVRAMSize, 0);
    std::vector<uint32> commands;
    std::vector<uint8> fbFill; // tiled over both framebuffers before drawing; empty = zero

    uint32 nextCommand = 0;
    uint32 nextGouraud = kGouraudBase;
    uint32 nextTexture = kTextureBase;

    void Write16(uint32 address, uint16 value) {
        vram[address] = static_cast<uint8>(value >> 8);
        vram[address + 1] = static_cast<uint8>(value);
    }

    // CMDCTRL, CMDLINK, CMDPMOD, CMDCOLR, CMDSRCA, CMDSIZE, XA, YA, XB, YB, XC, YC, XD, YD, CMDGRDA, unused
    void AddCommand(const std::array<uint16, 16> &words) {
        REQUIRE(nextCommand < kGouraudBase);
        for (uint32 i = 0; i < 16; ++i) {
            Write16(nextCommand + i * 2, words[i]);
        }
        commands.push_back(nextCommand);
        nextCommand += 0x20;
    }

    // Returns CMDSRCA (address / 8), 32-byte aligned
    uint16 AddTexture(const std::vector<uint8> &bytes) {
        const uint32 address = nextTexture;
        REQUIRE(address + bytes.size() <= kVRAMSize);
        std::copy(bytes.begin(), bytes.end(), vram.begin() + address);
        nextTexture = (address + static_cast<uint32>(bytes.size()) + 31) & ~31u;
        return static_cast<uint16>(address / 8);
    }

    // Returns CMDGRDA (address / 8)
    uint16 AddGouraud(std::array<uint16, 4> colors) {
        const uint32 address = nextGouraud;
        REQUIRE(address < kTextureBase);
        for (uint32 i = 0; i < 4; ++i) {
            Write16(address + i * 2, colors[i]);
        }
        nextGouraud += 8;
        return static_cast<uint16>(address / 8);
    }

    void Polygon(uint16 pmod, uint16 color, vdp::CoordS32 a, vdp::CoordS32 b, vdp::CoordS32 c, vdp::CoordS32 d,
                 uint16 grda = 0) {
        AddCommand({0x0004, 0, pmod, color, 0, 0, U(a.x()), U(a.y()), U(b.x()), U(b.y()), U(c.x()), U(c.y()), U(d.x()),
                    U(d.y()), grda, 0});
    }

    void Distorted(uint16 ctrlFlags, uint16 pmod, uint16 color, uint16 srca, uint16 size, vdp::CoordS32 a,
                   vdp::CoordS32 b, vdp::CoordS32 c, vdp::CoordS32 d, uint16 grda = 0) {
        AddCommand({static_cast<uint16>(0x0002 | ctrlFlags), 0, pmod, color, srca, size, U(a.x()), U(a.y()), U(b.x()),
                    U(b.y()), U(c.x()), U(c.y()), U(d.x()), U(d.y()), grda, 0});
    }

    void Normal(uint16 ctrlFlags, uint16 pmod, uint16 color, uint16 srca, uint16 size, sint32 x, sint32 y,
                uint16 grda = 0) {
        AddCommand({static_cast<uint16>(0x0000 | ctrlFlags), 0, pmod, color, srca, size, U(x), U(y), 0, 0, 0, 0, 0, 0,
                    grda, 0});
    }

    static uint16 U(sint32 v) {
        return static_cast<uint16>(v);
    }
};

struct VDP1SceneResult {
    std::vector<uint8> fb; // both main framebuffers
    std::string extra; // alt and mesh framebuffers, as DumpExtraVDP1Framebuffers writes them
    uint64 terminations = 0;

    bool SameFramebuffers(const VDP1SceneResult &rhs) const {
        return fb == rhs.fb && extra == rhs.extra;
    }
};

VDP1SceneResult DrawScene(const VDP1Scene &scene, bool quadTermination) {
    auto state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
    renderer->ConfigureEnhancements({.deinterlace = scene.deinterlace, .transparentMeshes = scene.transparentMeshes});
    renderer->SetVDP1QuadTerminationForTesting(quadTermination);

    state->regs2.WriteTVMD(scene.tvmd);
    state->regs2.LatchTVMD();
    state->regs1.WriteTVMR(scene.tvmr);
    state->regs1.WriteFBCR(scene.fbcr);
    auto &ctx = state->state1;
    ctx.sysClipH = scene.sysClipH;
    ctx.sysClipV = scene.sysClipV;
    ctx.userClipX0 = scene.userClipX0;
    ctx.userClipY0 = scene.userClipY0;
    ctx.userClipX1 = scene.userClipX1;
    ctx.userClipY1 = scene.userClipY1;
    ctx.localCoordX = scene.localX;
    ctx.localCoordY = scene.localY;
    state->state2.layerEnabled[0] = true;
    std::copy(scene.vram.begin(), scene.vram.end(), state->mem1.VRAM.begin());
    if (!scene.fbFill.empty()) {
        for (auto &fb : state->mem1.FBRAM) {
            for (size_t i = 0; i < fb.size(); ++i) {
                fb[i] = scene.fbFill[i % scene.fbFill.size()];
            }
        }
    }

    renderer->PostLoadStateSync();
    renderer->VDP1BeginFrame();
    for (const uint32 address : scene.commands) {
        const uint16 control = util::ReadBE<uint16>(&state->mem1.VRAM[address]);
        renderer->VDP1ExecuteCommand(address, vdp::VDP1Command::Control{.u16 = control});
    }
    renderer->VDP1EndFrame();

    VDP1SceneResult result;
    result.fb.assign(state->mem1.FBRAM[0].begin(), state->mem1.FBRAM[0].end());
    result.fb.insert(result.fb.end(), state->mem1.FBRAM[1].begin(), state->mem1.FBRAM[1].end());
    std::ostringstream extra;
    renderer->DumpExtraVDP1Framebuffers(extra);
    result.extra = extra.str();
    result.terminations = renderer->GetVDP1QuadTerminationsForTesting();
    return result;
}

// Draws the scene with and without quad termination; the framebuffers (main, alt, mesh) must be identical.
// Returns the number of quads that stopped early.
uint64 CheckTerminationIsPure(const VDP1Scene &scene) {
    const VDP1SceneResult terminated = DrawScene(scene, true);
    const VDP1SceneResult full = DrawScene(scene, false);
    CHECK(full.terminations == 0);
    CHECK(terminated.SameFramebuffers(full));
    return terminated.terminations;
}

constexpr uint16 kPModRGB = 0x0028;          // colour mode 5 (16 bpp RGB)
constexpr uint16 kPModEndCodeDisable = 0x0080;
constexpr uint16 kPModClipMode1 = 0x0200;    // user clipping mode: draw outside
constexpr uint16 kPModUserClip = 0x0400;     // user clipping enable

// A W x H RGB texture; texel (x, y) = 0x8000 | (y << 8) | x, except fully transparent rows (0x0000)
std::vector<uint8> RGBTexture(uint32 w, uint32 h, std::initializer_list<uint32> transparentRows = {}) {
    std::vector<uint8> bytes(w * h * 2);
    for (uint32 y = 0; y < h; ++y) {
        const bool transparent = std::find(transparentRows.begin(), transparentRows.end(), y) != transparentRows.end();
        for (uint32 x = 0; x < w; ++x) {
            const uint16 texel = transparent ? 0x0000 : static_cast<uint16>(0x8000 | (y << 8) | x);
            bytes[(y * w + x) * 2] = static_cast<uint8>(texel >> 8);
            bytes[(y * w + x) * 2 + 1] = static_cast<uint8>(texel);
        }
    }
    return bytes;
}

uint16 ReadFB16(const VDP1SceneResult &r, uint32 x, uint32 y) {
    return util::ReadBE<uint16>(&r.fb[(y * 512 + x) * 2]);
}

// Random scene generator: register variants, clip and local coordinate commands, sprites and polygons with random
// (convex, concave, self-intersecting, degenerate and far off-screen) shapes, modes and textures.
vdp::CoordS32 RandomPoint(TestRng &rng, sint32 w, sint32 h) {
    auto axis = [&](sint32 limit) {
        const uint32 r = static_cast<uint32>(rng.Range(0, 99));
        if (r < 60) {
            return rng.Range(-48, limit + 48);
        } else if (r < 85) {
            return rng.Range(-400, limit + 400);
        } else if (r < 95) {
            return rng.Range(-1100, limit + 1100);
        }
        return rng.Range(-4096, 4095);
    };
    return {axis(w), axis(h)};
}

std::vector<uint8> RandomTexture(TestRng &rng, uint32 bytesPerRow, uint32 rows, uint32 colorMode) {
    std::vector<uint8> bytes(bytesPerRow * rows);
    for (uint32 y = 0; y < rows; ++y) {
        const uint32 kind = static_cast<uint32>(rng.Range(0, 99));
        for (uint32 i = 0; i < bytesPerRow; ++i) {
            uint8 v;
            if (kind < 20) {
                v = 0x00; // transparent row
            } else if (kind < 25 && i < 4) {
                v = colorMode == 5 ? (i % 2 == 0 ? 0x7F : 0xFF) : 0xFF; // row starting with end codes
            } else {
                const uint32 r = static_cast<uint32>(rng.Range(0, 99));
                v = r < 25 ? 0x00 : r < 32 ? 0xFF : static_cast<uint8>(rng.Next());
            }
            bytes[y * bytesPerRow + i] = v;
        }
        if (colorMode == 5) {
            for (uint32 i = 0; i + 1 < bytesPerRow; i += 2) {
                if (rng.Chance(4)) {
                    bytes[y * bytesPerRow + i] = 0x7F; // end code
                    bytes[y * bytesPerRow + i + 1] = 0xFF;
                }
            }
        }
    }
    return bytes;
}

VDP1Scene RandomScene(uint64 seed) {
    TestRng rng{seed};
    VDP1Scene s;
    static constexpr uint16 kTVMD[] = {0x8000, 0x8000, 0x80C0, 0x80D0, 0x8080, 0x8001};
    s.tvmd = kTVMD[rng.Range(0, 5)];
    s.tvmr = rng.Chance(70) ? 0 : rng.Chance(70) ? 1 : static_cast<uint16>(rng.Range(0, 3));
    s.fbcr = static_cast<uint16>(rng.Range(0, 7) << 2); // DIL, DIE, EOS
    s.deinterlace = rng.Chance(50);
    s.transparentMeshes = rng.Chance(50);
    auto randomSysClip = [&](uint16 &h, uint16 &v) {
        const uint32 r = static_cast<uint32>(rng.Range(0, 99));
        if (r < 35) {
            h = 319;
            v = 223;
        } else if (r < 70) {
            h = static_cast<uint16>(rng.Range(0, 400));
            v = static_cast<uint16>(rng.Range(0, 300));
        } else if (r < 85) {
            h = static_cast<uint16>(rng.Range(0, 40));
            v = static_cast<uint16>(rng.Range(0, 40));
        } else {
            // raw 16-bit values beyond the 10/9 bits hardware registers hold (0x1140 style); software keeps them
            h = static_cast<uint16>(rng.Range(0x400, 0xFFFF));
            v = static_cast<uint16>(rng.Range(0x200, 0xFFFF));
        }
    };
    randomSysClip(s.sysClipH, s.sysClipV);
    s.userClipX0 = static_cast<uint16>(rng.Range(-8, 330));
    s.userClipY0 = static_cast<uint16>(rng.Range(-8, 250));
    s.userClipX1 = static_cast<uint16>(rng.Chance(80) ? rng.Range(s.userClipX0, 340) : rng.Range(-8, 340));
    s.userClipY1 = static_cast<uint16>(rng.Chance(80) ? rng.Range(s.userClipY0, 260) : rng.Range(-8, 260));
    if (rng.Chance(15)) {
        s.userClipX1 = static_cast<uint16>(rng.Range(0x400, 0xFFFF));
        s.userClipY1 = static_cast<uint16>(rng.Range(0x200, 0xFFFF));
    }
    if (rng.Chance(5)) {
        s.userClipX0 = static_cast<uint16>(rng.Range(0x400, 0xFFFF));
    }
    if (rng.Chance(5)) {
        s.userClipY0 = static_cast<uint16>(rng.Range(0x200, 0xFFFF));
    }
    if (rng.Chance(30)) {
        s.localX = rng.Range(-60, 60);
        s.localY = rng.Range(-60, 60);
    }
    if (rng.Chance(30)) {
        s.fbFill.resize(4096);
        for (auto &b : s.fbFill) {
            b = static_cast<uint8>(rng.Next());
        }
    }

    const sint32 w = std::min<sint32>(s.sysClipH, 420); // shapes stay around the screen whatever the raw clip value
    const sint32 h = std::min<sint32>(s.sysClipV, 320);
    const uint32 count = static_cast<uint32>(rng.Range(1, 12));
    for (uint32 n = 0; n < count; ++n) {
        const uint32 kind = static_cast<uint32>(rng.Range(0, 99));
        if (kind < 8) {
            uint16 ch, cv;
            randomSysClip(ch, cv);
            s.AddCommand({0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, ch, cv, 0, 0, 0, 0});
            continue;
        }
        if (kind < 16) {
            const auto a = RandomPoint(rng, w, h);
            const auto b = RandomPoint(rng, w, h);
            s.AddCommand({0x0008, 0, 0, 0, 0, 0, VDP1Scene::U(a.x()), VDP1Scene::U(a.y()), 0, 0, VDP1Scene::U(b.x()),
                          VDP1Scene::U(b.y()), 0, 0, 0, 0});
            continue;
        }
        if (kind < 20) {
            s.AddCommand({0x000A, 0, 0, 0, 0, 0, VDP1Scene::U(rng.Range(-80, 80)), VDP1Scene::U(rng.Range(-80, 80)), 0,
                          0, 0, 0, 0, 0, 0, 0});
            continue;
        }

        // Draw command
        uint16 pmod = static_cast<uint16>(rng.Next() & 0x9FFF);
        while (((pmod >> 3) & 7) > 5) {
            pmod = static_cast<uint16>((pmod & ~0x0038) | (rng.Range(0, 5) << 3));
        }
        if (!rng.Chance(15)) {
            pmod &= ~0x1000; // high-speed shrink mostly off
        }
        if (!rng.Chance(10)) {
            pmod &= ~0x8000; // MSB on mostly off
        }
        const uint16 color = static_cast<uint16>(rng.Next());
        const uint16 grda = s.AddGouraud({static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next()),
                                          static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next())});
        const uint32 sizeH = rng.Chance(5) ? 0 : static_cast<uint32>(rng.Range(1, 8)); // x8 pixels
        const uint32 sizeV = rng.Chance(5) ? 0 : static_cast<uint32>(rng.Range(1, 40));
        const uint32 colorMode = (pmod >> 3) & 7;
        const uint32 bytesPerRow = colorMode <= 1 ? sizeH * 4 : colorMode == 5 ? sizeH * 16 : sizeH * 8;
        const uint16 srca = s.AddTexture(RandomTexture(rng, std::max(bytesPerRow, 16u), std::max(sizeV, 1u), colorMode));
        const uint16 size = static_cast<uint16>((sizeH << 8) | sizeV);
        const uint16 flags = static_cast<uint16>((rng.Range(0, 3) << 4) | (rng.Range(0, 15) << 8)); // flip, zoom point

        // Quad shapes: random (convex, concave or bowtie), triangles, regular and rotated rectangles
        auto a = RandomPoint(rng, w, h);
        auto b = RandomPoint(rng, w, h);
        auto c = RandomPoint(rng, w, h);
        auto d = RandomPoint(rng, w, h);
        const uint32 shape = static_cast<uint32>(rng.Range(0, 99));
        if (shape < 15) {
            switch (rng.Range(0, 3)) {
            case 0: b = a; break;
            case 1: c = b; break;
            case 2: d = c; break;
            default: a = d; break;
            }
        } else if (shape < 30) {
            b = {c.x(), a.y()};
            d = {a.x(), c.y()};
        } else if (shape < 50) {
            const sint32 ux = rng.Range(-12, 12), uy = rng.Range(-12, 12);
            const sint32 ka = rng.Range(1, 30), kb = rng.Range(1, 30);
            b = {a.x() + ux * ka, a.y() + uy * ka};
            c = {b.x() - uy * kb, b.y() + ux * kb};
            d = {a.x() - uy * kb, a.y() + ux * kb};
        } else if (shape < 60) {
            // small quads straddling a corner or an edge of the system clip area
            const sint32 cx = rng.Chance(50) ? 0 : w, cy = rng.Chance(50) ? 0 : h;
            a = {cx + rng.Range(-20, 20), cy + rng.Range(-20, 20)};
            b = {cx + rng.Range(-20, 20), cy + rng.Range(-20, 20)};
            c = {cx + rng.Range(-20, 20), cy + rng.Range(-20, 20)};
            d = {cx + rng.Range(-20, 20), cy + rng.Range(-20, 20)};
        }

        const uint32 type = static_cast<uint32>(rng.Range(0, 99));
        if (type < 20) {
            s.Normal(flags, pmod, color, srca, size, a.x(), a.y(), grda);
        } else if (type < 40) {
            // Scaled sprite: XA/YA, XB/YB (zoom point width/height) and XC/YC (opposite corner)
            s.AddCommand({static_cast<uint16>(0x0001 | flags), 0, pmod, color, srca, size, VDP1Scene::U(a.x()),
                          VDP1Scene::U(a.y()), VDP1Scene::U(rng.Range(-60, 400)), VDP1Scene::U(rng.Range(-60, 300)),
                          VDP1Scene::U(c.x()), VDP1Scene::U(c.y()), 0, 0, grda, 0});
        } else if (type < 65) {
            s.Distorted(flags, pmod, color, srca, size, a, b, c, d, grda);
        } else if (type < 90) {
            s.Polygon(pmod, color, a, b, c, d, grda);
        } else if (type < 95) {
            s.AddCommand({0x0005, 0, pmod, color, 0, 0, VDP1Scene::U(a.x()), VDP1Scene::U(a.y()), VDP1Scene::U(b.x()),
                          VDP1Scene::U(b.y()), VDP1Scene::U(c.x()), VDP1Scene::U(c.y()), VDP1Scene::U(d.x()),
                          VDP1Scene::U(d.y()), grda, 0});
        } else {
            s.AddCommand({0x0006, 0, pmod, color, 0, 0, VDP1Scene::U(a.x()), VDP1Scene::U(a.y()), VDP1Scene::U(b.x()),
                          VDP1Scene::U(b.y()), 0, 0, 0, 0, grda, 0});
        }
    }
    return s;
}

} // namespace

TEST_CASE("VDP1 quad termination never changes the framebuffers on random command tables",
          "[vdp][vdp1][quad-termination][brimir]") {
    // Brimir fix: the quad loops stop only when no remaining line can reach the system clip area (vdp1_clip.hpp). Over
    // seeded random command tables the
    // framebuffers (main, alt and mesh) must equal those drawn with termination disabled, so it is a pure optimisation.
    constexpr uint64 kTables = 10'000;
    uint64 mismatches = 0;
    uint64 firstMismatch = ~0ull;
    uint64 terminations = 0;
    uint64 quadTables = 0;
    for (uint64 seed = 1; seed <= kTables; ++seed) {
        const VDP1Scene scene = RandomScene(seed);
        const VDP1SceneResult terminated = DrawScene(scene, true);
        const VDP1SceneResult full = DrawScene(scene, false);
        if (!terminated.SameFramebuffers(full)) {
            ++mismatches;
            firstMismatch = std::min(firstMismatch, seed);
        }
        terminations += terminated.terminations;
        quadTables += terminated.terminations != 0;
    }
    CAPTURE(firstMismatch, terminations, quadTables);
    CHECK(mismatches == 0);
    // The tables exercise the termination: over 40% stop at least one quad early
    CHECK(quadTables > kTables * 2 / 5);
}

TEST_CASE("VDP1 quad termination never changes the framebuffers on targeted quads",
          "[vdp][vdp1][quad-termination][brimir]") {
    // Each scene is drawn in every mode combination below: polygon and distorted sprite (RGB texture with a transparent
    // row), clipping modes 0 and 1 with and without user clipping, plain and deinterlaced double density.
    struct Shape {
        const char *name;
        vdp::CoordS32 a, b, c, d;
    };
    std::vector<Shape> shapes = {
        {"convex crossing the bottom right", {200, 120}, {420, 180}, {380, 330}, {150, 280}},
        {"convex crossing the top left", {-80, -40}, {120, -90}, {150, 60}, {-30, 100}},
        {"convex rotated across all edges", {160, -120}, {460, 112}, {160, 344}, {-140, 112}},
        {"concave crossing the right edge", {250, 40}, {420, 60}, {300, 110}, {380, 200}},
        {"concave crossing the bottom", {60, 150}, {200, 160}, {120, 190}, {90, 300}},
        {"bowtie at the top right corner", {300, 10}, {340, -20}, {315, 20}, {330, -30}},
        {"bowtie at the top right corner, mirrored", {315, 20}, {330, -30}, {300, 10}, {340, -20}},
        {"bowtie with lobes on both sides of the left edge", {-40, 50}, {40, 150}, {-40, 150}, {40, 50}},
        {"bowtie across the bottom edge", {100, 200}, {200, 260}, {100, 260}, {200, 200}},
        {"triangle (A = B) crossing the bottom", {160, 300}, {160, 300}, {300, 100}, {20, 100}},
        {"triangle (C = D) crossing the left edge", {-60, 40}, {100, 20}, {120, 180}, {120, 180}},
        {"triangle (B = C) apex outside the right edge", {100, 50}, {500, 120}, {500, 120}, {100, 200}},
        // cases upstream's and a line-touch predicate got wrong: corner grazes, second lobes, 13-bit wrapped deltas
        {"convex quad grazing the clip area on exit", {411, -98}, {150, 490}, {673, -115}, {532, -117}},
        {"bowtie at the top right corner, doubled", {296, 36}, {340, -40}, {307, 32}, {330, -60}},
        {"convex quad with 13-bit wrapped edge deltas", {-2865, 435}, {216, 158}, {3, 98}, {-906, 174}},
        {"quad wider than 4096", {-3000, 100}, {2000, 50}, {2100, 150}, {-2900, 200}},
        {"quad taller than 4096 (dmaj mask)", {100, -2100}, {200, -2100}, {220, 2100}, {80, 2100}},
        {"far off-screen quad", {-4000, -4000}, {-3900, -4000}, {-3900, -3900}, {-4000, -3900}},
    };
    // Sweep the bowtie and corner cases over small offsets
    for (sint32 dx = -12; dx <= 12; dx += 4) {
        for (sint32 dy = -12; dy <= 12; dy += 4) {
            shapes.push_back({"bowtie sweep", {300 + dx, 10 + dy}, {340, -20}, {315 - dy, 20 + dx}, {330, -30}});
            shapes.push_back({"corner sweep", {-20 + dx, -20}, {30, -10 + dy}, {20 - dx, 30}, {-10, 20 - dy}});
        }
    }

    const std::vector<uint8> texture = RGBTexture(16, 16, {5, 6});
    uint64 terminations = 0;
    uint64 scenes = 0;
    for (const Shape &shape : shapes) {
        for (uint16 clip : {uint16{0}, kPModUserClip, kPModClipMode1, uint16(kPModUserClip | kPModClipMode1)}) {
            for (bool doubleDensity : {false, true}) {
                for (bool textured : {false, true}) {
                    CAPTURE(shape.name, shape.a.x(), shape.a.y(), shape.b.x(), shape.b.y(), shape.c.x(), shape.c.y(),
                            shape.d.x(), shape.d.y(), clip, doubleDensity, textured);
                    VDP1Scene s;
                    if (doubleDensity) {
                        s.tvmd = 0x80C0;
                        s.deinterlace = true;
                        s.transparentMeshes = true;
                    }
                    s.userClipX0 = 100;
                    s.userClipY0 = 60;
                    s.userClipX1 = 220;
                    s.userClipY1 = 160;
                    const uint16 pmod = static_cast<uint16>(kPModRGB | kPModEndCodeDisable | clip);
                    if (textured) {
                        const uint16 srca = s.AddTexture(texture);
                        s.Distorted(0, pmod, 0, srca, 0x0210, shape.a, shape.b, shape.c, shape.d);
                    } else {
                        s.Polygon(pmod, 0x83E0, shape.a, shape.b, shape.c, shape.d);
                    }
                    terminations += CheckTerminationIsPure(s);
                    ++scenes;
                }
            }
        }
    }
    CAPTURE(scenes);
    CHECK(terminations > 0);
}

TEST_CASE("VDP1 quad termination never changes the framebuffers on rotated quads re-entering the user clip",
          "[vdp][vdp1][quad-termination][brimir]") {
    // A small user clip area and rotated rectangles and quads swept around it, in clipping modes 0 and 1: lines leave
    // and re-enter the user clip area, which upstream's predicate treated as the end of the quad in clipping mode 0.
    uint64 terminations = 0;
    for (sint32 ux = -12; ux <= 12; ux += 3) {
        for (sint32 uy = -12; uy <= 12; uy += 3) {
            if (ux == 0 && uy == 0) {
                continue;
            }
            for (uint16 clip : {kPModUserClip, uint16(kPModUserClip | kPModClipMode1)}) {
                for (bool textured : {false, true}) {
                    CAPTURE(ux, uy, clip, textured);
                    VDP1Scene s;
                    s.userClipX0 = 140;
                    s.userClipY0 = 90;
                    s.userClipX1 = 170;
                    s.userClipY1 = 120;
                    // rotated rectangle centred near the user clip area, crossing the system clip area's edges
                    const vdp::CoordS32 a{150 - ux * 14, 100 - uy * 14};
                    const vdp::CoordS32 b{a.x() + ux * 30, a.y() + uy * 30};
                    const vdp::CoordS32 c{b.x() - uy * 25, b.y() + ux * 25};
                    const vdp::CoordS32 d{a.x() - uy * 25, a.y() + ux * 25};
                    // and a quad whose lines fan around the user clip area
                    const vdp::CoordS32 e{a.x() + uy * 8, a.y() - ux * 8};
                    const uint16 pmod = static_cast<uint16>(kPModRGB | kPModEndCodeDisable | clip);
                    if (textured) {
                        const uint16 srca = s.AddTexture(RGBTexture(16, 16, {7}));
                        s.Distorted(0, pmod, 0, srca, 0x0210, a, b, c, d);
                        s.Distorted(0, pmod, 0, srca, 0x0210, e, b, c, a);
                    } else {
                        s.Polygon(pmod, 0x83E0, a, b, c, d);
                        s.Polygon(pmod, 0x83E0, e, b, c, a);
                    }
                    terminations += CheckTerminationIsPure(s);
                }
            }
        }
    }
    CHECK(terminations > 0);
}

TEST_CASE("VDP1 sprite rows after a transparent row are drawn in clipping mode 1",
          "[vdp][vdp1][quad-termination][brimir]") {
    // Brimir fix of an upstream bug: with the user clipping mode bit set ("draw outside"), transparent pixels did not
    // count as plotted, so a fully transparent sprite row ended the quad and every row below it was dropped. The same
    // happened to a row that starts with two end codes. (Upstream allows a second run of plotted lines for the quads
    // QuadStepper::IsDegenerate() reports, which includes this clockwise sprite, so it takes two such rows.)
    for (const bool endCodes : {false, true}) {
        for (const uint16 userClip : {uint16{0}, kPModUserClip}) {
            CAPTURE(endCodes, userClip);
            VDP1Scene s;
            // user clip area away from the sprite, so with clipping mode 1 the sprite is drawn
            s.userClipX0 = 200;
            s.userClipY0 = 150;
            s.userClipX1 = 250;
            s.userClipY1 = 200;
            std::vector<uint8> texture = endCodes ? RGBTexture(16, 8) : RGBTexture(16, 8, {2, 5});
            if (endCodes) {
                for (uint32 row : {2u, 5u}) {
                    for (uint32 x = 0; x < 2; ++x) {
                        texture[(row * 16 + x) * 2] = 0x7F;
                        texture[(row * 16 + x) * 2 + 1] = 0xFF;
                    }
                }
            }
            const uint16 srca = s.AddTexture(texture);
            const uint16 pmod = static_cast<uint16>(kPModRGB | kPModClipMode1 | userClip | (endCodes ? 0 : kPModEndCodeDisable));
            s.Normal(0, pmod, 0, srca, 0x0208, 40, 20); // 16x8 at (40, 20)

            const VDP1SceneResult r = DrawScene(s, true);
            for (uint32 y = 0; y < 8; ++y) {
                for (uint32 x = 0; x < 16; ++x) {
                    CAPTURE(x, y);
                    const uint16 expected = (y == 2 || y == 5) ? 0x0000 : static_cast<uint16>(0x8000 | (y << 8) | x);
                    CHECK(ReadFB16(r, 40 + x, 20 + y) == expected);
                }
            }
            CHECK(r.SameFramebuffers(DrawScene(s, false)));
        }
    }
}

TEST_CASE("VDP1 edge steppers move monotonically towards their final position", "[vdp][vdp1][quad-termination][brimir]") {
    // vdp1_clip.hpp relies on it: every remaining position of an edge lies in the box of its current and final
    // positions, also with 13-bit wrapped deltas and the 4095 step mask.
    TestRng rng{7};
    for (int i = 0; i < 20000; ++i) {
        vdp::CoordS32 p[4];
        for (auto &q : p) {
            q = rng.Chance(70) ? vdp::CoordS32{rng.Range(-500, 800), rng.Range(-500, 700)}
                               : vdp::CoordS32{rng.Range(-4096, 4095), rng.Range(-4096, 4095)};
        }
        vdp::QuadStepper quad{p[0], p[1], p[2], p[3]};
        std::vector<vdp::CoordS32> left, right;
        for (; quad.CanStep(); quad.Step()) {
            left.push_back(quad.LeftEdge().Coord());
            right.push_back(quad.RightEdge().Coord());
        }
        for (const auto *edge : {&left, &right}) {
            sint32 dirX = 0, dirY = 0;
            bool ok = true;
            for (size_t k = 1; k < edge->size(); ++k) {
                const sint32 dx = (*edge)[k].x() - (*edge)[k - 1].x();
                const sint32 dy = (*edge)[k].y() - (*edge)[k - 1].y();
                ok = ok && std::abs(dx) <= 1 && std::abs(dy) <= 1 && dx * dirX >= 0 && dy * dirY >= 0;
                dirX = dx != 0 ? dx : dirX;
                dirY = dy != 0 ? dy : dirY;
            }
            if (!ok) {
                CAPTURE(p[0].x(), p[0].y(), p[1].x(), p[1].y(), p[2].x(), p[2].y(), p[3].x(), p[3].y());
                FAIL("edge not monotonic");
            }
        }
    }
}

namespace {

// Whether every pixel the line plotters visit (main and antialias, after an optional SystemClip) lies in the box of
// the end points grown by one pixel
bool LinePixelsInEndPointBox(vdp::CoordS32 a, vdp::CoordS32 b, bool antiAlias, sint32 clipW, sint32 clipH) {
    vdp::LineStepper line{a, b, antiAlias};
    if (clipW >= 0) {
        line.SystemClip(clipW, clipH);
    }
    const sint32 minX = std::min(a.x(), b.x()) - 1, maxX = std::max(a.x(), b.x()) + 1;
    const sint32 minY = std::min(a.y(), b.y()) - 1, maxY = std::max(a.y(), b.y()) + 1;
    auto inside = [&](vdp::CoordS32 c) { return c.x() >= minX && c.x() <= maxX && c.y() >= minY && c.y() <= maxY; };
    bool aa = false;
    for (line.Step(); line.CanStep(); aa = line.Step()) {
        if (!inside(line.Coord()) || (aa && !inside(line.AACoord()))) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("VDP1 line pixels stay in the end point box", "[vdp][vdp1][quad-termination][brimir]") {
    // vdp1_clip.hpp relies on it for lines up to kMaxLineDelta long on both axes.
    constexpr sint32 kMax = vdp::VDP1QuadTermination::kMaxLineDelta;
    TestRng rng{11};
    uint64 failures = 0;
    for (sint32 dx = -24; dx <= 24; ++dx) {
        for (sint32 dy = -24; dy <= 24; ++dy) {
            for (bool antiAlias : {false, true}) {
                failures += !LinePixelsInEndPointBox({100, 100}, {100 + dx, 100 + dy}, antiAlias, -1, 0);
            }
        }
    }
    for (int i = 0; i < 300000; ++i) {
        const vdp::CoordS32 a{rng.Range(-2000, 2000), rng.Range(-2000, 2000)};
        const sint32 range = rng.Chance(50) ? kMax : 64;
        const vdp::CoordS32 b{a.x() + rng.Range(-range, range), a.y() + rng.Range(-range, range)};
        const bool clip = rng.Chance(50);
        const bool ok = LinePixelsInEndPointBox(a, b, rng.Chance(50), clip ? rng.Range(0, 704) : -1, rng.Range(0, 512));
        if (!ok) {
            CAPTURE(a.x(), a.y(), b.x(), b.y());
            ++failures;
        }
    }
    CHECK(failures == 0);

    // Longer lines wrap the 13-bit accumulators and leave the box, which is why the termination has a size limit
    uint64 escapes = 0;
    for (int i = 0; i < 20000; ++i) {
        const vdp::CoordS32 a{rng.Range(-4000, 0), rng.Range(-4000, 4000)};
        const vdp::CoordS32 b{a.x() + rng.Range(2048, 4000), a.y() + rng.Range(-4000, 4000)};
        escapes += !LinePixelsInEndPointBox(a, b, true, -1, 0);
    }
    CHECK(escapes > 0);
}

TEST_CASE("VDP1 coordinate-only quad stepping equals the full stepping", "[vdp][vdp1][quad-termination][brimir]") {
    // vdp1_clip.hpp pre-steps a copy with StepCoordinates; the gouraud shading must not influence the coordinates.
    TestRng rng{23};
    for (int i = 0; i < 5000; ++i) {
        vdp::CoordS32 p[4];
        for (auto &q : p) {
            q = rng.Chance(70) ? vdp::CoordS32{rng.Range(-500, 800), rng.Range(-500, 700)}
                               : vdp::CoordS32{rng.Range(-4096, 4095), rng.Range(-4096, 4095)};
        }
        vdp::QuadStepper full{p[0], p[1], p[2], p[3]};
        if (rng.Chance(50)) {
            full.SetupGouraud(vdp::Color555{.u16 = static_cast<uint16>(rng.Next())},
                              vdp::Color555{.u16 = static_cast<uint16>(rng.Next())},
                              vdp::Color555{.u16 = static_cast<uint16>(rng.Next())},
                              vdp::Color555{.u16 = static_cast<uint16>(rng.Next())});
        }
        vdp::QuadStepper coords = full;
        bool same = true;
        for (; full.CanStep(); full.Step(), coords.StepCoordinates()) {
            same = same && coords.CanStep() && coords.LeftEdge().X() == full.LeftEdge().X() &&
                   coords.LeftEdge().Y() == full.LeftEdge().Y() && coords.RightEdge().X() == full.RightEdge().X() &&
                   coords.RightEdge().Y() == full.RightEdge().Y();
        }
        REQUIRE((same && !coords.CanStep()));
    }
}
