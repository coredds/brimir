#include "vdp1_raster_reference.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace brimir::test::vdp1ref {

namespace gpu = brimir::gpu::vdp1;

namespace {

FORCE_INLINE uint8 Read8(const uint8 *vram, uint32 address) {
    return vram[address & kVRAMMask];
}

FORCE_INLINE uint16 Read16(const uint8 *vram, uint32 address) {
    address &= kVRAMMask & ~1u;
    return static_cast<uint16>((vram[address] << 8) | vram[address + 1]);
}

FORCE_INLINE uint16 ReadBE16(const SpriteFB &fb, uint32 address) {
    return static_cast<uint16>((fb[address] << 8) | fb[address + 1]);
}

FORCE_INLINE void WriteBE16(SpriteFB &fb, uint32 address, uint16 value) {
    fb[address] = static_cast<uint8>(value >> 8);
    fb[address + 1] = static_cast<uint8>(value);
}

// Clip rectangles with the doubleV doubling applied (VDP1IsPixelSystemClipped/VDP1IsPixelUserClipped)
struct ClipRect {
    sint32 sysH, sysV;
    sint32 userX0, userY0, userX1, userY1;
};

FORCE_INLINE ClipRect EffectiveClip(const ClipContext &c, uint32 doubleV) {
    const sint32 dV = static_cast<sint32>(doubleV);
    return {
        .sysH = c.sysClipH,
        .sysV = (c.sysClipV << dV) | dV,
        .userX0 = c.userClipX0,
        .userY0 = (c.userClipY0 << dV) | dV,
        .userX1 = c.userClipX1,
        .userY1 = (c.userClipY1 << dV) | dV,
    };
}

FORCE_INLINE bool SystemClipped(const ClipRect &clip, CoordS32 c) {
    return c.x() < 0 || c.x() > clip.sysH || c.y() < 0 || c.y() > clip.sysV;
}

FORCE_INLINE bool UserClipped(const ClipRect &clip, CoordS32 c) {
    return c.x() < clip.userX0 || c.x() > clip.userX1 || c.y() < clip.userY0 || c.y() > clip.userY1;
}

// VDP1IsPixelClipped
FORCE_INLINE bool PixelClipped(const ClipRect &clip, CoordS32 c, bool userClippingEnable, bool clippingMode) {
    if (SystemClipped(clip, c)) {
        return true;
    }
    return userClippingEnable && UserClipped(clip, c) != clippingMode;
}

// VDP1IsLineSystemClipped
bool LineSystemClipped(const ClipRect &clip, CoordS32 a, CoordS32 b) {
    return (a.x() < 0 && b.x() < 0) || (a.y() < 0 && b.y() < 0) || (a.x() > clip.sysH && b.x() > clip.sysH) ||
           (a.y() > clip.sysV && b.y() > clip.sysV);
}

} // namespace

// =====================================================================================================================
// Part 1: decoding

CommandParams DecodeCommand(const gpu::Vdp1CommandParams &c) {
    CommandParams p;
    p.address = c.cmdAddress;
    p.control.u16 = static_cast<uint16>((c.drawFlags >> 16) & gpu::kCmdctrlMask);
    p.mode.u16 = static_cast<uint16>(c.drawFlags & gpu::kCmdpmodMask);
    p.textured = gpu::Vdp1CmdTextured(c.drawFlags);
    p.antiAlias = gpu::Vdp1CmdAntiAlias(c.drawFlags);
    p.color = static_cast<uint16>(c.color);
    p.colorBank = c.colorBank;
    p.charAddr = c.charAddr;
    p.charSizeH = gpu::Vdp1Lo16(c.charSize);
    p.charSizeV = gpu::Vdp1Hi16(c.charSize);
    p.clip.sysClipH = static_cast<uint16>(gpu::Vdp1Lo16(c.sysClip));
    p.clip.sysClipV = static_cast<uint16>(gpu::Vdp1Hi16(c.sysClip));
    p.clip.userClipX0 = static_cast<uint16>(gpu::Vdp1Lo16(c.userClip0));
    p.clip.userClipY0 = static_cast<uint16>(gpu::Vdp1Hi16(c.userClip0));
    p.clip.userClipX1 = static_cast<uint16>(gpu::Vdp1Lo16(c.userClip1));
    p.clip.userClipY1 = static_cast<uint16>(gpu::Vdp1Hi16(c.userClip1));
    p.clip.localX = 0;
    p.clip.localY = 0;
    for (uint32 i = 0; i < 4; ++i) {
        p.vertices[i] = CoordS32{static_cast<sint32>(c.vertex[2 * i]), static_cast<sint32>(c.vertex[2 * i + 1])};
    }
    for (uint32 i = 0; i < 4; ++i) {
        const uint32 word = c.gouraud[i >> 1];
        p.gouraud[i].u16 = static_cast<uint16>((i & 1) ? gpu::Vdp1Hi16(word) : gpu::Vdp1Lo16(word));
    }
    return p;
}

RasterState DecodeRasterState(uint32 f, uint32 drawFB) {
    return {
        .fbSizeH = gpu::Vdp1RasFbSizeH(f),
        .pixel8Bits = gpu::Vdp1RasPixel8(f),
        .dblInterlaceEnable = gpu::Vdp1RasDblInterlace(f),
        .dblInterlaceDrawLine = gpu::Vdp1RasDblInterlaceDrawLine(f),
        .evenOddCoordSelect = gpu::Vdp1RasEvenOddCoordSelect(f),
        .doubleDensity = gpu::Vdp1RasDoubleDensity(f),
        .deinterlace = gpu::Vdp1RasDeinterlace(f),
        .transparentMeshes = gpu::Vdp1RasTransparentMeshes(f),
        .doubleV = gpu::Vdp1RasDoubleV(f),
        .drawFB = static_cast<uint8>(drawFB & 1),
    };
}

Span DecodeSpan(const gpu::Vdp1Span &s) {
    Span span;
    span.cmdIndex = s.cmdIndex;
    span.quadStep = gpu::Vdp1SpanQuadStep(s.stepTexV);
    span.a = CoordS32{static_cast<sint32>(s.x0), static_cast<sint32>(s.y0)};
    span.b = CoordS32{static_cast<sint32>(s.x1), static_cast<sint32>(s.y1)};
    span.texV = gpu::Vdp1SpanTexV(s.stepTexV);
    span.gouraudL.u16 = static_cast<uint16>(gpu::Vdp1Lo16(s.gouraudLR));
    span.gouraudR.u16 = static_cast<uint16>(gpu::Vdp1Hi16(s.gouraudLR));
    span.fragBase = s.fragBase;
    return span;
}

// =====================================================================================================================
// Part 2: span rasterization

uint32 SpanSlots(const RasterState &state, const CommandParams &cmd, const Span &span) {
    const ClipRect clip = EffectiveClip(cmd.clip, state.doubleV);
    if (LineSystemClipped(clip, span.a, span.b)) {
        return 0;
    }
    port::Line line{span.a, span.b, cmd.antiAlias};
    line.SystemClip(clip.sysH, clip.sysV);
    return 2 * line.length;
}

void RasterSpan(const RasterState &state, const CommandParams &cmd, const Span &span, const uint8 *vram,
                const RasterOptions &options, std::vector<Fragment> &out, SpanInfo *info) {
    const ClipRect clip = EffectiveClip(cmd.clip, state.doubleV);
    if (LineSystemClipped(clip, span.a, span.b)) {
        return;
    }
    const VDP1Command::DrawMode mode = cmd.mode;

    port::Line line{span.a, span.b, cmd.antiAlias};
    const uint32 skipSteps = line.SystemClip(clip.sysH, clip.sysV);

    port::Gouraud gouraud;
    if (mode.gouraudEnable) {
        gouraud.Setup(line.dmaj + 1, span.gouraudL, span.gouraudR);
        gouraud.Skip(skipSteps);
    }

    const bool meshToMeshFB = state.transparentMeshes && mode.meshEnable;
    const uint32 targetMode = (static_cast<uint32>(mode.colorCalcBits) << kTargetCalcShift) |
                              (meshToMeshFB ? kTargetMeshBit : 0u) | (mode.msbOn ? kTargetMsbOnBit : 0u) |
                              (state.pixel8Bits ? kTargetPixel8Bit : 0u) |
                              (state.transparentMeshes ? kTargetTransparentMeshesBit : 0u);

    // VDP1PlotPixel up to the write; returns its result (false only when clipped)
    auto plot = [&](CoordS32 coord, uint32 step, uint32 isAA, uint16 color) -> bool {
        if (PixelClipped(clip, coord, mode.userClippingEnable, mode.clippingMode)) {
            return false;
        }
        const sint32 x = coord.x();
        sint32 y = coord.y();
        if (!state.transparentMeshes && mode.meshEnable && ((x ^ y) & 1)) {
            return true;
        }
        const bool altFB = state.deinterlace && state.doubleDensity && (y & 1);
        if (state.doubleDensity && !state.deinterlace && state.dblInterlaceEnable &&
            (y & 1) != static_cast<sint32>(state.dblInterlaceDrawLine)) {
            return true;
        }
        if ((state.deinterlace && state.doubleDensity) || state.dblInterlaceEnable) {
            y >>= 1;
        }
        uint32 fbOffset = static_cast<uint32>(y) * state.fbSizeH + static_cast<uint32>(x);
        if (!state.pixel8Bits) {
            fbOffset *= 2u;
        }
        fbOffset &= 0x3FFFF;

        Fragment f;
        f.key = span.fragBase + 2u * step + isAA;
        f.target = (fbOffset >> 1) | (altFB ? kTargetAltBit : 0u) | ((fbOffset & 1) ? kTargetOddBit : 0u) | targetMode;
        if (mode.msbOn) {
            f.color = 0;
        } else if (state.pixel8Bits) {
            f.color = color & 0xFFu;
        } else if (mode.gouraudEnable) {
            f.color = gouraud.Blend(Color555{.u16 = color}).u16;
        } else {
            f.color = color;
        }
        out.push_back(f);
        return true;
    };

    if (!cmd.textured) {
        // VDP1PlotLine
        bool aa = false;
        bool plotted = false;
        for (line.Step(); line.CanStep(); aa = line.Step()) {
            const uint32 step = line.step - 1;
            bool plottedPixel = plot(line.Coord(), step, 0, cmd.color);
            if (aa) {
                plottedPixel |= plot(line.AACoord(), step, 1, cmd.color);
            }
            if (plottedPixel) {
                plotted = true;
            } else if (plotted && !mode.clippingMode && options.earlyBreak) {
                break;
            }
            if (mode.gouraudEnable) {
                gouraud.Step();
            }
        }
        return;
    }

    // VDP1PlotTexturedLine
    const uint32 charSizeH = std::max<uint32>(cmd.charSizeH, 1u);
    const uint32 v = span.texV;
    sint32 uStart = 0;
    sint32 uEnd = static_cast<sint32>(charSizeH) - 1;
    if (cmd.control.flipH) {
        std::swap(uStart, uEnd);
    }
    const bool useHighSpeedShrink = mode.highSpeedShrink && line.dmaj < charSizeH - 1;
    if (info != nullptr) {
        info->highSpeedShrink = useHighSpeedShrink;
    }

    port::Texture uStepper;
    uStepper.Setup(line.dmaj + 1, uStart, uEnd, useHighSpeedShrink, state.evenOddCoordSelect ? 1 : 0);
    uStepper.SkipPixels(skipSteps);

    uint16 color = 0;
    bool transparent = true;
    bool hasEndCode = false;
    sint32 endCodeCount = useHighSpeedShrink ? std::numeric_limits<sint32>::min() : 0;

    auto readTexel = [&] {
        const uint32 u = uStepper.Value();
        const uint32 charIndex = u + v * charSizeH;
        auto processEndCode = [&](bool endCode) {
            if (endCode && !mode.endCodeDisable) {
                hasEndCode = true;
                ++endCodeCount;
            } else {
                hasEndCode = false;
            }
        };
        switch (mode.colorMode) {
        case 0:
            color = (Read8(vram, cmd.charAddr + (charIndex >> 1)) >> ((~u & 1) * 4)) & 0xF;
            processEndCode(color == 0xF);
            transparent = color == 0x0;
            color |= static_cast<uint16>(cmd.colorBank);
            break;
        case 1:
            color = (Read8(vram, cmd.charAddr + (charIndex >> 1)) >> ((~u & 1) * 4)) & 0xF;
            processEndCode(color == 0xF);
            transparent = color == 0x0;
            color = Read16(vram, color * 2u + cmd.colorBank);
            break;
        case 2:
            color = Read8(vram, cmd.charAddr + charIndex);
            processEndCode(color == 0xFF);
            transparent = color == 0x00;
            color = static_cast<uint16>((color & 0x3F) | cmd.colorBank);
            break;
        case 3:
            color = Read8(vram, cmd.charAddr + charIndex);
            processEndCode(color == 0xFF);
            transparent = color == 0x00;
            color = static_cast<uint16>((color & 0x7F) | cmd.colorBank);
            break;
        case 4:
            color = Read8(vram, cmd.charAddr + charIndex);
            processEndCode(color == 0xFF);
            transparent = color == 0x00;
            color = static_cast<uint16>(color | cmd.colorBank);
            break;
        case 5:
            color = Read16(vram, cmd.charAddr + charIndex * 2u);
            processEndCode(color == 0x7FFF);
            transparent = (color & 0x8000) == 0;
            break;
        default: break; // colour modes 6 and 7 keep the previous texel (software's switch has no case)
        }
    };

    readTexel();

    bool aa = false;
    bool plotted = false;
    for (line.Step(); line.CanStep(); aa = line.Step()) {
        const uint32 step = line.step - 1;
        while (uStepper.ShouldStepTexel()) {
            uStepper.StepTexel();
            readTexel();
            if (endCodeCount == 2) {
                break;
            }
        }
        if (endCodeCount == 2) {
            if (info != nullptr) {
                info->endCodeTerminated = true;
            }
            break;
        }
        uStepper.StepPixel();

        if (hasEndCode || (transparent && !mode.transparentPixelDisable)) {
            if (mode.gouraudEnable) {
                gouraud.Step();
            }
            if (!mode.clippingMode) {
                if (!PixelClipped(clip, line.Coord(), mode.userClippingEnable, mode.clippingMode)) {
                    plotted = true;
                    continue;
                }
                if (aa && !PixelClipped(clip, line.AACoord(), mode.userClippingEnable, mode.clippingMode)) {
                    plotted = true;
                    continue;
                }
                if (plotted && options.earlyBreak) {
                    break;
                }
            }
            continue;
        }

        bool plottedPixel = plot(line.Coord(), step, 0, color);
        if (aa) {
            plottedPixel |= plot(line.AACoord(), step, 1, color);
        }
        if (plottedPixel) {
            plotted = true;
        } else if (plotted && !mode.clippingMode && options.earlyBreak) {
            break;
        }
        if (mode.gouraudEnable) {
            gouraud.Step();
        }
    }
}

// =====================================================================================================================
// Part 3: per-word ordered resolve

ResolveStats Resolve(uint32 drawFB, std::vector<Fragment> fragments, Fbram &fbram, ResolveOrder order) {
    // The GPU counts fragments per unit, scans, scatters fragment indices into per-unit buckets in arbitrary order and
    // sorts each bucket by key. Here: bucket by unit (a stable radix sort), then sort each bucket by key.
    const bool descending = order == ResolveOrder::DescendingForTesting;
    auto keyOrder = [&](const Fragment &lhs, const Fragment &rhs) {
        return descending ? lhs.key > rhs.key : lhs.key < rhs.key;
    };
    auto unitOf = [](const Fragment &f) { return f.target & kTargetUnitMask; };
    if (fragments.size() < 2048) {
        std::sort(fragments.begin(), fragments.end(), [&](const Fragment &lhs, const Fragment &rhs) {
            return unitOf(lhs) != unitOf(rhs) ? unitOf(lhs) < unitOf(rhs) : keyOrder(lhs, rhs);
        });
    } else {
        std::vector<Fragment> tmp(fragments.size());
        for (const uint32 shift : {0u, 9u}) {
            std::array<uint32, 513> start{};
            for (const Fragment &f : fragments) {
                ++start[((unitOf(f) >> shift) & 511) + 1];
            }
            for (uint32 i = 1; i < start.size(); ++i) {
                start[i] += start[i - 1];
            }
            for (const Fragment &f : fragments) {
                tmp[start[(unitOf(f) >> shift) & 511]++] = f;
            }
            fragments.swap(tmp);
        }
        for (size_t begin = 0; begin < fragments.size();) {
            size_t end = begin + 1;
            while (end < fragments.size() && unitOf(fragments[end]) == unitOf(fragments[begin])) {
                ++end;
            }
            if (!std::is_sorted(fragments.begin() + begin, fragments.begin() + end, keyOrder)) {
                std::sort(fragments.begin() + begin, fragments.begin() + end, keyOrder);
            }
            begin = end;
        }
    }

    ResolveStats stats;
    stats.fragments = fragments.size();
    const uint8 fb = drawFB & 1;
    for (size_t begin = 0; begin < fragments.size();) {
        const uint32 unit = fragments[begin].target & kTargetUnitMask;
        size_t end = begin + 1;
        while (end < fragments.size() && (fragments[end].target & kTargetUnitMask) == unit) {
            if (fragments[end].key == fragments[end - 1].key) {
                throw std::logic_error("vdp1ref::Resolve: duplicate fragment key");
            }
            ++end;
        }

        const bool alt = (unit & kTargetAltBit) != 0;
        SpriteFB &mainFB = alt ? fbram.alt[fb] : fbram.main[fb];
        SpriteFB &meshFB = fbram.mesh[alt ? 1 : 0][fb];
        const uint32 address = (unit & kTargetWordMask) * 2u;
        uint16 m = ReadBE16(mainFB, address);
        uint16 s = ReadBE16(meshFB, address);

        for (size_t i = begin; i < end; ++i) {
            const Fragment &f = fragments[i];
            if (f.target & kTargetMsbOnBit) {
                m |= 0x8000;
                continue;
            }
            const bool toMesh = (f.target & kTargetMeshBit) != 0;
            const bool transparentMeshes = (f.target & kTargetTransparentMeshesBit) != 0;
            if (f.target & kTargetPixel8Bit) {
                const uint32 shift = (f.target & kTargetOddBit) ? 0 : 8;
                const uint16 keep = static_cast<uint16>(~(0xFFu << shift));
                const uint16 byte = static_cast<uint16>((f.color & 0xFF) << shift);
                if (toMesh) {
                    s = (s & keep) | byte;
                } else {
                    m = (m & keep) | byte;
                    if (transparentMeshes) {
                        s &= keep;
                    }
                }
                continue;
            }

            const Color555 src{.u16 = static_cast<uint16>(f.color)};
            Color555 dst{.u16 = m};
            switch ((f.target >> kTargetCalcShift) & 3) {
            case 0: dst = src; break;
            case 1:
                if (dst.msb) {
                    dst.r >>= 1u;
                    dst.g >>= 1u;
                    dst.b >>= 1u;
                }
                break;
            case 2:
                dst.r = src.r >> 1u;
                dst.g = src.g >> 1u;
                dst.b = src.b >> 1u;
                dst.msb = src.msb;
                break;
            case 3:
                if (dst.msb) {
                    dst.r = (src.r + dst.r) >> 1u;
                    dst.g = (src.g + dst.g) >> 1u;
                    dst.b = (src.b + dst.b) >> 1u;
                } else {
                    dst = src;
                }
                break;
            }
            if (toMesh) {
                s = dst.u16;
            } else {
                m = dst.u16;
                if (transparentMeshes) {
                    s = 0;
                }
            }
        }

        WriteBE16(mainFB, address, m);
        WriteBE16(meshFB, address, s);
        ++stats.units;
        stats.maxPerUnit = std::max(stats.maxPerUnit, static_cast<uint32>(end - begin));
        begin = end;
    }
    return stats;
}

// =====================================================================================================================
// Part 4: erase and CPU framebuffer writes

uint64 Erase(const gpu::Vdp1ErasePushConstants &params, Fbram &fbram) {
    const bool mirror = gpu::Vdp1EraseMirrorAlt(params.flags);
    const bool clearMesh = gpu::Vdp1EraseClearMesh(params.flags);
    const uint8 fb = static_cast<uint8>(gpu::Vdp1EraseFb(params.flags));
    const uint32 width = params.x3 > params.x1 ? params.x3 - params.x1 : 0;
    const uint16 value = static_cast<uint16>(params.value);
    uint64 writes = 0;

    // One write per (x, y) of the region; write i (row-major) happens if i < writeLimit. Every write stores the same
    // value, so overlapping rows (offset shift 8 with x >= 256) need no order.
    for (uint32 y = params.y1; y <= params.y3; y++) {
        for (uint32 x = params.x1; x < params.x3; x++) {
            const uint64 i = static_cast<uint64>(y - params.y1) * width + (x - params.x1);
            if (i >= params.writeLimit) {
                continue;
            }
            const uint32 address = (((y << params.offsetShift) + x) * 2u) & 0x3FFFE;
            WriteBE16(fbram.main[fb], address, value);
            ++writes;
            if (mirror) {
                WriteBE16(fbram.alt[fb], address, value);
            }
            if (clearMesh) {
                WriteBE16(fbram.mesh[0][fb], address, 0);
                if (mirror) {
                    WriteBE16(fbram.mesh[1][fb], address, 0);
                }
            }
        }
    }
    return writes;
}

void ApplyFbWrites(std::span<const gpu::Vdp1FbWrite> writes, Fbram &fbram) {
    for (const gpu::Vdp1FbWrite &w : writes) {
        const uint32 address = gpu::Vdp1FbWriteWord(w.address) * 2u;
        const uint32 fb = gpu::Vdp1FbWriteFb(w.address);
        const uint16 mask = static_cast<uint16>(w.mask);
        auto apply = [&](SpriteFB &target) {
            WriteBE16(target, address, static_cast<uint16>((ReadBE16(target, address) & ~mask) | (w.value & mask)));
        };
        apply(fbram.main[fb]);
        if (gpu::Vdp1FbWriteMirrorAlt(w.address)) {
            apply(fbram.alt[fb]);
        }
    }
}

// =====================================================================================================================
// Driver

const char *DrawnCoverage::Name(uint32 counter) {
    static constexpr const char *kNames[Count] = {
        "command 0 (normal sprite)", "command 1 (scaled sprite)", "command 2 (distorted sprite)",
        "command 3 (distorted sprite alt)", "command 4 (polygon)", "command 5 (polylines)", "command 6 (line)",
        "command 7 (polylines alt)", "colour mode 0", "colour mode 1", "colour mode 2", "colour mode 3",
        "colour mode 4", "colour mode 5", "colour mode 6", "colour mode 7", "replace", "shadow", "half-luminance",
        "half-transparency", "Gouraud", "mesh", "alt field", "MSB-on", "8 bpp even byte", "8 bpp odd byte",
        "double interlace (DIE)", "DIE in double density", "doubleV", "HSS lines", "end-code-terminated lines",
        "erase writes", "mixed-mode batches"};
    return counter < Count ? kNames[counter] : "?";
}

ReferenceVDP1::ReferenceVDP1(Fbram &fbram, RasterOptions options, uint64 shuffleSeed)
    : m_fbram(fbram)
    , m_options(options)
    , m_shuffle(shuffleSeed)
    , m_vram(ymir::vdp::kVDP1VRAMSize, 0) {}

void ReferenceVDP1::Replay(const gpu::Vdp1OpList &ops) {
    ++m_stats.opLists;
    // The segments partition the ops, in order
    uint32 next = 0;
    for (const gpu::Vdp1Segment &segment : ops.segments) {
        if (segment.firstOp != next) {
            throw std::logic_error("vdp1ref::Replay: segments do not partition the ops");
        }
        next += segment.opCount;
    }
    if (next != ops.ops.size() || ops.segments.empty()) {
        throw std::logic_error("vdp1ref::Replay: segments do not cover the ops");
    }

    for (const gpu::Vdp1Op &op : ops.ops) {
        switch (op.kind) {
        case gpu::Vdp1OpKind::Draw: DrawBatch(ops, ops.batches.at(op.index)); break;
        case gpu::Vdp1OpKind::Erase:
            ++m_stats.erases;
            m_stats.drawn[DrawnCoverage::EraseWrites] += vdp1ref::Erase(ops.erases.at(op.index), m_fbram);
            break;
        case gpu::Vdp1OpKind::FbWrites: {
            const gpu::Vdp1FbWritePushConstants &run = ops.fbWriteRuns.at(op.index);
            if (run.count == 0 || static_cast<size_t>(run.offset) + run.count > ops.fbWrites.size()) {
                throw std::logic_error("vdp1ref::Replay: bad framebuffer write run");
            }
            ApplyFbWrites(std::span(ops.fbWrites).subspan(run.offset, run.count), m_fbram);
            m_stats.fbWrites += run.count;
            break;
        }
        }
    }
}

void ReferenceVDP1::DrawBatch(const gpu::Vdp1OpList &ops, const gpu::Vdp1DrawBatch &batch) {
    // VRAM uploads first
    for (uint32 i = 0; i < batch.copyCount; ++i) {
        const gpu::Vdp1VramCopy &copy = ops.vramCopies.at(batch.firstCopy + i);
        if (static_cast<size_t>(copy.vramOffset) + copy.size > m_vram.size() ||
            static_cast<size_t>(copy.arenaOffset) + copy.size > ops.arena.size()) {
            throw std::logic_error("vdp1ref::DrawBatch: VRAM copy out of range");
        }
        std::copy_n(ops.arena.begin() + copy.arenaOffset, copy.size, m_vram.begin() + copy.vramOffset);
        m_stats.vramUploadBytes += copy.size;
    }
    if (batch.spanCount == 0 || batch.cmdCount == 0 ||
        static_cast<size_t>(batch.spanOffset) + batch.spanCount > ops.spans.size() ||
        static_cast<size_t>(batch.cmdOffset) + batch.cmdCount > ops.commands.size()) {
        throw std::logic_error("vdp1ref::DrawBatch: bad batch");
    }

    std::vector<CommandParams> commands(batch.cmdCount);
    std::vector<RasterState> states(batch.cmdCount);
    for (uint32 i = 0; i < batch.cmdCount; ++i) {
        const gpu::Vdp1CommandParams &c = ops.commands[batch.cmdOffset + i];
        commands[i] = DecodeCommand(c);
        states[i] = DecodeRasterState(c.rasterFlags, batch.drawFB);
    }
    m_stats.commands += batch.cmdCount;

    std::vector<Fragment> fragments;
    fragments.reserve(batch.fragmentCount);
    uint32 modes = 0; // bit 0/1: 16/8 bpp fragments, bit 2/3: transparent meshes off/on
    for (uint32 s = 0; s < batch.spanCount; ++s) {
        const Span span = DecodeSpan(ops.spans[batch.spanOffset + s]);
        if (span.cmdIndex >= batch.cmdCount) {
            throw std::logic_error("vdp1ref::DrawBatch: span command index out of range");
        }
        const CommandParams &cmd = commands[span.cmdIndex];
        const RasterState &state = states[span.cmdIndex];
        const uint32 end = s + 1 < batch.spanCount ? ops.spans[batch.spanOffset + s + 1].fragBase : batch.fragmentCount;
        if ((s == 0 && span.fragBase != 0) || end <= span.fragBase) {
            throw std::logic_error("vdp1ref::DrawBatch: fragment bases are not a prefix sum");
        }
        const uint32 slots = end - span.fragBase;
        if (slots != SpanSlots(state, cmd, span)) {
            throw std::logic_error("vdp1ref::DrawBatch: span slots differ from SpanSlots");
        }
        const size_t first = fragments.size();
        SpanInfo info;
        RasterSpan(state, cmd, span, m_vram.data(), m_options, fragments, &info);
        DrawnCoverage &d = m_stats.drawn;
        for (size_t i = first; i < fragments.size(); ++i) {
            const Fragment &f = fragments[i];
            if (f.key < span.fragBase || f.key >= span.fragBase + slots) {
                throw std::logic_error("vdp1ref::DrawBatch: fragment key outside its span's slots");
            }
            using C = DrawnCoverage;
            ++d[C::CommandType0 + (static_cast<uint32>(cmd.control.command) & 7)];
            d[C::ColorMode0 + cmd.mode.colorMode] += cmd.textured;
            d[C::Mesh] += (f.target & kTargetMeshBit) != 0;
            d[C::Alt] += (f.target & kTargetAltBit) != 0;
            d[C::DoubleInterlace] += state.dblInterlaceEnable;
            d[C::DieDoubleDensity] += state.dblInterlaceEnable && state.doubleDensity && !state.deinterlace;
            d[C::DoubleV] += state.doubleV != 0;
            if (f.target & kTargetMsbOnBit) {
                ++d[C::MsbOn];
            } else if (f.target & kTargetPixel8Bit) {
                ++d[(f.target & kTargetOddBit) ? C::ByteOdd : C::ByteEven];
            } else {
                ++d[C::ColorCalc0 + ((f.target >> kTargetCalcShift) & 3)];
                d[C::Gouraud] += cmd.mode.gouraudEnable;
            }
            modes |= (f.target & kTargetPixel8Bit) ? 2u : 1u;
            modes |= (f.target & kTargetTransparentMeshesBit) ? 8u : 4u;
        }
        if (fragments.size() > first) {
            d[DrawnCoverage::HssLines] += info.highSpeedShrink;
            d[DrawnCoverage::EndCodeLines] += info.endCodeTerminated;
        }
        m_stats.slots += slots;
    }
    m_stats.drawn[DrawnCoverage::MixedModeBatches] += (modes & 3) == 3 || (modes & 12) == 12;
    if (m_shuffle != 0) {
        // the GPU scatters fragments into their buckets in arbitrary order
        for (size_t i = fragments.size(); i > 1; --i) {
            std::swap(fragments[i - 1], fragments[SplitMix64(m_shuffle) % i]);
        }
    }
    m_stats.spans += batch.spanCount;
    m_stats.fragments += fragments.size();
    ++m_stats.batches;
    const ResolveStats rs = Resolve(batch.drawFB, std::move(fragments), m_fbram, m_options.resolveOrder);
    m_stats.maxPerUnit = std::max(m_stats.maxPerUnit, rs.maxPerUnit);
}

} // namespace brimir::test::vdp1ref
