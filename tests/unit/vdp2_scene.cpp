#include "vdp2_scene.hpp"

#include "vdp1_nx_reference.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>

namespace brimir::test {

using namespace ymir;
using namespace ymir::vdp;

Vdp2SceneWriter::Vdp2SceneWriter(const config::Enhancements &enhancements)
    : m_enhancements(enhancements) {}

void Vdp2SceneWriter::Attach(VDPState &state, IVDPRenderer &renderer) {
    // VDP::InstallRenderer (vdp.cpp:54-81)
    renderer.ConfigureEnhancements(m_enhancements);
    renderer.VDP2SetResolution(m_HRes, m_VRes, m_exclusiveMonitor);
    renderer.VDP2SetField(state.regs2.TVSTAT.ODD);
    renderer.PostLoadStateSync();
    m_targets.push_back({&state, &renderer});
}

void Vdp2SceneWriter::Reg(uint32_t address, uint16_t value) {
    // VDP::VDP2WriteReg (vdp.cpp:490-506)
    for (Target &t : m_targets) {
        t.state->VDP2WriteReg<false>(address, value,
                                     [&](uint32 address, uint16 value) { t.renderer->VDP2WriteReg(address, value); });
    }
}

void Vdp2SceneWriter::VRAM16(uint32_t address, uint16_t value) {
    // VDP::VDP2WriteVRAM (vdp.cpp:460-464)
    for (Target &t : m_targets) {
        t.state->mem2.WriteVRAM<uint16>(address, value,
                                        [&](uint32 address, uint16 value) { t.renderer->VDP2WriteVRAM(address, value); });
    }
}

void Vdp2SceneWriter::VRAM32(uint32_t address, uint32_t value) {
    // 32-bit bus writes are two 16-bit writes, high word first (vdp.cpp MapMemory)
    VRAM16(address + 0, static_cast<uint16_t>(value >> 16u));
    VRAM16(address + 2, static_cast<uint16_t>(value >> 0u));
}

void Vdp2SceneWriter::CRAM16(uint32_t address, uint16_t value) {
    // VDP::VDP2WriteCRAM (vdp.cpp:475-483)
    for (Target &t : m_targets) {
        t.state->mem2.WriteCRAM<uint16>(address, value,
                                        [&](uint32 address, uint16 value) { t.renderer->VDP2WriteCRAM(address, value); });
    }
}

void Vdp2SceneWriter::VDP1Reg(uint32_t address, uint16_t value) {
    // VDP::VDP1WriteReg (vdp.cpp:410-413)
    for (Target &t : m_targets) {
        t.state->VDP1WriteReg<false>(address, value,
                                     [&](uint32 address, uint16 value) { t.renderer->VDP1WriteReg(address, value); });
    }
}

void Vdp2SceneWriter::FBRAM16(uint32_t fb, uint32_t address, uint16_t value) {
    for (Target &t : m_targets) {
        // The display framebuffer is written as the draw framebuffer of a swap, then swapped back (the index only).
        const bool display = (fb & 1) != t.state->fbIndex.draw;
        if (display) {
            t.state->fbIndex.Flip();
        }
        // VDP::VDP1WriteFB (vdp.cpp:400-402)
        t.state->VDP1WriteFB<uint16>(address, value,
                                     [&](uint32 address, uint16 value) { t.renderer->VDP1WriteFB(address, value); });
        if (display) {
            t.state->fbIndex.Flip();
        }
    }
}

void Vdp2SceneWriter::FBRAM8(uint32_t fb, uint32_t address, uint8_t value) {
    for (Target &t : m_targets) {
        const bool display = (fb & 1) != t.state->fbIndex.draw;
        if (display) {
            t.state->fbIndex.Flip();
        }
        t.state->VDP1WriteFB<uint8>(address, value,
                                    [&](uint32 address, uint8 value) { t.renderer->VDP1WriteFB(address, value); });
        if (display) {
            t.state->fbIndex.Flip();
        }
    }
}

void Vdp2SceneWriter::VDP1VRAMBytes(uint32_t address, std::span<const uint8_t> bytes) {
    for (size_t i = 0; i < bytes.size(); i += 2) {
        const uint8_t lo = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        VDP1VRAM16(address + static_cast<uint32_t>(i), static_cast<uint16_t>((bytes[i] << 8) | lo));
    }
}

void Vdp2SceneWriter::VDP1Command(uint32_t address, const Vdp1Cmd &cmd) {
    for (uint32_t i = 0; i < 16; ++i) {
        VDP1VRAM16(address + i * 2, cmd.w[i]);
    }
}

void Vdp2SceneWriter::VDP1Gouraud(uint32_t address, const std::array<uint16_t, 4> &colors) {
    for (uint32_t i = 0; i < 4; ++i) {
        VDP1VRAM16(address + i * 2, colors[i]);
    }
}

void Vdp2SceneWriter::VDP1LUT(uint32_t address, const std::array<uint16_t, 16> &colors) {
    for (uint32_t i = 0; i < 16; ++i) {
        VDP1VRAM16(address + i * 2, colors[i]);
    }
}

bool Vdp2SceneWriter::DrawVDP1Partial(uint32_t first, uint32_t count) {
    bool ended = false;
    for (Target &t : m_targets) {
        if (first == 0) {
            t.state->regs1.returnAddress = kVDP1NoReturn;
            t.state->regs1.nextCommandAddress = 0;
            t.state->regs1.currFrameEnded = false;
            t.renderer->VDP1BeginFrame();
        }
        ended = false;
        for (uint32_t slot = first; slot < first + count && slot * 0x20 < kVDP1VRAMSize; ++slot) {
            const uint32_t address = slot * 0x20;
            const VDP1Command::Control control{.u16 = t.state->mem1.ReadVRAM<uint16>(address)};
            if (control.end) {
                ended = true;
                break;
            }
            if (!control.skip && control.IsValid()) {
                t.state->regs1.currCommandAddress = address;
                t.renderer->VDP1ExecuteCommand(address, control);
            }
        }
        if (ended) {
            t.state->regs1.currFrameEnded = true;
            t.renderer->VDP1EndFrame();
        }
    }
    return ended;
}

std::vector<uint16_t> Vdp2SceneWriter::ReadFB16(uint32_t address) {
    std::vector<uint16_t> values;
    for (Target &t : m_targets) {
        // VDP::VDP1ReadFB (vdp.cpp:389-397)
        t.renderer->VDP1SyncFB();
        values.push_back(t.state->VDP1ReadFB<uint16>(address));
    }
    return values;
}

std::vector<uint8_t> Vdp2SceneWriter::ReadFB8(uint32_t address) {
    std::vector<uint8_t> values;
    for (Target &t : m_targets) {
        t.renderer->VDP1SyncFB();
        values.push_back(t.state->VDP1ReadFB<uint8>(address));
    }
    return values;
}

std::vector<std::unique_ptr<savestate::VDPSaveState>> Vdp2SceneWriter::SaveStates() {
    std::vector<std::unique_ptr<savestate::VDPSaveState>> saved;
    for (Target &t : m_targets) {
        // VDP::SaveState (vdp.cpp:512-523)
        auto state = std::make_unique<savestate::VDPSaveState>();
        t.renderer->PreSaveStateSync();
        t.state->SaveState(*state);
        t.renderer->SaveState(state->renderer);
        saved.push_back(std::move(state));
    }
    return saved;
}

void Vdp2SceneWriter::CrossLoad() {
    auto saved = SaveStates();
    for (size_t i = 0; i < m_targets.size(); ++i) {
        // VDP::LoadState (vdp.cpp:530-546)
        const savestate::VDPSaveState &state = *saved[(i + 1) % saved.size()];
        Target &t = m_targets[i];
        t.state->LoadState(state);
        t.renderer->LoadState(state.renderer);
        t.renderer->PostLoadStateSync();
    }
}

void Vdp2SceneWriter::SaveLoadMidFrame() {
    for (Target &t : m_targets) {
        // VDP::SaveState then VDP::LoadState (vdp.cpp:512-546), with a blank renderer section loaded in between
        auto saved = std::make_unique<savestate::VDPSaveState>();
        t.renderer->PreSaveStateSync();
        t.state->SaveState(*saved);
        t.renderer->SaveState(saved->renderer);
        auto blank = std::make_unique<savestate::VDPSaveState::VDPRendererSaveState>();
        t.renderer->LoadState(*blank);
        t.state->LoadState(*saved);
        t.renderer->LoadState(saved->renderer);
        t.renderer->PostLoadStateSync();
    }
}

// -----------------------------------------------------------------------------
// VDP1 command tables

namespace {

uint16_t U16(int v) {
    return static_cast<uint16_t>(v);
}

} // namespace

Vdp1Cmd Vdp1Cmd::Normal(uint16_t pmod, uint16_t colr, uint16_t srca, uint16_t size, int x, int y) {
    return {{0x0000, 0, pmod, colr, srca, size, U16(x), U16(y), 0, 0, 0, 0, 0, 0, 0, 0}};
}

Vdp1Cmd Vdp1Cmd::Scaled(uint16_t pmod, uint16_t colr, uint16_t srca, uint16_t size, int ax, int ay, int cx, int cy) {
    return {{0x0001, 0, pmod, colr, srca, size, U16(ax), U16(ay), 0, 0, U16(cx), U16(cy), 0, 0, 0, 0}};
}

Vdp1Cmd Vdp1Cmd::ScaledZoom(uint16_t zp, uint16_t pmod, uint16_t colr, uint16_t srca, uint16_t size, int ax, int ay,
                            int width, int height) {
    return {{static_cast<uint16_t>(0x0001 | ((zp & 0xF) << 8)), 0, pmod, colr, srca, size, U16(ax), U16(ay), U16(width),
             U16(height), 0, 0, 0, 0, 0, 0}};
}

Vdp1Cmd Vdp1Cmd::Distorted(uint16_t pmod, uint16_t colr, uint16_t srca, uint16_t size, int ax, int ay, int bx, int by,
                           int cx, int cy, int dx, int dy) {
    return {{0x0002, 0, pmod, colr, srca, size, U16(ax), U16(ay), U16(bx), U16(by), U16(cx), U16(cy), U16(dx), U16(dy),
             0, 0}};
}

Vdp1Cmd Vdp1Cmd::Polygon(uint16_t pmod, uint16_t colr, int ax, int ay, int bx, int by, int cx, int cy, int dx,
                         int dy) {
    return {{0x0004, 0, pmod, colr, 0, 0, U16(ax), U16(ay), U16(bx), U16(by), U16(cx), U16(cy), U16(dx), U16(dy), 0, 0}};
}

Vdp1Cmd Vdp1Cmd::Polyline(uint16_t pmod, uint16_t colr, int ax, int ay, int bx, int by, int cx, int cy, int dx,
                          int dy) {
    return {{0x0005, 0, pmod, colr, 0, 0, U16(ax), U16(ay), U16(bx), U16(by), U16(cx), U16(cy), U16(dx), U16(dy), 0, 0}};
}

Vdp1Cmd Vdp1Cmd::Line(uint16_t pmod, uint16_t colr, int ax, int ay, int bx, int by) {
    return {{0x0006, 0, pmod, colr, 0, 0, U16(ax), U16(ay), U16(bx), U16(by), 0, 0, 0, 0, 0, 0}};
}

Vdp1Cmd Vdp1Cmd::UserClip(int x0, int y0, int x1, int y1) {
    return {{0x0008, 0, 0, 0, 0, 0, U16(x0), U16(y0), 0, 0, U16(x1), U16(y1), 0, 0, 0, 0}};
}

Vdp1Cmd Vdp1Cmd::SystemClip(int x1, int y1) {
    return {{0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, U16(x1), U16(y1), 0, 0, 0, 0}};
}

Vdp1Cmd Vdp1Cmd::Local(int x, int y) {
    return {{0x000A, 0, 0, 0, 0, 0, U16(x), U16(y), 0, 0, 0, 0, 0, 0, 0, 0}};
}

Vdp1Cmd Vdp1Cmd::End() {
    return {{0x8000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
}

Vdp1Cmd &Vdp1Cmd::Flip(bool h, bool v) {
    w[0] = static_cast<uint16_t>((w[0] & ~0x0030) | (h ? 0x0010 : 0) | (v ? 0x0020 : 0));
    return *this;
}

Vdp1Cmd &Vdp1Cmd::Skip() {
    w[0] |= 0x4000;
    return *this;
}

Vdp1Cmd &Vdp1Cmd::Gouraud(uint16_t grda) {
    w[14] = grda;
    return *this;
}

void Vdp2SceneWriter::SetResolution(uint32_t h, uint32_t v, bool exclusive) {
    m_HRes = h;
    m_VRes = v;
    m_exclusiveMonitor = exclusive;
    for (Target &t : m_targets) {
        t.renderer->VDP2SetResolution(h, v, exclusive);
    }
}

void Vdp2SceneWriter::ResetVDP(bool hard) {
    // VDP::Reset (vdp.cpp:83-100)
    for (Target &t : m_targets) {
        t.state->Reset(hard);
        t.renderer->Reset(hard);
    }
    m_HRes = kDefaultResH;
    m_VRes = kDefaultResV;
    m_exclusiveMonitor = false;
    UpdateResolution();
    m_restartFrame = true;
}

void Vdp2SceneWriter::ConfigureEnhancements(const config::Enhancements &enhancements) {
    // VDP::SetEnhancements (vdp.hpp:176-179)
    m_enhancements = enhancements;
    for (Target &t : m_targets) {
        t.renderer->ConfigureEnhancements(enhancements);
    }
}

void Vdp2SceneWriter::Field(bool odd) {
    for (Target &t : m_targets) {
        t.state->regs2.TVSTAT.ODD = odd;
        t.renderer->VDP2SetField(odd);
    }
}

void Vdp2SceneWriter::SwapFramebuffers() {
    // VDP::VDP1SwapFramebuffer (vdp.cpp:980-999), the parts the renderers see
    for (Target &t : m_targets) {
        t.state->regs1.LatchEraseParameters();
        t.renderer->VDP1SwapFramebuffer();
        t.state->fbIndex.Flip();
    }
}

void Vdp2SceneWriter::EraseFramebuffer(uint64_t cycles) {
    for (Target &t : m_targets) {
        t.renderer->VDP1EraseFramebuffer(cycles);
    }
}

void Vdp2SceneWriter::VDP1VRAM16(uint32_t address, uint16_t value) {
    // VDP::VDP1WriteVRAM (vdp.cpp:379-386)
    for (Target &t : m_targets) {
        t.state->mem1.WriteVRAM<uint16>(
            address, value, [&](uint32 address, uint16 value) { t.renderer->VDP1WriteVRAM(address, value); });
    }
}

void Vdp2SceneWriter::DrawVDP1() {
    // VDP::VDP1BeginFrame, VDP1ProcessCommand and VDP1EndFrame (vdp.cpp:1005-1080), without timing or jumps.
    for (Target &t : m_targets) {
        t.state->regs1.returnAddress = kVDP1NoReturn;
        t.state->regs1.nextCommandAddress = 0;
        t.state->regs1.currFrameEnded = false;
        t.renderer->VDP1BeginFrame();
        for (uint32_t address = 0; address < kVDP1VRAMSize; address += 0x20) {
            const VDP1Command::Control control{.u16 = t.state->mem1.ReadVRAM<uint16>(address)};
            if (control.end) {
                break;
            }
            if (!control.skip && control.IsValid()) {
                t.state->regs1.currCommandAddress = address;
                t.renderer->VDP1ExecuteCommand(address, control);
            }
        }
        t.state->regs1.currFrameEnded = true;
        t.renderer->VDP1EndFrame();
    }
}

uint32_t Vdp2SceneWriter::DrawFramebuffer() const {
    return m_targets.empty() ? 0 : m_targets[0].state->fbIndex.draw;
}

uint32_t Vdp2SceneWriter::FieldLines() const {
    const bool interlaced = !m_exclusiveMonitor && m_VRes > kMaxNormalResV;
    return interlaced ? m_VRes >> 1u : m_VRes;
}

void Vdp2SceneWriter::UpdateResolution() {
    // VDP::UpdateResolution (vdp.cpp:594-616, 773), the resolution part. All targets hold the same registers.
    if (m_targets.empty() || !m_targets[0].state->regs2.TVMDDirty) {
        return;
    }
    const VDP2Regs &regs2 = m_targets[0].state->regs2;

    static constexpr uint32 hRes[] = {320, 352, 640, 704};
    static constexpr uint32 vRes[] = {224, 240, 256, 256};

    const bool exclusiveMonitor = regs2.TVMD.HRESOn & 4;
    const bool interlaced = regs2.TVMD.IsInterlaced();
    uint32_t h = hRes[regs2.TVMD.HRESOn & 3];
    uint32_t v = exclusiveMonitor ? 480 : vRes[regs2.TVMD.VRESOn & (regs2.TVSTAT.PAL ? 3 : 1)];
    if (!exclusiveMonitor && interlaced) {
        v *= 2;
    }
    for (Target &t : m_targets) {
        t.state->regs2.TVMDDirty = false;
        t.state->regs2.VCNTShift = t.state->regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity ? 1 : 0;
    }
    SetResolution(h, v, exclusiveMonitor);
}

void Vdp2SceneWriter::RunFrame(const std::function<void(Vdp2SceneWriter &, uint32_t)> &onLine, uint32_t firstLine,
                               uint32_t endLine, const std::function<void(Vdp2SceneWriter &)> &blanking) {
    if (firstLine == 0) {
        // Top border (vdp.cpp:955-965)
        UpdateResolution();
        for (Target &t : m_targets) {
            t.state->regs2.LatchTVMD();
            t.renderer->VDP2LatchTVMD();
        }

        // Last line: VBlank OUT (vdp.cpp:967-978)
        for (Target &t : m_targets) {
            t.renderer->VDP2BeginFrame();
            t.state->regs2.TVSTAT.VBLANK = 0;
        }
    }

    // Active display (vdp.cpp:812-821); writes for line y land between RenderLine(y-1) and RenderLine(y)
    const uint32_t lines = FieldLines();
    for (uint32_t y = firstLine; y < std::min(lines, endLine); ++y) {
        for (Target &t : m_targets) {
            t.state->regs2.VCNT = static_cast<uint16>(y);
        }
        if (onLine) {
            onLine(*this, y);
        }
        if (m_restartFrame) {
            // VDP::Reset renders line 0 at once (BeginHPhaseActiveDisplay with VCNT 0)
            m_restartFrame = false;
            y = 0;
            for (Target &t : m_targets) {
                t.state->regs2.VCNT = 0;
            }
        }
        if (lineObserver && !m_targets.empty()) {
            lineObserver(*this, *m_targets[0].state, y);
        }
        for (Target &t : m_targets) {
            t.renderer->VDP2RenderLine(y);
        }
    }

    if (endLine < lines) {
        return;
    }

    // VBlank IN at the right border of the first bottom-border line switches fields (vdp.cpp:823-853). The frame
    // ends when blanking starts (vdp.cpp:933-947): one line later for 224-line NTSC and every PAL and exclusive mode,
    // on the same line, before the field switch, for 240-line NTSC (whose bottom border has no lines).
    auto switchField = [&] {
        for (Target &t : m_targets) {
            VDP2Regs &regs2 = t.state->regs2;
            regs2.TVSTAT.VBLANK = 1;
            if (regs2.TVMD.LSMDn != InterlaceMode::None) {
                regs2.TVSTAT.ODD ^= 1;
                t.renderer->VDP2SetField(regs2.TVSTAT.ODD);
            } else if (regs2.TVSTAT.ODD != 1) {
                regs2.TVSTAT.ODD = 1;
                t.renderer->VDP2SetField(regs2.TVSTAT.ODD);
            }
        }
    };
    auto endFrame = [&] {
        for (Target &t : m_targets) {
            t.renderer->VDP2EndFrame();
        }
    };
    const bool pal = !m_targets.empty() && m_targets[0].state->regs2.TVSTAT.PAL;
    const bool endBeforeFieldSwitch = !pal && !m_exclusiveMonitor && lines == 240;
    if (endBeforeFieldSwitch) {
        endFrame();
        switchField();
    } else {
        switchField();
        endFrame();
    }
    if (blanking) {
        blanking(*this);
    }
}

void Vdp2SceneWriter::ForEachState(const std::function<void(VDPState &)> &fn) {
    for (Target &t : m_targets) {
        fn(*t.state);
    }
}

void Vdp2SceneWriter::Run(const Vdp2Scene &scene) {
    if (scene.setup) {
        scene.setup(*this);
    }
    for (uint32_t frame = 0; frame < scene.frames; ++frame) {
        RunFrame(scene.onLine, 0, UINT32_MAX, scene.blanking);
    }
}

std::vector<uint32_t> RenderSoftware(const Vdp2Scene &scene, uint32_t *outWidth, uint32_t *outHeight) {
    struct Capture {
        std::vector<uint32_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
    } capture;

    auto state = std::make_unique<VDPState>();
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    auto renderer = std::make_unique<SoftwareVDPRenderer>(*state, debug, access);
    renderer->SwCallbacks.FrameComplete.Rebind(&capture, [](uint32 *fb, uint32 w, uint32 h, void *context) {
        auto &out = *static_cast<Capture *>(context);
        out.pixels.assign(fb, fb + static_cast<size_t>(w) * h);
        out.width = w;
        out.height = h;
    });

    Vdp2SceneWriter writer{scene.enhancements};
    writer.Attach(*state, *renderer);
    writer.Run(scene);

    if (outWidth != nullptr) {
        *outWidth = capture.width;
    }
    if (outHeight != nullptr) {
        *outHeight = capture.height;
    }
    return std::move(capture.pixels);
}

std::string SceneResult::Describe() const {
    std::ostringstream out;
    if (!error.empty()) {
        out << "harness error: " << error;
        return out.str();
    }
    out << "frame " << frame << " (" << comparedFrames << " compared), " << width << "x" << height << ", "
        << distinctColors << " distinct software colours, ";
    if (scale > 1) {
        out << "internal resolution " << scale << "x (" << comparedPixels << " GPU pixels compared, " << skippedPixels
            << " sample points skipped), ";
    }
    if (identical) {
        out << "identical";
    } else {
        out << diffCount << " pixels differ; first at (" << diffX << ", " << diffY << "): software 0x" << std::hex
            << std::setw(6) << std::setfill('0') << (softwarePixel & 0xFFFFFF) << ", gpu 0x" << std::setw(6)
            << (gpuPixel & 0xFFFFFF) << " (XBGR)";
    }
    return out.str();
}

void LatticeForLine(const Vdp2SceneWriter &writer, const VDPState &state, uint32_t y, bool deinterlace,
                    const std::function<void(uint32_t row, SampleLatticeRow flags)> &set) {
    const VDP2Regs &regs2 = state.regs2;
    const bool rbg = regs2.bgEnabled[4] || regs2.bgEnabled[5];
    const bool nbg = regs2.bgEnabled[0] || regs2.bgEnabled[1] || regs2.bgEnabled[2] || regs2.bgEnabled[3];
    const bool hiRes = writer.HRes() >= 640;
    // The composite doubles the rows in interlaced modes (not in exclusive monitor modes), as FieldLines.
    const bool interlaced = !writer.ExclusiveMonitor() && writer.VRes() > kMaxNormalResV;
    const bool singleDensity = regs2.TVMD.LSMDn == InterlaceMode::SingleDensity;
    SampleLatticeRow flags{.skipOddColumns = rbg && hiRes};
    if (!interlaced) {
        set(y, flags);
        return;
    }
    // Odd rows: RBGs, deinterlaced or woven (both rows are the RBG line, from the same rotation bases), NBGs in single
    // density (both fields show the same line, deinterlaced or woven).
    SampleLatticeRow odd = flags;
    odd.skipRow = rbg || (nbg && singleDensity);
    if (deinterlace) {
        set(y * 2, flags);
        set(y * 2 + 1, odd);
    } else if (regs2.TVSTAT.ODD) {
        set(y * 2 + 1, odd);
    } else {
        set(y * 2, flags);
    }
}

namespace {

// The software frame's distinct RGB colours (up to 64).
uint32_t DistinctColors(const std::vector<uint32_t> &software) {
    std::vector<uint32_t> colors;
    for (const uint32_t pixel : software) {
        const uint32_t sw = pixel & 0xFFFFFF;
        if (colors.size() < 64 && std::find(colors.begin(), colors.end(), sw) == colors.end()) {
            colors.push_back(sw);
        }
    }
    return static_cast<uint32_t>(colors.size());
}

bool CheckScaledSizes(const std::vector<uint32_t> &software, uint32_t width, uint32_t height,
                      const std::vector<uint32_t> &scaled, uint32_t n, SceneResult &result) {
    if (n < 1 || width == 0 || height == 0 || software.size() != static_cast<size_t>(width) * height ||
        scaled.size() != static_cast<size_t>(width) * height * n * n) {
        result.error = "scaled comparison: " + std::to_string(software.size()) + " software pixels (" +
                       std::to_string(width) + "x" + std::to_string(height) + "), " + std::to_string(scaled.size()) +
                       " GPU pixels at " + std::to_string(n) + "x";
        return false;
    }
    return true;
}

// Records native pixel (x, y) as differing (gpu: the differing GPU pixel).
void MarkDiff(SceneResult &result, uint32_t x, uint32_t y, uint32_t gpu) {
    const size_t i = static_cast<size_t>(y) * result.width + x;
    result.gpu[i] = gpu;
    if (result.diffCount++ == 0) {
        result.diffX = x;
        result.diffY = y;
        result.softwarePixel = result.software[i];
        result.gpuPixel = gpu;
    }
}

void StartScaled(const std::vector<uint32_t> &software, uint32_t width, uint32_t height,
                 std::vector<uint32_t> &&scaled, uint32_t n, SceneResult &result) {
    result.width = width;
    result.height = height;
    result.scale = n;
    result.software = software;
    result.gpu = software; // differing pixels are replaced
    result.gpuScaled = std::move(scaled);
    result.diffCount = 0;
    result.comparedPixels = 0;
    result.skippedPixels = 0;
    result.distinctColors = DistinctColors(software);
}

} // namespace

bool CompareNearestPixels(const std::vector<uint32_t> &software, uint32_t width, uint32_t height,
                          std::vector<uint32_t> scaled, uint32_t n, SceneResult &result) {
    if (!CheckScaledSizes(software, width, height, scaled, n, result)) {
        return false;
    }
    StartScaled(software, width, height, std::move(scaled), n, result);
    const uint32_t pitch = width * n;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t sw = software[static_cast<size_t>(y) * width + x] & 0xFFFFFF;
            bool differs = false;
            for (uint32_t j = 0; j < n; ++j) {
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t gpu = result.gpuScaled[static_cast<size_t>(y * n + j) * pitch + x * n + i];
                    ++result.comparedPixels;
                    if (!differs && (gpu & 0xFFFFFF) != sw) {
                        differs = true;
                        MarkDiff(result, x, y, gpu);
                    }
                }
            }
        }
    }
    if (result.comparedPixels != static_cast<uint64_t>(width) * height * n * n) {
        result.error = "nearest comparison compared " + std::to_string(result.comparedPixels) + " pixels";
        return false;
    }
    result.identical = result.diffCount == 0;
    return true;
}

bool CompareSamplePointPixels(const std::vector<uint32_t> &software, uint32_t width, uint32_t height,
                              std::vector<uint32_t> scaled, uint32_t n, const std::vector<SampleLatticeRow> &lattice,
                              SceneResult &result) {
    if (!CheckScaledSizes(software, width, height, scaled, n, result)) {
        return false;
    }
    StartScaled(software, width, height, std::move(scaled), n, result);
    const uint32_t pitch = width * n;
    for (uint32_t y = 0; y < height; ++y) {
        const SampleLatticeRow row = y < lattice.size() ? lattice[y] : SampleLatticeRow{};
        for (uint32_t x = 0; x < width; ++x) {
            if (row.skipRow || (row.skipOddColumns && (x & 1) != 0)) {
                ++result.skippedPixels;
                continue;
            }
            const size_t i = static_cast<size_t>(y) * width + x;
            const uint32_t gpu = result.gpuScaled[static_cast<size_t>(y * n) * pitch + x * n];
            ++result.comparedPixels;
            if ((gpu & 0xFFFFFF) != (software[i] & 0xFFFFFF)) {
                MarkDiff(result, x, y, gpu);
            }
        }
    }
    // The lattice excludes at most odd columns and odd rows: three quarters of the frame are compared, or one quarter
    // with both.
    if (result.comparedPixels == 0 || result.comparedPixels * 4 < static_cast<uint64_t>(width) * height) {
        result.error = "sample-point comparison compared only " + std::to_string(result.comparedPixels) + " of " +
                       std::to_string(static_cast<uint64_t>(width) * height) + " native pixels";
        return false;
    }
    result.identical = result.diffCount == 0;
    return true;
}

} // namespace brimir::test

#if BRIMIR_VULKAN

    #include <gpu/vdp/vulkan_vdp_renderer.hpp>
    #include <gpu/vk_util.hpp>

namespace brimir::test {

namespace {

// The harness's fence wait: vk_util::WaitForFence (a bounded, yielding poll, then a blocking wait), bounded overall:
// a device that never finishes fails the scene with this error instead of hanging the test run. The bound is 60 s on
// a GPU and 600 s on a CPU device: on lavapipe, a submit of thousands of small VDP1 batches (fuzzer programs with tiny
// batch limits) takes over 60 s when several test processes share the CPU.
VkResult WaitBounded(gpu::VulkanDevice &device, VkFence fence) {
    const bool cpu = device.Properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    const uint64_t seconds = cpu ? 600 : 60;
    const VkResult result = gpu::vk_util::WaitForFence(device, fence, 2000, seconds * 1'000'000'000ull);
    if (result == VK_TIMEOUT) {
        std::fprintf(stderr, "vdp2_scene: a submit did not complete within %llu s (VK_TIMEOUT)\n",
                     static_cast<unsigned long long>(seconds));
    }
    return result;
}
// One command pool, command buffer and fence, reused by every submit (each recorded, submitted and waited for with
// WaitBounded): the first submit of a newly allocated command buffer costs milliseconds on some drivers (about 2.5 ms
// on the RX 9060 XT on Windows, against 0.05 ms for a reused one).
class PolledSubmitter {
public:
    explicit PolledSubmitter(gpu::VulkanDevice &device)
        : m_device(device) {
        const auto &vk = device.Table();
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = device.QueueFamily();
        m_ok = vk.vkCreateCommandPool(device.Device(), &poolInfo, nullptr, &m_pool) == VK_SUCCESS;
        if (m_ok) {
            VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocInfo.commandPool = m_pool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            m_ok = vk.vkAllocateCommandBuffers(device.Device(), &allocInfo, &m_cmd) == VK_SUCCESS;
        }
        if (m_ok) {
            const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            m_ok = vk.vkCreateFence(device.Device(), &fenceInfo, nullptr, &m_fence) == VK_SUCCESS;
        }
    }
    ~PolledSubmitter() {
        const auto &vk = m_device.Table();
        vk.vkDestroyFence(m_device.Device(), m_fence, nullptr);
        vk.vkDestroyCommandPool(m_device.Device(), m_pool, nullptr); // frees m_cmd
    }
    PolledSubmitter(const PolledSubmitter &) = delete;
    PolledSubmitter &operator=(const PolledSubmitter &) = delete;

    // vk_util::SubmitOneShot's contract: records, submits and waits.
    VkResult Submit(const std::function<void(VkCommandBuffer)> &record) {
        return SubmitIf([&](VkCommandBuffer cmd) {
            record(cmd);
            return true;
        });
    }
    // The same, but nothing is submitted when `record` returns false (VK_SUCCESS then).
    VkResult SubmitIf(const std::function<bool(VkCommandBuffer)> &record) {
        if (!m_ok) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto &vk = m_device.Table();
        VkResult result = vk.vkResetCommandBuffer(m_cmd, 0);
        if (result == VK_SUCCESS) {
            VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vk.vkBeginCommandBuffer(m_cmd, &beginInfo);
        }
        bool recorded = false;
        if (result == VK_SUCCESS) {
            recorded = record(m_cmd);
            result = vk.vkEndCommandBuffer(m_cmd);
        }
        if (result != VK_SUCCESS || !recorded) {
            return result;
        }
        result = vk.vkResetFences(m_device.Device(), 1, &m_fence);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &m_cmd;
            result = m_device.Submit(submit, m_fence);
            if (result == VK_SUCCESS) {
                result = WaitBounded(m_device, m_fence);
            }
        }
        return result;
    }

private:
    gpu::VulkanDevice &m_device;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    bool m_ok = false;
};

VkResult SubmitPolled(gpu::VulkanDevice &device, const std::function<void(VkCommandBuffer)> &record) {
    PolledSubmitter submitter(device);
    return submitter.Submit(record);
}

} // namespace

std::vector<uint32_t> ReadbackBuffer(gpu::VulkanDevice &device, VkBuffer buffer, uint32_t words) {
    namespace vu = gpu::vk_util;
    std::vector<uint32_t> out;
    vu::Buffer host;
    const VkDeviceSize bytes = VkDeviceSize{words} * sizeof(uint32_t);
    // Host-cached memory where there is some (uncached reads are slow)
    const auto create = [&](VkMemoryPropertyFlags extra) {
        return vu::CreateBuffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | extra,
                                host) == VK_SUCCESS;
    };
    if (words == 0 || (!create(VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && !create(0))) {
        return out;
    }
    const VkResult result = SubmitPolled(device, [&](VkCommandBuffer cmd) {
        vu::GlobalBarrier(device, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy region{0, 0, bytes};
        device.Table().vkCmdCopyBuffer(cmd, buffer, host.buffer, 1, &region);
        vu::GlobalBarrier(device, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    });
    if (result == VK_SUCCESS && host.mapped != nullptr) {
        const auto *words32 = static_cast<const uint32_t *>(host.mapped);
        out.assign(words32, words32 + words);
    }
    vu::DestroyBuffer(device, host);
    return out;
}

void InstallSyncSubmit(gpu::VulkanVDPRenderer &renderer, gpu::VulkanDevice &device, SyncSubmitCounter *counter) {
    // One submitter for the renderer's lifetime (it holds the callback; the device must outlive the renderer)
    auto submitter = std::make_shared<PolledSubmitter>(device);
    renderer.SetSyncSubmit([submitter, counter](const gpu::VulkanVDPRenderer::SyncRecord &record) {
        if (counter != nullptr && !counter->allowed) {
            ++counter->refused;
            return false;
        }
        bool recorded = false;
        const VkResult result = submitter->SubmitIf([&](VkCommandBuffer cmd) {
            recorded = record(cmd);
            return recorded;
        });
        const bool ok = result == VK_SUCCESS && recorded;
        if (counter != nullptr && (ok || recorded)) {
            ++(ok ? counter->submits : counter->refused);
        }
        return ok;
    });
}

namespace {

// Records a command buffer and destroys it without submitting it.
bool RecordWithoutSubmit(gpu::VulkanDevice &device, const std::function<void(VkCommandBuffer)> &record) {
    const auto &vk = device.Table();
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = device.QueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vk.vkCreateCommandPool(device.Device(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool ok = vk.vkAllocateCommandBuffers(device.Device(), &allocInfo, &cmd) == VK_SUCCESS;
    if (ok) {
        const VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        ok = vk.vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS;
    }
    if (ok) {
        record(cmd);
        ok = vk.vkEndCommandBuffer(cmd) == VK_SUCCESS;
    }
    vk.vkDestroyCommandPool(device.Device(), pool, nullptr);
    return ok;
}

// The N x reference of RenderBothOptions::vdp1Upscaling: native FBRAM and (while the renderer has one) the N x surface,
// following what the GPU replayed (VulkanVDPRenderer::Vdp1WorkEvent). Without a surface it replays at N = 1 so the
// native FBRAM (and VDP1 VRAM) stay current for the next surface.
class NxModel {
public:
    NxModel()
        : m_native(std::make_unique<vdp1ref::Fbram>()) {
        Rescale(0);
    }
    void OnEvent(const gpu::VulkanVDPRenderer::Vdp1WorkEvent &e) {
        using Kind = gpu::VulkanVDPRenderer::Vdp1WorkEvent::Kind;
        switch (e.kind) {
        case Kind::Ops: m_ref->Replay(*e.ops); break;
        case Kind::Upload:
            for (uint32_t slice = 0; slice < 8; ++slice) {
                if (((e.sliceMask >> slice) & 1u) == 0) {
                    continue;
                }
                std::memcpy(Slice(slice).data(), e.fbram.data() + size_t{slice} * kVDP1FBRAMSize, kVDP1FBRAMSize);
                ReplicateSlice(slice);
            }
            break;
        case Kind::Rescale: Rescale(e.scale); break;
        }
    }
    uint32_t Scale() const {
        return m_scale;
    }
    const vdp1nx::NxFbram &Surface() const {
        return *m_surface;
    }
    SpriteFB &Slice(uint32_t slice) {
        vdp1ref::Fbram &f = *m_native;
        switch (slice) {
        case 0: return f.main[0];
        case 1: return f.main[1];
        case 2: return f.alt[0];
        case 3: return f.alt[1];
        default: return f.mesh[(slice - 4) >> 1][slice & 1];
        }
    }

private:
    void Rescale(uint32_t n) {
        std::vector<uint8> vram;
        if (m_ref) {
            vram.assign(m_ref->Vram().begin(), m_ref->Vram().end());
        }
        m_ref.reset();
        m_scale = n;
        m_surface = std::make_unique<vdp1nx::NxFbram>(std::max(n, 1u));
        m_surface->ReplicateFrom(*m_native);
        vdp1nx::Options o;
        o.scale = std::max(n, 1u);
        m_ref = std::make_unique<vdp1nx::ReferenceNx>(*m_native, *m_surface, o, vram);
    }
    void ReplicateSlice(uint32_t slice) {
        const uint32_t sub = m_surface->Sub();
        uint16 *out = m_surface->slices[slice].data();
        const SpriteFB &s = Slice(slice);
        for (uint32_t w = 0; w < kVDP1FBRAMSize / 2; ++w) {
            const uint16 v = static_cast<uint16>((s[2 * w] << 8) | s[2 * w + 1]);
            for (uint32_t i = 0; i < sub; ++i) {
                *out++ = v;
            }
        }
    }

    std::unique_ptr<vdp1ref::Fbram> m_native;
    std::unique_ptr<vdp1nx::NxFbram> m_surface;
    std::unique_ptr<vdp1nx::ReferenceNx> m_ref;
    uint32_t m_scale = 0;
};

// Compares the software frame with the composite (RGB) into result, at the renderer's internal resolution (N > 1:
// `compare` with `lattice`). False (result.error set) when the harness failed.
bool CompareFrame(gpu::VulkanDevice &device, gpu::VulkanVDPRenderer &vulkan, const std::vector<uint32_t> &software,
                  uint32_t width, uint32_t height, uint32_t gpuWidth, uint32_t gpuHeight, uint32_t frame,
                  ScaleCompare compare, const std::vector<SampleLatticeRow> &lattice, SceneResult &result) {
    result.frame = frame;
    if (software.empty() || gpuWidth != width || gpuHeight != height) {
        result.error = "frame " + std::to_string(frame) + " size mismatch: software " + std::to_string(width) + "x" +
                       std::to_string(height) + ", gpu " + std::to_string(gpuWidth) + "x" + std::to_string(gpuHeight);
        return false;
    }
    if (!vulkan.Attached()) {
        result.error = "no device attached after frame " + std::to_string(frame);
        return false;
    }
    const uint32_t n = vulkan.InternalScale();
    if (vulkan.Backend()->Scale() != n) {
        result.error = "frame " + std::to_string(frame) + ": the back half renders at " +
                       std::to_string(vulkan.Backend()->Scale()) + "x, the renderer reports " + std::to_string(n) + "x";
        return false;
    }
    if (n > 1) {
        std::vector<uint32_t> scaled = ReadbackBuffer(device, vulkan.CompositeBuffer(), width * height * n * n);
        return compare == ScaleCompare::Nearest
                   ? CompareNearestPixels(software, width, height, std::move(scaled), n, result)
                   : CompareSamplePointPixels(software, width, height, std::move(scaled), n, lattice, result);
    }
    result.scale = 1;
    result.gpuScaled.clear();
    result.comparedPixels = static_cast<uint64_t>(width) * height;
    result.skippedPixels = 0;
    result.width = width;
    result.height = height;
    result.software = software;
    result.gpu = ReadbackBuffer(device, vulkan.CompositeBuffer(), width * height);
    if (result.gpu.size() != result.software.size()) {
        result.error = "composite readback failed";
        return false;
    }

    std::vector<uint32_t> colors;
    result.diffCount = 0;
    for (size_t i = 0; i < result.software.size(); ++i) {
        const uint32_t sw = result.software[i] & 0xFFFFFF;
        const uint32_t gpu = result.gpu[i] & 0xFFFFFF;
        if (colors.size() < 64 && std::find(colors.begin(), colors.end(), sw) == colors.end()) {
            colors.push_back(sw);
        }
        if (sw != gpu) {
            if (result.diffCount++ == 0) {
                result.diffX = static_cast<uint32_t>(i % width);
                result.diffY = static_cast<uint32_t>(i / width);
                result.softwarePixel = result.software[i];
                result.gpuPixel = result.gpu[i];
            }
        }
    }
    result.distinctColors = static_cast<uint32_t>(colors.size());
    result.identical = result.diffCount == 0;
    return true;
}

} // namespace

SceneResult RenderBoth(const Vdp2Scene &scene, gpu::VulkanDevice &device, const RenderBothOptions &options) {
    SceneResult result;

    struct Capture {
        std::vector<uint32_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
    } capture;

    // Separate states: the front half maintains per-line VDPState fields that the software renderer also maintains.
    auto swState = std::make_unique<VDPState>();
    auto vkState = std::make_unique<VDPState>();
    config::VDP2DebugRender swDebug{}, vkDebug{};
    config::VDP2AccessPatternsConfig access{};
    auto software = std::make_unique<SoftwareVDPRenderer>(*swState, swDebug, access);
    software->SwCallbacks.FrameComplete.Rebind(&capture, [](uint32 *fb, uint32 w, uint32 h, void *context) {
        auto &out = *static_cast<Capture *>(context);
        out.pixels.assign(fb, fb + static_cast<size_t>(w) * h);
        out.width = w;
        out.height = h;
    });
    auto vulkan = std::make_unique<gpu::VulkanVDPRenderer>(*vkState, vkDebug, access);
    uint32_t gpuWidth = 0, gpuHeight = 0;
    vulkan->FrameReady = [&](uint32_t w, uint32_t h) {
        gpuWidth = w;
        gpuHeight = h;
    };
    // BRIMIR_VDP1_MODE=gpu runs every scene with VDP1 on the GPU.
    Vdp1Path vdp1 = options.vdp1;
    if (vdp1 == Vdp1Path::Default) {
        const char *mode = std::getenv("BRIMIR_VDP1_MODE");
        vdp1 = mode != nullptr && std::string(mode) == "gpu" ? Vdp1Path::Gpu : Vdp1Path::Software;
    }
    if (options.fbram != nullptr) {
        vdp1 = Vdp1Path::Gpu;
    }
    vulkan->SetVdp1Mode(vdp1 == Vdp1Path::Gpu ? gpu::Vdp1Mode::Gpu : gpu::Vdp1Mode::Software);
    // VDP1 upscaling: the N x reference follows what the GPU replays
    // (shared with the observer: the renderer reports the surface's end when it is destroyed, after this scope's model)
    std::shared_ptr<NxModel> nxModel;
    if (options.vdp1Upscaling) {
        vulkan->SetVdp1Upscaling(true);
        if (options.fbram != nullptr) {
            nxModel = std::make_shared<NxModel>();
            vulkan->SetVdp1WorkObserverForTest(
                [nxModel](const gpu::VulkanVDPRenderer::Vdp1WorkEvent &e) { nxModel->OnEvent(e); });
        }
    }
    InstallSyncSubmit(*vulkan, device, options.syncCounter);
    vulkan->AttachDevice(device, options.frameContexts);
    if (!vulkan->Attached()) {
        result.error = "AttachDevice failed";
        return result;
    }
    if (vdp1 == Vdp1Path::Gpu && !vulkan->GpuVdp1()) {
        result.error = "VDP1 on the GPU unavailable";
        return result;
    }
    // BRIMIR_VDP2_SCALE=N runs every scene at internal resolution N.
    const uint32_t scale = options.scale != 0 ? options.scale : EnvScale();
    if (!vulkan->SetInternalScale(scale)) {
        result.error = "SetInternalScale(" + std::to_string(scale) + ") failed";
        return result;
    }
    if (options.uploadBlockSize != 0) {
        vulkan->Backend()->ForceUploadBlockSizeForTest(options.uploadBlockSize);
    }
    vulkan->EnableThreadedVDP1(options.threadedVDP1);
    // BRIMIR_VDP2_POISON=1 runs every scene with poisoned layer images (read-before-write check).
    const char *poison = std::getenv("BRIMIR_VDP2_POISON");
    const bool poisonImages = options.poisonImages || (poison && std::string(poison) == "1");

    // Optional GPU timing: two timestamps per submitted frame.
    struct QueryPool {
        const VolkDeviceTable *table = nullptr;
        VkDevice device = VK_NULL_HANDLE;
        VkQueryPool pool = VK_NULL_HANDLE;
        ~QueryPool() {
            if (pool != VK_NULL_HANDLE) {
                table->vkDestroyQueryPool(device, pool, nullptr);
            }
        }
    } timestamps;
    double timestampPeriod = 0;
    if (options.gpuMilliseconds != nullptr) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device.Physical(), &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device.Physical(), &familyCount, families.data());
        if (device.QueueFamily() < familyCount && families[device.QueueFamily()].timestampValidBits != 0) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = 2;
            timestamps.table = &device.Table();
            timestamps.device = device.Device();
            if (device.Table().vkCreateQueryPool(device.Device(), &info, nullptr, &timestamps.pool) != VK_SUCCESS) {
                timestamps.pool = VK_NULL_HANDLE;
            }
            timestampPeriod = device.Properties().limits.timestampPeriod;
        }
    }

    Vdp2SceneWriter writer{scene.enhancements};
    writer.Attach(*swState, *software);
    writer.Attach(*vkState, *vulkan);
    // The excluded sample-point lattice of the composite rows, from the lines that last wrote them.
    std::vector<SampleLatticeRow> lattice;
    writer.lineObserver = [&](const Vdp2SceneWriter &w, const VDPState &state, uint32_t y) {
        if (lattice.size() != w.VRes()) {
            lattice.assign(w.VRes(), SampleLatticeRow{});
        }
        LatticeForLine(w, state, y, scene.enhancements.deinterlace, [&](uint32_t row, SampleLatticeRow flags) {
            if (row < lattice.size()) {
                lattice[row] = flags;
            }
        });
    };
    // FBRAM checkpoints (RenderBothFBRAM): the pending VDP1 ops in an ops-only submit, then the GPU FBRAM read back and
    // compared with software's.
    PolledSubmitter submitter(device); // the frames' and checkpoints' submits
    FbramResult *fbram = options.fbram;
    bool fbramFailed = false;
    // One host buffer for every checkpoint's readback (checkpoints can come after every op of a scene)
    constexpr uint32_t kSlice = kVDP1FBRAMSize;
    struct HostBuffer {
        gpu::VulkanDevice &device;
        gpu::vk_util::Buffer buffer;
        ~HostBuffer() {
            gpu::vk_util::DestroyBuffer(device, buffer);
        }
    } fbramHost{device, {}}, nxHost{device, {}};
    uint32_t nxCheckpoint = 0;
    uint32_t currentFrame = 0;
    bool frameRecordedByCheckpoint = false;
    const auto frameContext = [&] {
        return options.contextForFrame ? options.contextForFrame(currentFrame)
                                       : currentFrame % std::max(options.frameContexts, 1u);
    };
    const auto compareFbram = [&](const std::string &label) {
        if (fbram == nullptr || fbramFailed) {
            return;
        }
        // Host-cached memory where there is some (the CPU reads all of it at every checkpoint)
        const auto createHost = [&](VkMemoryPropertyFlags extra) {
            return gpu::vk_util::CreateBuffer(device, VkDeviceSize{8} * kSlice, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | extra,
                                              fbramHost.buffer) == VK_SUCCESS;
        };
        if (fbramHost.buffer.buffer == VK_NULL_HANDLE && !createHost(VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
            !createHost(0)) {
            fbram->error = label + ": no FBRAM readback buffer";
            fbramFailed = true;
            return;
        }
        // VDP1 upscaling: the N x surface too (every nxCompareEvery-th checkpoint and at each frame's end)
        const uint32_t nxScale = nxModel ? vulkan->Vdp1UpscaleScale() : 0;
        const bool frameEnd = label.rfind("frame ", 0) == 0;
        const bool nxDue = nxScale > 1 && (frameEnd || nxCheckpoint++ % std::max(options.nxCompareEvery, 1u) == 0);
        const VkDeviceSize nxBytes = nxDue ? gpu::vdp1::Vdp1NxFbramBufferSize(nxScale) : 0;
        if (nxDue && nxHost.buffer.size != nxBytes) {
            gpu::vk_util::DestroyBuffer(device, nxHost.buffer);
            const auto create = [&](VkMemoryPropertyFlags extra) {
                return gpu::vk_util::CreateBuffer(device, nxBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | extra,
                                                  nxHost.buffer) == VK_SUCCESS;
            };
            if (!create(VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && !create(0)) {
                fbram->error = label + ": no N x surface readback buffer";
                fbramFailed = true;
                return;
            }
        }
        // The pending VDP1 ops (an ops-only submit's work) and the readback in one submit. In blanking (after
        // VDP2EndFrame, the frame not recorded yet) the ops after the frame's end can only follow the frame: the frame
        // is recorded here then, as a sync submit in blanking records it (FramePolicy::Sync), and the frame loop
        // compares it.
        const bool pending = vulkan->HasPendingVdp1Work();
        const bool withFrame = pending && vulkan->HasPendingFrame();
        const uint32_t context = withFrame ? frameContext() : 0;
        bool recorded = !pending;
        const VkResult submitted = submitter.Submit([&](VkCommandBuffer cmd) {
            if (pending) {
                recorded = withFrame ? vulkan->RecordPendingFrame(context, cmd) : vulkan->RecordPendingVdp1Work(0, cmd);
            }
            if (!recorded) {
                return;
            }
            gpu::vk_util::GlobalBarrier(device, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            const VkBufferCopy region{0, 0, VkDeviceSize{8} * kSlice};
            device.Table().vkCmdCopyBuffer(cmd, vulkan->Backend()->SpriteFBBuffer(), fbramHost.buffer.buffer, 1,
                                           &region);
            if (nxDue) {
                const VkBufferCopy nxRegion{0, 0, nxBytes};
                device.Table().vkCmdCopyBuffer(cmd, vulkan->Vdp1Backend()->NxSurfaceBuffer(), nxHost.buffer.buffer, 1,
                                               &nxRegion);
            }
            gpu::vk_util::GlobalBarrier(device, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                        VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        });
        if (submitted != VK_SUCCESS || !recorded) {
            fbram->error = label + (pending ? ": the ops-only submit failed" : ": FBRAM readback failed");
            fbramFailed = true;
            return;
        }
        if (pending) {
            vulkan->FrameSubmitted(context, VK_NULL_HANDLE); // completed (Submit waits)
            frameRecordedByCheckpoint |= withFrame;
        }
        const auto *gpuBytes = static_cast<const uint8_t *>(fbramHost.buffer.mapped);
        const std::array<const SpriteFB *, 8> sw{&swState->mem1.FBRAM[0],         &swState->mem1.FBRAM[1],
                                                 &software->GetAltFBRAM(0),        &software->GetAltFBRAM(1),
                                                 &software->GetMeshFBRAM(false, 0), &software->GetMeshFBRAM(false, 1),
                                                 &software->GetMeshFBRAM(true, 0),  &software->GetMeshFBRAM(true, 1)};
        const uint32_t sizeH = swState->regs1.tvmr.fbSizeH;
        const uint32_t rowBytes = swState->regs1.tvmr.pixel8Bits ? sizeH : sizeH * 2;
        uint64_t nonZero = 0;
        for (uint32_t slice = 0; slice < 8; ++slice) {
            const uint8_t *g = gpuBytes + size_t{slice} * kSlice;
            const SpriteFB &s = *sw[slice];
            // Non-zero bytes, eight at a time (bit 7 of each byte of t is set when that byte is non-zero)
            for (uint32_t i = 0; i < kSlice; i += 8) {
                uint64_t v;
                std::memcpy(&v, s.data() + i, 8);
                const uint64_t t = (((v & 0x7F7F7F7F7F7F7F7Full) + 0x7F7F7F7F7F7F7F7Full) | v) & 0x8080808080808080ull;
                nonZero += static_cast<uint64_t>(std::popcount(t));
            }
            if (std::memcmp(s.data(), g, kSlice) == 0) {
                continue;
            }
            for (uint32_t i = 0; i < kSlice; ++i) {
                if (s[i] != g[i] && fbram->count++ == 0) {
                    fbram->where = label;
                    fbram->slice = slice;
                    fbram->address = i;
                    fbram->y = rowBytes != 0 ? i / rowBytes : 0;
                    fbram->x = rowBytes != 0 ? (i % rowBytes) / (swState->regs1.tvmr.pixel8Bits ? 1 : 2) : 0;
                    fbram->software = s[i];
                    fbram->gpu = g[i];
                }
            }
        }
        ++fbram->checkpoints;
        fbram->maxNonZero = std::max(fbram->maxNonZero, nonZero);
        if (!nxDue) {
            return;
        }
        // The N x surface against the reference (which has seen every submitted op list and upload), and the
        // reference's native FBRAM against the GPU's
        ++fbram->nxCompares;
        fbram->nxScales |= 1u << nxScale;
        if (nxModel->Scale() != nxScale) {
            fbram->error = label + ": the N x reference is at " + std::to_string(nxModel->Scale()) + "x, the renderer at " +
                           std::to_string(nxScale) + "x";
            fbramFailed = true;
            return;
        }
        for (uint32_t slice = 0; slice < 8; ++slice) {
            if (std::memcmp(nxModel->Slice(slice).data(), gpuBytes + size_t{slice} * kSlice, kSlice) != 0) {
                if (fbram->nxNativeCount++ == 0) {
                    fbram->nxNativeWhere = label + ", slice " + std::to_string(slice);
                }
                break;
            }
        }
        const auto *nxBytesGpu = static_cast<const uint8_t *>(nxHost.buffer.mapped);
        const vdp1nx::NxFbram &want = nxModel->Surface();
        const uint32_t sub = nxScale * nxScale;
        uint64_t detail = 0;
        for (uint32_t slice = 0; slice < 8; ++slice) {
            const std::vector<uint16> &words = want.slices[slice];
            const uint8_t *g = nxBytesGpu + gpu::vdp1::Vdp1NxFbramByteAddress(slice, 0, nxScale);
            for (size_t k = 0; k < words.size(); k += 4) {
                // Four words at a time (the GPU's big-endian halves swapped), word by word only where they differ
                uint64_t g4 = 0, w4 = 0;
                std::memcpy(&g4, g + 2 * k, 8);
                std::memcpy(&w4, words.data() + k, 8);
                if (((g4 & 0x00FF00FF00FF00FFull) << 8 | ((g4 >> 8) & 0x00FF00FF00FF00FFull)) == w4) {
                    continue;
                }
                for (size_t j = k; j < k + 4; ++j) {
                    const uint16 got = static_cast<uint16>((g[2 * j] << 8) | g[2 * j + 1]);
                    if (got != words[j] && fbram->nxCount++ == 0) {
                        const uint32_t w = static_cast<uint32_t>(j / sub);
                        std::ostringstream s;
                        s << label << ", slice " << slice << " native word 0x" << std::hex << w << std::dec << " (x "
                          << w % 512 << ", y " << w / 512 << " at 512) sub " << j % sub << ": GPU 0x" << std::hex
                          << got << ", reference 0x" << words[j];
                        fbram->nxWhere = s.str();
                    }
                }
            }
            if (!frameEnd || slice >= 2) {
                continue; // detail (non-vacuity): main slices at frame ends
            }
            for (size_t b = 0; b < words.size(); b += sub) {
                for (uint32_t i = 1; i < sub; ++i) {
                    if (words[b + i] != words[b]) {
                        ++detail;
                        break;
                    }
                }
            }
        }
        fbram->nxDetailBlocks = std::max(fbram->nxDetailBlocks, detail);
        if (options.nxInspect) {
            options.nxInspect(label, nxScale, std::span<const uint8_t>(nxBytesGpu, static_cast<size_t>(nxBytes)),
                              std::span<const uint8_t>(gpuBytes, size_t{8} * kSlice));
        }
    };
    uint32_t checkpointIndex = 0;
    writer.checkpoint = [&](Vdp2SceneWriter &) { compareFbram("checkpoint " + std::to_string(checkpointIndex++)); };

    if (scene.setup) {
        scene.setup(writer);
    }
    for (uint32_t frame = 0; frame < scene.frames; ++frame) {
        currentFrame = frame;
        frameRecordedByCheckpoint = false;
        if (options.beforeFrame) {
            options.beforeFrame(frame, *vulkan, device);
        }
        // Optionally a save and load of the Vulkan renderer between two lines: the VDP state and the renderer section
        // are saved, a blank renderer section is loaded (the fetcher state is lost), then the saved state is loaded.
        const auto onLine = [&](Vdp2SceneWriter &w, uint32_t y) {
            if (scene.onLine) {
                scene.onLine(w, y);
            }
            if (frame == options.reloadFrame && y == options.reloadLine) {
                auto saved = std::make_unique<ymir::savestate::VDPSaveState>();
                vkState->SaveState(*saved);
                vulkan->PreSaveStateSync();
                vulkan->SaveState(saved->renderer);
                auto blank = std::make_unique<ymir::savestate::VDPSaveState::VDPRendererSaveState>();
                vulkan->LoadState(*blank);
                vkState->LoadState(*saved);
                vulkan->LoadState(saved->renderer);
                vulkan->PostLoadStateSync();
            }
        };
        writer.RunFrame(onLine, 0, UINT32_MAX, scene.blanking);
        // A VDP1 sync submit in blanking (a CPU read or a save after VDP2EndFrame) may have recorded the frame.
        const bool recordedBySync = vulkan->TakeFrameRecordedBySync() || frameRecordedByCheckpoint;
        if (frame + 1 == scene.frames && !vulkan->HasPendingFrame() && !recordedBySync) {
            result.error = "the last frame has no recorded GPU frame";
            return result;
        }
        if (fbramFailed) {
            result.error = "FBRAM: " + fbram->error;
            return result;
        }
        if (vulkan->HasPendingFrame() || vulkan->HasPendingVdp1Work()) {
            const bool hasFrame = vulkan->HasPendingFrame();
            vulkan->Backend()->SetPoisonImagesForTest(poisonImages); // the back half may be new (re-attached)
            bool recorded = false;
            const uint32_t context = options.contextForFrame ? options.contextForFrame(frame)
                                                             : frame % std::max(options.frameContexts, 1u);
            // The frame (with the VDP1 ops around it), or the VDP1 ops alone (no VDP2 frame ended: the ops-only submit)
            const auto record = [&](VkCommandBuffer cmd) {
                recorded = hasFrame ? vulkan->RecordPendingFrame(context, cmd)
                                    : vulkan->RecordPendingVdp1Work(context, cmd);
            };
            if (frame == options.dropFrame) {
                if (!RecordWithoutSubmit(device, record) || !recorded) {
                    result.error = "frame " + std::to_string(frame) + ": recording failed";
                    return result;
                }
                vulkan->RecordedFrameDropped();
                continue;
            }
            const VkResult submit = submitter.Submit([&](VkCommandBuffer cmd) {
                if (timestamps.pool != VK_NULL_HANDLE) {
                    device.Table().vkCmdResetQueryPool(cmd, timestamps.pool, 0, 2);
                    device.Table().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps.pool, 0);
                }
                record(cmd);
                if (timestamps.pool != VK_NULL_HANDLE) {
                    device.Table().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps.pool, 1);
                }
            });
            if (submit != VK_SUCCESS || !recorded) {
                result.error = "frame " + std::to_string(frame) + ": recording or submission failed";
                return result;
            }
            vulkan->FrameSubmitted(context, VK_NULL_HANDLE); // completed (Submit waits)
            if (hasFrame && vulkan->Backend()->LastRecordStatsForTest().scale != vulkan->InternalScale()) {
                result.error = "frame " + std::to_string(frame) + " recorded at " +
                               std::to_string(vulkan->Backend()->LastRecordStatsForTest().scale) + "x, expected " +
                               std::to_string(vulkan->InternalScale()) + "x";
                return result;
            }
            if (timestamps.pool != VK_NULL_HANDLE) {
                uint64_t ticks[2] = {};
                if (device.Table().vkGetQueryPoolResults(device.Device(), timestamps.pool, 0, 2, sizeof(ticks), ticks,
                                          sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                    VK_SUCCESS) {
                    options.gpuMilliseconds->push_back(static_cast<double>(ticks[1] - ticks[0]) * timestampPeriod *
                                                       1e-6);
                }
            }
            if (options.afterFrame) {
                options.afterFrame(frame, *vulkan, device);
            }
        }
        compareFbram("frame " + std::to_string(frame));
        if (fbramFailed) {
            result.error = "FBRAM: " + fbram->error;
            return result;
        }
        if (options.compareFrame && options.compareFrame(frame)) {
            if (!CompareFrame(device, *vulkan, capture.pixels, capture.width, capture.height, gpuWidth, gpuHeight,
                              frame, options.scaleCompare, lattice, result)) {
                return result;
            }
            ++result.comparedFrames;
            if (!result.identical) {
                return result; // the first differing frame
            }
        }
    }

    if (!options.compareFrame) {
        CompareFrame(device, *vulkan, capture.pixels, capture.width, capture.height, gpuWidth, gpuHeight,
                     scene.frames - 1, options.scaleCompare, lattice, result);
        result.comparedFrames = 1;
    }
    return result;
}

uint32_t EnvScale() {
    const char *value = std::getenv("BRIMIR_VDP2_SCALE");
    if (value == nullptr) {
        return 1;
    }
    const int n = std::atoi(value);
    return n >= 1 && n <= 4 ? static_cast<uint32_t>(n) : 1;
}

namespace {

SceneResult CompareScaled(const Vdp2Scene &scene, gpu::VulkanDevice &device, uint32_t n, RenderBothOptions options,
                          ScaleCompare compare) {
    options.scale = n;
    options.scaleCompare = compare;
    SceneResult result = RenderBoth(scene, device, options);
    if (!result.error.empty()) {
        result.identical = false;
        return result;
    }
    // Not vacuous: a non-trivial software frame, compared at n.
    if (result.distinctColors < 2) {
        result.error = "the software frame has " + std::to_string(result.distinctColors) + " colour(s)";
    } else if (result.scale != n) {
        result.error = "compared at " + std::to_string(result.scale) + "x, not " + std::to_string(n) + "x";
    }
    if (!result.error.empty()) {
        result.identical = false;
    }
    return result;
}

} // namespace

FbramResult RenderBothFBRAM(const Vdp2Scene &scene, gpu::VulkanDevice &device, RenderBothOptions options) {
    FbramResult result;
    options.fbram = &result;
    options.vdp1 = Vdp1Path::Gpu;
    result.frame = RenderBoth(scene, device, options);
    if (result.error.empty() && result.checkpoints == 0) {
        result.error = "no FBRAM checkpoint";
    }
    result.identical = result.error.empty() && result.frame.error.empty() && result.count == 0 &&
                       result.nxCount == 0 && result.nxNativeCount == 0;
    return result;
}

std::string FbramResult::Describe() const {
    static constexpr const char *kSlices[8] = {"main fb0",    "main fb1",    "alt fb0",     "alt fb1",
                                               "mesh[0] fb0", "mesh[0] fb1", "mesh[1] fb0", "mesh[1] fb1"};
    std::ostringstream out;
    out << "frame: " << frame.Describe() << "; FBRAM: ";
    if (!error.empty()) {
        out << "harness error: " << error;
        return out.str();
    }
    out << checkpoints << " checkpoints, up to " << maxNonZero << " non-zero software bytes, ";
    if (count == 0) {
        out << "identical";
    } else {
        out << count << " bytes differ; first at " << where << ", " << kSlices[slice & 7] << " byte 0x" << std::hex
            << address << std::dec << " (x " << x << ", y " << y << "): software 0x" << std::hex
            << static_cast<uint32_t>(software) << ", gpu 0x" << static_cast<uint32_t>(gpu) << std::dec;
    }
    if (nxCompares > 0) {
        out << "; N x surface: " << nxCompares << " compares (scales mask 0x" << std::hex << nxScales << std::dec
            << "), up to " << nxDetailBlocks << " blocks with detail, ";
        if (nxCount == 0 && nxNativeCount == 0) {
            out << "identical to the reference";
        } else {
            if (nxCount != 0) {
                out << nxCount << " words differ from the reference; first at " << nxWhere;
            }
            if (nxNativeCount != 0) {
                out << (nxCount != 0 ? "; " : "") << "the reference's native FBRAM differs from the GPU's at "
                    << nxNativeCount << " compares, first at " << nxNativeWhere;
            }
        }
    }
    return out.str();
}

SceneResult CompareNearest(const Vdp2Scene &scene, gpu::VulkanDevice &device, uint32_t n, RenderBothOptions options) {
    return CompareScaled(scene, device, n, std::move(options), ScaleCompare::Nearest);
}

SceneResult CompareSamplePoints(const Vdp2Scene &scene, gpu::VulkanDevice &device, uint32_t n,
                                RenderBothOptions options) {
    return CompareScaled(scene, device, n, std::move(options), ScaleCompare::SamplePoints);
}

} // namespace brimir::test

#endif // BRIMIR_VULKAN
