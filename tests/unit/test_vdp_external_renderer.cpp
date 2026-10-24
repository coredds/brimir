#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_hw_base.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_null.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/saturn.hpp>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

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

struct Call {
    std::string name;
    std::vector<uint32> args;

    bool operator==(const Call &) const = default;
};

std::ostream &operator<<(std::ostream &out, const Call &call) {
    out << call.name << '(';
    for (size_t i = 0; i < call.args.size(); i++) {
        out << (i > 0 ? "," : "") << call.args[i];
    }
    return out << ')';
}

// Hardware renderer that records every call made to it, in order.
class RecordingRenderer final : public vdp::HardwareVDPRendererBase {
public:
    RecordingRenderer(vdp::VDPState &state, bool valid = true)
        : HardwareVDPRendererBase(vdp::VDPRendererType::External)
        , state(&state)
        , valid(valid) {}

    vdp::VDPState *state;
    bool valid;
    std::vector<Call> calls;

    bool IsValid() const override {
        return valid;
    }

    void Reset(bool hard) override {
        Record("Reset", {hard});
    }

    void PreSaveStateSync() override {
        Record("PreSaveStateSync");
    }
    void PostLoadStateSync() override {
        Record("PostLoadStateSync");
    }
    void SaveState(savestate::VDPSaveState::VDPRendererSaveState &state) override {
        Record("SaveState");
    }
    bool ValidateState(const savestate::VDPSaveState::VDPRendererSaveState &state) const override {
        const_cast<RecordingRenderer *>(this)->Record("ValidateState");
        return true;
    }
    void LoadState(const savestate::VDPSaveState::VDPRendererSaveState &state) override {
        Record("LoadState");
    }

    void VDP1WriteVRAM(uint32 address, uint8 value) override {
        Record("VDP1WriteVRAM8", {address, value});
    }
    void VDP1WriteVRAM(uint32 address, uint16 value) override {
        Record("VDP1WriteVRAM16", {address, value});
    }
    void VDP1SyncFB() override {
        Record("VDP1SyncFB");
    }
    void VDP1DebugSyncFB() override {
        Record("VDP1DebugSyncFB");
    }
    void VDP1WriteFB(uint32 address, uint8 value) override {
        Record("VDP1WriteFB8", {address, value});
    }
    void VDP1WriteFB(uint32 address, uint16 value) override {
        Record("VDP1WriteFB16", {address, value});
    }
    void VDP1WriteReg(uint32 address, uint16 value) override {
        Record("VDP1WriteReg", {address, value});
    }

    void VDP2WriteVRAM(uint32 address, uint8 value) override {
        Record("VDP2WriteVRAM8", {address, value});
    }
    void VDP2WriteVRAM(uint32 address, uint16 value) override {
        Record("VDP2WriteVRAM16", {address, value});
    }
    void VDP2WriteCRAM(uint32 address, uint8 value) override {
        Record("VDP2WriteCRAM8", {address, value});
    }
    void VDP2WriteCRAM(uint32 address, uint16 value) override {
        Record("VDP2WriteCRAM16", {address, value});
    }
    void VDP2WriteReg(uint32 address, uint16 value) override {
        Record("VDP2WriteReg", {address, value});
    }

    void VDP1EraseFramebuffer(uint64 cycles) override {
        Record("VDP1EraseFramebuffer");
    }
    void VDP1SwapFramebuffer() override {
        Record("VDP1SwapFramebuffer");
    }
    void VDP1BeginFrame() override {
        Record("VDP1BeginFrame");
    }
    void VDP1ExecuteCommand(uint32 cmdAddress, vdp::VDP1Command::Control control) override {
        Record("VDP1ExecuteCommand", {cmdAddress, control.u16});
    }
    void VDP1EndFrame() override {
        Record("VDP1EndFrame");
    }

    void VDP2SetResolution(uint32 h, uint32 v, bool exclusive) override {
        Record("VDP2SetResolution", {h, v, exclusive});
    }
    void VDP2SetField(bool odd) override {
        Record("VDP2SetField", {odd});
    }
    void VDP2LatchTVMD() override {
        Record("VDP2LatchTVMD");
    }
    void VDP2BeginFrame() override {
        Record("VDP2BeginFrame");
    }
    void VDP2RenderLine(uint32 y) override {
        Record("VDP2RenderLine", {y});
    }
    void VDP2EndFrame() override {
        Record("VDP2EndFrame");
    }

    void UpdateEnabledLayers() override {
        Record("UpdateEnabledLayers");
    }

    void DumpExtraVDP1Framebuffers(std::ostream &out) const override {}

protected:
    void UpdateEnhancements() override {
        Record("UpdateEnhancements", {m_enhancements.deinterlace});
    }

private:
    void Record(std::string name, std::initializer_list<uint32> args = {}) {
        calls.push_back({std::move(name), std::vector<uint32>(args)});
    }
};

// Calls with any of the given names, in order.
std::vector<Call> Only(const std::vector<Call> &calls, std::initializer_list<std::string_view> names) {
    std::vector<Call> out;
    for (const auto &call : calls) {
        if (std::find(names.begin(), names.end(), call.name) != names.end()) {
            out.push_back(call);
        }
    }
    return out;
}

bool Contains(const std::vector<Call> &calls, const Call &call) {
    return std::find(calls.begin(), calls.end(), call) != calls.end();
}

RecordingRenderer *InstallRecorder(Saturn &saturn) {
    RecordingRenderer *recorder = nullptr;
    auto *installed = saturn.VDP.UseExternalRenderer(
        [&](vdp::VDPState &state, vdp::config::VDP2DebugRender &,
            const vdp::config::VDP2AccessPatternsConfig &) -> std::unique_ptr<vdp::IVDPRenderer> {
            auto renderer = std::make_unique<RecordingRenderer>(state);
            recorder = renderer.get();
            return renderer;
        });
    REQUIRE(installed != nullptr);
    REQUIRE(installed == recorder);
    return recorder;
}

constexpr uint32 kVDP1VRAM = 0x5C0'0000;
constexpr uint32 kVDP1Regs = 0x5D0'0000;
constexpr uint32 kVDP2VRAM = 0x5E0'0000;
constexpr uint32 kVDP2CRAM = 0x5F0'0000;
constexpr uint32 kVDP2Regs = 0x5F8'0000;

} // namespace

TEST_CASE("Hardware renderer base reports itself as a hardware renderer", "[vdp][renderer][brimir]") {
    SECTION("hardware renderer") {
        StubRenderer renderer{vdp::VDPRendererType::External};
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

TEST_CASE("External renderer type has a name", "[vdp][renderer][brimir]") {
    CHECK(vdp::GetRendererName(vdp::VDPRendererType::External) == "External");
}

TEST_CASE("External renderer install wires the VDP state and configuration", "[vdp][renderer][brimir]") {
    auto saturn = std::make_unique<Saturn>();
    REQUIRE(saturn->VDP.GetRendererAs<vdp::VDPRendererType::Software>() != nullptr);
    saturn->VDP.ModifyEnhancements([](vdp::config::Enhancements &enh) { enh.deinterlace = true; });
    saturn->VDP.SetSoftwareRenderCallback({nullptr, [](uint32 *, uint32, uint32, void *) {}});

    vdp::VDPState *gotState = nullptr;
    vdp::config::VDP2DebugRender *gotDebug = nullptr;
    const vdp::config::VDP2AccessPatternsConfig *gotAccess = nullptr;
    RecordingRenderer *recorder = nullptr;
    auto *installed = saturn->VDP.UseExternalRenderer(
        [&](vdp::VDPState &state, vdp::config::VDP2DebugRender &debug,
            const vdp::config::VDP2AccessPatternsConfig &access) -> std::unique_ptr<vdp::IVDPRenderer> {
            gotState = &state;
            gotDebug = &debug;
            gotAccess = &access;
            auto renderer = std::make_unique<RecordingRenderer>(state);
            recorder = renderer.get();
            return renderer;
        });

    REQUIRE(recorder != nullptr);
    CHECK(installed == recorder);
    CHECK(installed == &saturn->VDP.GetRenderer());
    CHECK(saturn->VDP.GetRenderer().GetType() == vdp::VDPRendererType::External);
    CHECK(saturn->VDP.GetRendererAs<vdp::VDPRendererType::Software>() == nullptr);

    REQUIRE(gotState != nullptr);
    CHECK(&gotState->regs2 == &saturn->VDP.GetProbe().GetVDP2Regs());
    CHECK(&gotState->regs1 == &saturn->VDP.GetProbe().GetVDP1Regs());
    CHECK(gotDebug == &saturn->VDP.vdp2DebugRenderOptions);
    CHECK(gotAccess == &saturn->VDP.vdp2AccessPatternsConfig);

    const uint32 odd = saturn->VDP.GetProbe().GetVDP2Regs().TVSTAT.ODD;
    const std::vector<Call> expected{
        {"UpdateEnhancements", {1}},
        {"VDP2SetResolution", {320, 224, 0}},
        {"VDP2SetField", {odd}},
        {"PostLoadStateSync", {}},
    };
    CHECK(recorder->calls == expected);
}

TEST_CASE("External renderer install failure keeps the current renderer", "[vdp][renderer][brimir]") {
    auto saturn = std::make_unique<Saturn>();
    auto *software = saturn->VDP.GetRendererAs<vdp::VDPRendererType::Software>();
    REQUIRE(software != nullptr);

    SECTION("factory returns nullptr") {
        auto *installed = saturn->VDP.UseExternalRenderer(
            [](vdp::VDPState &, vdp::config::VDP2DebugRender &, const vdp::config::VDP2AccessPatternsConfig &)
                -> std::unique_ptr<vdp::IVDPRenderer> { return nullptr; });
        CHECK(installed == nullptr);
    }

    SECTION("factory returns an invalid renderer") {
        auto *installed = saturn->VDP.UseExternalRenderer(
            [](vdp::VDPState &state, vdp::config::VDP2DebugRender &, const vdp::config::VDP2AccessPatternsConfig &)
                -> std::unique_ptr<vdp::IVDPRenderer> { return std::make_unique<RecordingRenderer>(state, false); });
        CHECK(installed == nullptr);
    }

    CHECK(saturn->VDP.GetRendererAs<vdp::VDPRendererType::Software>() == software);
}

TEST_CASE("External renderer receives memory writes, frames, resets and save states", "[vdp][renderer][brimir]") {
    auto saturn = std::make_unique<Saturn>();
    auto *recorder = InstallRecorder(*saturn);
    auto &bus = saturn->mainBus;

    SECTION("memory and register writes") {
        recorder->calls.clear();
        bus.Write<uint16>(kVDP1VRAM + 0x100, 0x1234);
        bus.Write<uint16>(kVDP1Regs + 0x006, 0x5678); // EWDR
        bus.Write<uint16>(kVDP2VRAM + 0x200, 0x9ABC);
        bus.Write<uint16>(kVDP2CRAM + 0x010, 0x7C1F);
        bus.Write<uint16>(kVDP2Regs + 0x0F8, 0x0007); // PRINA

        CHECK(Contains(recorder->calls, {"VDP1WriteVRAM16", {0x100, 0x1234}}));
        CHECK(Contains(recorder->calls, {"VDP1WriteReg", {0x006, 0x5678}}));
        CHECK(Contains(recorder->calls, {"VDP2WriteVRAM16", {0x200, 0x9ABC}}));
        CHECK(Contains(recorder->calls, {"VDP2WriteCRAM16", {0x010, 0x7C1F}}));
        CHECK(Contains(recorder->calls, {"VDP2WriteReg", {0x0F8, 0x0007}}));
    }

    SECTION("CRAM writes in CRMD 2 arrive with the mapped address") {
        bus.Write<uint16>(kVDP2Regs + 0x00E, 0x2000); // RAMCTL: CRMD 2
        REQUIRE(saturn->VDP.GetProbe().VDP2GetCRAMMode() == 2);
        recorder->calls.clear();

        bus.Write<uint16>(kVDP2CRAM + 0x016, 0x1234);

        const auto writes = Only(recorder->calls, {"VDP2WriteCRAM16"});
        REQUIRE(writes.size() == 1);
        const uint32 address = writes[0].args[0];
        CHECK(address == 0x80A);
        CHECK(writes[0].args[1] == 0x1234);
        CHECK(recorder->state->mem2.UnmapCRAMAddress<uint16>(address) == 0x016);
    }

    SECTION("one frame") {
        saturn->RunFrame(); // align to the start of a frame
        recorder->calls.clear();
        saturn->RunFrame();

        const auto frame = Only(recorder->calls, {"VDP2BeginFrame", "VDP2RenderLine", "VDP2EndFrame"});
        std::vector<Call> expected{{"VDP2BeginFrame", {}}};
        for (uint32 y = 0; y < 224; y++) {
            expected.push_back({"VDP2RenderLine", {y}});
        }
        expected.push_back({"VDP2EndFrame", {}});
        CHECK(frame == expected);
    }

    SECTION("hard reset") {
        recorder->calls.clear();
        saturn->Reset(true);
        CHECK(Contains(recorder->calls, {"Reset", {1}}));
        CHECK(&saturn->VDP.GetRenderer() == recorder);
    }

    SECTION("save and load") {
        auto state = std::make_unique<savestate::SaveState>();
        recorder->calls.clear();
        saturn->SaveState(*state);
        REQUIRE(saturn->LoadState(*state, true));

        const auto calls = Only(recorder->calls,
                                {"PreSaveStateSync", "SaveState", "ValidateState", "LoadState", "PostLoadStateSync"});
        const std::vector<Call> expected{
            {"PreSaveStateSync", {}}, {"SaveState", {}},         {"ValidateState", {}},
            {"LoadState", {}},        {"PostLoadStateSync", {}},
        };
        CHECK(calls == expected);
    }
}

TEST_CASE("External renderer ignores the software renderer threading options", "[vdp][renderer][brimir]") {
    auto saturn = std::make_unique<Saturn>();
    auto *recorder = InstallRecorder(*saturn);
    recorder->calls.clear();

    saturn->configuration.video.threadedVDP1 = false;
    saturn->configuration.video.threadedVDP2 = false;
    saturn->configuration.video.threadedDeinterlacer = false;

    CHECK(recorder->calls.empty());
    CHECK(&saturn->VDP.GetRenderer() == recorder);
}

TEST_CASE("Falling back from an external renderer to software keeps colours and the frame callback",
          "[vdp][renderer][brimir]") {
    // A newly installed software renderer starts with an empty CRAM colour cache. Without a state sync on install,
    // palette layers render black until the game rewrites CRAM. (Threaded VDP2 rebuilds it when its render thread
    // starts; the unthreaded renderer did not.)
    const bool threaded = GENERATE(false, true);
    CAPTURE(threaded);
    auto saturn = std::make_unique<Saturn>();
    saturn->configuration.video.threadedVDP1 = threaded;
    saturn->configuration.video.threadedVDP2 = threaded;
    REQUIRE(saturn->VDP.GetRendererAs<vdp::VDPRendererType::Software>() != nullptr);

    struct Capture {
        uint32 frames = 0;
        uint32 firstPixel = 0;
    } capture;
    saturn->VDP.SetSoftwareRenderCallback({&capture, [](uint32 *fb, uint32, uint32, void *ctx) {
                                               auto &c = *static_cast<Capture *>(ctx);
                                               c.frames++;
                                               c.firstPixel = fb[0];
                                           }});

    InstallRecorder(*saturn);

    // NBG0: 512x256 256-colour bitmap at 0x20000, every dot palette index 1. CRAM entry 1 is pure green.
    auto &bus = saturn->mainBus;
    bus.Write<uint16>(kVDP2Regs + 0x000, 0x8000); // TVMD: display on, 320x224
    bus.Write<uint16>(kVDP2Regs + 0x010, 0x4444); // CYCA0L..CYCA1U: NBG0 data reads
    bus.Write<uint16>(kVDP2Regs + 0x012, 0x4444);
    bus.Write<uint16>(kVDP2Regs + 0x014, 0x4444);
    bus.Write<uint16>(kVDP2Regs + 0x016, 0x4444);
    bus.Write<uint16>(kVDP2Regs + 0x020, 0x0001); // BGON: NBG0
    bus.Write<uint16>(kVDP2Regs + 0x028, 0x0012); // CHCTLA: NBG0 bitmap, 512x256, 256 colours
    bus.Write<uint16>(kVDP2Regs + 0x03C, 0x0001); // MPOFN: NBG0 bitmap at 0x20000
    bus.Write<uint16>(kVDP2Regs + 0x0F8, 0x0007); // PRINA: NBG0 priority 7
    for (uint32 x = 0; x < 512; x += 2) {
        bus.Write<uint16>(kVDP2VRAM + 0x20000 + x, 0x0101);
    }
    bus.Write<uint16>(kVDP2CRAM + 0x002, 0x03E0);
    saturn->RunFrame(); // latch TVMD while the external renderer is active
    REQUIRE(capture.frames == 0);

    REQUIRE(saturn->VDP.UseSoftwareRenderer() != nullptr);
    saturn->RunFrame();

    CHECK(capture.frames == 1);
    CHECK((capture.firstPixel & 0xFFFFFF) == 0x00F800);
}
