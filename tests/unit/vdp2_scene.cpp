#include "vdp2_scene.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <cstdlib>
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

void Vdp2SceneWriter::ResetVDP() {
    // VDP::Reset (vdp.cpp:83-100), soft
    for (Target &t : m_targets) {
        t.state->Reset(false);
        t.renderer->Reset(false);
    }
    m_HRes = kDefaultResH;
    m_VRes = kDefaultResV;
    m_exclusiveMonitor = false;
    UpdateResolution();
    m_restartFrame = true;
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

void Vdp2SceneWriter::RunFrame(const std::function<void(Vdp2SceneWriter &, uint32_t)> &onLine, uint32_t firstLine,
                               uint32_t endLine) {
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
    // Odd rows: RBGs with deinterlacing (both rows are the RBG line), NBGs in single density (both fields show the
    // same line, deinterlaced or woven).
    SampleLatticeRow odd = flags;
    odd.skipRow = (rbg && deinterlace) || (nbg && singleDensity);
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
    if (result.comparedPixels * 4 < static_cast<uint64_t>(width) * height) {
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
    vulkan->AttachDevice(device, options.frameContexts);
    if (!vulkan->Attached()) {
        result.error = "AttachDevice failed";
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
    if (scene.setup) {
        scene.setup(writer);
    }
    for (uint32_t frame = 0; frame < scene.frames; ++frame) {
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
        writer.RunFrame(onLine);
        if (frame + 1 == scene.frames && !vulkan->HasPendingFrame()) {
            result.error = "the last frame has no recorded GPU frame";
            return result;
        }
        if (vulkan->HasPendingFrame()) {
            vulkan->Backend()->SetPoisonImagesForTest(poisonImages); // the back half may be new (re-attached)
            bool recorded = false;
            const uint32_t context = options.contextForFrame ? options.contextForFrame(frame)
                                                             : frame % std::max(options.frameContexts, 1u);
            if (frame == options.dropFrame) {
                if (!RecordWithoutSubmit(device, [&](VkCommandBuffer cmd) {
                        recorded = vulkan->RecordPendingFrame(context, cmd);
                    }) || !recorded) {
                    result.error = "frame " + std::to_string(frame) + ": recording failed";
                    return result;
                }
                vulkan->RecordedFrameDropped();
                continue;
            }
            const VkResult submit = gpu::vk_util::SubmitOneShot(device, [&](VkCommandBuffer cmd) {
                if (timestamps.pool != VK_NULL_HANDLE) {
                    device.Table().vkCmdResetQueryPool(cmd, timestamps.pool, 0, 2);
                    device.Table().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps.pool, 0);
                }
                recorded = vulkan->RecordPendingFrame(context, cmd);
                if (timestamps.pool != VK_NULL_HANDLE) {
                    device.Table().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps.pool, 1);
                }
            });
            if (submit != VK_SUCCESS || !recorded) {
                result.error = "frame " + std::to_string(frame) + ": recording or submission failed";
                return result;
            }
            if (vulkan->Backend()->LastRecordStatsForTest().scale != vulkan->InternalScale()) {
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

SceneResult CompareNearest(const Vdp2Scene &scene, gpu::VulkanDevice &device, uint32_t n, RenderBothOptions options) {
    return CompareScaled(scene, device, n, std::move(options), ScaleCompare::Nearest);
}

SceneResult CompareSamplePoints(const Vdp2Scene &scene, gpu::VulkanDevice &device, uint32_t n,
                                RenderBothOptions options) {
    return CompareScaled(scene, device, n, std::move(options), ScaleCompare::SamplePoints);
}

} // namespace brimir::test

#endif // BRIMIR_VULKAN
