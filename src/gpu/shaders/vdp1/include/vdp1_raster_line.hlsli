// Brimir: the VDP1 raster passes' shared part (cs_vdp1_raster.hlsl at native resolution, cs_vdp1_raster_nx.hlsl at N x;
// GPU stages 2 and 3). Written from the CPU references' RasterSpan (tests/unit/vdp1_raster_reference.cpp, the loop body
// of the software renderer's VDP1PlotLine / VDP1PlotTexturedLine and the read half of VDP1PlotPixel, and
// tests/unit/vdp1_nx_reference.cpp). Brimir only; Ymir's shaders were consulted for structure only.
//
// The including shader defines VDP1_RASTER_NX (0 or 1) and declares g_batch (BatchParams), g_commands, g_vram,
// g_fragments, g_unitCounts and g_batchState, then defines RasterSpan(uint3 id) after including this file. Here:
// - VRAM reads, the clip rectangles (VDP1IsPixelClipped);
// - the slot writer: a span writes each of its slots once, in increasing order (a fragment, or 0 for the slots it
//   skips), counts every fragment for its unit and keeps the thread's unit range;
// - RasterNativeLine: software's line loop over one native span; its fragments go through WriteFragment, which at
//   native resolution writes the fragment at slot 2 * step + isAA, and at N x (replicate spans) writes its N x N block
//   at slots N^2 * (2 * step + isAA) + j * N + i with N x targets (vdp1_nx_reference.hpp, Replicate);
// - RasterMain: the group's unit range into the batch's accumulator (g_batchState; one atomic pair per group).
#ifndef BRIMIR_VDP1_RASTER_LINE_HLSLI
#define BRIMIR_VDP1_RASTER_LINE_HLSLI

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"
#include "vdp1_steppers.hlsli"

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

// VDP1IsLineSystemClipped
bool LineSystemClipped(ClipRect clip, int ax, int ay, int bx, int by) {
    return (ax < 0 && bx < 0) || (ay < 0 && by < 0) || (ax > clip.sysH && bx > clip.sysH) ||
           (ay > clip.sysV && by > clip.sysV);
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

// The units this thread counted (thread-private)
static uint s_unitLo = 0xFFFFFFFFu;
static uint s_unitHi = 0;

// Writes the fragment (target, color) of unit `unit` at `slot` (relative to the span's first slot)
void WriteSlot(inout SlotWriter w, uint slot, uint unit, uint target, uint color) {
    if (slot < w.next || slot >= w.end) {
        return; // outside the span's slots (a span list that does not match its commands)
    }
    uint counts, stride;
    g_unitCounts.GetDimensions(counts, stride);
    if (unit >= counts) {
        return; // a counts buffer smaller than the unit range: the slot stays empty
    }
    ZeroSlotsUpTo(w, slot);
    Fragment f;
    f.target = target;
    f.color = color;
    g_fragments[w.fragBase + slot] = f;
    w.next = slot + 1;
#if VDP1_RASTER_NX && VDP1_NX_RASTER_MODE != 0
    NxCountFragment(unit, w.fragBase + slot, target); // cs_vdp1_raster_nx.hlsl (the overwrite-class fast path)
#else
    InterlockedAdd(g_unitCounts[unit], 1);
#endif
    s_unitLo = min(s_unitLo, unit);
    s_unitHi = max(s_unitHi, unit + 1);
}

#if VDP1_RASTER_NX
static uint s_scale = 1; // N (the N x raster sets it)
#endif

// A fragment of the native line loop: native slot `slot`, native target
void WriteFragment(inout SlotWriter w, uint slot, uint target, uint color) {
#if VDP1_RASTER_NX
    const uint sub = s_scale * s_scale;
    for (uint s = 0; s < sub; ++s) {
        const uint unit = Vdp1NxUnit(Vdp1FragUnit(target), s, s_scale);
        WriteSlot(w, slot * sub + s, unit, Vdp1MakeNxFragmentTarget(unit, target), color);
    }
#else
    WriteSlot(w, slot, Vdp1FragUnit(target), target, color);
#endif
}

// The texel reader of VDP1PlotTexturedLine: the texel's colour, transparency and end code (colour modes 6 and 7 keep the
// previous texel)
struct TexelState {
    uint color;
    bool transparent;
    bool hasEndCode;
    int endCodeCount;
};

void ReadTexel(CommandParams cmd, uint colorMode, bool endCodeDisable, uint u, uint v, uint charSizeH,
               inout TexelState t) {
    const uint charIndex = u + v * charSizeH;
    bool endCode = false;
    bool known = true;
    uint color = t.color;
    switch (colorMode) {
    case 0:
        color = (ReadVRAM8(cmd.charAddr + (charIndex >> 1)) >> ((~u & 1) * 4)) & 0xF;
        endCode = color == 0xF;
        t.transparent = color == 0x0;
        color = (color | cmd.colorBank) & 0xFFFF;
        break;
    case 1:
        color = (ReadVRAM8(cmd.charAddr + (charIndex >> 1)) >> ((~u & 1) * 4)) & 0xF;
        endCode = color == 0xF;
        t.transparent = color == 0x0;
        color = ReadVRAM16(color * 2 + cmd.colorBank);
        break;
    case 2:
        color = ReadVRAM8(cmd.charAddr + charIndex);
        endCode = color == 0xFF;
        t.transparent = color == 0x00;
        color = ((color & 0x3F) | cmd.colorBank) & 0xFFFF;
        break;
    case 3:
        color = ReadVRAM8(cmd.charAddr + charIndex);
        endCode = color == 0xFF;
        t.transparent = color == 0x00;
        color = ((color & 0x7F) | cmd.colorBank) & 0xFFFF;
        break;
    case 4:
        color = ReadVRAM8(cmd.charAddr + charIndex);
        endCode = color == 0xFF;
        t.transparent = color == 0x00;
        color = (color | cmd.colorBank) & 0xFFFF;
        break;
    case 5:
        color = ReadVRAM16(cmd.charAddr + charIndex * 2);
        endCode = color == 0x7FFF;
        t.transparent = (color & 0x8000) == 0;
        break;
    default: known = false; break;
    }
    t.color = color;
    if (known) {
        if (endCode && !endCodeDisable) {
            t.hasEndCode = true;
            t.endCodeCount += 1;
        } else {
            t.hasEndCode = false;
        }
    }
}

// Software's line loop over the native span (ax, ay) - (bx, by) of command `cmd`: every pixel VDP1PlotPixel would write
// becomes a fragment through WriteFragment at native slot 2 * step + isAA. The line is rejected as
// VDP1IsLineSystemClipped does (nothing written), else stepped after SystemClip. With `endFromLine` the writer's end is
// set here to min(2 * Length(), room) (native spans; a replicate span's end comes from its slot range). The caller
// writes the slots after the last fragment (ZeroSlotsUpTo(w, w.end)).
void RasterNativeLine(CommandParams cmd, int ax, int ay, int bx, int by, uint gouraudLR, uint v, inout SlotWriter writer,
                      bool endFromLine, uint room) {
    const uint df = cmd.drawFlags;
    const uint rf = cmd.rasterFlags;
    const ClipRect clip = EffectiveClip(cmd, Vdp1RasDoubleV(rf));
    // VDP1IsLineSystemClipped: no span is emitted for such a line (it has no slots)
    if (LineSystemClipped(clip, ax, ay, bx, by)) {
        return;
    }

    LineStepper ln = MakeLineStepper(ax, ay, bx, by, Vdp1CmdAntiAlias(df));
    const uint skipSteps = LineSystemClip(ln, clip.sysH, clip.sysV);
    if (endFromLine) {
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
        gouraud = MakeGouraudStepper(ln.dmaj + 1, Vdp1Lo16(gouraudLR), Vdp1Hi16(gouraudLR));
        GouraudSkip(gouraud, skipSteps);
    }

    const bool textured = Vdp1CmdTextured(df);
    const uint colorMode = Vdp1CmdColorMode(df);
    const bool endCodeDisable = Vdp1CmdEndCodeDisable(df);
    const bool transparentPixelDisable = Vdp1CmdTransparentPixelDisable(df);
    const uint charSizeH = max(Vdp1Lo16(cmd.charSize), 1u);

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

    TexelState texel;
    texel.color = textured ? 0 : (cmd.color & 0xFFFF);
    texel.transparent = true;
    texel.hasEndCode = false;
    texel.endCodeCount = useHighSpeedShrink ? kIntMin : 0;

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
            PLOT(ln.x, ln.y, 0, texel.color, plottedPixel);
            if (aa) {
                bool plottedAA;
                PLOT(ln.x + ln.aaXInc, ln.y + ln.aaYInc, 1, texel.color, plottedAA);
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
        ReadTexel(cmd, colorMode, endCodeDisable, uint(uStepper.value), v, charSizeH, texel);
        LineStep(ln);
        for (; LineCanStep(ln); aa = LineStep(ln)) {
            while (TextureShouldStepTexel(uStepper)) {
                TextureStepTexel(uStepper);
                ReadTexel(cmd, colorMode, endCodeDisable, uint(uStepper.value), v, charSizeH, texel);
                if (texel.endCodeCount == 2) {
                    break;
                }
            }
            if (texel.endCodeCount == 2) {
                break;
            }
            TextureStepPixel(uStepper);

            if (texel.hasEndCode || (texel.transparent && !transparentPixelDisable)) {
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
            PLOT(ln.x, ln.y, 0, texel.color, plottedPixel);
            if (aa) {
                bool plottedAA;
                PLOT(ln.x + ln.aaXInc, ln.y + ln.aaYInc, 1, texel.color, plottedAA);
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
#undef PLOT
}

// The group's unit range: ~lo (max) and hi (max), so zero is empty
groupshared uint gs_rangeLoInv;
groupshared uint gs_rangeHi;

void RasterSpan(uint3 id);

void RasterMain(uint3 id, uint3 tid) {
    if (tid.x == 0) {
        gs_rangeLoInv = 0;
        gs_rangeHi = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    RasterSpan(id);
    if (s_unitHi != 0) {
        InterlockedMax(gs_rangeLoInv, ~s_unitLo);
        InterlockedMax(gs_rangeHi, s_unitHi);
    }
    GroupMemoryBarrierWithGroupSync();
    uint words, stride;
    g_batchState.GetDimensions(words, stride);
    if (tid.x == 0 && gs_rangeHi != 0 && kBatchStateAccHi < words) {
        InterlockedMax(g_batchState[kBatchStateAccLoInv], gs_rangeLoInv);
        InterlockedMax(g_batchState[kBatchStateAccHi], gs_rangeHi);
    }
}

#endif
