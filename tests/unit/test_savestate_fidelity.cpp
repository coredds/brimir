// Save-state fidelity: a save followed by a load (into the same system or a fresh one) must continue exactly like the
// uninterrupted run (rewind, run-ahead and netplay rely on it). Each case sets up the point where upstream Ymir lost or
// changed state on a load, and checks the state after a round trip. Where the fix is the BrimirSaveState extension, the
// case also loads the same state without the extension (as a state from an older version loads) and shows the loss.

#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/saturn.hpp>

#include <functional>
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
        RoundTrip(*saturn);
        CHECK(Regs2(*saturn).TVSTAT.ODD == odd);
        CHECK(FreshWith(*Save(*saturn))->VDP.GetProbe().GetVDP2Regs().TVSTAT.ODD == odd);
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
