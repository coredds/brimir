// Vdp2FrontEnd (src/gpu/vdp2/vdp2_front.*), the Vulkan-free front half of the GPU VDP2 renderer, driven through the
// scene harness (vdp2_scene.hpp) exactly as the VDP drives a renderer:
// - parameter packing goldens for six register setups;
// - the batching rule at its boundaries (frame setup, first and last line), memcmp dirtiness, compose-only splits,
//   VRAM chunk snapshots, deinterlace row doubling and the weave line count;
// - the CRAM colour cache in modes 0-2 and the unconditional coefficient snapshot;
// - the resolution clear against the software renderer; sprite slices; feature counters; dropped frames;
// - the scene writer driving two (state, renderer) pairs in lockstep.
// GPU-free: tagged [vdp2], so it also runs in BRIMIR_VULKAN=OFF builds.
#include "catch_amalgamated.hpp"

#include "vdp2_scene.hpp"

#include <gpu/vdp2/vdp2_front.hpp>

#include <ymir/hw/vdp/renderer/vdp_renderer_null.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/util/data_ops.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace brimir::gpu::vdp2;
using brimir::test::Vdp2Scene;
using brimir::test::Vdp2SceneWriter;

namespace {

namespace vdp = ymir::vdp;

// Register addresses (VDP2 register block offsets)
constexpr uint32_t TVMD = 0x000, RAMCTL = 0x00E, BGON = 0x020, MZCTL = 0x022, CHCTLA = 0x028, BMPNA = 0x02C,
                   PNCN0 = 0x030, MPOFN = 0x03C, SCXIN0 = 0x070, SCXDN0 = 0x072, ZMXIN0 = 0x078, ZMXDN0 = 0x07A,
                   SCYN2 = 0x092, LCTAU = 0x0A8, LCTAL = 0x0AA, BKTAU = 0x0AC, BKTAL = 0x0AE, RPMD = 0x0B0, KTCTL = 0x0B4, WPSX0 = 0x0C0,
                   WPSY0 = 0x0C2, WPEX0 = 0x0C4, WPEY0 = 0x0C6, WCTLA = 0x0D0, LWTA0U = 0x0D8, LWTA0L = 0x0DA,
                   SPCTL = 0x0E0, SDCTL = 0x0E2, CCCTL = 0x0EC, PRISA = 0x0F0, PRISB = 0x0F2, PRISC = 0x0F4,
                   PRISD = 0x0F6, PRINA = 0x0F8, CCRSA = 0x100, CCRSB = 0x102, CLOFEN = 0x110, CLOFSL = 0x112,
                   COAR = 0x114, COAG = 0x116, COAB = 0x118, COBR = 0x11A, COBG = 0x11C, COBB = 0x11E;
constexpr uint32_t CYCA0L = 0x010, CYCA0U = 0x012, CYCB0L = 0x018, CYCB0U = 0x01A, CYCB1L = 0x01C, CYCB1U = 0x01E,
                   CHCTLB = 0x02A, SCYIN0 = 0x074, ZMYIN0 = 0x07C, ZMYDN0 = 0x07E, SCXIN1 = 0x080, ZMXIN1 = 0x088,
                   SCYN3 = 0x096, SCRCTL = 0x09A, VCSTAU = 0x09C, VCSTAL = 0x09E, LSTA0U = 0x0A0, LSTA0L = 0x0A2,
                   LSTA1U = 0x0A4, LSTA1L = 0x0A6, RPRCTL = 0x0B2, RPTAU = 0x0BC, RPTAL = 0x0BE;

// An IVDPRenderer that forwards the VDP2 calls to a Vdp2FrontEnd, the way the Vulkan renderer will.
class FrontEndRenderer final : public vdp::NullVDPRenderer {
public:
    FrontEndRenderer(vdp::VDPState &state, const vdp::config::VDP2AccessPatternsConfig &access)
        : front(state, access) {}

    Vdp2FrontEnd front;

    void Reset(bool hard) override {
        front.Reset(hard);
    }
    void PostLoadStateSync() override {
        front.PostLoadStateSync();
    }
    void SaveState(ymir::savestate::VDPSaveState::VDPRendererSaveState &state) override {
        front.SaveFetchers(state);
    }
    void LoadState(const ymir::savestate::VDPSaveState::VDPRendererSaveState &state) override {
        front.LoadFetchers(state);
    }
    void VDP2WriteVRAM(uint32_t address, uint8_t) override {
        front.WriteVRAM(address);
    }
    void VDP2WriteVRAM(uint32_t address, uint16_t) override {
        front.WriteVRAM(address);
    }
    void VDP2WriteCRAM(uint32_t address, uint8_t) override {
        front.WriteCRAM(address);
    }
    void VDP2WriteCRAM(uint32_t address, uint16_t) override {
        front.WriteCRAM(address);
    }
    void VDP2WriteReg(uint32_t address, uint16_t value) override {
        front.WriteReg(address, value);
    }
    void VDP2SetResolution(uint32_t h, uint32_t v, bool exclusive) override {
        front.SetResolution(h, v, exclusive);
    }
    void VDP2BeginFrame() override {
        front.BeginFrame();
    }
    void VDP2RenderLine(uint32_t y) override {
        front.RenderLine(y);
    }
    void VDP2EndFrame() override {
        front.EndFrame();
    }

protected:
    void UpdateEnhancements() override {
        front.SetEnhancements(m_enhancements);
    }
};

struct FrontRig {
    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<FrontEndRenderer> renderer = std::make_unique<FrontEndRenderer>(*state, access);
    Vdp2SceneWriter writer;

    explicit FrontRig(const vdp::config::Enhancements &enh = {})
        : writer(enh) {
        writer.Attach(*state, *renderer);
    }

    Vdp2FrontEnd &Front() {
        return renderer->front;
    }
    const Vdp2FrameRecord &Frame() {
        return renderer->front.Frame();
    }

    // Runs a frame and leaves its record in Frame(); the previous record is consumed first.
    const Vdp2FrameRecord &Run(const std::function<void(Vdp2SceneWriter &, uint32_t)> &onLine = {}) {
        Front().ConsumeFrame();
        writer.RunFrame(onLine);
        REQUIRE(Front().HasFrame());
        return Frame();
    }
};

template <typename T>
T ArenaAt(const Vdp2FrameRecord &record, uint32_t offset) {
    REQUIRE(offset + sizeof(T) <= record.arena.size());
    T value;
    std::memcpy(&value, record.arena.data() + offset, sizeof(T));
    return value;
}

VDP2LayerRenderParams LayerParams(const Vdp2FrameRecord &record, size_t batch = 0) {
    return ArenaAt<VDP2LayerRenderParams>(record, record.batches.at(batch).layerParamsOffset);
}

VDP2ComposeParams ComposeParams(const Vdp2FrameRecord &record, size_t batch = 0) {
    return ArenaAt<VDP2ComposeParams>(record, record.batches.at(batch).composeParamsOffset);
}

void DisplayOn(Vdp2SceneWriter &w) {
    w.Reg(TVMD, 0x8000); // DISP, 320x224 progressive
}

void CheckSpan(const Vdp2Batch &b, uint32_t layerStart, uint32_t layerLines, uint32_t composeStart,
               uint32_t composeLines) {
    CHECK(b.layerStartY == layerStart);
    CHECK(b.layerLines == layerLines);
    CHECK(b.composeStartY == composeStart);
    CHECK(b.composeLines == composeLines);
}

} // namespace

// -----------------------------------------------------------------------------
// Parameter packing

TEST_CASE("Vdp2FrontEnd packs NBG0 cell parameters", "[vdp2][front]") {
    FrontRig rig;
    auto &w = rig.writer;
    DisplayOn(w);
    w.Reg(BGON, 0x0001);   // NBG0 on, transparency on
    w.Reg(CHCTLA, 0x0011); // NBG0 2x2 cells, 256 colours
    w.Reg(PNCN0, 0xC16A);  // 1 word, CNSM, special CC, palette bits 0b011, char bits 0x0A
    w.Reg(SCXIN0, 0x0123);
    w.Reg(SCXDN0, 0x4500);
    w.Reg(ZMXIN0, 0x0002);
    w.Reg(ZMXDN0, 0x8000);
    w.Reg(PRINA, 0x0005);
    w.Reg(MZCTL, 0x2101); // NBG0 mosaic, H size 2, V size 3

    const auto &rec = rig.Run();
    const NBGParams nbg = LayerParams(rec).nbg[0];
    CHECK(nbg.base.enabled == 1);
    CHECK(nbg.base.enableTransparency == 1);
    CHECK(nbg.base.bitmap == 0);
    CHECK(nbg.base.cellSizeShift == 1);
    CHECK(nbg.base.colorFormat == 1);
    CHECK(nbg.base.twoWordChar == 0);
    CHECK(nbg.base.extChar == 1);
    CHECK(nbg.base.supplScrollCharNum == 0x0A);
    CHECK(nbg.base.supplPalNum == 0x30);
    CHECK(nbg.base.supplSpecialColorCalc == 1);
    CHECK(nbg.base.supplSpecialPriority == 0);
    CHECK(nbg.base.priorityNumber == 5);
    CHECK(nbg.base.mosaicEnable == 1);
    CHECK(nbg.scrollAmount.x == 0x12345);
    CHECK(nbg.scrollInc.x == 0x280);

    const VDP2CommonRenderParams &common = rec.batches[0].common;
    CHECK(common.layerParams.bgEnabled == 0b000001);
    CHECK(common.layerParams.layerEnabled == 0b000101); // sprite + NBG0
    CHECK(common.layerParams.mosaicH == 1);
    CHECK(common.layerParams.mosaicV == 2);
    CHECK(common.displayParams.displayEnable == 1);
    CHECK(common.displayParams.hresMode == 0);
}

TEST_CASE("Vdp2FrontEnd packs NBG1 bitmap parameters", "[vdp2][front]") {
    FrontRig rig;
    auto &w = rig.writer;
    DisplayOn(w);
    w.Reg(BGON, 0x0202);   // NBG1 on, NBG1 transparency off
    w.Reg(CHCTLA, 0x1A00); // NBG1 bitmap 1024x256, 256 colours
    w.Reg(BMPNA, 0x3500);  // NBG1 palette 5, special CC, special priority
    w.Reg(MPOFN, 0x0030);  // NBG1 bitmap at 0x60000

    const auto &rec = rig.Run();
    const NBGParams nbg = LayerParams(rec).nbg[1];
    CHECK(nbg.base.enabled == 1);
    CHECK(nbg.base.enableTransparency == 0);
    CHECK(nbg.base.bitmap == 1);
    CHECK(nbg.base.colorFormat == 1);
    CHECK(nbg.base.bitmapSize.x == 1024);
    CHECK(nbg.base.bitmapSize.y == 256);
    CHECK(nbg.base.supplPalNum == 0x500);
    CHECK(nbg.base.supplSpecialColorCalc == 1);
    CHECK(nbg.base.supplSpecialPriority == 1);
    CHECK(nbg.base.bitmapBaseAddress == 0x60000);
    CHECK(LayerParams(rec).nbg[0].base.enabled == 0);
}

TEST_CASE("Vdp2FrontEnd packs RBG0 coefficient parameters", "[vdp2][front]") {
    FrontRig rig;
    auto &w = rig.writer;
    DisplayOn(w);
    w.Reg(RAMCTL, 0x9001); // CRAM coefficients, CRAM mode 1, VRAM-A0 holds coefficients
    w.Reg(BGON, 0x0010);   // RBG0 on
    w.Reg(KTCTL, 0x001D);  // table A on, 2 words, mode 3 (Px), line colour data
    w.Reg(RPMD, 0x0002);

    const auto &rec = rig.Run();
    const VDP2CommonRenderParams &common = rec.batches[0].common;
    CHECK(common.rotParams.coeffTableCRAM == 1);
    CHECK(common.rotParams.coeffDataAccess == 0b0011); // A0, and A1 follows A0 when VRAM-A is not partitioned
    CHECK(common.rotParams.coeffATableEnable == 1);
    CHECK(common.rotParams.coeffADataSize == 0);
    CHECK(common.rotParams.coeffADataMode == 3);
    CHECK(common.rotParams.coeffAUseLineColorData == 1);
    CHECK(common.rotParams.coeffBTableEnable == 0);
    CHECK(common.layerParams.rotParamMode == 2);
    CHECK(common.layerParams.bgEnabled == 0b010000);
    CHECK(common.layerParams.layerEnabled == 0b000011); // sprite + RBG0
    CHECK(common.displayParams.colorRAMMode == 1);
    CHECK(LayerParams(rec).rbg[0].base.enabled == 1);
    CHECK(LayerParams(rec).rbg[1].base.enabled == 0);
}

TEST_CASE("Vdp2FrontEnd packs window parameters", "[vdp2][front]") {
    FrontRig rig;
    auto &w = rig.writer;
    DisplayOn(w);
    w.Reg(WPSX0, 0x0010);
    w.Reg(WPSY0, 0x0020);
    w.Reg(WPEX0, 0x0100);
    w.Reg(WPEY0, 0x00C0);
    w.Reg(LWTA0U, 0x8001);
    w.Reg(LWTA0L, 0x2000);
    w.Reg(WCTLA, 0x00AB); // NBG0: AND, SW on, W1 on, W0 on + inverted

    const auto &rec = rig.Run();
    const VDP2LayerRenderParams layer = LayerParams(rec);
    CHECK(layer.windows[0].start.x == 0x10);
    CHECK(layer.windows[0].start.y == 0x20);
    CHECK(layer.windows[0].end.x == 0x100);
    CHECK(layer.windows[0].end.y == 0xC0);
    CHECK(layer.windows[0].lineWindowTableAddress == 0x24000);
    CHECK(layer.windows[0].lineWindowTableEnable == 1);
    CHECK(layer.windows[1].lineWindowTableEnable == 0);
    const VDP2LayerWindowParamsS &win = layer.nbg[0].base.windowParams;
    CHECK(win.base.windowLogicAnd == 1);
    CHECK(win.base.window0Enable == 1);
    CHECK(win.base.window0Invert == 1);
    CHECK(win.base.window1Enable == 1);
    CHECK(win.base.window1Invert == 0);
    CHECK(win.spriteWindowEnable == 1);
    CHECK(win.spriteWindowInvert == 0);
}

TEST_CASE("Vdp2FrontEnd packs sprite priorities and ratios", "[vdp2][front]") {
    FrontRig rig;
    auto &w = rig.writer;
    DisplayOn(w);
    w.Reg(PRISA, 0x0201);
    w.Reg(PRISB, 0x0403);
    w.Reg(PRISC, 0x0605);
    w.Reg(PRISD, 0x0007);
    w.Reg(CCRSA, 0x011F); // ratios stored inverted: 0, 30
    w.Reg(CCRSB, 0x0302); // 29, 28; CCRSC/CCRSD stay 0 -> 31
    w.Reg(SPCTL, 0x2135); // type 5, sprite window, mixed, CC value 1, CC condition 2

    const auto &rec = rig.Run();
    const VDP2CommonRenderParams &common = rec.batches[0].common;
    CHECK(common.spritePriosRatios.x == 0xE4EBF201u);
    CHECK(common.spritePriosRatios.y == 0xF8FFFEFDu);
    CHECK(common.spriteParams.type == 5);
    CHECK(common.spriteParams.useSpriteWindow == 1);
    CHECK(common.spriteParams.mixedFormat == 1);
    CHECK(common.spriteParams.colorCalcValue == 1);
    CHECK(common.spriteParams.colorCalcCond == 2);
    CHECK(common.spriteParams.displayFB == rig.state->fbIndex.display);
}

TEST_CASE("Vdp2FrontEnd packs colour offset, colour calculation and shadow parameters", "[vdp2][front]") {
    FrontRig rig;
    auto &w = rig.writer;
    w.Reg(TVMD, 0x8100);  // DISP + BDCLMD
    w.Reg(CLOFEN, 0x0041); // sprite, NBG0
    w.Reg(CLOFSL, 0x0020); // back screen uses B
    w.Reg(COAR, 0x01FF);
    w.Reg(COAG, 0x00FF);
    w.Reg(COAB, 0x0100);
    w.Reg(COBR, 0x0001);
    w.Reg(COBG, 0x01F0);
    w.Reg(COBB, 0x0000);
    w.Reg(SDCTL, 0x0021); // NBG0 and back screen shadow
    w.Reg(CCCTL, 0x0041); // NBG0 and sprite colour calculation

    const auto &rec = rig.Run();
    const VDP2ComposeParams compose = ComposeParams(rec);
    CHECK(compose.colorOffsetEnable == 0b0000101);
    CHECK(compose.colorOffsetSelect == 0b1000000);
    CHECK(compose.colorOffsetA.r == -1);
    CHECK(compose.colorOffsetA.g == 255);
    CHECK(compose.colorOffsetA.b == -256);
    CHECK(compose.colorOffsetB.r == 1);
    CHECK(compose.colorOffsetB.g == -16);
    CHECK(compose.colorOffsetB.b == 0);
    CHECK(compose.shadowEnable == 0b1000100);
    CHECK(compose.colorCalcEnable == 0b00000101);

    const VDP2CommonRenderParams &common = rec.batches[0].common;
    CHECK(common.borderColorModeLatch == 1);
    CHECK(common.displayParams.borderColorMode == 1);
    CHECK(common.displayParams.firstInterlacedFrame == 0);
}

// -----------------------------------------------------------------------------
// Batching

TEST_CASE("Vdp2FrontEnd records a static frame as one batch", "[vdp2][front]") {
    FrontRig rig;
    DisplayOn(rig.writer);

    const auto &first = rig.Run();
    REQUIRE(first.batches.size() == 1);
    CheckSpan(first.batches[0], 0, 224, 0, 224);
    // The first frame uploads all of VRAM (one merged run) and CRAM.
    REQUIRE(first.batches[0].copyCount == 3);
    REQUIRE(first.copies.size() == 3);
    CHECK(first.copies[0].target == Vdp2Copy::Target::VRAM);
    CHECK(first.copies[0].dstOffset == 0);
    CHECK(first.copies[0].size == vdp::kVDP2VRAMSize);
    CHECK(first.copies[1].target == Vdp2Copy::Target::CRAMColors);
    CHECK(first.copies[2].target == Vdp2Copy::Target::CRAMCoeffs);
    CHECK(first.hres == 320);
    CHECK(first.vres == 224);
    CHECK(first.fieldLines == 224);
    CHECK(first.features.batches == 1);

    const auto &second = rig.Run();
    REQUIRE(second.batches.size() == 1);
    CheckSpan(second.batches[0], 0, 224, 0, 224);
    CHECK(second.copies.empty());
    CHECK(second.batches[0].copyCount == 0);
}

TEST_CASE("Vdp2FrontEnd splits layers at a register write before RenderLine(y)", "[vdp2][front]") {
    FrontRig rig;
    DisplayOn(rig.writer);
    rig.Run();

    const uint32_t y = GENERATE(1u, 100u, 223u);
    CAPTURE(y);
    const auto &rec = rig.Run([&](Vdp2SceneWriter &w, uint32_t line) {
        if (line == y) {
            w.Reg(SCXIN0, 0x0042);
        }
    });
    REQUIRE(rec.batches.size() == 2);
    CheckSpan(rec.batches[0], 0, y, 0, 0);
    CheckSpan(rec.batches[1], y, 224 - y, 0, 224);
    CHECK(LayerParams(rec, 0).nbg[0].scrollAmount.x == 0);
    CHECK(LayerParams(rec, 1).nbg[0].scrollAmount.x == 0x4200);
    CHECK(rec.batches[0].composeParamsOffset == rec.batches[1].composeParamsOffset);
}

TEST_CASE("Vdp2FrontEnd applies writes before line 0 as frame setup", "[vdp2][front]") {
    FrontRig rig;
    DisplayOn(rig.writer);
    rig.Run();

    const auto &rec = rig.Run([](Vdp2SceneWriter &w, uint32_t line) {
        if (line == 0) {
            w.Reg(SCXIN0, 0x0042);
            w.Reg(CLOFEN, 0x0001);
            w.VRAM16(0x2000, 0x1234);
        }
    });
    REQUIRE(rec.batches.size() == 1);
    CheckSpan(rec.batches[0], 0, 224, 0, 224);
    CHECK(LayerParams(rec).nbg[0].scrollAmount.x == 0x4200);
    CHECK(ComposeParams(rec).colorOffsetEnable == 0b100);
    REQUIRE(rec.batches[0].copyCount == 1);
    CHECK(rec.copies[0].dstOffset == 0x2000);
}

TEST_CASE("Vdp2FrontEnd does not split on a write of an identical value", "[vdp2][front]") {
    FrontRig rig;
    DisplayOn(rig.writer);
    rig.writer.Reg(SCXIN0, 0x0042);
    rig.writer.Reg(CLOFEN, 0x0001);
    rig.writer.Reg(BGON, 0x0000);
    rig.Run();

    const auto &rec = rig.Run([](Vdp2SceneWriter &w, uint32_t line) {
        if (line == 100) {
            w.Reg(SCXIN0, 0x0042);
            w.Reg(CLOFEN, 0x0001);
            w.Reg(BGON, 0x0000);
        }
    });
    REQUIRE(rec.batches.size() == 1);
    CheckSpan(rec.batches[0], 0, 224, 0, 224);
}

TEST_CASE("Vdp2FrontEnd splits only compose at a compose-only register write", "[vdp2][front]") {
    FrontRig rig;
    DisplayOn(rig.writer);
    rig.Run();

    const auto &rec = rig.Run([](Vdp2SceneWriter &w, uint32_t line) {
        if (line == 100) {
            w.Reg(CLOFEN, 0x0001);
        }
    });
    REQUIRE(rec.batches.size() == 2);
    // One layer dispatch over the whole frame, two compose ranges; the second compose waits for the layer rows.
    CheckSpan(rec.batches[0], 0, 224, 0, 100);
    CheckSpan(rec.batches[1], 0, 0, 100, 124);
    CHECK(ComposeParams(rec, 0).colorOffsetEnable == 0);
    CHECK(ComposeParams(rec, 1).colorOffsetEnable == 0b100);
    CHECK(rec.batches[1].copyCount == 0);
}

TEST_CASE("Vdp2FrontEnd interleaves compose ranges with later layer splits", "[vdp2][front]") {
    FrontRig rig;
    DisplayOn(rig.writer);
    rig.Run();

    const auto &rec = rig.Run([](Vdp2SceneWriter &w, uint32_t line) {
        if (line == 50) {
            w.Reg(CLOFEN, 0x0001); // compose only
        }
        if (line == 120) {
            w.Reg(SCXIN0, 0x0007); // layers only
        }
        if (line == 200) {
            w.Reg(TVMD, 0x8100); // common (BDCLMD): both
        }
    });
    REQUIRE(rec.batches.size() == 3);
    CheckSpan(rec.batches[0], 0, 120, 0, 50);
    CheckSpan(rec.batches[1], 120, 80, 50, 150); // compose [50, 200) waited for layers [120, 200)
    CheckSpan(rec.batches[2], 200, 24, 200, 24);
    CHECK(ComposeParams(rec, 0).colorOffsetEnable == 0);
    CHECK(ComposeParams(rec, 1).colorOffsetEnable == 0b100);
    CHECK(LayerParams(rec, 1).nbg[0].scrollAmount.x == 0x0700);
    CHECK(rec.batches[1].common.displayParams.borderColorMode == 0);
    CHECK(rec.batches[2].common.displayParams.borderColorMode == 1);
    // In replay order (copies, layers, compose per batch), a compose range never ends past the layer rows recorded
    // before or with it.
    uint32_t layersEnd = 0;
    for (const Vdp2Batch &b : rec.batches) {
        if (b.layerLines > 0) {
            layersEnd = b.layerStartY + b.layerLines;
        }
        if (b.composeLines > 0) {
            CHECK(b.composeStartY + b.composeLines <= layersEnd);
        }
    }
}

TEST_CASE("Vdp2FrontEnd splits at VRAM writes and snapshots exactly the dirty chunks", "[vdp2][front]") {
    FrontRig rig;
    DisplayOn(rig.writer);
    rig.Run();

    const auto &rec = rig.Run([](Vdp2SceneWriter &w, uint32_t line) {
        if (line == 50) {
            w.VRAM16(0x1000, 0xAAAA); // chunk 0x10
            w.VRAM16(0x10FE, 0xBBBB); // chunk 0x10
            w.VRAM16(0x1100, 0xCCCC); // chunk 0x11, merges with 0x10
            w.VRAM16(0x4000, 0xDDDD); // chunk 0x40
        }
        if (line == 60) {
            w.VRAM16(0x1000, 0xEEEE); // same chunk again, new value
        }
    });
    REQUIRE(rec.batches.size() == 3);
    CheckSpan(rec.batches[0], 0, 50, 0, 0);
    CheckSpan(rec.batches[1], 50, 10, 0, 0);
    CheckSpan(rec.batches[2], 60, 164, 0, 224);
    CHECK(rec.batches[0].copyCount == 0);

    REQUIRE(rec.batches[1].copyCount == 2);
    const Vdp2Copy &run = rec.copies[rec.batches[1].firstCopy];
    const Vdp2Copy &single = rec.copies[rec.batches[1].firstCopy + 1];
    CHECK(run.target == Vdp2Copy::Target::VRAM);
    CHECK(run.dstOffset == 0x1000);
    CHECK(run.size == 0x200);
    CHECK(single.dstOffset == 0x4000);
    CHECK(single.size == 0x100);
    // Snapshots hold the VRAM contents at flush time, not at the end of the frame.
    CHECK(rec.arena[run.arenaOffset + 0] == 0xAA);
    CHECK(rec.arena[run.arenaOffset + 0xFE] == 0xBB);
    CHECK(rec.arena[run.arenaOffset + 0x100] == 0xCC);
    CHECK(rec.arena[single.arenaOffset] == 0xDD);

    REQUIRE(rec.batches[2].copyCount == 1);
    const Vdp2Copy &again = rec.copies[rec.batches[2].firstCopy];
    CHECK(again.dstOffset == 0x1000);
    CHECK(again.size == 0x100);
    CHECK(rec.arena[again.arenaOffset] == 0xEE);
    CHECK(rec.features.copyBytes == 0x400);
}

TEST_CASE("Vdp2FrontEnd doubles rows when deinterlacing and follows the weave line count", "[vdp2][front]") {
    const bool deinterlace = GENERATE(false, true);
    CAPTURE(deinterlace);
    vdp::config::Enhancements enh{};
    enh.deinterlace = deinterlace;
    FrontRig rig{enh};
    rig.writer.Reg(TVMD, 0x80C0); // DISP, double-density interlace, 320x448
    rig.Run();
    REQUIRE(rig.writer.VRes() == 448);
    REQUIRE(rig.writer.FieldLines() == 224);

    const auto &rec = rig.Run([](Vdp2SceneWriter &w, uint32_t line) {
        if (line == 100) {
            w.Reg(CLOFEN, 0x0001);
        }
        if (line == 150) {
            w.Reg(SCXIN0, 0x0003);
        }
    });
    CHECK(rec.vres == 448);
    CHECK(rec.fieldLines == 224);
    CHECK(rec.interlaced);
    CHECK(rec.deinterlace == deinterlace);
    const uint32_t s = deinterlace ? 1 : 0;
    REQUIRE(rec.batches.size() == 2);
    CheckSpan(rec.batches[0], 0, 150 << s, 0, 100 << s);
    CheckSpan(rec.batches[1], 150 << s, 74 << s, 100 << s, 124 << s);
    // The last compose ends at VRes >> (interlaced && !deinterlace) rows.
    const Vdp2Batch &last = rec.batches.back();
    CHECK(last.composeStartY + last.composeLines == (448u >> (deinterlace ? 0 : 1)));
    CHECK(last.layerStartY + last.layerLines == (448u >> (deinterlace ? 0 : 1)));
}

TEST_CASE("Vdp2FrontEnd batches reproduce per-line state for random mid-frame writes", "[vdp2][front]") {
    // Every line must render with the registers and VRAM as of its own RenderLine call: replaying the batches in
    // order (copies, then layers, then compose) must cover each row once and see that line's values.
    const uint32_t seed = GENERATE(1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u);
    const bool deinterlace = GENERATE(false, true);
    CAPTURE(seed, deinterlace);
    std::mt19937 rng{seed};

    vdp::config::Enhancements enh{};
    enh.deinterlace = deinterlace;
    FrontRig rig{enh};
    rig.writer.Reg(TVMD, deinterlace ? 0x80C0 : 0x8000);
    rig.Run();
    const uint32_t lines = rig.writer.FieldLines();
    const uint32_t s = deinterlace ? 1 : 0;

    std::vector<uint16_t> scroll(lines), clofen(lines), vram(lines);
    uint16_t curScroll = 0, curClofen = 0, curVram = 0;
    std::vector<int> writes(lines, 0);
    for (uint32_t y = 0; y < lines; ++y) {
        if (rng() % 8 == 0) {
            writes[y] = 1 + rng() % 7; // bit 0: layer register, bit 1: compose register, bit 2: VRAM
        }
    }
    const auto &rec = rig.Run([&](Vdp2SceneWriter &w, uint32_t y) {
        if (writes[y] & 1) {
            curScroll = static_cast<uint16_t>(rng() & 0x7FF);
            w.Reg(SCXIN0, curScroll);
        }
        if (writes[y] & 2) {
            curClofen = static_cast<uint16_t>(rng() & 0x7F);
            w.Reg(CLOFEN, curClofen);
        }
        if (writes[y] & 4) {
            curVram = static_cast<uint16_t>(rng());
            w.VRAM16(0x3000, curVram);
        }
        scroll[y] = curScroll;
        clofen[y] = curClofen;
        vram[y] = curVram;
    });

    std::vector<uint8_t> gpuVRAM(rig.state->mem2.VRAM.size(), 0);
    std::vector<int> layerCover(lines << s, 0), composeCover(lines << s, 0);
    for (const Vdp2Batch &b : rec.batches) {
        for (uint32_t i = 0; i < b.copyCount; ++i) {
            const Vdp2Copy &c = rec.copies[b.firstCopy + i];
            if (c.target == Vdp2Copy::Target::VRAM) {
                std::memcpy(gpuVRAM.data() + c.dstOffset, rec.arena.data() + c.arenaOffset, c.size);
            }
        }
        const VDP2LayerRenderParams layer = ArenaAt<VDP2LayerRenderParams>(rec, b.layerParamsOffset);
        for (uint32_t row = b.layerStartY; row < b.layerStartY + b.layerLines; ++row) {
            const uint32_t y = row >> s;
            CAPTURE(row);
            ++layerCover[row];
            CHECK(layer.nbg[0].scrollAmount.x == (static_cast<uint32_t>(scroll[y]) << 8));
            CHECK(util::ReadBE<uint16_t>(&gpuVRAM[0x3000]) == vram[y]);
        }
        const VDP2ComposeParams compose = ArenaAt<VDP2ComposeParams>(rec, b.composeParamsOffset);
        for (uint32_t row = b.composeStartY; row < b.composeStartY + b.composeLines; ++row) {
            const uint32_t y = row >> s;
            CAPTURE(row);
            CHECK(layerCover[row] == 1); // its layer rows were recorded before or with it
            ++composeCover[row];
            uint16_t expected = 0;
            for (uint32_t bit = 0; bit < 7; ++bit) {
                // CLOFEN bit order -> packed layer order (sprite, RBG0, NBG0, NBG1, NBG2, NBG3, back)
                static constexpr std::array<uint32_t, 7> kLayer{2, 3, 4, 5, 1, 6, 0};
                if (clofen[y] & (1u << bit)) {
                    expected |= 1u << kLayer[bit];
                }
            }
            CHECK(compose.colorOffsetEnable == expected);
        }
    }
    for (uint32_t row = 0; row < (lines << s); ++row) {
        CAPTURE(row);
        CHECK(layerCover[row] == 1);
        CHECK(composeCover[row] == 1);
    }
}

// -----------------------------------------------------------------------------
// CRAM

TEST_CASE("Vdp2FrontEnd CRAM colour cache matches each CRAM mode", "[vdp2][front]") {
    FrontRig rig;
    auto &w = rig.writer;
    const Vdp2FrontEnd &front = rig.Front();

    auto expect555 = [](uint16_t c) {
        return ColorR8G8B8A8{static_cast<uint8_t>((c & 0x1F) << 3), static_cast<uint8_t>(((c >> 5) & 0x1F) << 3),
                             static_cast<uint8_t>(((c >> 10) & 0x1F) << 3), static_cast<uint8_t>(c >> 15)};
    };
    auto same = [](ColorR8G8B8A8 a, ColorR8G8B8A8 b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };

    SECTION("mode 0: 1024 RGB555 words, mirrored") {
        w.Reg(RAMCTL, 0x0000);
        w.CRAM16(0x002, 0x801F); // red, MSB
        w.CRAM16(0x7FE, 0x7C00); // blue
        CHECK(same(front.ColorCache()[1], expect555(0x801F)));
        CHECK(same(front.ColorCache()[1023], expect555(0x7C00)));
        CHECK(same(front.ColorCache()[1024 + 1], expect555(0x801F))); // the mirror half
    }
    SECTION("mode 1: 2048 RGB555 words") {
        w.Reg(RAMCTL, 0x1000);
        w.CRAM16(0x002, 0x03E0);  // green
        w.CRAM16(0x802, 0x7FFF);  // white, upper half
        CHECK(same(front.ColorCache()[1], expect555(0x03E0)));
        CHECK(same(front.ColorCache()[1025], expect555(0x7FFF)));
    }
    SECTION("mode 2: 1024 RGB888 longwords") {
        w.Reg(RAMCTL, 0x2000);
        w.CRAM16(0x008, 0x8056); // MSB, blue 0x56
        w.CRAM16(0x00A, 0x3412); // green 0x34, red 0x12
        const ColorR8G8B8A8 c = front.ColorCache()[2];
        CHECK(c.r == 0x12);
        CHECK(c.g == 0x34);
        CHECK(c.b == 0x56);
        CHECK(c.a == 1);
    }

    // The frame uploads the cache as it is, then the RGB555 table of every CRAM word.
    const auto &rec = rig.Run();
    bool found = false;
    for (const Vdp2Copy &copy : rec.copies) {
        if (copy.target == Vdp2Copy::Target::CRAMColors) {
            found = true;
            REQUIRE(copy.size == 2 * sizeof(CRAMColorCache));
            REQUIRE(copy.dstOffset == 0);
            const uint8_t *data = rec.arena.data() + copy.arenaOffset;
            CHECK(std::memcmp(data, front.ColorCache().data(), sizeof(CRAMColorCache)) == 0);
            for (uint32_t i = 0; i < kVDP2CRAMColorCacheEntries; ++i) {
                CAPTURE(i);
                ColorR8G8B8A8 c;
                std::memcpy(&c, data + (kVDP2CRAM555TableBase + i) * sizeof(c), sizeof(c));
                const uint16_t word = util::ReadBE<uint16_t>(&rig.state->mem2.CRAM[i * 2]);
                REQUIRE(same(c, expect555(word)));
            }
        }
    }
    CHECK(found);
}

TEST_CASE("Vdp2FrontEnd RGB555 colour cache follows software's in every CRAM mode", "[vdp2][front]") {
    // Software's m_CRAMCache (sw:897-914) converts every raw CRAM word, whatever the mode; LNCL reads it.
    const uint32_t mode = GENERATE(0u, 1u, 2u);
    CAPTURE(mode);
    FrontRig rig;
    auto &w = rig.writer;
    w.Reg(RAMCTL, static_cast<uint16_t>(mode << 12));
    std::mt19937 rng{mode + 1};
    for (uint32_t a = 0; a < 0x1000; a += 2) {
        w.CRAM16(a, static_cast<uint16_t>(rng()));
    }
    const auto &cache = rig.Front().Color555Cache();
    for (uint32_t i = 0; i < kVDP2CRAMColorCacheEntries; ++i) {
        CAPTURE(i);
        const vdp::Color555 c5{.u16 = util::ReadBE<uint16_t>(&rig.state->mem2.CRAM[i * 2])};
        REQUIRE(cache[i].u32 == vdp::ConvertRGB555to888(c5).u32);
    }
}

TEST_CASE("Vdp2FrontEnd snapshots CRAM coefficients after a CRAM write with all RBGs off", "[vdp2][front]") {
    FrontRig rig;
    DisplayOn(rig.writer);
    rig.writer.Reg(BGON, 0x0000);
    rig.Run();

    const auto &rec = rig.Run([](Vdp2SceneWriter &w, uint32_t line) {
        if (line == 10) {
            w.CRAM16(0x804, 0x1234);
        }
    });
    REQUIRE(rec.batches.size() == 2);
    REQUIRE(rec.batches[1].copyCount == 2);
    const Vdp2Copy &colors = rec.copies[rec.batches[1].firstCopy];
    const Vdp2Copy &coeffs = rec.copies[rec.batches[1].firstCopy + 1];
    CHECK(colors.target == Vdp2Copy::Target::CRAMColors);
    REQUIRE(coeffs.target == Vdp2Copy::Target::CRAMCoeffs);
    CHECK(coeffs.size == kVDP2CRAMRotCoeffBufferSize);
    CHECK(rec.arena[coeffs.arenaOffset + 4] == 0x12);
    CHECK(rec.arena[coeffs.arenaOffset + 5] == 0x34);
}

// -----------------------------------------------------------------------------
// Resolution, sprite slices, counters, dropped frames

TEST_CASE("Vdp2FrontEnd clears the composite on a resolution change as software does", "[vdp2][front]") {
    const bool borderColorMode = GENERATE(false, true);
    CAPTURE(borderColorMode);

    auto state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2AccessPatternsConfig access{};
    vdp::config::VDP2DebugRender debug{};
    Vdp2FrontEnd front{*state, access};
    front.BeginFrame();
    front.EndFrame();
    front.ConsumeFrame();

    state->regs2.WriteTVMD(borderColorMode ? 0x8100 : 0x8000);
    state->state2.lineBackLayerState.backColor.u32 = 0x00563412;

    // Software clears its framebuffer when the resolution changes.
    auto sw = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
    uint32_t swPixel = 0;
    sw->SwCallbacks.FrameComplete.Rebind(&swPixel, [](uint32_t *fb, uint32_t, uint32_t, void *ctx) {
        *static_cast<uint32_t *>(ctx) = fb[0];
    });
    sw->VDP2SetResolution(352, 240, false);
    sw->VDP2EndFrame();

    front.SetResolution(352, 240, false);
    front.BeginFrame();
    front.EndFrame();
    const Vdp2FrameRecord &rec = front.Frame();
    CHECK(rec.clearComposite);
    CHECK(rec.clearColor == swPixel);
    CHECK(rec.clearColor == (borderColorMode ? 0xFF563412u : 0xFF000000u));
    CHECK(rec.hres == 352);
    CHECK(rec.vres == 240);

    front.ConsumeFrame();
    front.BeginFrame();
    front.EndFrame();
    CHECK_FALSE(front.Frame().clearComposite);
}

TEST_CASE("Vdp2FrontEnd copies sprite slices into the frame arena", "[vdp2][front]") {
    auto state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2AccessPatternsConfig access{};
    Vdp2FrontEnd front{*state, access};

    std::vector<uint8_t> main(kSpriteSliceSize), mesh(kSpriteSliceSize);
    for (size_t i = 0; i < kSpriteSliceSize; ++i) {
        main[i] = static_cast<uint8_t>(i * 7);
        mesh[i] = static_cast<uint8_t>(i * 13 + 1);
    }

    state->fbIndex.display = 1;
    front.BeginFrame();
    front.RenderLine(0);
    front.AddSpriteSlice(kSpriteSliceMain, std::span<const uint8_t, kSpriteSliceSize>{main.data(), kSpriteSliceSize});
    front.AddSpriteSlice(kSpriteSliceMesh0,
                         std::span<const uint8_t, kSpriteSliceSize>{mesh.data(), kSpriteSliceSize});
    main[0] ^= 0xFF; // the arena holds a copy
    for (uint32_t y = 1; y < 224; ++y) {
        front.RenderLine(y);
    }
    front.EndFrame();

    const Vdp2FrameRecord &rec = front.Frame();
    REQUIRE(rec.spriteSliceOffset[kSpriteSliceMain] != UINT32_MAX);
    REQUIRE(rec.spriteSliceOffset[kSpriteSliceMesh0] != UINT32_MAX);
    CHECK(rec.spriteSliceOffset[kSpriteSliceAlt] == UINT32_MAX);
    CHECK(rec.spriteSliceOffset[kSpriteSliceMesh1] == UINT32_MAX);
    CHECK(rec.spriteSliceOffset[kSpriteSliceMain] % 4 == 0);
    CHECK(rec.spriteDisplayFB == 1); // where the back half places the slices
    main[0] ^= 0xFF;
    CHECK(std::memcmp(rec.arena.data() + rec.spriteSliceOffset[kSpriteSliceMain], main.data(), kSpriteSliceSize) == 0);
    CHECK(std::memcmp(rec.arena.data() + rec.spriteSliceOffset[kSpriteSliceMesh0], mesh.data(), kSpriteSliceSize) ==
          0);
    CHECK(rec.features.meshSlices == 1);
    CHECK(rec.features.arenaBytes == rec.arena.size());

    front.ConsumeFrame();
    front.BeginFrame();
    front.EndFrame();
    for (uint32_t offset : front.Frame().spriteSliceOffset) {
        CHECK(offset == UINT32_MAX);
    }
}

TEST_CASE("Vdp2FrontEnd counts feature usage per line", "[vdp2][front]") {
    FrontRig rig;
    auto &w = rig.writer;
    DisplayOn(w);
    w.Reg(BGON, 0x0001);
    rig.Run();

    const auto &rec = rig.Run([](Vdp2SceneWriter &w, uint32_t line) {
        if (line == 100) {
            w.Reg(MZCTL, 0x0001);  // NBG0 mosaic
            w.Reg(SDCTL, 0x0020);  // back screen shadow
            w.Reg(RAMCTL, 0x1000); // CRAM mode 1
        }
        if (line == 200) {
            w.Reg(CCCTL, 0x0400); // extended colour calculation
        }
    });
    CHECK(rec.features.mosaicLines == 124);
    CHECK(rec.features.shadowLines == 124);
    CHECK(rec.features.extendedCCLines == 24);
    CHECK(rec.features.cramModeMask == 0b11);
    CHECK(rec.features.vcellLines == 0);
    CHECK(rec.features.batches == rec.batches.size());
}

TEST_CASE("Vdp2FrontEnd re-uploads everything after a dropped frame", "[vdp2][front]") {
    FrontRig rig;
    DisplayOn(rig.writer);
    rig.Run();
    rig.Run();
    REQUIRE(rig.Frame().copies.empty());

    // A frame that is never consumed (no device) loses its snapshots.
    rig.Front().ConsumeFrame();
    rig.writer.SetResolution(320, 224, false);
    rig.writer.RunFrame();
    REQUIRE(rig.Front().HasFrame());
    rig.writer.RunFrame();
    const Vdp2FrameRecord &rec = rig.Frame();
    REQUIRE(rec.copies.size() == 3);
    CHECK(rec.copies[0].size == vdp::kVDP2VRAMSize);
    CHECK(rec.clearComposite); // the dropped frame's clear is kept

    rig.Front().ConsumeFrame();
    rig.Front().MarkAllDirty();
    rig.writer.RunFrame();
    CHECK(rig.Frame().copies.size() == 3);
    CHECK_FALSE(rig.Frame().clearComposite);
}

TEST_CASE("Vdp2FrontEnd records LNCL/BACK and rotation bases per line", "[vdp2][front]") {
    FrontRig rig;
    auto &w = rig.writer;
    DisplayOn(w);
    w.Reg(BKTAU, 0x8000); // back screen per line at 0x10000
    w.Reg(BKTAL, 0x8000);
    for (uint32_t y = 0; y < 224; ++y) {
        w.VRAM16(0x10000 + y * 2, static_cast<uint16_t>(y & 0x1F)); // red ramp
    }
    // Rotation parameter table A at 0: Xst = 0x100 (integer 1), delta Xst per line = 0x40 (fraction 1/16)
    w.Reg(BGON, 0x0010);
    w.VRAM32(0x00, 1u << 16);
    w.VRAM32(0x0C, 0x40u << 6);

    const auto &rec = rig.Run();
    for (uint32_t y : {0u, 1u, 31u, 223u}) {
        CAPTURE(y);
        const uint32_t back = rec.lnclBack[kMaxResV + y];
        CHECK((back & 0xFF) == ((y & 0x1F) << 3));
    }
    CHECK(rec.rotBases[0].Xst == (1 << 10));
    CHECK(rec.rotBases[1].Xst == (1 << 10) + 0x40);
    CHECK(rec.rotBases[223].Xst == (1 << 10) + 223 * 0x40);
    CHECK(rec.rotBases[kMaxResV].tableAddress == 0x80);
}

TEST_CASE("Vdp2FrontEnd computes lineBackLayerState per line as software does", "[vdp2][front]") {
    // The software renderer and the front half on their own states, driven in lockstep: per-line LNCL and BACK tables,
    // CRAM and table writes mid-frame, and the display switched off and on again mid-frame (the latch stays off, so
    // no line recomputes the colours). After every line, lineBackLayerState must be identical (sw:2513-2528,
    // 2645-2664), and so must the composite clear colour of a resolution change (sw:518-530).
    const uint32_t cramMode = GENERATE(0u, 1u, 2u);
    CAPTURE(cramMode);

    vdp::config::VDP2AccessPatternsConfig access{};
    vdp::config::VDP2DebugRender debug{};
    auto swState = std::make_unique<vdp::VDPState>();
    auto sw = std::make_unique<vdp::SoftwareVDPRenderer>(*swState, debug, access);
    auto frState = std::make_unique<vdp::VDPState>();
    auto fr = std::make_unique<FrontEndRenderer>(*frState, access);
    Vdp2SceneWriter writer;
    writer.Attach(*swState, *sw);
    writer.Attach(*frState, *fr);

    constexpr uint32_t kLineTable = 0x10000, kBackTable = 0x11000;
    constexpr uint16_t kDispBorder = 0x8100; // DISP, BDCLMD
    std::mt19937 rng{cramMode + 7};
    writer.Reg(TVMD, kDispBorder);
    writer.Reg(RAMCTL, static_cast<uint16_t>(cramMode << 12));
    for (uint32_t a = 0; a < 0x1000; a += 2) {
        writer.CRAM16(a, static_cast<uint16_t>(rng()));
    }
    for (uint32_t y = 0; y < 512; ++y) {
        writer.VRAM16(kLineTable + y * 2, static_cast<uint16_t>(rng())); // any word: software masks & 0x7FF
        writer.VRAM16(kBackTable + y * 2, static_cast<uint16_t>(rng()));
    }
    writer.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineTable >> 17)));
    writer.Reg(LCTAL, static_cast<uint16_t>((kLineTable >> 1) & 0xFFFF));
    writer.Reg(BKTAU, static_cast<uint16_t>(0x8000 | (kBackTable >> 17)));
    writer.Reg(BKTAL, static_cast<uint16_t>((kBackTable >> 1) & 0xFFFF));

    const auto &swLB = swState->state2.lineBackLayerState;
    const auto &frLB = frState->state2.lineBackLayerState;
    const auto same = [&] {
        return swLB.lineColor.u32 == frLB.lineColor.u32 && swLB.backColor.u32 == frLB.backColor.u32;
    };
    uint32_t lines = 0, mismatches = 0;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        writer.RunFrame([&](Vdp2SceneWriter &w, uint32_t y) {
            if (y > 0) {
                ++lines;
                mismatches += same() ? 0 : 1;
            }
            if (y == 0) {
                w.Reg(TVMD, kDispBorder);
            } else if (y == 30) {
                w.CRAM16(0x7FE, static_cast<uint16_t>(rng())); // a line colour of mode 1
                w.CRAM16(0x0FE, static_cast<uint16_t>(rng()));
            } else if (y == 50) {
                w.VRAM16(kLineTable + 60 * 2, static_cast<uint16_t>(rng()));
                w.VRAM16(kBackTable + 60 * 2, static_cast<uint16_t>(rng()));
            } else if (y == 100 && frame == 2) {
                w.Reg(TVMD, 0x0100); // display off: the latch goes off
            } else if (y == 150 && frame == 2) {
                w.Reg(TVMD, kDispBorder); // display on; the latch stays off until the next frame
            }
        });
        CHECK(same());
    }
    CHECK(lines > 600);
    CHECK(mismatches == 0);
    CHECK(swLB.backColor.u32 != 0);

    // A resolution change clears software's framebuffer (and the front half's composite) to the back colour.
    uint32_t swPixel = 0;
    sw->SwCallbacks.FrameComplete.Rebind(&swPixel, [](uint32_t *fb, uint32_t, uint32_t, void *ctx) {
        *static_cast<uint32_t *>(ctx) = fb[0];
    });
    writer.SetResolution(352, 224, false);
    sw->VDP2EndFrame();
    fr->front.ConsumeFrame();
    fr->front.BeginFrame();
    fr->front.EndFrame();
    const Vdp2FrameRecord &rec = fr->front.Frame();
    CHECK(rec.clearComposite);
    CHECK(rec.clearColor == swPixel);
    CHECK((swPixel & 0xFFFFFF) == (swLB.backColor.u32 & 0xFFFFFF));
}

// -----------------------------------------------------------------------------
// Per-line NBG and rotation parameter state against the software renderer

namespace {

using RendererSaveState = ymir::savestate::VDPSaveState::VDPRendererSaveState;

// The first difference between a software renderer's and a front half's per-line state: NBG layer states, rotation
// parameter states (and their read flags) and the NBG fetchers (vramFetchers[field][0-3] of the renderer save state:
// characters, lastCharIndex, lastCellX, lastVCellScroll); empty if there is none.
std::string StateMismatch(vdp::VDPState &swState, vdp::SoftwareVDPRenderer &sw, vdp::VDPState &frState,
                          FrontEndRenderer &fr, RendererSaveState &swSaved, RendererSaveState &frSaved,
                          bool fetchersOnly = false, uint32_t cellXFields = 2) {
    std::ostringstream out;
    const auto cmp = [&](const std::string &what, size_t index, uint64_t a, uint64_t b) {
        if (a != b && out.tellp() == 0) {
            out << what << "[" << index << "]: software " << a << ", front half " << b;
        }
    };
    for (size_t i = 0; i < (fetchersOnly ? 0u : 4u); ++i) {
        const vdp::NBGLayerState &a = swState.state2.nbgLayerStates[i];
        const vdp::NBGLayerState &b = frState.state2.nbgLayerStates[i];
        cmp("fracScrollX", i, a.fracScrollX, b.fracScrollX);
        cmp("fracScrollY", i, a.fracScrollY, b.fracScrollY);
        cmp("scrollIncH", i, a.scrollIncH, b.scrollIncH);
        cmp("lineScrollTableAddress", i, a.lineScrollTableAddress, b.lineScrollTableAddress);
        cmp("vcellScrollOffset", i, a.vcellScrollOffset, b.vcellScrollOffset);
        cmp("vcellScrollDelay", i, a.vcellScrollDelay, b.vcellScrollDelay);
        cmp("vcellScrollRepeat", i, a.vcellScrollRepeat, b.vcellScrollRepeat);
        cmp("mosaicCounterY", i, a.mosaicCounterY, b.mosaicCounterY);
    }
    for (size_t i = 0; i < (fetchersOnly ? 0u : 2u); ++i) {
        const vdp::RotationParamState &a = swState.state2.rotParamStates[i];
        const vdp::RotationParamState &b = frState.state2.rotParamStates[i];
        cmp("Xst", i, static_cast<uint32_t>(a.Xst), static_cast<uint32_t>(b.Xst));
        cmp("Yst", i, static_cast<uint32_t>(a.Yst), static_cast<uint32_t>(b.Yst));
        cmp("KA", i, a.KA, b.KA);
        const vdp::RotationParams &ra = swState.regs2.rotParams[i];
        const vdp::RotationParams &rb = frState.regs2.rotParams[i];
        cmp("readXst", i, ra.readXst, rb.readXst);
        cmp("readYst", i, ra.readYst, rb.readYst);
        cmp("readKAst", i, ra.readKAst, rb.readKAst);
    }
    sw.SaveState(swSaved);
    fr.SaveState(frSaved);
    const auto charBits = [](const RendererSaveState::CharacterSaveState &c) {
        return uint64_t{c.charNum} | (uint64_t{c.palNum} << 16) | (uint64_t{c.specColorCalc} << 24) |
               (uint64_t{c.specPriority} << 25) | (uint64_t{c.flipH} << 26) | (uint64_t{c.flipV} << 27);
    };
    for (uint32_t field = 0; field < 2; ++field) {
        for (uint32_t nbg = 0; nbg < 4; ++nbg) {
            const auto &a = swSaved.vramFetchers[field][nbg];
            const auto &b = frSaved.vramFetchers[field][nbg];
            const std::string prefix = "field " + std::to_string(field) + " NBG fetcher ";
            cmp(prefix + "lastVCellScroll", nbg, a.lastVCellScroll, b.lastVCellScroll);
            cmp(prefix + "nextChar", nbg, charBits(a.nextChar), charBits(b.nextChar));
            cmp(prefix + "currChar", nbg, charBits(a.currChar), charBits(b.currChar));
            cmp(prefix + "lastCharIndex", nbg, a.lastCharIndex, b.lastCharIndex);
            // Software's VRAMFetcher::Reset leaves lastCellX indeterminate until a line resets it (sw:2531-2539)
            if (field < cellXFields) {
                cmp(prefix + "lastCellX", nbg, a.lastCellX, b.lastCellX);
            }
        }
    }
    return out.str();
}

// The scene of the lockstep tests. NBG0 and NBG1: line scroll (X, Y, zoom; the flags and interval change mid-frame),
// vertical cell scroll with repeat and delay; NBG2 and NBG3: SCYN2/SCYN3 written mid-frame; ZMYIN changes; vertical
// mosaic switched off and on mid-frame; RBG0 with a rotation table re-read mid-frame (RPRCTL); in the third frame
// the display goes off and on again (the latch stays off). With `delay`, NBG1 (2x2, 256 colours) has a character
// pattern delay (PN1 at T7, CP1 at T1-T2). With `still`, nothing is written mid-frame.
struct NBGStateScene {
    uint16_t tvmd;
    uint32_t interval;
    bool delay;
    bool still;

    void Setup(Vdp2SceneWriter &w) const {
        std::mt19937 rng{interval * 31 + tvmd + (delay ? 7 : 0)};
        w.Reg(TVMD, tvmd);
        for (uint32_t a = 0x60000; a < 0x80000; a += 2) {
            w.VRAM16(a, static_cast<uint16_t>(rng()));
        }
        for (uint32_t a = 0x00000; a < 0x08000; a += 2) {
            w.VRAM16(a, static_cast<uint16_t>(rng())); // pattern names (pages at 0) and characters
        }
        w.Reg(BGON, 0x001F); // NBG0-3, RBG0
        if (delay) {
            w.Reg(CHCTLA, 0x1100); // NBG1 256 colours, 2x2
            w.Reg(CYCA0L, 0x455F); // CP0 at T0, CP1 at T1-T2 of bank A0
            w.Reg(CYCA0U, 0x0FF1); // PN0 at T4, PN1 at T7 of bank A0: CP1 is illegal for PN1 at T7
            w.Reg(CYCB0L, 0xFF23);
        } else {
            w.Reg(CYCA0L, 0x4455); // CP0 at T0-T1, CP1 at T2-T3 of bank A0
            w.Reg(CYCA0U, 0x0123); // PN0-PN3 at T4-T7 of bank A0 (the pages are at 0)
        }
        w.Reg(CYCB1L, 0xFFFC); // vertical cell scroll: NBG0 at T3 (repeat and delay), NBG1 at T4 (delay)
        w.Reg(CYCB1U, 0xDFFF);
        w.Reg(VCSTAU, 0x0003); // 0x7D000
        w.Reg(VCSTAL, 0xE800);
        w.Reg(LSTA0U, 0x0003); // 0x7C000
        w.Reg(LSTA0L, 0xE000);
        w.Reg(LSTA1U, 0x0003); // 0x7A000
        w.Reg(LSTA1L, 0xD000);
        w.Reg(RPTAU, 0x0003); // 0x7FE00
        w.Reg(RPTAL, 0xFF00);
        w.Reg(ZMYIN0, 0);
        w.Reg(ZMYDN0, 0xC000);
        w.Reg(SCRCTL, Scrctl());
        w.Reg(MZCTL, 0x3105); // NBG0 and NBG2, 2 x 4
        w.Reg(ZMXIN1, 1);
    }

    uint16_t Scrctl() const {
        return static_cast<uint16_t>((0x0F | (interval << 4)) | ((0x07 | (interval << 4)) << 8));
    }

    void OnLine(Vdp2SceneWriter &w, uint32_t frame, uint32_t y) const {
        if (still) {
            return;
        }
        switch (y) {
        case 0:
            w.Reg(SCRCTL, Scrctl());
            w.Reg(MZCTL, 0x3105);
            w.Reg(ZMXIN1, 1);
            w.Reg(TVMD, tvmd);
            break;
        case 30: w.Reg(SCYN2, 17); break;
        case 45: w.Reg(SCYN3, 3); break;
        case 60: w.Reg(ZMYIN0, 2); break;
        case 75: w.Reg(SCRCTL, static_cast<uint16_t>(Scrctl() ^ 0x0018)); break; // NBG0: zoom off, interval
        case 90: w.Reg(MZCTL, 0); break;
        case 101: w.Reg(MZCTL, 0x2105); break; // 2 x 3, the counters resume
        case 120: w.Reg(RPRCTL, 0x0707); break; // re-read Xst, Yst and KAst of both parameters
        case 130: w.Reg(ZMXIN1, 2); break;
        case 150:
            if (frame == 2) {
                w.Reg(TVMD, static_cast<uint16_t>(tvmd & 0x7FFF));
            }
            break;
        case 170:
            if (frame == 2) {
                w.Reg(TVMD, tvmd);
            }
            break;
        }
    }
};

// The software renderer and the front half on their own states, driven in lockstep by one writer.
struct LockstepPair {
    vdp::config::VDP2AccessPatternsConfig access{};
    vdp::config::VDP2DebugRender debug{};
    std::unique_ptr<vdp::VDPState> swState = std::make_unique<vdp::VDPState>();
    std::unique_ptr<vdp::SoftwareVDPRenderer> sw = std::make_unique<vdp::SoftwareVDPRenderer>(*swState, debug, access);
    std::unique_ptr<vdp::VDPState> frState = std::make_unique<vdp::VDPState>();
    std::unique_ptr<FrontEndRenderer> fr = std::make_unique<FrontEndRenderer>(*frState, access);
    std::unique_ptr<RendererSaveState> swSaved = std::make_unique<RendererSaveState>();
    std::unique_ptr<RendererSaveState> frSaved = std::make_unique<RendererSaveState>();
    Vdp2SceneWriter writer;

    explicit LockstepPair(const vdp::config::Enhancements &enh)
        : writer(enh) {
        writer.Attach(*swState, *sw);
        writer.Attach(*frState, *fr);
    }

    // Fields whose lastCellX software has set (from the first line on: the alternate field only while deinterlacing)
    uint32_t cellXFields = 0;

    std::string Mismatch() {
        return StateMismatch(*swState, *sw, *frState, *fr, *swSaved, *frSaved, false, cellXFields);
    }
};

// Records the first mismatch of a run, checked before every line and after each frame.
struct MismatchLog {
    std::string first;
    uint32_t checks = 0;

    void Check(uint32_t frame, uint32_t y, const std::string &mismatch) {
        ++checks;
        if (first.empty() && !mismatch.empty()) {
            first = "frame " + std::to_string(frame) + " line " + std::to_string(y) + ": " + mismatch;
        }
    }
};

struct LockstepMode {
    uint16_t tvmd;
    bool deinterlace;
};

} // namespace

TEST_CASE("Vdp2FrontEnd keeps the per-line NBG and rotation state as software does", "[vdp2][front]") {
    // The software renderer and the front half on their own states, in lockstep, over NBGStateScene (with and without
    // mid-frame writes, with and without a character pattern delay). Before every line and after each frame the NBG
    // layer states, rotation parameter states and NBG fetchers must be identical (sw:430-438, 2081-2143, 2145-2184,
    // 2513-2565, 4526-4541, 5193-5215).
    const LockstepMode mode =
        GENERATE(LockstepMode{0x8000, false}, LockstepMode{0x80C0, false}, LockstepMode{0x80C0, true},
                 LockstepMode{0x8080, false}, LockstepMode{0x8080, true}, LockstepMode{0x8001, false});
    const uint32_t interval = GENERATE(0u, 2u);
    const bool delay = GENERATE(false, true);
    const bool still = GENERATE(false, true);
    CAPTURE(mode.tvmd, mode.deinterlace, interval, delay, still);

    const NBGStateScene scene{mode.tvmd, interval, delay, still};
    LockstepPair pair{{.deinterlace = mode.deinterlace}};
    scene.Setup(pair.writer);
    MismatchLog log;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        pair.writer.RunFrame([&](Vdp2SceneWriter &w, uint32_t y) {
            if (frame > 0 || y > 0) {
                pair.cellXFields = mode.deinterlace ? 2 : 1;
            }
            log.Check(frame, y, pair.Mismatch());
            scene.OnLine(w, frame, y);
        });
        log.Check(frame, 9999, pair.Mismatch());
    }
    INFO(log.first);
    CHECK(log.first.empty());
    CHECK(log.checks > 600);
    // The scene exercises what it checks: line scroll, vertical cell scroll reads, the rotation tables, characters.
    CHECK(pair.frState->state2.nbgLayerStates[0].lineScrollTableAddress != 0x7C000);
    if (!still) {
        CHECK(pair.fr->front.LastVCellScroll(0, 0) != 0xFFFFFFFF); // with mosaic and repeat-delay, NBG0 never reads
    }
    CHECK(pair.fr->front.LastVCellScroll(0, 1) != 0xFFFFFFFF);
    CHECK(pair.frState->state2.rotParamStates[0].Xst != 0);
    pair.fr->SaveState(*pair.frSaved);
    CHECK(pair.frSaved->vramFetchers[0][1].nextChar.charNum != 0);
}

TEST_CASE("Vdp2FrontEnd continues a mid-frame save state of the software renderer and vice versa", "[vdp2][front]") {
    // One renderer runs NBGStateScene (with a character pattern delay, so every fetcher field matters) to line 100 of
    // the second frame and is saved there (VDPState and the renderer section). A fresh renderer of the other kind
    // loads the state, and the two continue in lockstep (the saved one goes on uninterrupted): the per-line state must
    // match before every line from the load on (Ruling 16).
    const bool fromSoftware = GENERATE(true, false);
    const LockstepMode mode = GENERATE(LockstepMode{0x8000, false}, LockstepMode{0x80C0, true});
    CAPTURE(fromSoftware, mode.tvmd, mode.deinterlace);
    const NBGStateScene scene{mode.tvmd, 0, true, false};
    const vdp::config::Enhancements enh{.deinterlace = mode.deinterlace};

    vdp::config::VDP2AccessPatternsConfig access{};
    vdp::config::VDP2DebugRender debug{};
    auto swState = std::make_unique<vdp::VDPState>();
    auto sw = std::make_unique<vdp::SoftwareVDPRenderer>(*swState, debug, access);
    auto frState = std::make_unique<vdp::VDPState>();
    auto fr = std::make_unique<FrontEndRenderer>(*frState, access);
    auto swSaved = std::make_unique<RendererSaveState>();
    auto frSaved = std::make_unique<RendererSaveState>();

    // The saved renderer runs alone up to the save
    vdp::VDPState &savedState = fromSoftware ? *swState : *frState;
    vdp::IVDPRenderer &savedRenderer = fromSoftware ? static_cast<vdp::IVDPRenderer &>(*sw) : *fr;
    vdp::VDPState &loadedState = fromSoftware ? *frState : *swState;
    vdp::IVDPRenderer &loadedRenderer = fromSoftware ? static_cast<vdp::IVDPRenderer &>(*fr) : *sw;
    constexpr uint32_t kSaveLine = 100;
    Vdp2SceneWriter first{enh};
    first.Attach(savedState, savedRenderer);
    scene.Setup(first);
    first.RunFrame([&](Vdp2SceneWriter &w, uint32_t y) { scene.OnLine(w, 0, y); });
    first.RunFrame([&](Vdp2SceneWriter &w, uint32_t y) { scene.OnLine(w, 1, y); }, 0, kSaveLine);

    auto saved = std::make_unique<ymir::savestate::VDPSaveState>();
    savedState.SaveState(*saved);
    savedRenderer.PreSaveStateSync();
    savedRenderer.SaveState(saved->renderer);

    // The other renderer loads it, and both continue
    Vdp2SceneWriter second{enh};
    second.SetResolution(first.HRes(), first.VRes(), first.ExclusiveMonitor());
    second.Attach(*swState, *sw);
    second.Attach(*frState, *fr);
    loadedState.LoadState(*saved);
    loadedRenderer.LoadState(saved->renderer);
    loadedRenderer.PostLoadStateSync();

    // Right after the load the fetchers match; the rest of the state from the first line drawn after the load on
    // (vcellScrollRepeat is not in VDPState's save state; both renderers recompute it with the access patterns at the
    // first line, sw:2522-2523).
    MismatchLog log;
    log.Check(1, kSaveLine, StateMismatch(*swState, *sw, *frState, *fr, *swSaved, *frSaved, true));
    second.RunFrame(
        [&](Vdp2SceneWriter &w, uint32_t y) {
            if (y > kSaveLine) {
                log.Check(1, y, StateMismatch(*swState, *sw, *frState, *fr, *swSaved, *frSaved));
            }
            scene.OnLine(w, 1, y);
        },
        kSaveLine);
    for (uint32_t frame = 2; frame < 4; ++frame) {
        second.RunFrame([&](Vdp2SceneWriter &w, uint32_t y) {
            log.Check(frame, y, StateMismatch(*swState, *sw, *frState, *fr, *swSaved, *frSaved));
            scene.OnLine(w, frame, y);
        });
    }
    INFO(log.first);
    CHECK(log.first.empty());
    CHECK(log.checks > 400);
    // The fetchers carried across the load are not trivial
    CHECK(saved->renderer.vramFetchers[0][0].lastVCellScroll != 0xFFFFFFFF);
    CHECK(saved->renderer.vramFetchers[0][1].nextChar.charNum != 0);
}

TEST_CASE("Vdp2FrontEnd data access shifts are 0 or 8", "[vdp2][front]") {
    // (h): the bgs shader packs vramDataOffset into one bit per bank and adds 8 (dots for scroll BGs, bytes for
    // bitmaps), as software adds the value (sw:5167-5170, 5370-5372). Software only ever sets 0 or 8
    // (VDP2State::CalcAccessPatterns); these two cycle patterns set 8.
    const uint32_t variant = GENERATE(0u, 1u);
    CAPTURE(variant);
    FrontRig rig;
    auto &w = rig.writer;
    if (variant == 0) {
        DisplayOn(w);
        w.Reg(BGON, 0x0004);     // NBG2 cells
        w.Reg(CYCA0L, 0xFFF6);   // CP2 at T3 of bank A0: illegal after PN2 at T0
        w.Reg(CYCB0L, 0x2FFF);   // PN2 at T0 of bank B0
    } else {
        w.Reg(TVMD, 0x8002);     // DISP, 640x224
        w.Reg(BGON, 0x0001);     // NBG0 bitmap
        w.Reg(CHCTLA, 0x0012);   // NBG0 256-colour bitmap
        w.Reg(CYCA0L, 0x44FF);   // CP0 at T0-T1 of bank A0
        w.Reg(CYCB0L, 0xFF44);   // and at T2-T3 of bank B0: out of phase on the other VRAM chip
    }
    rig.Run();
    const auto &offsets = rig.state->regs2.bgParams[variant == 0 ? 3 : 1].vramDataOffset;
    for (uint32_t bank = 0; bank < 4; ++bank) {
        CAPTURE(bank);
        CHECK((offsets[bank] == 0 || offsets[bank] == 8));
    }
    if (variant == 0) {
        CHECK(offsets == std::array<uint32_t, 4>{8, 8, 8, 8});
    } else {
        CHECK(offsets == std::array<uint32_t, 4>{0, 0, 8, 8});
    }
    const NBGParams params = LayerParams(rig.Frame()).nbg[variant == 0 ? 2 : 0];
    CHECK(params.base.vramDataOffset == (variant == 0 ? 0xFu : 0xCu));
}

TEST_CASE("Vdp2FrontEnd records per-line NBG state as software draws it", "[vdp2][front]") {
    // Per-row VDP2NBGLine contents: the scroll state before the line's counters advance (scroll amounts excluded),
    // the double-density alternate line one vertical increment further, vertical mosaic sources, and the fetcher's
    // last vertical cell scroll value and nextChar.
    FrontRig rig{{.deinterlace = true}};
    auto &w = rig.writer;
    w.Reg(TVMD, 0x80C0);     // DISP, double density, 320x448
    w.Reg(BGON, 0x0003);     // NBG0, NBG1
    w.Reg(CYCA0L, 0x15FF);   // NBG1 pattern names at T0 and characters at T1 of bank A0
    w.Reg(PNCN0 + 2, 0x0000); // NBG1 two-word pattern names
    w.Reg(CYCB1L, 0xFCFF);   // NBG0 vertical cell scroll at T1
    w.Reg(SCRCTL, 0x0001);
    w.Reg(MZCTL, 0x2002);    // NBG1, 1 x 3
    w.Reg(ZMYIN0, 1);
    w.Reg(ZMYDN0, 0x8000);   // 1.5 lines per line
    w.Reg(SCXIN1, 4);
    w.Reg(ZMXIN1, 1);
    w.VRAM32(0x00, 0x00012300); // vertical cell scroll entries from 0
    w.VRAM32(0x04, 0x00045600);
    w.VRAM32(0xA0, 0x00078900); // entry 40: the last one each line reads (the first, then one per cell column change
                                // up to the extra tile at x = 320)

    // The first frame starts from reset fetchers
    const Vdp2FrameRecord &first = rig.Run();
    CHECK(first.nbgLines[0].prevLastVCellScroll == 0xFFFFFFFF);
    CHECK(first.nbgLines[kMaxResV].nextChar == 0);

    const auto &rec = rig.Run();
    const auto at = [&](uint32_t nbg, uint32_t row) { return rec.nbgLines[nbg * kMaxResV + row]; };

    // NBG0, line 10: fracScrollY = 10 lines * 2 (double density) * 0x180
    CHECK(at(0, 20).fracScrollY == 10 * 2 * 0x180);
    CHECK(at(0, 21).fracScrollY == 10 * 2 * 0x180 + 0x180); // the alternate line
    CHECK(at(0, 20).scrollIncH == 0x100);
    CHECK(at(0, 20).mosaicSrcLine == 20);
    // Each line ends on entry 41, which the next line starts from (each field has its own fetcher)
    CHECK(at(0, 0).prevLastVCellScroll == 0x789);
    CHECK(at(0, 21).prevLastVCellScroll == 0x789);
    CHECK(rig.Front().LastVCellScroll(0, 0) == 0x789);
    CHECK(rig.Front().LastVCellScroll(1, 0) == 0x789);
    // NBG1 vertical mosaic of 3 lines: rows of lines 1 and 2 repeat line 0, per field
    CHECK(at(1, 0).mosaicSrcLine == 0);
    CHECK(at(1, 2).mosaicSrcLine == 0);
    CHECK(at(1, 3).mosaicSrcLine == 1);
    CHECK(at(1, 5).mosaicSrcLine == 1);
    CHECK(at(1, 6).mosaicSrcLine == 6);
    // The character NBG1's line 0 fetched last (the extra tile at x = 320, scrolled by 4: two-word pattern name 40 of
    // the page at 0, which is also vertical cell scroll entry 40) is the fetcher's nextChar when line 3 (row 6) starts
    CHECK(at(1, 6).nextChar == 0x00078900);
}

// -----------------------------------------------------------------------------
// Scene harness

TEST_CASE("RenderSoftware renders a scene's last frame", "[vdp2][front][scene]") {
    Vdp2Scene scene;
    scene.setup = [](Vdp2SceneWriter &w) {
        w.Reg(TVMD, 0x8001); // DISP, 352x224
        w.Reg(BKTAU, 0x0000);
        w.Reg(BKTAL, 0x0100); // back colour at 0x200
        w.VRAM16(0x200, 0x001F);
    };
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 112) {
            w.VRAM16(0x200, 0x7C00); // blue from line 112
        }
        if (y == 0) {
            w.VRAM16(0x200, 0x001F); // red again from the top of every frame
        }
    };
    uint32_t width = 0, height = 0;
    const std::vector<uint32_t> frame = brimir::test::RenderSoftware(scene, &width, &height);
    REQUIRE(width == 352);
    REQUIRE(height == 224);
    REQUIRE(frame.size() == 352u * 224u);
    CHECK(frame[0] == 0xFF0000F8u);
    CHECK(frame[111 * 352 + 351] == 0xFF0000F8u);
    CHECK(frame[112 * 352] == 0xFFF80000u);
    CHECK(frame[223 * 352 + 351] == 0xFFF80000u);
}

TEST_CASE("Vdp2SceneWriter drives several state and renderer pairs in lockstep", "[vdp2][front][scene]") {
    struct Pair {
        std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
        vdp::config::VDP2DebugRender debug{};
        vdp::config::VDP2AccessPatternsConfig access{};
        std::unique_ptr<vdp::SoftwareVDPRenderer> renderer =
            std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
        std::vector<uint32_t> frame;
        Pair() {
            renderer->SwCallbacks.FrameComplete.Rebind(this, [](uint32_t *fb, uint32_t w, uint32_t h, void *ctx) {
                static_cast<Pair *>(ctx)->frame.assign(fb, fb + static_cast<size_t>(w) * h);
            });
        }
    };
    Pair a, b;

    vdp::config::Enhancements enh{};
    enh.deinterlace = true;
    Vdp2SceneWriter writer{enh};
    writer.Attach(*a.state, *a.renderer);
    writer.Attach(*b.state, *b.renderer);

    writer.Reg(TVMD, 0x80C3); // DISP, DD interlace, 704x448
    writer.Reg(BKTAU, 0x0000);
    writer.Reg(BKTAL, 0x0100);
    writer.VRAM16(0x200, 0x03E0);
    writer.CRAM16(0x000, 0x1234);
    for (int f = 0; f < 3; ++f) {
        writer.RunFrame([](Vdp2SceneWriter &w, uint32_t y) {
            if (y == 50) {
                w.VRAM16(0x200, static_cast<uint16_t>(y));
            }
        });
    }
    CHECK(writer.HRes() == 704);
    CHECK(writer.VRes() == 448);
    REQUIRE(a.frame.size() == 704u * 448u);
    CHECK(a.frame == b.frame);
    CHECK(a.state->regs2.TVSTAT.ODD == b.state->regs2.TVSTAT.ODD);
    CHECK(a.state->mem2.VRAM == b.state->mem2.VRAM);
    CHECK(a.state->mem2.CRAM == b.state->mem2.CRAM);
    // Field by field: NBGLayerState has padding after its bools, which memcmp would compare too.
    for (size_t i = 0; i < a.state->state2.nbgLayerStates.size(); ++i) {
        const auto &sa = a.state->state2.nbgLayerStates[i];
        const auto &sb = b.state->state2.nbgLayerStates[i];
        CHECK(sa.fracScrollX == sb.fracScrollX);
        CHECK(sa.fracScrollY == sb.fracScrollY);
        CHECK(sa.scrollIncH == sb.scrollIncH);
        CHECK(sa.lineScrollTableAddress == sb.lineScrollTableAddress);
        CHECK(sa.vcellScrollOffset == sb.vcellScrollOffset);
        CHECK(sa.vcellScrollDelay == sb.vcellScrollDelay);
        CHECK(sa.vcellScrollRepeat == sb.vcellScrollRepeat);
        CHECK(sa.mosaicCounterY == sb.mosaicCounterY);
    }
}
