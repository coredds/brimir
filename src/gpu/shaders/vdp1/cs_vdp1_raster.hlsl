// Brimir: VDP1 raster pass, one thread per span (GPU stage 2; docs/ymir-hw-renderer-notes/stage2-vdp1-research.md
// section 4.3). Written from the CPU reference's RasterSpan (tests/unit/vdp1_raster_reference.cpp), which is the loop
// body of the software renderer's VDP1PlotLine / VDP1PlotTexturedLine and the read half of VDP1PlotPixel
// (vdp_renderer_sw.cpp). Brimir only; Ymir's shaders were consulted for structure only.
//
// Thread i (i < spanCount) takes span spanOffset + i and its command (cmdOffset + cmdIndex):
// - rejects the line as VDP1IsLineSystemClipped does, else builds the LineStepper, runs SystemClip and steps the line
//   one pixel at a time (Gouraud and texture steppers alongside, as software);
// - every pixel VDP1PlotPixel would write becomes a fragment in slot fragBase + 2 * step + isAA (the order key; the
//   fragment buffer holds no key) and counts one for its unit (the FBRAM word and field) in g_unitCounts;
// - writes every one of the span's 2 * Length() slots: the fragment, or 0 (not live) for a slot without one. The spans'
//   slots partition [0, fragmentCount), so the batch's fragment buffer needs no clearing.
// Bindings (src/gpu/vdp1/vdp1_bindings.hpp kRasterBindings): commands (0), spans (1), VDP1 VRAM (2), fragments (16),
// unit counts (17). Push constants: BatchParams (scale is 1: stage 2 draws at native resolution).

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"
#include "vdp1_steppers.hlsli"

[[vk::push_constant]] BatchParams g_batch;

StructuredBuffer<CommandParams> g_commands : register(t0);
StructuredBuffer<Span> g_spans : register(t1);
ByteAddressBuffer g_vram : register(t2);
RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_unitCounts : register(u1);

static const uint kVRAMMask = kVRAMBufferSize - 1;

uint ReadVRAM8(uint address) {
    address &= kVRAMMask;
    return (g_vram.Load(address & ~3u) >> ((address & 3) * 8)) & 0xFF;
}

uint ReadVRAM16(uint address) {
    address &= kVRAMMask & ~1u;
    return (ReadVRAM8(address) << 8) | ReadVRAM8(address + 1);
}

// The clip rectangles with doubleV applied (VDP1IsPixelSystemClipped / VDP1IsPixelUserClipped)
struct ClipRect {
    int sysH, sysV;
    int userX0, userY0, userX1, userY1;
};

ClipRect EffectiveClip(CommandParams c, uint doubleV) {
    ClipRect r;
    const int dV = int(doubleV);
    r.sysH = int(Vdp1Lo16(c.sysClip));
    r.sysV = (int(Vdp1Hi16(c.sysClip)) << dV) | dV;
    r.userX0 = int(Vdp1Lo16(c.userClip0));
    r.userY0 = (int(Vdp1Hi16(c.userClip0)) << dV) | dV;
    r.userX1 = int(Vdp1Lo16(c.userClip1));
    r.userY1 = (int(Vdp1Hi16(c.userClip1)) << dV) | dV;
    return r;
}

// VDP1IsPixelClipped
bool PixelClipped(ClipRect clip, int x, int y, bool userClippingEnable, bool clippingMode) {
    if (x < 0 || x > clip.sysH || y < 0 || y > clip.sysV) {
        return true;
    }
    const bool userClipped = x < clip.userX0 || x > clip.userX1 || y < clip.userY0 || y > clip.userY1;
    return userClippingEnable && userClipped != clippingMode;
}

// The span's state while it is plotted: the fragment slots written so far (every slot is written once, in order)
struct SlotWriter {
    uint fragBase;
    uint next; // first slot not yet written (relative to fragBase)
    uint end;  // the span's slots, clamped to the batch and the buffer
};

void ZeroSlotsUpTo(inout SlotWriter w, uint slot) {
    const uint stop = min(slot, w.end);
    Fragment none;
    none.target = 0;
    none.color = 0;
    for (; w.next < stop; ++w.next) {
        g_fragments[w.fragBase + w.next] = none;
    }
}

void WriteFragment(inout SlotWriter w, uint slot, uint target, uint color) {
    if (slot < w.next || slot >= w.end) {
        return; // outside the span's slots (a span list that does not match its commands)
    }
    ZeroSlotsUpTo(w, slot);
    Fragment f;
    f.target = target;
    f.color = color;
    g_fragments[w.fragBase + slot] = f;
    w.next = slot + 1;
    InterlockedAdd(g_unitCounts[Vdp1FragUnit(target)], 1);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_batch.spanCount) {
        return;
    }
    uint spanCount, spanStride, cmdCount, cmdStride, fragCapacity, fragStride;
    g_spans.GetDimensions(spanCount, spanStride);
    g_commands.GetDimensions(cmdCount, cmdStride);
    g_fragments.GetDimensions(fragCapacity, fragStride);
    const uint spanIndex = g_batch.spanOffset + id.x;
    if (spanIndex >= spanCount) {
        return;
    }
    const Span span = g_spans[spanIndex];
    const uint cmdIndex = g_batch.cmdOffset + span.cmdIndex;
    if (cmdIndex >= cmdCount) {
        return;
    }
    const CommandParams cmd = g_commands[cmdIndex];
    const uint df = cmd.drawFlags;
    const uint rf = cmd.rasterFlags;
    const int ax = asint(span.x0), ay = asint(span.y0), bx = asint(span.x1), by = asint(span.y1);

    const ClipRect clip = EffectiveClip(cmd, Vdp1RasDoubleV(rf));
    // VDP1IsLineSystemClipped: no span is emitted for such a line (it has no slots)
    if ((ax < 0 && bx < 0) || (ay < 0 && by < 0) || (ax > clip.sysH && bx > clip.sysH) ||
        (ay > clip.sysV && by > clip.sysV)) {
        return;
    }

    LineStepper ln = MakeLineStepper(ax, ay, bx, by, Vdp1CmdAntiAlias(df));
    const uint skipSteps = LineSystemClip(ln, clip.sysH, clip.sysV);

    SlotWriter writer;
    writer.fragBase = span.fragBase;
    writer.next = 0;
    {
        const uint limit = min(g_batch.fragmentCount, fragCapacity);
        const uint room = span.fragBase < limit ? limit - span.fragBase : 0;
        writer.end = min(2 * ln.length, room);
    }

    // Draw mode and registers
    const bool gouraudEnable = Vdp1CmdGouraud(df);
    const bool meshEnable = Vdp1CmdMesh(df);
    const bool clippingMode = Vdp1CmdClippingMode(df);
    const bool userClippingEnable = Vdp1CmdUserClip(df);
    const bool msbOn = Vdp1CmdMsbOn(df);
    const bool pixel8 = Vdp1RasPixel8(rf);
    const bool transparentMeshes = Vdp1RasTransparentMeshes(rf);
    const bool deinterlace = Vdp1RasDeinterlace(rf);
    const bool doubleDensity = Vdp1RasDoubleDensity(rf);
    const bool dblInterlaceEnable = Vdp1RasDblInterlace(rf);
    const int dblInterlaceDrawLine = Vdp1RasDblInterlaceDrawLine(rf) ? 1 : 0;
    const uint fbSizeH = Vdp1RasFbSizeH(rf);
    const bool meshToMeshFB = transparentMeshes && meshEnable;
    const uint calc = Vdp1CmdColorCalc(df);

    GouraudStepper gouraud = MakeGouraudStepper(1, 0, 0);
    if (gouraudEnable) {
        gouraud = MakeGouraudStepper(ln.dmaj + 1, Vdp1Lo16(span.gouraudLR), Vdp1Hi16(span.gouraudLR));
        GouraudSkip(gouraud, skipSteps);
    }

    const bool textured = Vdp1CmdTextured(df);
    const uint colorMode = Vdp1CmdColorMode(df);
    const bool endCodeDisable = Vdp1CmdEndCodeDisable(df);
    const bool transparentPixelDisable = Vdp1CmdTransparentPixelDisable(df);
    const uint charSizeH = max(Vdp1Lo16(cmd.charSize), 1u);
    const uint v = Vdp1SpanTexV(span.stepTexV);

    // Texture setup (VDP1PlotTexturedLine)
    int uStart = 0;
    int uEnd = int(charSizeH) - 1;
    if (Vdp1CmdFlipH(df)) {
        const int t = uStart;
        uStart = uEnd;
        uEnd = t;
    }
    const bool useHighSpeedShrink = Vdp1CmdHighSpeedShrink(df) && ln.dmaj < charSizeH - 1;
    TextureStepper uStepper = MakeTextureStepper(ln.dmaj + 1, uStart, uEnd, useHighSpeedShrink,
                                                 Vdp1RasEvenOddCoordSelect(rf) ? 1 : 0);
    TextureSkipPixels(uStepper, skipSteps);

    uint color = textured ? 0 : (cmd.color & 0xFFFF);
    bool transparent = true;
    bool hasEndCode = false;
    int endCodeCount = useHighSpeedShrink ? kIntMin : 0;

    // readTexel: the current texel's colour, transparency and end code (colour modes 6 and 7 keep the previous texel)
#define READ_TEXEL()                                                                                                   \
    {                                                                                                                  \
        const uint u = uint(uStepper.value);                                                                           \
        const uint charIndex = u + v * charSizeH;                                                                      \
        bool endCode = false;                                                                                          \
        bool known = true;                                                                                             \
        switch (colorMode) {                                                                                           \
        case 0:                                                                                                        \
            color = (ReadVRAM8(cmd.charAddr + (charIndex >> 1)) >> ((~u & 1) * 4)) & 0xF;                              \
            endCode = color == 0xF;                                                                                    \
            transparent = color == 0x0;                                                                                \
            color = (color | cmd.colorBank) & 0xFFFF;                                                                  \
            break;                                                                                                     \
        case 1:                                                                                                        \
            color = (ReadVRAM8(cmd.charAddr + (charIndex >> 1)) >> ((~u & 1) * 4)) & 0xF;                              \
            endCode = color == 0xF;                                                                                    \
            transparent = color == 0x0;                                                                                \
            color = ReadVRAM16(color * 2 + cmd.colorBank);                                                             \
            break;                                                                                                     \
        case 2:                                                                                                        \
            color = ReadVRAM8(cmd.charAddr + charIndex);                                                               \
            endCode = color == 0xFF;                                                                                   \
            transparent = color == 0x00;                                                                               \
            color = ((color & 0x3F) | cmd.colorBank) & 0xFFFF;                                                         \
            break;                                                                                                     \
        case 3:                                                                                                        \
            color = ReadVRAM8(cmd.charAddr + charIndex);                                                               \
            endCode = color == 0xFF;                                                                                   \
            transparent = color == 0x00;                                                                               \
            color = ((color & 0x7F) | cmd.colorBank) & 0xFFFF;                                                         \
            break;                                                                                                     \
        case 4:                                                                                                        \
            color = ReadVRAM8(cmd.charAddr + charIndex);                                                               \
            endCode = color == 0xFF;                                                                                   \
            transparent = color == 0x00;                                                                               \
            color = (color | cmd.colorBank) & 0xFFFF;                                                                  \
            break;                                                                                                     \
        case 5:                                                                                                        \
            color = ReadVRAM16(cmd.charAddr + charIndex * 2);                                                          \
            endCode = color == 0x7FFF;                                                                                 \
            transparent = (color & 0x8000) == 0;                                                                       \
            break;                                                                                                     \
        default: known = false; break;                                                                                 \
        }                                                                                                              \
        if (known) {                                                                                                   \
            if (endCode && !endCodeDisable) {                                                                          \
                hasEndCode = true;                                                                                     \
                endCodeCount += 1;                                                                                     \
            } else {                                                                                                   \
                hasEndCode = false;                                                                                    \
            }                                                                                                          \
        }                                                                                                              \
    }

    // VDP1PlotPixel up to the write: false only when clipped
#define PLOT(px, py, isAA, plotColor, outPlotted)                                                                      \
    {                                                                                                                  \
        const int plotX = (px);                                                                                        \
        int plotY = (py);                                                                                              \
        if (PixelClipped(clip, plotX, plotY, userClippingEnable, clippingMode)) {                                      \
            outPlotted = false;                                                                                        \
        } else {                                                                                                       \
            outPlotted = true;                                                                                         \
            const bool meshSkip = !transparentMeshes && meshEnable && ((plotX ^ plotY) & 1) != 0;                      \
            const bool altFB = deinterlace && doubleDensity && (plotY & 1) != 0;                                       \
            const bool dieSkip = doubleDensity && !deinterlace && dblInterlaceEnable &&                                \
                                 (plotY & 1) != dblInterlaceDrawLine;                                                  \
            if (!meshSkip && !dieSkip) {                                                                               \
                if ((deinterlace && doubleDensity) || dblInterlaceEnable) {                                            \
                    plotY >>= 1;                                                                                       \
                }                                                                                                      \
                uint fbOffset = uint(plotY) * fbSizeH + uint(plotX);                                                   \
                if (!pixel8) {                                                                                         \
                    fbOffset *= 2;                                                                                     \
                }                                                                                                      \
                fbOffset &= 0x3FFFF;                                                                                   \
                uint fragColor;                                                                                        \
                if (msbOn) {                                                                                           \
                    fragColor = 0;                                                                                     \
                } else if (pixel8) {                                                                                   \
                    fragColor = (plotColor) & 0xFF;                                                                    \
                } else if (gouraudEnable) {                                                                            \
                    fragColor = GouraudBlend(gouraud, (plotColor) & 0xFFFF);                                           \
                } else {                                                                                               \
                    fragColor = (plotColor) & 0xFFFF;                                                                  \
                }                                                                                                      \
                const uint target = Vdp1MakeFragmentTarget(fbOffset >> 1, altFB, (fbOffset & 1) != 0, calc,            \
                                                           meshToMeshFB, msbOn, pixel8, transparentMeshes);            \
                WriteFragment(writer, 2 * (ln.step - 1) + (isAA), target, fragColor);                                \
            }                                                                                                          \
        }                                                                                                              \
    }

    bool aa = false;
    bool plotted = false;
    if (!textured) {
        // VDP1PlotLine
        LineStep(ln);
        for (; LineCanStep(ln); aa = LineStep(ln)) {
            bool plottedPixel;
            PLOT(ln.x, ln.y, 0, color, plottedPixel);
            if (aa) {
                bool plottedAA;
                PLOT(ln.x + ln.aaXInc, ln.y + ln.aaYInc, 1, color, plottedAA);
                plottedPixel = plottedPixel || plottedAA;
            }
            if (plottedPixel) {
                plotted = true;
            } else if (plotted && !clippingMode) {
                break;
            }
            if (gouraudEnable) {
                GouraudStep(gouraud);
            }
        }
    } else {
        // VDP1PlotTexturedLine
        READ_TEXEL();
        LineStep(ln);
        for (; LineCanStep(ln); aa = LineStep(ln)) {
            while (TextureShouldStepTexel(uStepper)) {
                TextureStepTexel(uStepper);
                READ_TEXEL();
                if (endCodeCount == 2) {
                    break;
                }
            }
            if (endCodeCount == 2) {
                break;
            }
            TextureStepPixel(uStepper);

            if (hasEndCode || (transparent && !transparentPixelDisable)) {
                if (gouraudEnable) {
                    GouraudStep(gouraud);
                }
                if (!clippingMode) {
                    if (!PixelClipped(clip, ln.x, ln.y, userClippingEnable, clippingMode)) {
                        plotted = true;
                        continue;
                    }
                    if (aa && !PixelClipped(clip, ln.x + ln.aaXInc, ln.y + ln.aaYInc, userClippingEnable,
                                            clippingMode)) {
                        plotted = true;
                        continue;
                    }
                    if (plotted) {
                        break;
                    }
                }
                continue;
            }

            bool plottedPixel;
            PLOT(ln.x, ln.y, 0, color, plottedPixel);
            if (aa) {
                bool plottedAA;
                PLOT(ln.x + ln.aaXInc, ln.y + ln.aaYInc, 1, color, plottedAA);
                plottedPixel = plottedPixel || plottedAA;
            }
            if (plottedPixel) {
                plotted = true;
            } else if (plotted && !clippingMode) {
                break;
            }
            if (gouraudEnable) {
                GouraudStep(gouraud);
            }
        }
    }

    // The slots after the last fragment
    ZeroSlotsUpTo(writer, writer.end);
}
