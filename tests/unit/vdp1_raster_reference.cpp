#include "vdp1_raster_reference.hpp"

#include <ymir/hw/vdp/renderer/common/vdp1_clip.hpp>
#include <ymir/hw/vdp/renderer/common/vdp1_steppers.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace brimir::test::vdp1ref {

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

FORCE_INLINE sint32 SignExtend13(uint16 value) {
    return static_cast<sint32>(static_cast<uint32>(value) << 19) >> 19;
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

// VDP1IsQuadSystemClipped
bool QuadSystemClipped(const ClipRect &clip, const std::array<CoordS32, 4> &v) {
    auto all = [&](auto pred) { return pred(v[0]) && pred(v[1]) && pred(v[2]) && pred(v[3]); };
    return all([](CoordS32 c) { return c.x() < 0; }) || all([](CoordS32 c) { return c.y() < 0; }) ||
           all([&](CoordS32 c) { return c.x() > clip.sysH; }) || all([&](CoordS32 c) { return c.y() > clip.sysV; });
}

uint64 SplitMix(uint64 &s) {
    uint64 z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

// =====================================================================================================================
// Part 1: span builder

namespace {

std::array<Color555, 4> ReadGouraudTable(const uint8 *vram, uint32 address) {
    const uint32 table = static_cast<uint32>(Read16(vram, address + 0x1C)) << 3u;
    return {Color555{.u16 = Read16(vram, table + 0u)}, Color555{.u16 = Read16(vram, table + 2u)},
            Color555{.u16 = Read16(vram, table + 4u)}, Color555{.u16 = Read16(vram, table + 6u)}};
}

// VDP1PlotTexturedQuad
void BuildTexturedQuad(const uint8 *vram, CommandSpans &out, const ClipRect &clip, VDP1Command::Size size) {
    CommandParams &p = out.params;
    if (QuadSystemClipped(clip, p.vertices)) {
        return;
    }
    p.textured = true;
    p.antiAlias = true;
    p.mode.u16 = Read16(vram, p.address + 0x04);
    p.color = Read16(vram, p.address + 0x06);
    p.charAddr = static_cast<uint32>(Read16(vram, p.address + 0x08)) * 8u;
    p.charSizeH = size.H * 8u;
    p.charSizeV = size.V;
    p.colorBank = p.color;
    switch (p.mode.colorMode) {
    case 0: p.colorBank &= 0xFFF0; break;
    case 1: p.colorBank <<= 3u; break;
    case 2: p.colorBank &= 0xFFC0; break;
    case 3: p.colorBank &= 0xFF80; break;
    case 4: p.colorBank &= 0xFF00; break;
    case 5: p.charAddr &= ~0xFu; break; // VDP1PlotTexturedLine force-aligns it
    }

    ymir::vdp::QuadStepper<> quad{p.vertices[0], p.vertices[1], p.vertices[2], p.vertices[3]};
    if (p.mode.gouraudEnable) {
        p.gouraud = ReadGouraudTable(vram, p.address);
        quad.SetupGouraud(p.gouraud[0], p.gouraud[1], p.gouraud[2], p.gouraud[3]);
    }
    ymir::vdp::TextureStepper texV;
    quad.SetupTexture(texV, p.charSizeH == 0 ? 1 : p.charSizeV, p.control.flipV);

    const ymir::vdp::VDP1QuadTermination termination{quad, clip.sysH, clip.sysV};
    uint32 quadStep = 0;
    for (; quad.CanStep(); quad.Step(), ++quadStep) {
        if (termination.Done(quad)) {
            break;
        }
        const CoordS32 l = quad.LeftEdge().Coord();
        const CoordS32 r = quad.RightEdge().Coord();
        while (texV.ShouldStepTexel()) {
            texV.StepTexel();
        }
        texV.StepPixel();
        if (LineSystemClipped(clip, l, r)) {
            continue;
        }
        Span span{.quadStep = quadStep, .a = l, .b = r, .texV = texV.Value()};
        if (p.mode.gouraudEnable) {
            span.gouraudL = quad.LeftEdge().GouraudValue();
            span.gouraudR = quad.RightEdge().GouraudValue();
        }
        out.spans.push_back(span);
    }
    out.draws = true;
}

// VDP1Cmd_DrawPolygon's quad loop
void BuildPolygon(const uint8 *vram, CommandSpans &out, const ClipRect &clip) {
    CommandParams &p = out.params;
    p.mode.u16 = Read16(vram, p.address + 0x04);
    p.color = Read16(vram, p.address + 0x06);
    p.antiAlias = true;
    if (QuadSystemClipped(clip, p.vertices)) {
        return;
    }
    ymir::vdp::QuadStepper<> quad{p.vertices[0], p.vertices[1], p.vertices[2], p.vertices[3]};
    if (p.mode.gouraudEnable) {
        p.gouraud = ReadGouraudTable(vram, p.address);
        quad.SetupGouraud(p.gouraud[0], p.gouraud[1], p.gouraud[2], p.gouraud[3]);
    }
    const ymir::vdp::VDP1QuadTermination termination{quad, clip.sysH, clip.sysV};
    uint32 quadStep = 0;
    for (; quad.CanStep(); quad.Step(), ++quadStep) {
        if (termination.Done(quad)) {
            break;
        }
        const CoordS32 l = quad.LeftEdge().Coord();
        const CoordS32 r = quad.RightEdge().Coord();
        if (LineSystemClipped(clip, l, r)) {
            continue;
        }
        Span span{.quadStep = quadStep, .a = l, .b = r};
        if (p.mode.gouraudEnable) {
            span.gouraudL = quad.LeftEdge().GouraudValue();
            span.gouraudR = quad.RightEdge().GouraudValue();
        }
        out.spans.push_back(span);
    }
    out.draws = true;
}

// The four vertices of a quad command after local coordinates and doubleV (polygon, polyline, distorted sprite)
std::array<CoordS32, 4> QuadVertices(const uint8 *vram, uint32 address, const ClipContext &ctx, sint32 doubleV,
                                     bool deinterlace) {
    sint32 x[4], y[4];
    for (uint32 i = 0; i < 4; ++i) {
        x[i] = SignExtend13(Read16(vram, address + 0x0C + i * 4)) + ctx.localX;
        y[i] = SignExtend13(Read16(vram, address + 0x0E + i * 4)) + ctx.localY;
    }
    const bool isRegularRect = (x[0] == x[3]) && (x[1] == x[2]) && (y[0] == y[1]) && (y[2] == y[3]);
    const sint32 yAddAB = deinterlace && isRegularRect && (y[0] >= y[2]) ? doubleV : 0;
    const sint32 yAddCD = deinterlace && isRegularRect && (y[0] < y[2]) ? doubleV : 0;
    return {CoordS32{x[0], (y[0] << doubleV) + yAddAB}, CoordS32{x[1], (y[1] << doubleV) + yAddAB},
            CoordS32{x[2], (y[2] << doubleV) + yAddCD}, CoordS32{x[3], (y[3] << doubleV) + yAddCD}};
}

} // namespace

CommandSpans BuildCommandSpans(const uint8 *vram, uint32 address, ClipContext &ctx, const RasterState &state) {
    using enum VDP1Command::CommandType;

    CommandSpans out;
    CommandParams &p = out.params;
    p.address = address;
    p.control.u16 = Read16(vram, address);
    p.clip = ctx;
    const ClipRect clip = EffectiveClip(ctx, state.doubleV);
    const sint32 doubleV = static_cast<sint32>(state.doubleV);
    auto coord = [&](uint32 offset) { return SignExtend13(Read16(vram, address + offset)); };

    switch (p.control.command) {
    case DrawNormalSprite: {
        const VDP1Command::Size size{.u16 = Read16(vram, address + 0x0A)};
        const uint32 charSizeH = size.H * 8u;
        const uint32 charSizeV = size.V;
        const sint32 xa = coord(0x0C) + ctx.localX;
        const sint32 ya = coord(0x0E) + ctx.localY;
        const sint32 xb = static_cast<sint32>(static_cast<uint32>(xa) + std::max(charSizeH, 1u) - 1u);
        const sint32 yb = static_cast<sint32>(static_cast<uint32>(ya) + std::max(charSizeV, 1u) - 1u);
        const sint32 yAdd = state.deinterlace ? doubleV : 0;
        p.vertices = {CoordS32{xa, ya << doubleV}, CoordS32{xb, ya << doubleV}, CoordS32{xb, (yb << doubleV) + yAdd},
                      CoordS32{xa, (yb << doubleV) + yAdd}};
        BuildTexturedQuad(vram, out, clip, size);
        break;
    }
    case DrawScaledSprite: {
        const VDP1Command::Size size{.u16 = Read16(vram, address + 0x0A)};
        const sint32 xa = coord(0x0C);
        const sint32 ya = coord(0x0E);
        sint32 qxa = xa, qya = ya, qxb = xa, qyb = ya, qxc = xa, qyc = ya, qxd = xa, qyd = ya;
        const uint8 zoomPointH = p.control.zoomPoint & 3;
        const uint8 zoomPointV = (p.control.zoomPoint >> 2) & 3;
        if (zoomPointH == 0) {
            const sint32 xc = coord(0x14);
            qxb = xc;
            qxc = xc;
        } else {
            const sint32 xb = coord(0x10);
            switch (zoomPointH) {
            case 1: qxb += xb, qxc += xb; break;
            case 2:
                qxa -= xb >> 1;
                qxb += (xb + 1) >> 1;
                qxc += (xb + 1) >> 1;
                qxd -= xb >> 1;
                break;
            case 3: qxa -= xb, qxd -= xb; break;
            }
        }
        if (zoomPointV == 0) {
            const sint32 yc = coord(0x16);
            qyc = yc;
            qyd = yc;
        } else {
            const sint32 yb = coord(0x12);
            switch (zoomPointV) {
            case 1: qyc += yb, qyd += yb; break;
            case 2:
                qya -= yb >> 1;
                qyb -= yb >> 1;
                qyc += (yb + 1) >> 1;
                qyd += (yb + 1) >> 1;
                break;
            case 3: qya -= yb, qyb -= yb; break;
            }
        }
        const sint32 yAdd = state.deinterlace ? doubleV : 0;
        p.vertices = {CoordS32{qxa + ctx.localX, (qya + ctx.localY) << doubleV},
                      CoordS32{qxb + ctx.localX, (qyb + ctx.localY) << doubleV},
                      CoordS32{qxc + ctx.localX, ((qyc + ctx.localY) << doubleV) + yAdd},
                      CoordS32{qxd + ctx.localX, ((qyd + ctx.localY) << doubleV) + yAdd}};
        BuildTexturedQuad(vram, out, clip, size);
        break;
    }
    case DrawDistortedSprite:
    case DrawDistortedSpriteAlt: {
        const VDP1Command::Size size{.u16 = Read16(vram, address + 0x0A)};
        p.vertices = QuadVertices(vram, address, ctx, doubleV, state.deinterlace);
        BuildTexturedQuad(vram, out, clip, size);
        break;
    }
    case DrawPolygon:
        p.vertices = QuadVertices(vram, address, ctx, doubleV, state.deinterlace);
        BuildPolygon(vram, out, clip);
        break;
    case DrawPolylines:
    case DrawPolylinesAlt: {
        p.vertices = QuadVertices(vram, address, ctx, doubleV, state.deinterlace);
        p.mode.u16 = Read16(vram, address + 0x04);
        p.color = Read16(vram, address + 0x06);
        if (QuadSystemClipped(clip, p.vertices)) {
            break;
        }
        if (p.mode.gouraudEnable) {
            p.gouraud = ReadGouraudTable(vram, address);
        }
        for (uint32 i = 0; i < 4; ++i) {
            const uint32 j = (i + 1) & 3;
            if (LineSystemClipped(clip, p.vertices[i], p.vertices[j])) {
                continue;
            }
            Span span{.quadStep = i, .a = p.vertices[i], .b = p.vertices[j]};
            if (p.mode.gouraudEnable) {
                span.gouraudL = p.gouraud[i];
                span.gouraudR = p.gouraud[j];
            }
            out.spans.push_back(span);
        }
        out.draws = true;
        break;
    }
    case DrawLine: {
        const sint32 xa = coord(0x0C) + ctx.localX;
        const sint32 ya = coord(0x0E) + ctx.localY;
        const sint32 xb = coord(0x10) + ctx.localX;
        const sint32 yb = coord(0x12) + ctx.localY;
        p.vertices = {CoordS32{xa, ya << doubleV}, CoordS32{xb, yb << doubleV}, CoordS32{}, CoordS32{}};
        p.mode.u16 = Read16(vram, address + 0x04);
        p.color = Read16(vram, address + 0x06);
        if (LineSystemClipped(clip, p.vertices[0], p.vertices[1])) {
            break;
        }
        Span span{.a = p.vertices[0], .b = p.vertices[1]};
        if (p.mode.gouraudEnable) {
            p.gouraud = ReadGouraudTable(vram, address);
            span.gouraudL = p.gouraud[0];
            span.gouraudR = p.gouraud[1];
        }
        out.spans.push_back(span);
        out.draws = true;
        break;
    }
    case UserClipping:
    case UserClippingAlt:
        ctx.userClipX0 = Read16(vram, address + 0x0C);
        ctx.userClipY0 = Read16(vram, address + 0x0E);
        ctx.userClipX1 = Read16(vram, address + 0x14);
        ctx.userClipY1 = Read16(vram, address + 0x16);
        break;
    case SystemClipping:
        ctx.sysClipH = Read16(vram, address + 0x14);
        ctx.sysClipV = Read16(vram, address + 0x16);
        break;
    case SetLocalCoordinates:
        ctx.localX = coord(0x0C);
        ctx.localY = coord(0x0E);
        break;
    default: break;
    }
    return out;
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
    if (line.undefined) {
        throw UndefinedLineError("vdp1ref: LineStepper::SystemClip never ends for this line");
    }
    return 2 * line.length;
}

void RasterSpan(const RasterState &state, const CommandParams &cmd, const Span &span, const uint8 *vram,
                const RasterOptions &options, std::vector<Fragment> &out) {
    const ClipRect clip = EffectiveClip(cmd.clip, state.doubleV);
    if (LineSystemClipped(clip, span.a, span.b)) {
        return;
    }
    const VDP1Command::DrawMode mode = cmd.mode;

    port::Line line{span.a, span.b, cmd.antiAlias};
    const uint32 skipSteps = line.SystemClip(clip.sysH, clip.sysV);
    if (line.undefined) {
        throw UndefinedLineError("vdp1ref: LineStepper::SystemClip never ends for this line");
    }

    port::Gouraud gouraud;
    if (mode.gouraudEnable) {
        gouraud.Setup(line.dmaj + 1, span.gouraudL, span.gouraudR);
        gouraud.Skip(skipSteps);
    }

    const bool meshToMeshFB = state.transparentMeshes && mode.meshEnable;
    const uint32 targetMode = (static_cast<uint32>(mode.colorCalcBits) << kTargetCalcShift) |
                              (meshToMeshFB ? kTargetMeshBit : 0u) | (mode.msbOn ? kTargetMsbOnBit : 0u);

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

ResolveStats Resolve(const RasterState &state, std::vector<Fragment> fragments, Fbram &fbram, ResolveOrder order) {
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
    const uint8 fb = state.drawFB & 1;
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
            if (state.pixel8Bits) {
                const uint32 shift = (f.target & kTargetOddBit) ? 0 : 8;
                const uint16 keep = static_cast<uint16>(~(0xFFu << shift));
                const uint16 byte = static_cast<uint16>((f.color & 0xFF) << shift);
                if (toMesh) {
                    s = (s & keep) | byte;
                } else {
                    m = (m & keep) | byte;
                    if (state.transparentMeshes) {
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
                if (state.transparentMeshes) {
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
// Erase

void Erase(const EraseParams &params, Fbram &fbram) {
    const uint32 scaleV = params.doubleDensity ? 1 : 0;
    const uint32 maxH = params.hiResH ? 428 : 400;
    const uint32 maxV = params.vres >> scaleV;
    const uint32 x1 = std::min<uint32>(params.x1, maxH);
    const uint32 x3 = std::min<uint32>(params.x3, maxH);
    const uint32 y1 = std::min<uint32>(params.y1, maxV) << scaleV;
    const uint32 y3 = std::min<uint32>(params.y3, maxV) << scaleV;
    const bool mirror = params.deinterlace && params.doubleDensity;
    const uint8 fb = params.fbIndex & 1;
    const uint32 width = x3 > x1 ? x3 - x1 : 0;

    // One write per (x, y) of the region; with a cycle budget, write i (row-major) happens if i <= cycles. Every write
    // stores the same value, so overlapping rows (offset shift 8 with x >= 256) need no order.
    for (uint32 y = y1; y <= y3; y++) {
        for (uint32 x = x1; x < x3; x++) {
            const uint64 i = static_cast<uint64>(y - y1) * width + (x - x1);
            if (params.cycles != 0 && i > params.cycles) {
                continue;
            }
            const uint32 address = (((y << params.offsetShift) + x) * 2u) & 0x3FFFE;
            WriteBE16(fbram.main[fb], address, params.value);
            if (mirror) {
                WriteBE16(fbram.alt[fb], address, params.value);
            }
            if (params.transparentMeshes) {
                WriteBE16(fbram.mesh[0][fb], address, 0);
                if (mirror) {
                    WriteBE16(fbram.mesh[1][fb], address, 0);
                }
            }
        }
    }
}

// =====================================================================================================================
// Driver

ReferenceVDP1::ReferenceVDP1(const uint8 *vram, Fbram &fbram, RasterOptions options, uint32 maxBatchFragments,
                             uint64 shuffleSeed)
    : m_vram(vram)
    , m_fbram(fbram)
    , m_options(options)
    , m_maxBatchFragments(std::max(maxBatchFragments, 1u))
    , m_shuffle(shuffleSeed) {}

void ReferenceVDP1::Command(uint32 address, const RasterState &state) {
    ++m_stats.commands;
    CommandSpans cs = BuildCommandSpans(m_vram, address, clip, state);
    if (!cs.draws) {
        return;
    }
    m_commandInBatch = false;
    for (const Span &span : cs.spans) {
        const uint32 slots = SpanSlots(state, cs.params, span);
        if (slots == 0) {
            continue;
        }
        AddSpan(cs.params, state, span, slots);
    }
}

void ReferenceVDP1::AddSpan(const CommandParams &params, const RasterState &state, Span span, uint32 slots) {
    if (m_pending && (!(m_state == state) ||
                      (m_slots > 0 && static_cast<uint64>(m_slots) + slots > m_maxBatchFragments))) {
        Flush();
    }
    if (!m_pending) {
        m_pending = true;
        m_state = state;
        m_commandInBatch = false;
    }
    if (!m_commandInBatch) {
        m_commands.push_back(params);
        m_commandInBatch = true;
    }
    span.cmdIndex = static_cast<uint32>(m_commands.size() - 1);
    span.fragBase = m_slots;
    m_slots += slots;
    m_spans.push_back(span);
}

void ReferenceVDP1::Flush() {
    if (!m_pending) {
        return;
    }
    std::vector<Fragment> fragments;
    fragments.reserve(m_slots);
    for (size_t s = 0; s < m_spans.size(); ++s) {
        const Span &span = m_spans[s];
        const CommandParams &cmd = m_commands[span.cmdIndex];
        const size_t first = fragments.size();
        RasterSpan(m_state, cmd, span, m_vram, m_options, fragments);
        const uint32 slots = (s + 1 < m_spans.size() ? m_spans[s + 1].fragBase : m_slots) - span.fragBase;
        for (size_t i = first; i < fragments.size(); ++i) {
            if (fragments[i].key < span.fragBase || fragments[i].key >= span.fragBase + slots) {
                throw std::logic_error("vdp1ref::Flush: fragment key outside its span's slots");
            }
        }
        m_stats.slots += slots;
    }
    if (m_shuffle != 0) {
        // the GPU scatters fragments into their buckets in arbitrary order
        for (size_t i = fragments.size(); i > 1; --i) {
            std::swap(fragments[i - 1], fragments[SplitMix(m_shuffle) % i]);
        }
    }
    m_stats.spans += m_spans.size();
    m_stats.fragments += fragments.size();
    ++m_stats.batches;
    const ResolveStats rs = Resolve(m_state, std::move(fragments), m_fbram, m_options.resolveOrder);
    m_stats.maxPerUnit = std::max(m_stats.maxPerUnit, rs.maxPerUnit);

    m_pending = false;
    m_commands.clear();
    m_spans.clear();
    m_slots = 0;
    m_commandInBatch = false;
}

void ReferenceVDP1::Erase(const EraseParams &params) {
    Flush();
    vdp1ref::Erase(params, m_fbram);
}

} // namespace brimir::test::vdp1ref
