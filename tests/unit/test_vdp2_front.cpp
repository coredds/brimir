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
#include <vector>

using namespace brimir::gpu::vdp2;
using brimir::test::Vdp2Scene;
using brimir::test::Vdp2SceneWriter;

namespace {

namespace vdp = ymir::vdp;

// Register addresses (VDP2 register block offsets)
constexpr uint32_t TVMD = 0x000, RAMCTL = 0x00E, BGON = 0x020, MZCTL = 0x022, CHCTLA = 0x028, BMPNA = 0x02C,
                   PNCN0 = 0x030, MPOFN = 0x03C, SCXIN0 = 0x070, SCXDN0 = 0x072, ZMXIN0 = 0x078, ZMXDN0 = 0x07A,
                   SCYN2 = 0x092, BKTAU = 0x0AC, BKTAL = 0x0AE, RPMD = 0x0B0, KTCTL = 0x0B4, WPSX0 = 0x0C0,
                   WPSY0 = 0x0C2, WPEX0 = 0x0C4, WPEY0 = 0x0C6, WCTLA = 0x0D0, LWTA0U = 0x0D8, LWTA0L = 0x0DA,
                   SPCTL = 0x0E0, SDCTL = 0x0E2, CCCTL = 0x0EC, PRISA = 0x0F0, PRISB = 0x0F2, PRISC = 0x0F4,
                   PRISD = 0x0F6, PRINA = 0x0F8, CCRSA = 0x100, CCRSB = 0x102, CLOFEN = 0x110, CLOFSL = 0x112,
                   COAR = 0x114, COAG = 0x116, COAB = 0x118, COBR = 0x11A, COBG = 0x11C, COBB = 0x11E;

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

    // The frame uploads the cache as it is.
    const auto &rec = rig.Run();
    bool found = false;
    for (const Vdp2Copy &copy : rec.copies) {
        if (copy.target == Vdp2Copy::Target::CRAMColors) {
            found = true;
            REQUIRE(copy.size == sizeof(CRAMColorCache));
            CHECK(std::memcmp(rec.arena.data() + copy.arenaOffset, front.ColorCache().data(), copy.size) == 0);
        }
    }
    CHECK(found);
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
    CHECK(std::memcmp(&a.state->state2.nbgLayerStates, &b.state->state2.nbgLayerStates,
                      sizeof(a.state->state2.nbgLayerStates)) == 0);
}
