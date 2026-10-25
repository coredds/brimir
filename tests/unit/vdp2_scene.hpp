#pragma once

// CPU half of the VDP2 scene harness (stage1b-port-inventory.md section 7.2). A scene is a deterministic recipe of
// VDP register, VRAM, CRAM and FBRAM writes, applied by Vdp2SceneWriter to one or more (VDPState, renderer) pairs in
// exactly the order and with exactly the latches ymir::vdp::VDP uses, so that every renderer sees the same calls.

#include <ymir/hw/vdp/renderer/vdp_renderer_base.hpp>
#include <ymir/hw/vdp/vdp_configs.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace brimir::test {

class Vdp2SceneWriter;

struct Vdp2Scene {
    std::function<void(Vdp2SceneWriter &)> setup;                // writes before the first frame
    std::function<void(Vdp2SceneWriter &, uint32_t y)> onLine;  // optional writes before RenderLine(y), every frame
    uint32_t frames = 2;                                         // >= 2 exercises fields and frame-to-frame state
    ymir::vdp::config::Enhancements enhancements{};
};

class Vdp2SceneWriter {
public:
    explicit Vdp2SceneWriter(const ymir::vdp::config::Enhancements &enhancements = {});

    /// @brief Adds a (state, renderer) pair, installing the renderer as VDP::InstallRenderer does (enhancements,
    /// resolution, field, PostLoadStateSync). Every later write and frame step applies to all pairs, in Attach order.
    void Attach(ymir::vdp::VDPState &state, ymir::vdp::IVDPRenderer &renderer);

    // Bus-side writes, as VDP::VDP2WriteReg / VDP2WriteVRAM / VDP2WriteCRAM / VDP1WriteReg do them.
    void Reg(uint32_t address, uint16_t value);
    void VRAM16(uint32_t address, uint16_t value);
    void VRAM32(uint32_t address, uint32_t value);
    void CRAM16(uint32_t address, uint16_t value);
    void VDP1Reg(uint32_t address, uint16_t value);

    /// @brief Writes a word of VDP1 framebuffer `fb`. The draw framebuffer goes through VDP::VDP1WriteFB (the renderer
    /// is notified); the display framebuffer, which the bus cannot reach, is written directly into the state.
    void FBRAM16(uint32_t fb, uint32_t address, uint16_t value);

    /// @brief Forces a resolution, as VDP::UpdateResolution reports it (normally derived from TVMD at the top border).
    void SetResolution(uint32_t h, uint32_t v, bool exclusive);

    /// @brief Sets TVSTAT.ODD and notifies the renderers, as the VDP does at VBlank IN.
    void Field(bool odd);

    /// @brief VDP1 framebuffer swap, as VDP::VDP1SwapFramebuffer (renderer first, then the index flip).
    void SwapFramebuffers();

    /// @brief Runs one VDP frame from the top border: UpdateResolution, LatchTVMD, VDP2BeginFrame, the active lines
    /// (onLine(y) then VDP2RenderLine(y)), the VBlank IN field switch and VDP2EndFrame, in the order vdp.cpp uses.
    void RunFrame(const std::function<void(Vdp2SceneWriter &, uint32_t)> &onLine = {});

    /// @brief Runs a whole scene: setup, then `frames` frames.
    void Run(const Vdp2Scene &scene);

    uint32_t HRes() const {
        return m_HRes;
    }
    uint32_t VRes() const {
        return m_VRes;
    }
    bool ExclusiveMonitor() const {
        return m_exclusiveMonitor;
    }
    /// @brief Active lines per field of the current resolution (RenderLine calls per frame).
    uint32_t FieldLines() const;

private:
    struct Target {
        ymir::vdp::VDPState *state;
        ymir::vdp::IVDPRenderer *renderer;
    };

    void UpdateResolution();

    ymir::vdp::config::Enhancements m_enhancements;
    std::vector<Target> m_targets;
    uint32_t m_HRes = ymir::vdp::kDefaultResH;
    uint32_t m_VRes = ymir::vdp::kDefaultResV;
    bool m_exclusiveMonitor = false;
};

/// @brief Renders the scene with a SoftwareVDPRenderer and returns the last frame (XBGR, HRes x VRes).
std::vector<uint32_t> RenderSoftware(const Vdp2Scene &scene, uint32_t *outWidth = nullptr,
                                     uint32_t *outHeight = nullptr);

} // namespace brimir::test
