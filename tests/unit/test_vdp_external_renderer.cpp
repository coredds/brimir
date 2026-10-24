#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_hw_base.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_null.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>

#include <memory>

namespace {

using namespace ymir;

// Minimal hardware renderer: every pure virtual is a no-op. Reused by the external renderer hook tests.
class StubRenderer final : public vdp::HardwareVDPRendererBase {
public:
    StubRenderer(vdp::VDPRendererType type)
        : HardwareVDPRendererBase(type) {}

    bool IsValid() const override {
        return true;
    }

    void Reset(bool hard) override {}

    void PreSaveStateSync() override {}
    void PostLoadStateSync() override {}
    void SaveState(savestate::VDPSaveState::VDPRendererSaveState &state) override {}
    bool ValidateState(const savestate::VDPSaveState::VDPRendererSaveState &state) const override {
        return true;
    }
    void LoadState(const savestate::VDPSaveState::VDPRendererSaveState &state) override {}

    void VDP1WriteVRAM(uint32 address, uint8 value) override {}
    void VDP1WriteVRAM(uint32 address, uint16 value) override {}
    void VDP1SyncFB() override {}
    void VDP1DebugSyncFB() override {}
    void VDP1WriteFB(uint32 address, uint8 value) override {}
    void VDP1WriteFB(uint32 address, uint16 value) override {}
    void VDP1WriteReg(uint32 address, uint16 value) override {}

    void VDP2WriteVRAM(uint32 address, uint8 value) override {}
    void VDP2WriteVRAM(uint32 address, uint16 value) override {}
    void VDP2WriteCRAM(uint32 address, uint8 value) override {}
    void VDP2WriteCRAM(uint32 address, uint16 value) override {}
    void VDP2WriteReg(uint32 address, uint16 value) override {}

    void VDP1EraseFramebuffer(uint64 cycles) override {}
    void VDP1SwapFramebuffer() override {}
    void VDP1BeginFrame() override {}
    void VDP1ExecuteCommand(uint32 cmdAddress, vdp::VDP1Command::Control control) override {}
    void VDP1EndFrame() override {}

    void VDP2SetResolution(uint32 h, uint32 v, bool exclusive) override {}
    void VDP2SetField(bool odd) override {}
    void VDP2LatchTVMD() override {}
    void VDP2BeginFrame() override {}
    void VDP2RenderLine(uint32 y) override {}
    void VDP2EndFrame() override {}

    void UpdateEnabledLayers() override {}

    void DumpExtraVDP1Framebuffers(std::ostream &out) const override {}
};

} // namespace

TEST_CASE("Hardware renderer base reports itself as a hardware renderer", "[vdp][renderer][brimir]") {
    SECTION("hardware renderer") {
        StubRenderer renderer{vdp::VDPRendererType::Null};
        vdp::IVDPRenderer &base = renderer;
        CHECK(base.IsHardwareRenderer());
        CHECK(base.AsHardwareRenderer() == &renderer);
    }

    SECTION("software renderer") {
        auto state = std::make_unique<vdp::VDPState>();
        vdp::config::VDP2DebugRender debug{};
        vdp::config::VDP2AccessPatternsConfig access{};
        // SoftwareVDPRenderer is too large for the stack
        auto renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
        vdp::IVDPRenderer &base = *renderer;
        CHECK_FALSE(base.IsHardwareRenderer());
        CHECK(base.AsHardwareRenderer() == nullptr);
    }

    SECTION("null renderer") {
        vdp::NullVDPRenderer renderer;
        vdp::IVDPRenderer &base = renderer;
        CHECK_FALSE(base.IsHardwareRenderer());
        CHECK(base.AsHardwareRenderer() == nullptr);
    }
}
