#include "gpu/vdp1/vdp1_front.hpp"

#include <ymir/hw/vdp/renderer/common/vdp1_clip.hpp>
#include <ymir/hw/vdp/renderer/common/vdp1_steppers.hpp>
#include <ymir/hw/vdp/renderer/vdp1_probe.hpp>
#include <ymir/util/bit_ops.hpp>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <utility>

namespace brimir::gpu::vdp1 {

using namespace ymir;
using namespace ymir::vdp;

namespace {

uint32_t AsU(sint32 v) {
    return static_cast<uint32_t>(v);
}

} // namespace

void Vdp1OpList::Clear() {
    ops.clear();
    segments.clear();
    batches.clear();
    commands.clear();
    spans.clear();
    vramCopies.clear();
    arena.clear();
    erases.clear();
    fbWrites.clear();
    fbWriteRuns.clear();
}

Vdp1FrontEnd::Vdp1FrontEnd(VDPState &state)
    : m_state(state)
    , m_vram(kVRAMSize) {
    ResyncVRAM();
}

void Vdp1FrontEnd::SetEnhancements(const config::Enhancements &enhancements) {
    m_enhancements = enhancements;
}

void Vdp1FrontEnd::SetBatchLimitsForTesting(const Vdp1BatchLimits &limits) {
    m_limits.fragments = std::clamp(limits.fragments, 1u, kMaxFragmentsPerBatch);
    m_limits.spans = std::clamp(limits.spans, 1u, kMaxSpansPerBatch);
    m_limits.commands = std::clamp(limits.commands, 1u, kMaxCommandsPerBatch);
    m_limits.fbWrites = std::clamp(limits.fbWrites, 1u, kMaxFbWritesPerBatch);
}

void Vdp1FrontEnd::Reset([[maybe_unused]] bool hard) {
    CloseAll(Vdp1BatchEnd::Take);
    ResyncVRAM();
}

void Vdp1FrontEnd::PostLoadStateSync() {
    CloseAll(Vdp1BatchEnd::Take);
    ResyncVRAM();
}

// ---------------------------------------------------------------------------------------------------------------------
// Frame, erase and memory calls

// sw:487-500 (Brimir: the emulator thread's registers)
void Vdp1FrontEnd::BeginFrame() {
    const VDP1Regs &regs1 = m_state.regs1;
    const VDP2Regs &regs2 = m_state.regs2;
    m_doubleV = m_enhancements.deinterlace && regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity &&
                !regs1.dblInterlaceEnable;
}

// sw:453-470 and VDP1DoEraseFramebuffer (sw:959-1022): the region, clamps and flags as software computes them; the
// pass writes (y1 <= y <= y3) x (x1 <= x < x3) in row-major order, write i when i < writeLimit
void Vdp1FrontEnd::Erase(uint64_t cycles, uint32_t vres) {
    CloseAll(Vdp1BatchEnd::OpKind);

    const VDP1Regs &regs1 = m_state.regs1;
    const VDP2Regs &regs2 = m_state.regs2;
    const bool doubleDensity = regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity;
    const uint32 scaleV = doubleDensity ? 1 : 0;
    const uint32 maxH = (regs2.TVMD.HRESOn & 1) ? 428 : 400;
    const uint32 maxV = vres >> scaleV;

    Vdp1ErasePushConstants e{};
    e.x1 = std::min<uint32>(regs1.eraseX1Latch, maxH);
    e.x3 = std::min<uint32>(regs1.eraseX3Latch, maxH);
    e.y1 = std::min<uint32>(regs1.eraseY1Latch, maxV) << scaleV;
    e.y3 = std::min<uint32>(regs1.eraseY3Latch, maxV) << scaleV;
    e.value = regs1.eraseWriteValueLatch;
    e.offsetShift = regs1.tvmr.eraseOffsetShift;
    // With a budget, software writes a pixel and then stops when fewer than one cycle is left: cycles + 1 writes
    e.writeLimit = cycles == 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(std::min<uint64_t>(cycles, 0xFFFFFFFEu) + 1u);
    e.flags = Vdp1MakeEraseFlags(m_state.fbIndex.display, m_enhancements.deinterlace && doubleDensity,
                                 m_enhancements.transparentMeshes);

    m_ops.ops.push_back({Vdp1OpKind::Erase, static_cast<uint32_t>(m_ops.erases.size())});
    m_ops.erases.push_back(e);
    ++m_stats.erases;
}

// sw:364-374: VDPState wrote mem1.FBRAM[draw]; software mirrors to the alternate field and leaves the meshes alone
void Vdp1FrontEnd::WriteFB(uint32_t address, uint16_t value, uint16_t byteMask) {
    if (m_batchOpen) {
        CloseBatch(Vdp1BatchEnd::OpKind);
    }
    if (m_fbRunOpen && m_ops.fbWriteRuns.back().count >= m_limits.fbWrites) {
        CloseFbRun();
    }
    if (!m_fbRunOpen) {
        m_ops.ops.push_back({Vdp1OpKind::FbWrites, static_cast<uint32_t>(m_ops.fbWriteRuns.size())});
        m_ops.fbWriteRuns.push_back({static_cast<uint32_t>(m_ops.fbWrites.size()), 0});
        m_fbRunOpen = true;
    }
    const bool mirrorAlt = m_enhancements.deinterlace && m_state.regs2.TVMD.IsInterlaced();
    const uint32_t word = (address & 0x3FFFF) >> 1;
    m_ops.fbWrites.push_back({Vdp1MakeFbWriteAddress(word, m_state.fbIndex.draw, mirrorAlt),
                              static_cast<uint32_t>(value & byteMask), byteMask, 0});
    ++m_ops.fbWriteRuns.back().count;
    ++m_stats.fbWrites;
}

void Vdp1FrontEnd::WriteVRAM(uint32_t address) {
    const uint32_t word = address & kVRAMMask & ~1u;
    if (m_batchOpen && m_hazardTracking) {
        const uint32_t granule = word >> kHazardShift;
        if ((m_hazard[granule >> 6] >> (granule & 63)) & 1u) {
            CloseBatch(Vdp1BatchEnd::VramHazard);
        }
    }
    m_vram[word] = m_state.mem1.VRAM[word];
    m_vram[word + 1] = m_state.mem1.VRAM[word + 1];
    const uint32_t chunk = word >> kUploadChunkShift;
    m_vramDirty[chunk >> 6] |= uint64_t{1} << (chunk & 63);
}

void Vdp1FrontEnd::MarkFrameBoundary() {
    CloseAll(Vdp1BatchEnd::FrameBoundary);
    // the frame's batch counts
    m_stats.lastFrameBatchEnds = m_stats.frameBatchEnds;
    uint64_t frameBatches = 0;
    for (const uint64_t n : m_stats.frameBatchEnds) {
        frameBatches += n;
    }
    m_stats.maxFrameBatches = std::max(m_stats.maxFrameBatches, frameBatches);
    m_stats.frameBatchEnds.fill(0);
    ++m_stats.frames;
    const uint32_t end = static_cast<uint32_t>(m_ops.ops.size());
    m_ops.segments.push_back({m_segmentStart, end - m_segmentStart});
    m_segmentStart = end;
}

Vdp1OpList Vdp1FrontEnd::TakeOps() {
    CloseAll(Vdp1BatchEnd::Take);
    const uint32_t end = static_cast<uint32_t>(m_ops.ops.size());
    m_ops.segments.push_back({m_segmentStart, end - m_segmentStart});
    Vdp1OpList out = std::move(m_ops);
    m_ops = Vdp1OpList{};
    // The next list is likely as large: no regrowth while recording it
    m_ops.spans.reserve(out.spans.size());
    m_ops.commands.reserve(out.commands.size());
    m_ops.ops.reserve(out.ops.size());
    m_segmentStart = 0;
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Batching

void Vdp1FrontEnd::CloseAll(Vdp1BatchEnd end) {
    if (m_batchOpen) {
        CloseBatch(end);
    }
    CloseFbRun();
}

void Vdp1FrontEnd::CloseFbRun() {
    m_fbRunOpen = false;
}

void Vdp1FrontEnd::OpenBatch() {
    CloseFbRun();
    m_batch = {};
    m_batch.cmdOffset = static_cast<uint32_t>(m_ops.commands.size());
    m_batch.spanOffset = static_cast<uint32_t>(m_ops.spans.size());
    m_batch.drawFB = m_state.fbIndex.draw & 1u;
    m_batchOpen = true;
    m_cmdInBatch = false;
}

void Vdp1FrontEnd::CloseBatch(Vdp1BatchEnd end) {
    assert(m_batchOpen);
    m_batch.end = end;
    UploadDirtyVRAM(m_batch);
    m_ops.ops.push_back({Vdp1OpKind::Draw, static_cast<uint32_t>(m_ops.batches.size())});
    m_ops.batches.push_back(m_batch);
    m_hazard.fill(0);
    m_batchOpen = false;
    m_cmdInBatch = false;
    ++m_stats.batches;
    ++m_stats.batchEnds[static_cast<size_t>(end)];
    ++m_stats.frameBatchEnds[static_cast<size_t>(end)];
    BRIMIR_VDP1_PROBE_BATCH(static_cast<uint32_t>(end));
}

void Vdp1FrontEnd::MarkHazards(uint32_t address, uint32_t size) {
    if (size >= kVRAMSize) {
        m_hazard.fill(~uint64_t{0});
        return;
    }
    address &= kVRAMMask;
    while (size > 0) {
        const uint32_t part = std::min(size, kVRAMSize - address);
        uint32_t first = address >> kHazardShift;
        const uint32_t last = (address + part - 1) >> kHazardShift;
        while (first <= last) {
            const uint32_t bit = first & 63;
            const uint32_t count = std::min(64u - bit, last - first + 1);
            const uint64_t mask = count == 64 ? ~uint64_t{0} : ((uint64_t{1} << count) - 1) << bit;
            m_hazard[first >> 6] |= mask;
            first += count;
        }
        size -= part;
        address = 0;
    }
}

void Vdp1FrontEnd::UploadDirtyVRAM(Vdp1DrawBatch &batch) {
    batch.firstCopy = static_cast<uint32_t>(m_ops.vramCopies.size());
    constexpr uint32_t kChunkSize = 1u << kUploadChunkShift;
    constexpr uint32_t kChunks = kVRAMSize >> kUploadChunkShift;
    uint32_t chunk = 0;
    while (chunk < kChunks) {
        const uint64_t word = m_vramDirty[chunk >> 6] >> (chunk & 63);
        if (word == 0) {
            chunk = (chunk | 63) + 1;
            continue;
        }
        chunk += static_cast<uint32_t>(std::countr_zero(word));
        uint32_t end = chunk;
        while (end < kChunks && ((m_vramDirty[end >> 6] >> (end & 63)) & 1u)) {
            ++end;
        }
        const uint32_t offset = chunk * kChunkSize;
        const uint32_t size = (end - chunk) * kChunkSize;
        const uint32_t arenaOffset = static_cast<uint32_t>(m_ops.arena.size());
        m_ops.arena.insert(m_ops.arena.end(), m_vram.begin() + offset, m_vram.begin() + offset + size);
        m_ops.vramCopies.push_back({arenaOffset, offset, size});
        m_stats.vramUploadBytes += size;
        chunk = end;
    }
    m_vramDirty.fill(0);
    batch.copyCount = static_cast<uint32_t>(m_ops.vramCopies.size()) - batch.firstCopy;
}

void Vdp1FrontEnd::ResyncVRAM() {
    std::memcpy(m_vram.data(), m_state.mem1.VRAM.data(), kVRAMSize);
    m_vramDirty.fill(~uint64_t{0});
}

// ---------------------------------------------------------------------------------------------------------------------
// Commands

uint16_t Vdp1FrontEnd::ReadVRAM16(uint32_t address) const {
    return m_state.mem1.ReadVRAM<uint16>(address);
}

void Vdp1FrontEnd::AddRead(uint32_t address, uint32_t size) {
    assert(m_cmdReadCount < m_cmdReads.size());
    m_cmdReads[m_cmdReadCount++] = {address & kVRAMMask, size};
}

void Vdp1FrontEnd::BeginDrawCommand(uint32_t cmdAddress, VDP1Command::Control control, uint16_t cmdpmod,
                                    uint16_t color, bool textured, bool antiAlias,
                                    const std::array<CoordS32, 4> &vertices) {
    const VDP1Regs &regs1 = m_state.regs1;
    const auto &ctx = m_state.state1;
    const bool doubleDensity = m_state.regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity;

    m_cmd = {};
    for (size_t i = 0; i < 4; ++i) {
        m_cmd.vertex[2 * i] = AsU(vertices[i].x());
        m_cmd.vertex[2 * i + 1] = AsU(vertices[i].y());
    }
    m_cmd.drawFlags = Vdp1MakeDrawFlags(cmdpmod, control.u16, textured, antiAlias);
    m_cmd.rasterFlags = Vdp1MakeRasterFlags(regs1.tvmr.pixel8Bits, regs1.tvmr.fbSizeH, regs1.dblInterlaceEnable,
                                            regs1.dblInterlaceDrawLine, regs1.evenOddCoordSelect, doubleDensity,
                                            m_enhancements.deinterlace, m_enhancements.transparentMeshes, m_doubleV);
    m_cmd.color = color;
    m_cmd.sysClip = Vdp1Pack16(ctx.sysClipH, ctx.sysClipV);
    m_cmd.userClip0 = Vdp1Pack16(ctx.userClipX0, ctx.userClipY0);
    m_cmd.userClip1 = Vdp1Pack16(ctx.userClipX1, ctx.userClipY1);
    m_cmd.cmdAddress = cmdAddress;

    m_cmdInBatch = false;
    m_cmdHasSpans = false;
    m_cmdReadCount = 0;
    AddRead(cmdAddress, 0x20);

    const sint32 doubleV = static_cast<sint32>(m_doubleV);
    m_clipH = ctx.sysClipH;
    m_clipV = (static_cast<sint32>(ctx.sysClipV) << doubleV) | doubleV;

    if (m_batchOpen && m_batch.drawFB != (m_state.fbIndex.draw & 1u)) {
        CloseBatch(Vdp1BatchEnd::FbSwap);
    }
}

// VDP1PlotLine / VDP1PlotTexturedLine up to the line loop (sw:1233-1240, 1281-1299): the line's slots after SystemClip
void Vdp1FrontEnd::AddSpan(CoordS32 a, CoordS32 b, uint32_t quadStep, uint32_t texV, uint16_t gouraudL,
                           uint16_t gouraudR) {
    if (IsLineSystemClipped(a, b)) {
        return;
    }
    LineStepper<> line{a, b, Vdp1CmdAntiAlias(m_cmd.drawFlags)};
    line.SystemClip(m_clipH, m_clipV);
    const uint32_t slots = 2u * line.Length();
    if (slots == 0) {
        return;
    }
    assert(slots <= kMaxSpanSlots);
    assert(quadStep <= 4095);
    assert(static_cast<int32_t>(texV) >= -1 && static_cast<int32_t>(texV) <= 255);

    if (m_batchOpen && (m_batch.spanCount >= m_limits.spans ||
                        (m_batch.fragmentCount > 0 && m_batch.fragmentCount + slots > m_limits.fragments) ||
                        (!m_cmdInBatch && m_batch.cmdCount >= m_limits.commands))) {
        CloseBatch(Vdp1BatchEnd::Capacity);
    }
    if (!m_batchOpen) {
        OpenBatch();
    }
    if (!m_cmdInBatch) {
        m_ops.commands.push_back(m_cmd);
        ++m_batch.cmdCount;
        m_cmdInBatch = true;
        for (uint32_t i = 0; i < m_cmdReadCount; ++i) {
            MarkHazards(m_cmdReads[i].address, m_cmdReads[i].size);
        }
    }
    m_ops.spans.push_back({AsU(a.x()), AsU(a.y()), AsU(b.x()), AsU(b.y()), m_batch.cmdCount - 1,
                           m_batch.fragmentCount, Vdp1Pack16(gouraudL, gouraudR),
                           Vdp1Pack16(quadStep, texV & 0xFFFFu)});
    ++m_batch.spanCount;
    m_batch.fragmentCount += slots;
    ++m_stats.spans;
    m_stats.fragmentSlots += slots;
    if (!m_cmdHasSpans) {
        m_cmdHasSpans = true;
        ++m_stats.drawCommands;
    }
}

// sw:1069-1088
bool Vdp1FrontEnd::IsLineSystemClipped(CoordS32 coord1, CoordS32 coord2) const {
    auto [x1, y1] = coord1;
    auto [x2, y2] = coord2;
    if (x1 < 0 && x2 < 0) {
        return true;
    }
    if (y1 < 0 && y2 < 0) {
        return true;
    }
    if (x1 > m_clipH && x2 > m_clipH) {
        return true;
    }
    if (y1 > m_clipV && y2 > m_clipV) {
        return true;
    }
    return false;
}

// sw:1090-1114
bool Vdp1FrontEnd::IsQuadSystemClipped(CoordS32 coord1, CoordS32 coord2, CoordS32 coord3, CoordS32 coord4) const {
    auto [x1, y1] = coord1;
    auto [x2, y2] = coord2;
    auto [x3, y3] = coord3;
    auto [x4, y4] = coord4;
    if (x1 < 0 && x2 < 0 && x3 < 0 && x4 < 0) {
        return true;
    }
    if (y1 < 0 && y2 < 0 && y3 < 0 && y4 < 0) {
        return true;
    }
    if (x1 > m_clipH && x2 > m_clipH && x3 > m_clipH && x4 > m_clipH) {
        return true;
    }
    if (y1 > m_clipV && y2 > m_clipV && y3 > m_clipV && y4 > m_clipV) {
        return true;
    }
    return false;
}

// sw:1471-1566 (the line loop's per-line part is AddSpan)
void Vdp1FrontEnd::PlotTexturedQuad(uint32_t cmdAddress, VDP1Command::Control control, VDP1Command::Size size,
                                    CoordS32 coordA, CoordS32 coordB, CoordS32 coordC, CoordS32 coordD) {
    const VDP1Command::DrawMode mode{.u16 = ReadVRAM16(cmdAddress + 0x04)};
    const uint16 color = ReadVRAM16(cmdAddress + 0x06);
    BeginDrawCommand(cmdAddress, control, mode.u16, color, true, true, {coordA, coordB, coordC, coordD});
    if (IsQuadSystemClipped(coordA, coordB, coordC, coordD)) {
        return;
    }

    uint32 charAddr = ReadVRAM16(cmdAddress + 0x08) * 8u;
    const uint32 charSizeH = size.H * 8;
    const uint32 charSizeV = size.V;

    uint32 colorBank = color;
    switch (mode.colorMode) {
    case 0: colorBank &= 0xFFF0; break;
    case 1: colorBank <<= 3u; break;
    case 2: colorBank &= 0xFFC0; break;
    case 3: colorBank &= 0xFF80; break;
    case 4: colorBank &= 0xFF00; break;
    case 5: charAddr &= ~0xFu; break; // VDP1PlotTexturedLine force-aligns it (sw:1290-1293)
    }
    m_cmd.colorBank = colorBank;
    m_cmd.charAddr = charAddr;
    m_cmd.charSize = Vdp1Pack16(charSizeH, charSizeV);

    // GPU reads: the texels (rows -1 to the last; row -1 when CMDSIZE.V is 0 with a non-zero width) and the look-up
    // table (sw:1327-1383). A few bytes of margin for the 4-bpp byte rounding.
    if (mode.colorMode <= 5) {
        const uint32 width = std::max<uint32>(charSizeH, 1u);
        const uint32 rows = charSizeH == 0 ? 1u : std::max<uint32>(charSizeV, 1u);
        const uint32 rowBytes = mode.colorMode <= 1 ? (width + 1) / 2 : mode.colorMode == 5 ? width * 2 : width;
        const bool rowMinusOne = charSizeH != 0 && charSizeV == 0;
        const uint32 before = rowMinusOne ? rowBytes + 2 : 0;
        AddRead(charAddr - before, before + rows * rowBytes + 2);
        if (mode.colorMode == 1) {
            AddRead(colorBank, 32);
        }
    }

    QuadStepper quad{coordA, coordB, coordC, coordD};
    if (mode.gouraudEnable) {
        const uint32 gouraudTable = static_cast<uint32>(ReadVRAM16(cmdAddress + 0x1C)) << 3u;
        const Color555 colorA{.u16 = ReadVRAM16(gouraudTable + 0u)};
        const Color555 colorB{.u16 = ReadVRAM16(gouraudTable + 2u)};
        const Color555 colorC{.u16 = ReadVRAM16(gouraudTable + 4u)};
        const Color555 colorD{.u16 = ReadVRAM16(gouraudTable + 6u)};
        quad.SetupGouraud(colorA, colorB, colorC, colorD);
        m_cmd.gouraud[0] = Vdp1Pack16(colorA.u16, colorB.u16);
        m_cmd.gouraud[1] = Vdp1Pack16(colorC.u16, colorD.u16);
        AddRead(gouraudTable, 8);
    }

    TextureStepper texVStepper;
    quad.SetupTexture(texVStepper, charSizeH == 0 ? 1 : charSizeV, control.flipV);

    const VDP1QuadTermination termination{quad, m_clipH, m_clipV};
    for (uint32_t quadStep = 0; quad.CanStep(); quad.Step(), ++quadStep) {
        if (termination.Done(quad)) {
            break;
        }
        const CoordS32 coordL = quad.LeftEdge().Coord();
        const CoordS32 coordR = quad.RightEdge().Coord();
        while (texVStepper.ShouldStepTexel()) {
            texVStepper.StepTexel();
        }
        texVStepper.StepPixel();
        uint16 gouraudL = 0, gouraudR = 0;
        if (mode.gouraudEnable) {
            gouraudL = quad.LeftEdge().GouraudValue().u16;
            gouraudR = quad.RightEdge().GouraudValue().u16;
        }
        AddSpan(coordL, coordR, quadStep, texVStepper.Value(), gouraudL, gouraudR);
    }
}

// sw:1568-1590
void Vdp1FrontEnd::ExecuteCommand(uint32_t cmdAddress, VDP1Command::Control control) {
    using enum VDP1Command::CommandType;
    ++m_stats.commands;

    switch (control.command) {
    case DrawNormalSprite: CmdDrawNormalSprite(cmdAddress, control); break;
    case DrawScaledSprite: CmdDrawScaledSprite(cmdAddress, control); break;
    case DrawDistortedSprite: [[fallthrough]];
    case DrawDistortedSpriteAlt: CmdDrawDistortedSprite(cmdAddress, control); break;

    case DrawPolygon: CmdDrawPolygon(cmdAddress, control); break;
    case DrawPolylines: [[fallthrough]];
    case DrawPolylinesAlt: CmdDrawPolylines(cmdAddress, control); break;
    case DrawLine: CmdDrawLine(cmdAddress, control); break;

    case UserClipping: [[fallthrough]];
    case UserClippingAlt: CmdSetUserClipping(cmdAddress); break;
    case SystemClipping: CmdSetSystemClipping(cmdAddress); break;
    case SetLocalCoordinates: CmdSetLocalCoordinates(cmdAddress); break;
    }
}

// sw:1592-1621
void Vdp1FrontEnd::CmdDrawNormalSprite(uint32_t cmdAddress, VDP1Command::Control control) {
    if (!m_state.state2.layerEnabled[0]) {
        return;
    }

    const VDP1Command::Size size{.u16 = ReadVRAM16(cmdAddress + 0x0A)};
    const uint32 charSizeH = size.H * 8;
    const uint32 charSizeV = size.V;

    auto &ctx = m_state.state1;
    const sint32 xa = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x0C)) + ctx.localCoordX;
    const sint32 ya = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x0E)) + ctx.localCoordY;

    const sint32 xb = static_cast<sint32>(static_cast<uint32>(xa) + std::max(charSizeH, 1u) - 1u); // right X
    const sint32 yb = static_cast<sint32>(static_cast<uint32>(ya) + std::max(charSizeV, 1u) - 1u); // bottom Y

    const sint32 doubleV = static_cast<sint32>(m_doubleV);
    const sint32 yAdd = m_enhancements.deinterlace ? doubleV : 0;

    const CoordS32 coordA{xa, ya << doubleV};
    const CoordS32 coordB{xb, ya << doubleV};
    const CoordS32 coordC{xb, (yb << doubleV) + yAdd};
    const CoordS32 coordD{xa, (yb << doubleV) + yAdd};

    PlotTexturedQuad(cmdAddress, control, size, coordA, coordB, coordC, coordD);
}

// sw:1623-1721
void Vdp1FrontEnd::CmdDrawScaledSprite(uint32_t cmdAddress, VDP1Command::Control control) {
    if (!m_state.state2.layerEnabled[0]) {
        return;
    }

    const VDP1Command::Size size{.u16 = ReadVRAM16(cmdAddress + 0x0A)};

    auto &ctx = m_state.state1;
    const sint32 xa = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x0C));
    const sint32 ya = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x0E));

    sint32 qxa = xa;
    sint32 qya = ya;
    sint32 qxb = xa;
    sint32 qyb = ya;
    sint32 qxc = xa;
    sint32 qyc = ya;
    sint32 qxd = xa;
    sint32 qyd = ya;

    const uint8 zoomPointH = bit::extract<0, 1>(control.zoomPoint);
    const uint8 zoomPointV = bit::extract<2, 3>(control.zoomPoint);

    if (zoomPointH == 0) {
        const sint32 xc = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x14));
        qxb = xc;
        qxc = xc;
    } else {
        const sint32 xb = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x10));
        switch (zoomPointH) {
        case 1:
            qxb += xb;
            qxc += xb;
            break;
        case 2:
            qxa -= xb >> 1;
            qxb += (xb + 1) >> 1;
            qxc += (xb + 1) >> 1;
            qxd -= xb >> 1;
            break;
        case 3:
            qxa -= xb;
            qxd -= xb;
            break;
        }
    }

    if (zoomPointV == 0) {
        const sint32 yc = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x16));
        qyc = yc;
        qyd = yc;
    } else {
        const sint32 yb = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x12));
        switch (zoomPointV) {
        case 1:
            qyc += yb;
            qyd += yb;
            break;
        case 2:
            qya -= yb >> 1;
            qyb -= yb >> 1;
            qyc += (yb + 1) >> 1;
            qyd += (yb + 1) >> 1;
            break;
        case 3:
            qya -= yb;
            qyb -= yb;
            break;
        }
    }

    qxa += ctx.localCoordX;
    qya += ctx.localCoordY;
    qxb += ctx.localCoordX;
    qyb += ctx.localCoordY;
    qxc += ctx.localCoordX;
    qyc += ctx.localCoordY;
    qxd += ctx.localCoordX;
    qyd += ctx.localCoordY;

    const sint32 doubleV = static_cast<sint32>(m_doubleV);
    const sint32 yAdd = m_enhancements.deinterlace ? doubleV : 0;

    const CoordS32 coordA{qxa, qya << doubleV};
    const CoordS32 coordB{qxb, qyb << doubleV};
    const CoordS32 coordC{qxc, (qyc << doubleV) + yAdd};
    const CoordS32 coordD{qxd, (qyd << doubleV) + yAdd};

    PlotTexturedQuad(cmdAddress, control, size, coordA, coordB, coordC, coordD);
}

namespace {

// The vertices of the distorted sprite, polygon and polyline commands (sw:1731-1750, 1765-1788, 1855-1878)
std::array<CoordS32, 4> QuadVertices(const VDPState &state, uint32_t cmdAddress, uint32_t doubleVBit,
                                      bool deinterlace) {
    const auto &ctx = state.state1;
    auto read = [&](uint32_t offset) { return bit::sign_extend<13>(state.mem1.ReadVRAM<uint16>(cmdAddress + offset)); };
    const sint32 xa = read(0x0C) + ctx.localCoordX;
    const sint32 ya = read(0x0E) + ctx.localCoordY;
    const sint32 xb = read(0x10) + ctx.localCoordX;
    const sint32 yb = read(0x12) + ctx.localCoordY;
    const sint32 xc = read(0x14) + ctx.localCoordX;
    const sint32 yc = read(0x16) + ctx.localCoordY;
    const sint32 xd = read(0x18) + ctx.localCoordX;
    const sint32 yd = read(0x1A) + ctx.localCoordY;

    const bool isRegularRect = (xa == xd) && (xb == xc) && (ya == yb) && (yc == yd);

    const sint32 doubleV = static_cast<sint32>(doubleVBit);
    const sint32 yAddAB = deinterlace && isRegularRect && (ya >= yc) ? doubleV : 0;
    const sint32 yAddCD = deinterlace && isRegularRect && (ya < yc) ? doubleV : 0;

    return {CoordS32{xa, (ya << doubleV) + yAddAB}, CoordS32{xb, (yb << doubleV) + yAddAB},
            CoordS32{xc, (yc << doubleV) + yAddCD}, CoordS32{xd, (yd << doubleV) + yAddCD}};
}

} // namespace

// sw:1723-1757
void Vdp1FrontEnd::CmdDrawDistortedSprite(uint32_t cmdAddress, VDP1Command::Control control) {
    if (!m_state.state2.layerEnabled[0]) {
        return;
    }
    const VDP1Command::Size size{.u16 = ReadVRAM16(cmdAddress + 0x0A)};
    const auto v = QuadVertices(m_state, cmdAddress, m_doubleV, m_enhancements.deinterlace);
    PlotTexturedQuad(cmdAddress, control, size, v[0], v[1], v[2], v[3]);
}

// sw:1759-1847
void Vdp1FrontEnd::CmdDrawPolygon(uint32_t cmdAddress, VDP1Command::Control control) {
    if (!m_state.state2.layerEnabled[0]) {
        return;
    }

    const VDP1Command::DrawMode mode{.u16 = ReadVRAM16(cmdAddress + 0x04)};
    const uint16 color = ReadVRAM16(cmdAddress + 0x06);
    const auto v = QuadVertices(m_state, cmdAddress, m_doubleV, m_enhancements.deinterlace);
    const uint32 gouraudTable = static_cast<uint32>(ReadVRAM16(cmdAddress + 0x1C)) << 3u;

    BeginDrawCommand(cmdAddress, control, mode.u16, color, false, true, v);
    if (IsQuadSystemClipped(v[0], v[1], v[2], v[3])) {
        return;
    }

    QuadStepper quad{v[0], v[1], v[2], v[3]};
    if (mode.gouraudEnable) {
        const Color555 colorA{.u16 = ReadVRAM16(gouraudTable + 0u)};
        const Color555 colorB{.u16 = ReadVRAM16(gouraudTable + 2u)};
        const Color555 colorC{.u16 = ReadVRAM16(gouraudTable + 4u)};
        const Color555 colorD{.u16 = ReadVRAM16(gouraudTable + 6u)};
        quad.SetupGouraud(colorA, colorB, colorC, colorD);
        m_cmd.gouraud[0] = Vdp1Pack16(colorA.u16, colorB.u16);
        m_cmd.gouraud[1] = Vdp1Pack16(colorC.u16, colorD.u16);
        AddRead(gouraudTable, 8);
    }

    const VDP1QuadTermination termination{quad, m_clipH, m_clipV};
    for (uint32_t quadStep = 0; quad.CanStep(); quad.Step(), ++quadStep) {
        if (termination.Done(quad)) {
            break;
        }
        const CoordS32 coordL = quad.LeftEdge().Coord();
        const CoordS32 coordR = quad.RightEdge().Coord();
        uint16 gouraudL = 0, gouraudR = 0;
        if (mode.gouraudEnable) {
            gouraudL = quad.LeftEdge().GouraudValue().u16;
            gouraudR = quad.RightEdge().GouraudValue().u16;
        }
        AddSpan(coordL, coordR, quadStep, 0, gouraudL, gouraudR);
    }
}

// sw:1849-1926
void Vdp1FrontEnd::CmdDrawPolylines(uint32_t cmdAddress, VDP1Command::Control control) {
    if (!m_state.state2.layerEnabled[0]) {
        return;
    }

    const VDP1Command::DrawMode mode{.u16 = ReadVRAM16(cmdAddress + 0x04)};
    const uint16 color = ReadVRAM16(cmdAddress + 0x06);
    const auto v = QuadVertices(m_state, cmdAddress, m_doubleV, m_enhancements.deinterlace);
    const uint32 gouraudTable = static_cast<uint32>(ReadVRAM16(cmdAddress + 0x1C)) << 3u;

    BeginDrawCommand(cmdAddress, control, mode.u16, color, false, false, v);
    if (IsQuadSystemClipped(v[0], v[1], v[2], v[3])) {
        return;
    }

    const std::array<uint16, 4> gouraud{ReadVRAM16(gouraudTable + 0u), ReadVRAM16(gouraudTable + 2u),
                                        ReadVRAM16(gouraudTable + 4u), ReadVRAM16(gouraudTable + 6u)};
    if (mode.gouraudEnable) {
        m_cmd.gouraud[0] = Vdp1Pack16(gouraud[0], gouraud[1]);
        m_cmd.gouraud[1] = Vdp1Pack16(gouraud[2], gouraud[3]);
        AddRead(gouraudTable, 8);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t j = (i + 1) & 3;
        AddSpan(v[i], v[j], i, 0, mode.gouraudEnable ? gouraud[i] : 0, mode.gouraudEnable ? gouraud[j] : 0);
    }
}

// sw:1928-1977
void Vdp1FrontEnd::CmdDrawLine(uint32_t cmdAddress, VDP1Command::Control control) {
    if (!m_state.state2.layerEnabled[0]) {
        return;
    }

    auto &ctx = m_state.state1;
    const VDP1Command::DrawMode mode{.u16 = ReadVRAM16(cmdAddress + 0x04)};
    const uint16 color = ReadVRAM16(cmdAddress + 0x06);
    const sint32 xa = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x0C)) + ctx.localCoordX;
    const sint32 ya = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x0E)) + ctx.localCoordY;
    const sint32 xb = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x10)) + ctx.localCoordX;
    const sint32 yb = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x12)) + ctx.localCoordY;
    const uint32 gouraudTable = static_cast<uint32>(ReadVRAM16(cmdAddress + 0x1C)) << 3u;

    const sint32 doubleV = static_cast<sint32>(m_doubleV);
    const CoordS32 coordA{xa, ya << doubleV};
    const CoordS32 coordB{xb, yb << doubleV};

    BeginDrawCommand(cmdAddress, control, mode.u16, color, false, false, {coordA, coordB, CoordS32{}, CoordS32{}});
    if (IsLineSystemClipped(coordA, coordB)) {
        return;
    }

    uint16 gouraudL = 0, gouraudR = 0;
    if (mode.gouraudEnable) {
        gouraudL = ReadVRAM16(gouraudTable + 0u);
        gouraudR = ReadVRAM16(gouraudTable + 2u);
        m_cmd.gouraud[0] = Vdp1Pack16(gouraudL, gouraudR);
        AddRead(gouraudTable, 4);
    }
    AddSpan(coordA, coordB, 0, 0, gouraudL, gouraudR);
}

// sw:1979-1984
void Vdp1FrontEnd::CmdSetSystemClipping(uint32_t cmdAddress) {
    auto &ctx = m_state.state1;
    ctx.sysClipH = ReadVRAM16(cmdAddress + 0x14);
    ctx.sysClipV = ReadVRAM16(cmdAddress + 0x16);
}

// sw:1986-1994
void Vdp1FrontEnd::CmdSetUserClipping(uint32_t cmdAddress) {
    auto &ctx = m_state.state1;
    ctx.userClipX0 = ReadVRAM16(cmdAddress + 0x0C);
    ctx.userClipY0 = ReadVRAM16(cmdAddress + 0x0E);
    ctx.userClipX1 = ReadVRAM16(cmdAddress + 0x14);
    ctx.userClipY1 = ReadVRAM16(cmdAddress + 0x16);
}

// sw:1996-2002
void Vdp1FrontEnd::CmdSetLocalCoordinates(uint32_t cmdAddress) {
    auto &ctx = m_state.state1;
    ctx.localCoordX = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x0C));
    ctx.localCoordY = bit::sign_extend<13>(ReadVRAM16(cmdAddress + 0x0E));
}

} // namespace brimir::gpu::vdp1
