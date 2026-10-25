#include "vdp2_scene.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
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
        if (fb == t.state->fbIndex.draw) {
            // VDP::VDP1WriteFB (vdp.cpp:400-402)
            t.state->VDP1WriteFB<uint16>(address, value,
                                         [&](uint32 address, uint16 value) { t.renderer->VDP1WriteFB(address, value); });
        } else {
            util::WriteBE<uint16>(&t.state->mem1.FBRAM[fb & 1][address & 0x3FFFE], value);
        }
    }
}

void Vdp2SceneWriter::SetResolution(uint32_t h, uint32_t v, bool exclusive) {
    m_HRes = h;
    m_VRes = v;
    m_exclusiveMonitor = exclusive;
    for (Target &t : m_targets) {
        t.renderer->VDP2SetResolution(h, v, exclusive);
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

void Vdp2SceneWriter::EraseFramebuffer() {
    for (Target &t : m_targets) {
        t.renderer->VDP1EraseFramebuffer(0);
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

void Vdp2SceneWriter::RunFrame(const std::function<void(Vdp2SceneWriter &, uint32_t)> &onLine) {
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

    // Active display (vdp.cpp:812-821); writes for line y land between RenderLine(y-1) and RenderLine(y)
    const uint32_t lines = FieldLines();
    for (uint32_t y = 0; y < lines; ++y) {
        for (Target &t : m_targets) {
            t.state->regs2.VCNT = static_cast<uint16>(y);
        }
        if (onLine) {
            onLine(*this, y);
        }
        for (Target &t : m_targets) {
            t.renderer->VDP2RenderLine(y);
        }
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
}

void Vdp2SceneWriter::Run(const Vdp2Scene &scene) {
    if (scene.setup) {
        scene.setup(*this);
    }
    for (uint32_t frame = 0; frame < scene.frames; ++frame) {
        RunFrame(scene.onLine);
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
    out << width << "x" << height << ", " << distinctColors << " distinct software colours, ";
    if (identical) {
        out << "identical";
    } else {
        out << diffCount << " pixels differ; first at (" << diffX << ", " << diffY << "): software 0x" << std::hex
            << std::setw(6) << std::setfill('0') << (softwarePixel & 0xFFFFFF) << ", gpu 0x" << std::setw(6)
            << (gpuPixel & 0xFFFFFF) << " (XBGR)";
    }
    return out.str();
}

} // namespace brimir::test

#if BRIMIR_VULKAN

    #include <gpu/vdp/vulkan_vdp_renderer.hpp>
    #include <gpu/vk_util.hpp>

namespace brimir::test {

std::vector<uint32_t> ReadbackBuffer(gpu::VulkanDevice &device, VkBuffer buffer, uint32_t words) {
    namespace vu = gpu::vk_util;
    std::vector<uint32_t> out;
    vu::Buffer host;
    const VkDeviceSize bytes = VkDeviceSize{words} * sizeof(uint32_t);
    if (words == 0 || vu::CreateBuffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       host) != VK_SUCCESS) {
        return out;
    }
    const VkResult result = vu::SubmitOneShot(device, [&](VkCommandBuffer cmd) {
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
    vulkan->AttachDevice(device, options.frameContexts);
    if (!vulkan->Attached()) {
        result.error = "AttachDevice failed";
        return result;
    }
    if (options.uploadBlockSize != 0) {
        vulkan->Backend()->ForceUploadBlockSizeForTest(options.uploadBlockSize);
    }
    vulkan->EnableThreadedVDP1(options.threadedVDP1);

    Vdp2SceneWriter writer{scene.enhancements};
    writer.Attach(*swState, *software);
    writer.Attach(*vkState, *vulkan);
    if (scene.setup) {
        scene.setup(writer);
    }
    for (uint32_t frame = 0; frame < scene.frames; ++frame) {
        if (options.beforeFrame) {
            options.beforeFrame(frame, *vulkan, device);
        }
        writer.RunFrame(scene.onLine);
        if (vulkan->HasPendingFrame()) {
            bool recorded = false;
            const uint32_t context = frame % std::max(options.frameContexts, 1u);
            const VkResult submit = gpu::vk_util::SubmitOneShot(
                device, [&](VkCommandBuffer cmd) { recorded = vulkan->RecordPendingFrame(context, cmd); });
            if (submit != VK_SUCCESS || !recorded) {
                result.error = "frame " + std::to_string(frame) + ": recording or submission failed";
                return result;
            }
        }
    }

    if (capture.pixels.empty() || gpuWidth != capture.width || gpuHeight != capture.height) {
        result.error = "frame size mismatch: software " + std::to_string(capture.width) + "x" +
                       std::to_string(capture.height) + ", gpu " + std::to_string(gpuWidth) + "x" +
                       std::to_string(gpuHeight);
        return result;
    }
    if (!vulkan->Attached()) {
        result.error = "no device attached after the last frame";
        return result;
    }
    result.width = capture.width;
    result.height = capture.height;
    result.software = std::move(capture.pixels);
    result.gpu = ReadbackBuffer(device, vulkan->CompositeBuffer(), result.width * result.height);
    if (result.gpu.size() != result.software.size()) {
        result.error = "composite readback failed";
        return result;
    }

    std::vector<uint32_t> colors;
    for (size_t i = 0; i < result.software.size(); ++i) {
        const uint32_t sw = result.software[i] & 0xFFFFFF;
        const uint32_t gpu = result.gpu[i] & 0xFFFFFF;
        if (colors.size() < 64 && std::find(colors.begin(), colors.end(), sw) == colors.end()) {
            colors.push_back(sw);
        }
        if (sw != gpu) {
            if (result.diffCount++ == 0) {
                result.diffX = static_cast<uint32_t>(i % result.width);
                result.diffY = static_cast<uint32_t>(i / result.width);
                result.softwarePixel = result.software[i];
                result.gpuPixel = result.gpu[i];
            }
        }
    }
    result.distinctColors = static_cast<uint32_t>(colors.size());
    result.identical = result.diffCount == 0;
    return result;
}

} // namespace brimir::test

#endif // BRIMIR_VULKAN
