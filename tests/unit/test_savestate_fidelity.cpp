// Save-state fidelity: a save followed by a load (into the same system or a fresh one) must continue exactly like the
// uninterrupted run (rewind, run-ahead and netplay rely on it). Each case sets up the point where upstream Ymir lost or
// changed state on a load, and checks the state after a round trip. Where the fix is the BrimirSaveState extension, the
// case also loads the same state without the extension (as a state from an older version loads) and shows the loss.

#include "catch_amalgamated.hpp"

#include <brimir/lockstep.hpp>
#ifdef BRIMIR_VULKAN
#include <gpu/vdp/vulkan_vdp_renderer.hpp>
#endif
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/backup_ram.hpp>
#include <ymir/sys/saturn.hpp>

#include <cstring>
#include <functional>
#include <new>
#include <memory>

namespace {

using namespace ymir;

constexpr uint32 kVDP1VRAM = 0x5C0'0000;
constexpr uint32 kVDP1FB = 0x5C8'0000;
constexpr uint32 kVDP1PTMR = 0x5D0'0004;
constexpr uint32 kVDP2TVMD = 0x5F8'0000;
constexpr uint32 kVDP2EXTEN = 0x5F8'0002;
constexpr uint32 kSCSPMPRO = 0x5B0'0800;

constexpr uint16 kTVMDDisplayOn = 0x8000;
constexpr uint16 kTVMDDoubleDensity = 0x00C0; // LSMD = 11
constexpr uint16 kTVMDHiResA = 0x0002;        // HRESO = 2 (640 pixels)
constexpr uint16 kTVMDNormalB = 0x0001;       // HRESO = 1 (352 pixels)

// Steps the master SH-2 (which also runs the scheduler, so the VDP advances) until the predicate holds.
bool RunUntil(Saturn &saturn, const std::function<bool()> &done, uint64 maxCycles = 40'000'000) {
    uint64 cycles = 0;
    while (!done()) {
        if (cycles >= maxCycles) {
            return false;
        }
        cycles += saturn.StepMasterSH2();
    }
    return true;
}

const vdp::VDP2Regs &Regs2(Saturn &saturn) {
    return saturn.VDP.GetProbe().GetVDP2Regs();
}

// Runs to the given VCNT of the next frame (VCNT counts lines from the top of the display area).
bool RunToLine(Saturn &saturn, uint16 line) {
    return RunUntil(saturn, [&] { return Regs2(saturn).VCNT != line; }) &&
           RunUntil(saturn, [&] { return Regs2(saturn).VCNT == line; });
}

std::unique_ptr<savestate::SaveState> Save(Saturn &saturn) {
    auto state = std::make_unique<savestate::SaveState>();
    saturn.SaveState(*state);
    return state;
}

void RoundTrip(Saturn &saturn) {
    const auto state = Save(saturn);
    REQUIRE(saturn.LoadState(*state, true));
}

// A fresh system holding the given state, with or without the Brimir extension.
std::unique_ptr<Saturn> FreshWith(const savestate::SaveState &state, bool withExtension = true) {
    auto fresh = std::make_unique<Saturn>();
    if (withExtension) {
        REQUIRE(fresh->LoadState(state, true));
    } else {
        auto older = std::make_unique<savestate::SaveState>(state);
        older->brimir.valid = false;
        REQUIRE(fresh->LoadState(*older, true));
    }
    return fresh;
}

// A VDP1 command table that takes several frames to draw: system clipping to the screen, then full-screen polygons.
void WriteHeavyVDP1Table(Saturn &saturn, int polygons) {
    uint32 addr = 0;
    const auto cmd = [&](uint16 ctrl) {
        for (uint32 i = 0; i < 0x20; i += 2) {
            saturn.mainBus.Write<uint16>(kVDP1VRAM + addr + i, 0);
        }
        saturn.mainBus.Write<uint16>(kVDP1VRAM + addr, ctrl);
    };
    const auto field = [&](uint32 offset, uint16 value) { saturn.mainBus.Write<uint16>(kVDP1VRAM + addr + offset, value); };
    cmd(0x0009); // system clipping
    field(0x14, 319);
    field(0x16, 223);
    addr += 0x20;
    cmd(0x000A); // local coordinates (0, 0)
    addr += 0x20;
    for (int i = 0; i < polygons; ++i) {
        cmd(0x0004); // polygon
        field(0x06, static_cast<uint16>(0x8000 | (i * 0x0421)));
        field(0x0C, 0);
        field(0x0E, 0);
        field(0x10, 319);
        field(0x12, 0);
        field(0x14, 319);
        field(0x16, 223);
        field(0x18, 0);
        field(0x1A, 223);
        addr += 0x20;
    }
    cmd(0x8000); // end
}

std::unique_ptr<Saturn> ThreadedVDP1Saturn() {
    auto saturn = std::make_unique<Saturn>();
    saturn->configuration.video.threadedVDP1 = true;
    return saturn;
}

bool SameVDPExtension(const savestate::BrimirSaveState::VDP &a, const savestate::BrimirSaveState::VDP &b) {
    return a.appliedTVMD == b.appliedTVMD && a.TVMDDirty == b.TVMDDirty && a.VTimingField == b.VTimingField &&
           a.VCounterSkip == b.VCounterSkip && a.vdp1LastJumpAddress == b.vdp1LastJumpAddress &&
           a.vdp1LoopCount == b.vdp1LoopCount && a.vdp1InInfiniteLoop == b.vdp1InInfiniteLoop &&
           a.vdp1ReturnAddress == b.vdp1ReturnAddress;
}

} // namespace

TEST_CASE("Save state round trip keeps the interlaced field (TVSTAT.ODD)", "[savestate][fidelity][vdp][brimir]") {
    // Upstream saved TVSTAT as the CPU reads it (ODD inverted in interlaced modes) and loaded it raw.
    auto saturn = std::make_unique<Saturn>();
    saturn->mainBus.Write<uint16>(kVDP2TVMD, kTVMDDisplayOn | kTVMDDoubleDensity);
    for (int field = 0; field < 2; ++field) {
        CAPTURE(field);
        REQUIRE(RunToLine(*saturn, 100));
        REQUIRE(Regs2(*saturn).TVMD.IsInterlaced());
        const uint16 odd = Regs2(*saturn).TVSTAT.ODD;
        const auto state = Save(*saturn); // before the in-place round trip, so the two checks are independent
        CHECK(FreshWith(*state)->VDP.GetProbe().GetVDP2Regs().TVSTAT.ODD == odd);
        REQUIRE(saturn->LoadState(*state, true));
        CHECK(Regs2(*saturn).TVSTAT.ODD == odd);
    }
}

TEST_CASE("Save state round trip keeps the latched H counter", "[savestate][fidelity][vdp][brimir]") {
    // Upstream loaded HCNT through WriteHCNT, which shifts and masks the stored (already converted) counter again.
    auto saturn = std::make_unique<Saturn>();
    saturn->mainBus.Write<uint16>(kVDP2TVMD, kTVMDDisplayOn | kTVMDHiResA);
    REQUIRE(RunToLine(*saturn, 100)); // past the top border: the hi-res timings apply
    (void)saturn->mainBus.Read<uint16>(kVDP2EXTEN); // EXLTEN = 0: a read latches HCNT
    const uint16 hcnt = Regs2(*saturn).HCNT;
    REQUIRE(hcnt != 0);
    RoundTrip(*saturn);
    CHECK(Regs2(*saturn).HCNT == hcnt);
}

TEST_CASE("Save state round trip leaves a pending TVMD change pending", "[savestate][fidelity][vdp][brimir]") {
    // A TVMD write takes effect at the next top border. Upstream applied it at once on load, changing the line
    // timings in the middle of the frame (Sega Rally diverged after a round trip at such a point).
    auto saturn = std::make_unique<Saturn>();
    saturn->mainBus.Write<uint16>(kVDP2TVMD, kTVMDDisplayOn);
    REQUIRE(RunToLine(*saturn, 100));
    REQUIRE(saturn->VDP.GetProbe().GetResolution().width == 320);
    saturn->mainBus.Write<uint16>(kVDP2TVMD, kTVMDDisplayOn | kTVMDNormalB);
    REQUIRE(saturn->VDP.GetProbe().GetResolution().width == 320); // pending

    const auto state = Save(*saturn);
    RoundTrip(*saturn);
    CHECK(saturn->VDP.GetProbe().GetResolution().width == 320);
    CHECK(Regs2(*saturn).TVMDDirty);
    auto fresh = FreshWith(*state);
    CHECK(fresh->VDP.GetProbe().GetResolution().width == 320);
    CHECK(SameVDPExtension(fresh->VDP.GetProbe().GetBrimirState(), saturn->VDP.GetProbe().GetBrimirState()));

    // Both apply it at the next top border.
    REQUIRE(RunToLine(*saturn, 10));
    REQUIRE(RunToLine(*fresh, 10));
    CHECK(saturn->VDP.GetProbe().GetResolution().width == 352);
    CHECK(fresh->VDP.GetProbe().GetResolution().width == 352);

    // Without the extension (an older state) the change applied at the load.
    CHECK(FreshWith(*state, false)->VDP.GetProbe().GetResolution().width == 352);
}

TEST_CASE("A fresh system keeps the field timing of a state saved in the vertical blank",
          "[savestate][fidelity][vdp][brimir]") {
    // The field toggles at VBlank IN, but the V counter skip and timing field keep the values derived at the last top
    // border until the next one. Upstream re-derived them from the new field on load.
    auto saturn = std::make_unique<Saturn>();
    saturn->mainBus.Write<uint16>(kVDP2TVMD, kTVMDDisplayOn | kTVMDDoubleDensity);
    bool sawDifference = false;
    for (int field = 0; field < 2; ++field) {
        CAPTURE(field);
        REQUIRE(RunToLine(*saturn, 240)); // bottom blanking: after VBlank IN, before the top border
        const auto state = Save(*saturn);
        const auto expected = saturn->VDP.GetProbe().GetBrimirState();
        CHECK(SameVDPExtension(FreshWith(*state)->VDP.GetProbe().GetBrimirState(), expected));
        const auto older = FreshWith(*state, false)->VDP.GetProbe().GetBrimirState();
        sawDifference |= older.VCounterSkip != expected.VCounterSkip || older.VTimingField != expected.VTimingField;
    }
    CHECK(sawDifference); // the loss this fixes
}

TEST_CASE("A fresh system keeps the VDP1 return address and infinite loop state", "[savestate][fidelity][vdp][brimir]") {
    auto saturn = std::make_unique<Saturn>();
    // 0x000: skipped command, Call 0x100; 0x100: skipped command jumping to itself (an infinite loop)
    saturn->mainBus.Write<uint16>(kVDP1VRAM + 0x000, 0x4000 | 0x2000);
    saturn->mainBus.Write<uint16>(kVDP1VRAM + 0x002, 0x100 >> 3);
    saturn->mainBus.Write<uint16>(kVDP1VRAM + 0x100, 0x4000 | 0x1000);
    saturn->mainBus.Write<uint16>(kVDP1VRAM + 0x102, 0x100 >> 3);
    saturn->mainBus.Write<uint16>(kVDP1PTMR, 0x0001); // draw now
    REQUIRE(RunUntil(*saturn, [&] { return saturn->VDP.GetProbe().GetBrimirState().vdp1InInfiniteLoop; }));

    const auto expected = saturn->VDP.GetProbe().GetBrimirState();
    REQUIRE(expected.vdp1ReturnAddress == 0x20);
    REQUIRE(expected.vdp1LastJumpAddress == 0x100);
    const auto state = Save(*saturn);
    CHECK(SameVDPExtension(FreshWith(*state)->VDP.GetProbe().GetBrimirState(), expected));
    RoundTrip(*saturn);
    CHECK(SameVDPExtension(saturn->VDP.GetProbe().GetBrimirState(), expected));

    const auto older = FreshWith(*state, false)->VDP.GetProbe().GetBrimirState();
    CHECK_FALSE(older.vdp1InInfiniteLoop);
    CHECK(older.vdp1ReturnAddress != 0x20);
}

TEST_CASE("The VDP1 loop detection state starts defined", "[savestate][fidelity][vdp][uninit][brimir]") {
    // Upstream left loopCount and inInfiniteLoop uninitialized; a state saved during a draw (which upstream states hold
    // without them) loaded into a fresh system then ran the VDP1 with whatever the memory held.
    alignas(Saturn) static unsigned char storage[sizeof(Saturn)];
    std::memset(storage, 0xCD, sizeof(storage));
    auto *saturn = new (storage) Saturn();
    const auto ext = saturn->VDP.GetProbe().GetBrimirState();
    CHECK(ext.vdp1LoopCount == 0);
    CHECK_FALSE(ext.vdp1InInfiniteLoop);
    CHECK(ext.vdp1LastJumpAddress == 0xFFFFFFFF);
    saturn->~Saturn();
}

TEST_CASE("Save state keeps the SCSP DSP program length", "[savestate][fidelity][scsp][brimir]") {
    // The DSP runs m_programLength steps per sample; MPRO writes maintain it incrementally (a NOP written below the
    // end does not shrink it), so it cannot be derived from the program. Upstream re-derived it on load.
    auto saturn = std::make_unique<Saturn>();
    saturn->mainBus.Write<uint16>(kSCSPMPRO + 5 * 8, 0x1234); // step 5: not a NOP -> length 6 + 1
    saturn->mainBus.Write<uint16>(kSCSPMPRO + 5 * 8, 0x0000); // a NOP again below the end -> length + 1
    const uint8 length = saturn->SCSP.GetDSP().GetProgramLength();
    REQUIRE(length == 8);

    const auto state = Save(*saturn);
    RoundTrip(*saturn);
    CHECK(saturn->SCSP.GetDSP().GetProgramLength() == length);
    CHECK(FreshWith(*state)->SCSP.GetDSP().GetProgramLength() == length);
    CHECK(FreshWith(*state, false)->SCSP.GetDSP().GetProgramLength() != length);
}

TEST_CASE("A threaded VDP1 state saved in the middle of a draw continues identically",
          "[savestate][fidelity][vdp][vdp1][threaded][brimir]") {
    // A threaded software VDP1 draws into its own framebuffer copy and copies it into VDPState only when the draw
    // ends, so the state's FBRAM lacks what was drawn so far; a load continued from that older framebuffer.
    auto a = ThreadedVDP1Saturn();
    a->mainBus.Write<uint16>(kVDP2TVMD, kTVMDDisplayOn);
    WriteHeavyVDP1Table(*a, 40);
    a->mainBus.Write<uint16>(kVDP1PTMR, 0x0001);
    REQUIRE(RunToLine(*a, 100));
    const auto state = Save(*a);
    REQUIRE(state->vdp.vdp1State.drawing);
    const uint8 draw = state->vdp.displayFB ^ 1;
    REQUIRE(state->brimir.vdpRenderer.vdp1DrawFB != state->vdp.FBRAM[draw]); // drawn so far, not yet in the state

    auto b = ThreadedVDP1Saturn();
    REQUIRE(b->LoadState(*state, true));
    auto older = ThreadedVDP1Saturn();
    {
        auto copy = std::make_unique<savestate::SaveState>(*state);
        copy->brimir.valid = false;
        REQUIRE(older->LoadState(*copy, true));
    }
    bool olderDiffered = false;
    for (int frame = 0; frame < 6; ++frame) {
        CAPTURE(frame);
        for (Saturn *s : {a.get(), b.get(), older.get()}) {
            REQUIRE(RunToLine(*s, 100));
        }
        const auto sa = Save(*a);
        CHECK(brimir::DiffSaveStates(*sa, *Save(*b)).empty());
        olderDiffered |= !brimir::DiffSaveStates(*sa, *Save(*older)).empty();
    }
    CHECK(olderDiffered); // the loss this fixes
}

TEST_CASE("Loading a state waits for the threaded VDP1's queued draws", "[savestate][fidelity][vdp][vdp1][threaded][brimir]") {
    // Run-ahead loads the state of the previous frame right after a frame: the VDP1 render thread can still be drawing
    // that frame's commands, and those draws must not land in the loaded framebuffers.
    auto x = ThreadedVDP1Saturn();
    WriteHeavyVDP1Table(*x, 40);
    const auto state = Save(*x); // drains the render thread (the table is in its VRAM copy)
    auto *sw = x->VDP.GetRendererAs<vdp::VDPRendererType::Software>();
    REQUIRE(sw != nullptr);
    for (int attempt = 0; attempt < 5; ++attempt) {
        CAPTURE(attempt);
        // A burst of draws for the render thread (system clipping, local coordinates, 40 full-screen polygons, twice),
        // then at once the load, as run-ahead does after a frame.
        for (int pass = 0; pass < 2; ++pass) {
            for (uint32 cmd = 0; cmd < 42; ++cmd) {
                const vdp::VDP1Command::Control control{.u16 = x->mainBus.Peek<uint16>(kVDP1VRAM + cmd * 0x20)};
                sw->VDP1ExecuteCommand(cmd * 0x20, control);
            }
        }
        REQUIRE(x->LoadState(*state, true));
        CHECK(brimir::DiffSaveStates(*state, *Save(*x)).empty());
    }
}

#ifdef BRIMIR_VULKAN
TEST_CASE("Loading a state waits for the threaded VDP1 under the Vulkan renderer with VDP1 in software",
          "[savestate][fidelity][vdp][vdp1][threaded][brimir]") {
    // The Vulkan renderer without VDP1 on the GPU (no device here, as in its software fallback) draws VDP1 with its
    // inner software renderer, threaded; its PreLoadState must drain that renderer's threads too.
    auto x = std::make_unique<Saturn>();
    brimir::gpu::VulkanVDPRenderer *vk = nullptr;
    REQUIRE(x->VDP.UseExternalRenderer([&](vdp::VDPState &state, vdp::config::VDP2DebugRender &debug,
                                           const vdp::config::VDP2AccessPatternsConfig &access) {
        auto renderer = std::make_unique<brimir::gpu::VulkanVDPRenderer>(state, debug, access);
        vk = renderer.get();
        return renderer;
    }) != nullptr);
    vk->EnableThreadedVDP1(true);
    WriteHeavyVDP1Table(*x, 40);
    const auto state = Save(*x);
    for (int attempt = 0; attempt < 5; ++attempt) {
        CAPTURE(attempt);
        for (int pass = 0; pass < 2; ++pass) {
            for (uint32 cmd = 0; cmd < 42; ++cmd) {
                const vdp::VDP1Command::Control control{.u16 = x->mainBus.Peek<uint16>(kVDP1VRAM + cmd * 0x20)};
                vk->VDP1ExecuteCommand(cmd * 0x20, control);
            }
        }
        REQUIRE(x->LoadState(*state, true));
        CHECK(brimir::DiffSaveStates(*state, *Save(*x)).empty());
    }
}
#endif

TEST_CASE("Loading a state restores the SCSP slot modulation inputs", "[savestate][fidelity][scsp][brimir]") {
    // Each slot reads its modulation inputs from the sound stack in pipeline steps 2-2/2-3 and uses them in step 3-2,
    // possibly in a later sample; upstream did not save them, so a run-ahead load kept the values the frame run ahead
    // had read (Guardian Heroes' sound RAM diverged in a --rollback run).
    auto saturn = std::make_unique<Saturn>();
    const auto state = Save(*saturn);
    REQUIRE(state->brimir.scspSlots.valid);
    const sint16 savedX = saturn->SCSP.GetSlot(3).modXSample;
    const sint16 savedY = saturn->SCSP.GetSlot(17).modYSample;
    // As a frame run ahead leaves them (the test reaches the slot through its const accessor)
    const_cast<scsp::Slot &>(saturn->SCSP.GetSlot(3)).modXSample = static_cast<sint16>(savedX ^ 0x1234);
    const_cast<scsp::Slot &>(saturn->SCSP.GetSlot(17)).modYSample = static_cast<sint16>(savedY ^ 0x0F0F);
    REQUIRE(saturn->LoadState(*state, true));
    CHECK(saturn->SCSP.GetSlot(3).modXSample == savedX);
    CHECK(saturn->SCSP.GetSlot(17).modYSample == savedY);

    // Into a fresh system, with values the fresh one does not have
    auto modified = std::make_unique<savestate::SaveState>(*state);
    modified->brimir.scspSlots.modXSample[5] = 0x1357;
    modified->brimir.scspSlots.modYSample[30] = -0x2468;
    auto fresh = FreshWith(*modified);
    CHECK(fresh->SCSP.GetSlot(5).modXSample == 0x1357);
    CHECK(fresh->SCSP.GetSlot(30).modYSample == -0x2468);
    const auto resaved = Save(*fresh);
    CHECK(resaved->brimir.scspSlots.modXSample[5] == 0x1357);
    CHECK(resaved->brimir.scspSlots.modYSample[30] == -0x2468);
    // Without the extension (older states) the slots keep what they hold, as upstream
    CHECK(FreshWith(*modified, false)->SCSP.GetSlot(5).modXSample == 0);
}

namespace {

void GiveInternalBackupRAM(Saturn &saturn) {
    bup::BackupMemory memory;
    memory.CreateInMemory(bup::BackupMemorySize::_256Kbit);
    REQUIRE(saturn.mem.SetInternalBackupRAM(std::move(memory)));
}

} // namespace

TEST_CASE("Loading a state restores the internal backup RAM", "[savestate][fidelity][backup][brimir]") {
    // Games write the internal backup RAM while running (Daytona USA CE's records in attract mode); upstream states do
    // not hold it, so a run-ahead frame's write survived the load and the game read it a frame early.
    auto saturn = std::make_unique<Saturn>();
    GiveInternalBackupRAM(*saturn);
    auto &bup = saturn->mem.GetInternalBackupRAM();
    bup.WriteByte(0x80 << 1, 0x11);
    const auto state = Save(*saturn);
    REQUIRE(state->brimir.backupRAM.valid);
    CHECK(state->brimir.backupRAM.internal[0x80] == 0x11);

    bup.WriteByte(0x80 << 1, 0x80); // as the frame run ahead writes it
    bup.WriteByte(0x1234 << 1, 0x5A);
    REQUIRE(saturn->LoadState(*state, true));
    CHECK(bup.ReadByte(0x80 << 1) == 0x11);
    CHECK(bup.ReadByte(0x1234 << 1) == state->brimir.backupRAM.internal[0x1234]);

    // A fresh system with its own (formatted) image takes the state's bytes
    auto fresh = std::make_unique<Saturn>();
    GiveInternalBackupRAM(*fresh);
    {
        auto copy = std::make_unique<savestate::SaveState>(*state);
        REQUIRE(fresh->LoadState(*copy, true));
    }
    CHECK(fresh->mem.GetInternalBackupRAM().ReadAll() ==
          std::vector<uint8>(state->brimir.backupRAM.internal.begin(), state->brimir.backupRAM.internal.end()));

    // Older states (no extension) leave the backup RAM alone, as upstream
    auto older = std::make_unique<Saturn>();
    GiveInternalBackupRAM(*older);
    older->mem.GetInternalBackupRAM().WriteByte(0x80 << 1, 0x77);
    {
        auto copy = std::make_unique<savestate::SaveState>(*state);
        copy->brimir.valid = false;
        REQUIRE(older->LoadState(*copy, true));
    }
    CHECK(older->mem.GetInternalBackupRAM().ReadByte(0x80 << 1) == 0x77);

    // Without an image (a bare system) nothing is saved or written
    auto bare = std::make_unique<Saturn>();
    CHECK_FALSE(Save(*bare)->brimir.backupRAM.valid);
}

TEST_CASE("Save state round trip keeps the SMPC port registers", "[savestate][fidelity][smpc][brimir]") {
    // Upstream LoadState applied EXLE through WriteEXLE, which re-writes PDR1/PDR2 through the peripheral ports, and
    // SaveState stored DDR2 through ReadDDR2, which returns DDR1. Every BIOS boot diverged after a round trip.
    constexpr uint32 kPDR1 = 0x010'0075;
    constexpr uint32 kPDR2 = 0x010'0077;
    auto saturn = std::make_unique<Saturn>();
    saturn->SMPC.GetPeripheralPort1().ConnectControlPad();
    saturn->SMPC.GetPeripheralPort2().ConnectControlPad();
    auto state = Save(*saturn);
    state->smpc.DDR1 = 0x60; // TH and TR are outputs
    state->smpc.DDR2 = 0x20;
    state->smpc.PDR1 = 0x40;
    state->smpc.PDR2 = 0x1F;
    state->smpc.EXLE = 0;
    REQUIRE(saturn->LoadState(*state, true));
    CHECK(saturn->mainBus.Peek<uint8>(kPDR1) == 0x40);
    CHECK(saturn->mainBus.Peek<uint8>(kPDR2) == 0x1F);
    const auto resaved = Save(*saturn);
    CHECK(resaved->smpc.PDR1 == 0x40);
    CHECK(resaved->smpc.PDR2 == 0x1F);
    CHECK(resaved->smpc.DDR1 == 0x60);
    CHECK(resaved->smpc.DDR2 == 0x20);
    CHECK(resaved->smpc.EXLE == 0);
}

TEST_CASE("A fresh system keeps the other field's VDP1 framebuffer", "[savestate][fidelity][vdp][vdp1][brimir]") {
    // With deinterlacing, the software renderer keeps the other field of double-density interlace in altFBRAM; the
    // first frame after a load combines it with the loaded fields. Upstream did not save it.
    auto saturn = std::make_unique<Saturn>();
    saturn->VDP.ModifyEnhancements([](vdp::config::Enhancements &enh) { enh.deinterlace = true; });
    saturn->mainBus.Write<uint16>(kVDP2TVMD, kTVMDDisplayOn | kTVMDDoubleDensity);
    saturn->mainBus.Write<uint16>(kVDP1FB + 0x100, 0xBEEF); // a CPU write lands in both fields' framebuffers
    auto *sw = saturn->VDP.GetRendererAs<vdp::VDPRendererType::Software>();
    REQUIRE(sw != nullptr);
    sw->PreSaveStateSync();
    const auto alt0 = sw->GetAltFBRAM(0);
    const auto alt1 = sw->GetAltFBRAM(1);
    REQUIRE((alt0[0x100] == 0xBE || alt1[0x100] == 0xBE));

    const auto state = Save(*saturn);
    auto fresh = FreshWith(*state);
    fresh->VDP.ModifyEnhancements([](vdp::config::Enhancements &enh) { enh.deinterlace = true; });
    auto *freshSw = fresh->VDP.GetRendererAs<vdp::VDPRendererType::Software>();
    CHECK(freshSw->GetAltFBRAM(0) == alt0);
    CHECK(freshSw->GetAltFBRAM(1) == alt1);

    auto older = FreshWith(*state, false);
    auto *olderSw = older->VDP.GetRendererAs<vdp::VDPRendererType::Software>();
    CHECK_FALSE((olderSw->GetAltFBRAM(0) == alt0 && olderSw->GetAltFBRAM(1) == alt1));
}
