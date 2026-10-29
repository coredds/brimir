#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/savestate/savestate_vdp.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

// Brimir: regression tests for VDP state that was read before it was written. Each object is built on memory
// pre-filled with a byte pattern, the way a reused heap block looks, and then reset as the core does. Fresh pages from
// the OS are zero, which hides the bugs.

namespace {

using namespace ymir;
using namespace ymir::vdp;

constexpr uint8 kZeroed = 0x00;
constexpr uint8 kDirty = 0xCD;

constexpr uint32 kWidth = 320;
constexpr uint32 kHeight = 224;

// Constructs a T in place on `fill`-filled storage. Without constructor arguments T is default-initialised, so members
// without an initialiser keep the pattern.
template <typename T>
class Placed {
public:
    template <typename... Args>
    explicit Placed(uint8 fill, Args &&...args) {
        m_mem = ::operator new(sizeof(T), std::align_val_t{alignof(T)});
        std::memset(m_mem, fill, sizeof(T));
        if constexpr (sizeof...(Args) == 0) {
            m_obj = new (m_mem) T;
        } else {
            m_obj = new (m_mem) T(std::forward<Args>(args)...);
        }
    }

    ~Placed() {
        m_obj->~T();
        ::operator delete(m_mem, std::align_val_t{alignof(T)});
    }

    Placed(const Placed &) = delete;
    Placed &operator=(const Placed &) = delete;

    T *operator->() const {
        return m_obj;
    }
    T &operator*() const {
        return *m_obj;
    }

private:
    void *m_mem = nullptr;
    T *m_obj = nullptr;
};

// A VDP state and software renderer on `fill`-filled memory.
struct Instance {
    explicit Instance(uint8 fill)
        : state(fill)
        , renderer(fill, *state, debug, access) {}

    Placed<VDPState> state;
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    Placed<SoftwareVDPRenderer> renderer;
    std::vector<uint32> frame;

    void BindOutput() {
        renderer->SwCallbacks.FrameComplete.Rebind(this, [](uint32 *fb, uint32 w, uint32 h, void *context) {
            auto &out = *static_cast<Instance *>(context);
            out.frame.assign(fb, fb + w * h);
        });
    }

    std::unique_ptr<savestate::VDPSaveState> Save() {
        auto saved = std::make_unique<savestate::VDPSaveState>();
        renderer->PreSaveStateSync();
        state->SaveState(*saved);
        renderer->SaveState(saved->renderer);
        return saved;
    }
};

// Sprite layer fed by VDP1 framebuffer rotation (TVMR.ROTE) with no rotation background enabled, so the rotation
// parameter tables are never computed. Every sprite coordinate then comes from the reset state.
void SetUpSpriteRotation(Instance &inst) {
    auto &regs1 = inst.state->regs1;
    auto &regs2 = inst.state->regs2;

    regs1.WriteTVMR(0x2); // Rotation enabled, 16 bits per pixel
    regs2.WriteTVMD(0x8000);
    regs2.LatchTVMD();
    regs2.WriteSPCTL(0x0020); // Sprite type 0, palette and RGB data
    regs2.WritePRISA(0x0505);
    regs2.WritePRISB(0x0505);
    regs2.WritePRISC(0x0505);
    regs2.WritePRISD(0x0505);

    // The only sprite pixel in the display framebuffer: opaque magenta at (0, 0).
    util::WriteBE<uint16>(&inst.state->mem1.FBRAM[inst.state->fbIndex.display][0], 0xFC1F);

    inst.renderer->PostLoadStateSync();
    inst.BindOutput();
    inst.renderer->VDP2SetResolution(kWidth, kHeight, false);
}

void RenderFrame(Instance &inst) {
    inst.renderer->VDP2BeginFrame();
    for (uint32 y = 0; y < kHeight; ++y) {
        inst.renderer->VDP2RenderLine(y);
    }
    inst.renderer->VDP2EndFrame();
}

} // namespace

TEST_CASE("VDP2Regs::Reset defines the vertical cell scroll state", "[vdp][uninit][regression][brimir]") {
    const uint8 fill = GENERATE(kZeroed, kDirty);
    CAPTURE(static_cast<int>(fill));

    Placed<VDP2Regs> regs(fill);
    regs->Reset();

    CHECK(regs->vcellScrollInc == 0);
    CHECK(regs->vcellScrollTableAddress == 0);
    CHECK(regs->cellScrollTableAddress == 0);
    CHECK(regs->vcellScrollDirty);
    CHECK(regs->accessPatternsDirty);
}

TEST_CASE("VDPState and SoftwareVDPRenderer start with defined VDP2 and VDP1 state", "[vdp][uninit][regression][brimir]") {
    Instance dirty(kDirty);
    Instance clean(kZeroed);

    const auto dirtySaved = dirty.Save();
    const auto cleanSaved = clean.Save();

    CHECK(dirtySaved->renderer.vcellScrollInc == 0);
    CHECK(dirtySaved->renderer.vdp1State.doubleV == 0);

    // The whole save state is the same whatever the memory held. Both start zeroed, so padding matches too.
    CHECK(std::memcmp(dirtySaved.get(), cleanSaved.get(), sizeof(savestate::VDPSaveState)) == 0);
}

TEST_CASE("SoftwareVDPRenderer::Reset clears the VDP1 vertical doubling flag", "[vdp][uninit][regression][brimir]") {
    Instance inst(kDirty);

    auto saved = inst.Save();
    saved->renderer.vdp1State.doubleV = 1;
    inst.renderer->LoadState(saved->renderer);
    REQUIRE(inst.Save()->renderer.vdp1State.doubleV == 1);

    inst.renderer->Reset(false);
    CHECK(inst.Save()->renderer.vdp1State.doubleV == 0);

    saved->renderer.vdp1State.doubleV = 1;
    inst.renderer->LoadState(saved->renderer);
    inst.renderer->Reset(true);
    CHECK(inst.Save()->renderer.vdp1State.doubleV == 0);
}

TEST_CASE("VDP2 sprite rotation without a rotation background reads defined coordinates",
          "[vdp][uninit][regression][brimir]") {
    Instance dirty(kDirty);
    Instance clean(kZeroed);
    SetUpSpriteRotation(dirty);
    SetUpSpriteRotation(clean);

    RenderFrame(dirty);
    RenderFrame(clean);

    REQUIRE(clean.frame.size() == kWidth * kHeight);
    REQUIRE(dirty.frame.size() == kWidth * kHeight);

    // With no rotation parameters every sprite coordinate is (0, 0), so the whole screen shows that pixel.
    constexpr uint32 kMagenta = 0xFFF800F8u;
    for (uint32 y = 0; y < kHeight; ++y) {
        CAPTURE(y);
        for (uint32 x = 0; x < kWidth; ++x) {
            CAPTURE(x);
            REQUIRE(clean.frame[y * kWidth + x] == kMagenta);
        }
    }
    CHECK(dirty.frame == clean.frame);

    const auto dirtySaved = dirty.Save();
    const auto cleanSaved = clean.Save();
    CHECK(std::memcmp(dirtySaved.get(), cleanSaved.get(), sizeof(savestate::VDPSaveState)) == 0);
}

TEST_CASE("VDP2 frame and save state do not depend on the memory the renderer was built on",
          "[vdp][uninit][regression][brimir]") {
    // NBG0 with 16-colour cells, vertical cell scroll enabled and its table read in T4, so the access pattern code
    // computes the vertical cell scroll increment and delays.
    const auto setUp = [](Instance &inst) {
        auto &regs = inst.state->regs2;
        regs.WriteTVMD(0x8000);
        regs.LatchTVMD();
        regs.WriteBGON(0x0001);
        regs.WriteCHCTLA(0x0000);
        regs.WritePNCNA(0x8000);
        regs.WritePRINA(0x0007);
        regs.WriteSCRCTL(0x0001);
        regs.WriteCYCA0L(0xF0FF);
        regs.WriteCYCA0U(0xCFFF);
        regs.WriteCYCA1L(0xFFFF);
        regs.WriteCYCA1U(0xFFFF);
        regs.WriteCYCB0L(0xFFFF);
        regs.WriteCYCB0U(0xFFFF);
        regs.WriteCYCB1L(0xFFFF);
        regs.WriteCYCB1U(0xFFFF);
        for (uint32 i = 0; i < 64 * 64; ++i) {
            inst.state->mem2.WriteVRAM<uint16>(i * 2, static_cast<uint16>(((1 + i % 15) << 12) | 0x200));
        }
        for (uint32 i = 0; i < 32; ++i) {
            inst.state->mem2.WriteVRAM<uint8>(0x4000 + i, 0x11);
        }
        for (uint32 pal = 1; pal < 16; ++pal) {
            inst.state->mem2.WriteCRAM<uint16>(((pal << 4) | 1) * 2, static_cast<uint16>(pal * 0x0421));
        }
        inst.renderer->PostLoadStateSync();
        inst.BindOutput();
        inst.renderer->VDP2SetResolution(kWidth, kHeight, false);
    };

    Instance dirty(kDirty);
    Instance clean(kZeroed);
    setUp(dirty);
    setUp(clean);

    // Before any line is drawn
    CHECK(std::memcmp(dirty.Save().get(), clean.Save().get(), sizeof(savestate::VDPSaveState)) == 0);

    RenderFrame(dirty);
    RenderFrame(clean);
    REQUIRE(clean.frame.size() == kWidth * kHeight);
    CHECK(dirty.frame == clean.frame);
    CHECK(std::memcmp(dirty.Save().get(), clean.Save().get(), sizeof(savestate::VDPSaveState)) == 0);
}
