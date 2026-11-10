#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/savestate/savestate_vdp.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

using namespace ymir;
using namespace ymir::vdp;

constexpr uint32 kWidth = 320;
constexpr uint32 kHeight = 224;

uint32 CellPalette(uint32 cx, uint32 cy) {
    return 1 + (cx + 7 * cy) % 15;
}

uint16 PaletteColor(uint32 pal) {
    return static_cast<uint16>(((pal * 2) & 31) | (((pal * 5 + 3) & 31) << 5) | (((31 - pal) & 31) << 10));
}

// NBG0: 16-colour 1x1 cells with one-word pattern names, whose pattern name read (T1) and character pattern read
// (T4) in bank A0 make the character pattern fetch lag one cell behind, so the first cell of each line shows the
// character fetched last on the line before.
void SetUpDelayedNBG0(VDPState &state) {
    auto &regs = state.regs2;
    regs.WriteTVMD(0x8000);
    regs.LatchTVMD();
    regs.WriteBGON(0x0001);
    regs.WriteCHCTLA(0x0000); // NBG0: cells, 1x1, 16 colours
    regs.WritePNCNA(0x8000);  // One-word pattern names
    regs.WritePRINA(0x0007);
    regs.WriteCYCA0L(0xF0FF); // T1: NBG0 pattern name
    regs.WriteCYCA0U(0x4FFF); // T4: NBG0 character pattern (illegal after a T1 pattern name read -> delay)
    regs.WriteCYCA1L(0xFFFF);
    regs.WriteCYCA1U(0xFFFF);
    regs.WriteCYCB0L(0xFFFF);
    regs.WriteCYCB0U(0xFFFF);
    regs.WriteCYCB1L(0xFFFF);
    regs.WriteCYCB1U(0xFFFF);

    // Page of 64x64 pattern names at 0; every cell uses character 0x200 (address 0x4000) with its own palette.
    for (uint32 cy = 0; cy < 64; ++cy) {
        for (uint32 cx = 0; cx < 64; ++cx) {
            const uint16 pn = static_cast<uint16>((CellPalette(cx, cy) << 12) | 0x200);
            state.mem2.WriteVRAM<uint16>((cy * 64 + cx) * 2, pn);
        }
    }
    for (uint32 i = 0; i < 32; ++i) {
        state.mem2.WriteVRAM<uint8>(0x4000 + i, 0x11); // every dot uses colour 1 of the cell's palette
    }
    state.mem2.WriteCRAM<uint16>(1 * 2, 0x7C1F); // palette 0 (a palette number missing its low bits)
    for (uint32 pal = 1; pal < 16; ++pal) {
        state.mem2.WriteCRAM<uint16>(((pal << 4) | 1) * 2, PaletteColor(pal));
    }
}

struct Instance {
    std::unique_ptr<VDPState> state = std::make_unique<VDPState>();
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<SoftwareVDPRenderer> renderer = std::make_unique<SoftwareVDPRenderer>(*state, debug, access);
    std::vector<uint32> frame;

    void BindOutput() {
        renderer->SwCallbacks.FrameComplete.Rebind(this, [](uint32 *fb, uint32 w, uint32 h, void *context) {
            auto &out = *static_cast<Instance *>(context);
            out.frame.assign(fb, fb + w * h);
        });
    }
};

} // namespace

TEST_CASE("VDP2 delayed 16-colour character continues identically across a save state",
          "[vdp][savestate][regression][brimir]") {
    const uint32 split = GENERATE(1u, 37u, 112u, 223u);
    CAPTURE(split);

    Instance a;
    SetUpDelayedNBG0(*a.state);
    a.renderer->PostLoadStateSync(); // build the CRAM cache from the CRAM written above
    a.BindOutput();
    a.renderer->VDP2SetResolution(kWidth, kHeight, false);
    a.renderer->VDP2BeginFrame();
    for (uint32 y = 0; y < split; ++y) {
        a.renderer->VDP2RenderLine(y);
    }
    REQUIRE(a.state->regs2.bgParams[1].charPatDelay[0]);

    // Save as VDP::SaveState does and load into a fresh state and renderer as VDP::LoadState does.
    auto saved = std::make_unique<savestate::VDPSaveState>();
    a.renderer->PreSaveStateSync();
    a.state->SaveState(*saved);
    a.renderer->SaveState(saved->renderer);

    Instance b;
    b.state->LoadState(*saved);
    b.renderer->LoadState(saved->renderer);
    b.renderer->PostLoadStateSync();
    b.BindOutput();
    b.renderer->VDP2SetResolution(kWidth, kHeight, false);

    for (uint32 y = split; y < kHeight; ++y) {
        a.renderer->VDP2RenderLine(y);
        b.renderer->VDP2RenderLine(y);
    }
    a.renderer->VDP2EndFrame();
    b.renderer->VDP2EndFrame();
    REQUIRE(a.frame.size() == kWidth * kHeight);
    REQUIRE(b.frame.size() == kWidth * kHeight);

    // The uninterrupted run starts each line with the character delayed from the line before, whose palette
    // number has low bits: not palette 0 (0x7C1F).
    constexpr uint32 kPalette0 = 0xFFF800F8u;
    for (uint32 row = std::max(split, 1u); row < kHeight; ++row) {
        CAPTURE(row);
        REQUIRE(a.frame[row * kWidth] != kPalette0);
    }

    for (uint32 row = split; row < kHeight; ++row) {
        CAPTURE(row);
        for (uint32 x = 0; x < kWidth; ++x) {
            CAPTURE(x);
            REQUIRE(b.frame[row * kWidth + x] == a.frame[row * kWidth + x]);
        }
    }
}

TEST_CASE("VDP2 vertical cell scroll delay and repeat clear when the access goes away",
          "[vdp][vcellscroll][regression][brimir]") {
    auto state = std::make_unique<VDPState>();
    auto &regs = state->regs2;
    auto &nbg0 = state->state2.nbgLayerStates[0];
    auto &nbg1 = state->state2.nbgLayerStates[1];

    regs.WriteSCRCTL(0x0101); // Vertical cell scroll on NBG0 and NBG1
    regs.WriteCYCA0L(0xFFFF);
    regs.WriteCYCA0U(0xCDFF); // T4: NBG0 vertical cell scroll, T5: NBG1 vertical cell scroll
    regs.vcellScrollDirty = true;
    state->state2.CalcVCellScrollDelay(regs);
    REQUIRE(nbg0.vcellScrollDelay);
    REQUIRE(nbg0.vcellScrollRepeat);
    REQUIRE(nbg1.vcellScrollDelay);
    REQUIRE(regs.vcellScrollInc == 8);

    // Vertical cell scroll stays enabled, but no slot reads its table any more.
    regs.WriteCYCA0U(0xFFFF);
    regs.vcellScrollDirty = true;
    state->state2.CalcVCellScrollDelay(regs);
    CHECK(regs.vcellScrollInc == 0);
    CHECK_FALSE(nbg0.vcellScrollDelay);
    CHECK_FALSE(nbg0.vcellScrollRepeat);
    CHECK_FALSE(nbg1.vcellScrollDelay);
    CHECK_FALSE(nbg1.vcellScrollRepeat);

    // A slot before T2 neither delays nor repeats.
    regs.WriteCYCA0L(0xCFFF); // T0: NBG0 vertical cell scroll
    regs.vcellScrollDirty = true;
    state->state2.CalcVCellScrollDelay(regs);
    CHECK(regs.vcellScrollInc == 4);
    CHECK_FALSE(nbg0.vcellScrollDelay);
    CHECK_FALSE(nbg0.vcellScrollRepeat);
}

// Ymir ff93537: the assignment (render thread reset and post-load sync) returned a copy of VRAM and CRAM.
TEST_CASE("VDP2 memory assignment copies the contents and returns the target", "[vdp][savestate][backport]") {
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<VDP2Memory &>() = std::declval<const VDP2Memory &>()),
                                  VDP2Memory &>);
    VDP2Regs regs{};
    auto src = std::make_unique<VDP2Memory>(regs);
    auto dst = std::make_unique<VDP2Memory>(regs);
    src->VRAM.fill(0x5A);
    src->CRAM.fill(0xA5);
    VDP2Memory &result = (*dst = *src);
    CHECK(&result == dst.get());
    CHECK(dst->VRAM == src->VRAM);
    CHECK(dst->CRAM == src->CRAM);
}
