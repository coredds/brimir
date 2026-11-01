// Brimir: N x VDP1 raster pass, one thread per N x span (GPU stage 3; docs/ymir-hw-renderer-notes/stage3-research.md
// section 1.4). Written from the CPU reference's RasterSpan (tests/unit/vdp1_nx_reference.cpp: RasterRect, RasterQuad,
// RasterReplicate and the PixelWriter), which is the specification: the chosen rules R1 + T2 + L2, the direction-aware
// clamp-and-phase sub-steps of rect spans, 32-bit N x steppers for quad spans. Brimir only.
//
// Thread i (i < spanCount) takes N x span spanOffset + i (Vdp1NxSpan) and its command (cmdOffset + cmdIndex). Its slots
// are [slotBase, the next span's slotBase) (the last span's end at fragmentCount, the sub-batch's slot count); every slot
// is written once, in increasing order: a fragment, or 0. A fragment's target is the N x one (vdp1_hw_params.hpp): N x
// unit (native unit) * N^2 + j * N + i, the native target's flags in bits 28-22. By kind:
// - Rect (a row that is not horizontal is skipped and counted in g_batchState[kBatchStateBadRectSpans], as the reference throws): N x row N * y + subRow of the native row (x0, y0) - (x1, y1): the native LineStepper and SystemClip; native
//   pixel p gives N sub-steps (textures and Gouraud by K = clamp(phase((N P + s) num / (N den)), c(P), c(P + 1)), phase
//   floor forwards and ceil backwards) and its N x N block's row: slot N p + s is sub-texel i = s forwards, and
//   backwards i = 0 for s = 0 and i = N - s (sub-step s of the previous pixel; pixel 0 repeats itself) otherwise;
// - Quad: the 32-bit LineStepper over the N x end points, SystemClip over the N x extent of the clip area, antialiased
//   as the span says; texture and Gouraud steppers over the N x length, HSS from the native-equivalent length dmaj / N;
//   slot 2 * step + isAA;
// - Replicate: the native raster's line loop (include/vdp1_raster_line.hlsli), each native fragment written as its
//   N x N block at slots N^2 * (2 * step + isAA) + j * N + i.
// Clipping, mesh, double interlace, the alternate field and the FB address come from the native coordinates
// (floor(X / N), floor(Y / N)) of each N x pixel.
// Bindings (src/gpu/vdp1/vdp1_bindings.hpp kRasterNxBindings): commands (0), N x spans (1), VDP1 VRAM (2), fragments
// (16), unit counts (17, N^2 * 262,144), batch state (22). Push constants: BatchParams (scale = N, 2 to 4).

#define VDP1_RASTER_NX 1

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] BatchParams g_batch;

StructuredBuffer<CommandParams> g_commands : register(t0);
StructuredBuffer<NxSpan> g_spans : register(t1);
ByteAddressBuffer g_vram : register(t2);
RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWStructuredBuffer<uint> g_batchState : register(u6);

#include "vdp1_raster_line.hlsli"

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID) {
    RasterMain(id, tid);
}

int FloorDiv(int v, int n) {
    return v >= 0 ? v / n : -((-v + n - 1) / n);
}

// VDP1PlotPixel up to the write at the native coordinates of an N x pixel (the reference's PixelWriter)
struct NxPlotter {
    ClipRect clip;
    bool userClippingEnable, clippingMode, meshEnable, transparentMeshes;
    bool deinterlace, doubleDensity, dblInterlaceEnable;
    int dblInterlaceDrawLine;
    uint fbSizeH;
    bool pixel8, msbOn, gouraudEnable, meshToMeshFB;
    uint calc;
    int n;
};

NxPlotter MakePlotter(CommandParams cmd, uint n) {
    const uint df = cmd.drawFlags;
    const uint rf = cmd.rasterFlags;
    NxPlotter p;
    p.clip = EffectiveClip(cmd, Vdp1RasDoubleV(rf));
    p.userClippingEnable = Vdp1CmdUserClip(df);
    p.clippingMode = Vdp1CmdClippingMode(df);
    p.meshEnable = Vdp1CmdMesh(df);
    p.transparentMeshes = Vdp1RasTransparentMeshes(rf);
    p.deinterlace = Vdp1RasDeinterlace(rf);
    p.doubleDensity = Vdp1RasDoubleDensity(rf);
    p.dblInterlaceEnable = Vdp1RasDblInterlace(rf);
    p.dblInterlaceDrawLine = Vdp1RasDblInterlaceDrawLine(rf) ? 1 : 0;
    p.fbSizeH = Vdp1RasFbSizeH(rf);
    p.pixel8 = Vdp1RasPixel8(rf);
    p.msbOn = Vdp1CmdMsbOn(df);
    p.gouraudEnable = Vdp1CmdGouraud(df);
    p.meshToMeshFB = p.transparentMeshes && p.meshEnable;
    p.calc = Vdp1CmdColorCalc(df);
    p.n = int(n);
    return p;
}

// The fragment colour of VDP1PlotPixel, `blended` the colour after Gouraud
uint PlotColor(NxPlotter p, uint color, uint blended) {
    if (p.msbOn) {
        return 0;
    }
    if (p.pixel8) {
        return color & 0xFF;
    }
    return p.gouraudEnable ? blended : color & 0xFFFF;
}

// The N x pixel (X, Y) at `slot`: false only when clipped
bool NxPlot(NxPlotter p, inout SlotWriter w, int X, int Y, uint slot, uint color) {
    const int x = FloorDiv(X, p.n);
    int y = FloorDiv(Y, p.n);
    const uint i = uint(X - x * p.n);
    const uint j = uint(Y - y * p.n);
    if (PixelClipped(p.clip, x, y, p.userClippingEnable, p.clippingMode)) {
        return false;
    }
    if (!p.transparentMeshes && p.meshEnable && ((x ^ y) & 1) != 0) {
        return true;
    }
    const bool altFB = p.deinterlace && p.doubleDensity && (y & 1) != 0;
    if (p.doubleDensity && !p.deinterlace && p.dblInterlaceEnable && (y & 1) != p.dblInterlaceDrawLine) {
        return true;
    }
    if ((p.deinterlace && p.doubleDensity) || p.dblInterlaceEnable) {
        y >>= 1;
    }
    uint fbOffset = uint(y) * p.fbSizeH + uint(x);
    if (!p.pixel8) {
        fbOffset *= 2;
    }
    fbOffset &= 0x3FFFF;
    const uint target = Vdp1MakeFragmentTarget(fbOffset >> 1, altFB, (fbOffset & 1) != 0, p.calc, p.meshToMeshFB,
                                               p.msbOn, p.pixel8, p.transparentMeshes);
    const uint unit = Vdp1NxUnit(Vdp1FragUnit(target), j * uint(p.n) + i, uint(p.n));
    WriteSlot(w, slot, unit, Vdp1MakeNxFragmentTarget(unit, target), color);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rect sub-steps (the reference's PhaseCount, NextTexelCount and SubGouraud)

// floor (or ceil) of (N P + s) num / (N den), 32-bit unsigned as the reference
uint PhaseCount(uint num, uint den, uint n, uint P, uint s, bool ceilPhase) {
    if (den == 0) {
        return 0;
    }
    const uint numerator = (n * P + s) * num;
    const uint denominator = n * den;
    return (numerator + (ceilPhase ? denominator - 1 : 0)) / denominator;
}

// The native texture stepper's texel count at the next pixel, without changing `t`
uint NextTexelCount(TextureStepper t, uint count) {
    TextureStepPixel(t);
    while (TextureShouldStepTexel(t)) {
        TextureStepTexel(t);
        ++count;
    }
    return count;
}

// Gouraud sub-steps per channel: v(P) from the native stepper, U(P) = (v(P) - v0) * fracInc the steps from the value
// after setup, rate (|intInc| den + num) / den of the reduced channel:
//   U = clamp(phase((N P + s) rate), U(P), U(P + 1))
struct SubGouraudChannel {
    int v0, f;
    uint rateNum, rateDen;
};

SubGouraudChannel MakeSubGouraudChannel(GouraudChannel c) {
    SubGouraudChannel s;
    s.v0 = c.value;
    s.f = c.fracInc;
    s.rateNum = uint(abs(c.intInc) * c.den + c.num);
    s.rateDen = uint(c.den);
    return s;
}

struct SubGouraud {
    GouraudStepper cur, next;
    SubGouraudChannel r, g, b;
    int value[3];
    uint n, P, s;
    bool ceilPhase;
};

int SubGouraudValue(SubGouraud sg, SubGouraudChannel c, GouraudChannel cur, GouraudChannel next) {
    if (sg.s == 0 || c.rateDen == 0) {
        return cur.value;
    }
    const int u0 = (cur.value - c.v0) * c.f;
    const int u1 = max(u0, (next.value - c.v0) * c.f);
    const int fl = int(PhaseCount(c.rateNum, c.rateDen, sg.n, sg.P, sg.s, sg.ceilPhase));
    return c.v0 + c.f * clamp(fl, u0, u1);
}

void SubGouraudUpdate(inout SubGouraud sg) {
    sg.value[0] = SubGouraudValue(sg, sg.r, sg.cur.r, sg.next.r);
    sg.value[1] = SubGouraudValue(sg, sg.g, sg.cur.g, sg.next.g);
    sg.value[2] = SubGouraudValue(sg, sg.b, sg.cur.b, sg.next.b);
}

// `setup`: the native stepper right after setup; `skip`: the native pixels skipped before the first one
SubGouraud MakeSubGouraud(GouraudStepper setup, uint n, uint skip, bool ceilPhase) {
    SubGouraud sg;
    sg.n = n;
    sg.P = skip;
    sg.s = 0;
    sg.ceilPhase = ceilPhase;
    sg.r = MakeSubGouraudChannel(setup.r);
    sg.g = MakeSubGouraudChannel(setup.g);
    sg.b = MakeSubGouraudChannel(setup.b);
    sg.cur = setup;
    GouraudSkip(sg.cur, skip);
    sg.next = sg.cur;
    GouraudStep(sg.next);
    sg.value[0] = 0;
    sg.value[1] = 0;
    sg.value[2] = 0;
    SubGouraudUpdate(sg);
    return sg;
}

void SubGouraudStep(inout SubGouraud sg) {
    if (++sg.s == sg.n) {
        sg.s = 0;
        ++sg.P;
        sg.cur = sg.next;
        GouraudStep(sg.next);
    }
    SubGouraudUpdate(sg);
}

uint SubGouraudBlend(SubGouraud sg, uint base) {
    const uint r = uint(clamp(sg.value[0] + int(base & 0x1F) - 16, 0, 31));
    const uint g = uint(clamp(sg.value[1] + int((base >> 5) & 0x1F) - 16, 0, 31));
    const uint b = uint(clamp(sg.value[2] + int((base >> 10) & 0x1F) - 16, 0, 31));
    return r | (g << 5) | (b << 10) | (base & 0x8000);
}

// ---------------------------------------------------------------------------------------------------------------------
// Rect

// The states of one native pixel's sub-steps (stepping order): drawn and the colour; `count` sub-steps reached
struct PixelStates {
    bool drawn[4];
    uint color[4];
    uint count;
};

PixelStates NoStates() {
    PixelStates st;
    [unroll] for (uint k = 0; k < 4; ++k) {
        st.drawn[k] = false;
        st.color[k] = 0;
    }
    st.count = 0;
    return st;
}

// Native pixel p's N x N block row: slot N p + s. Forwards sub-texel s takes sub-step s of p; backwards sub-texel 0 is
// sub-step 0 of p and sub-texel N - s (s > 0) sub-step s of p - 1 (pixel 0: its own sub-step 0)
void EmitRectBlock(NxPlotter p, inout SlotWriter w, uint n, bool forward, uint pixel, int x, int Y, PixelStates cur,
                   PixelStates prev) {
    for (uint s = 0; s < n; ++s) {
        bool has;
        bool drawn;
        uint color;
        if (forward || s == 0 || pixel == 0) {
            const uint k = forward ? s : 0;
            has = k < cur.count;
            drawn = cur.drawn[k];
            color = cur.color[k];
        } else {
            has = true;
            drawn = prev.drawn[s];
            color = prev.color[s];
        }
        if (!has || !drawn) {
            continue;
        }
        const uint i = forward || s == 0 ? s : n - s;
        NxPlot(p, w, int(n) * x + int(i), Y, n * pixel + s, color);
    }
}

void RasterRect(NxSpan span, CommandParams cmd, inout SlotWriter w, uint n) {
    const NxPlotter p = MakePlotter(cmd, n);
    const int ax = asint(span.x0), ay = asint(span.y0), bx = asint(span.x1), by = asint(span.y1);
    if (LineSystemClipped(p.clip, ax, ay, bx, by)) {
        return;
    }
    const uint df = cmd.drawFlags;
    const uint rf = cmd.rasterFlags;
    LineStepper ln = MakeLineStepper(ax, ay, bx, by, Vdp1CmdAntiAlias(df));
    const uint skip = LineSystemClip(ln, p.clip.sysH, p.clip.sysV);
    if (ln.xMajInc == 0) {
        // not a horizontal row (the reference throws; the generator never emits one): skipped, counted
        uint words, stride;
        g_batchState.GetDimensions(words, stride);
        if (kBatchStateBadRectSpans < words) {
            InterlockedAdd(g_batchState[kBatchStateBadRectSpans], 1);
        }
        return;
    }
    const bool forward = ln.xMajInc > 0;
    const int Y = int(n) * ay + int(Vdp1NxSpanSubRow(span.flags));

    SubGouraud gouraud;
    if (p.gouraudEnable) {
        gouraud = MakeSubGouraud(MakeGouraudStepper(ln.dmaj + 1, Vdp1Lo16(span.gouraudLR), Vdp1Hi16(span.gouraudLR)),
                                 n, skip, !forward);
    } else {
        gouraud = MakeSubGouraud(MakeGouraudStepper(1, 0, 0), n, 0, false);
    }

    PixelStates prev = NoStates();
    uint pixel = 0;
    if (!Vdp1CmdTextured(df)) {
        const uint color = cmd.color & 0xFFFF;
        for (LineStep(ln); LineCanStep(ln); LineStep(ln)) {
            PixelStates cur = NoStates();
            for (uint s = 0; s < n; ++s) {
                cur.drawn[s] = true;
                cur.color[s] = PlotColor(p, color, SubGouraudBlend(gouraud, color));
                cur.count = s + 1;
                if (p.gouraudEnable) {
                    SubGouraudStep(gouraud);
                }
            }
            EmitRectBlock(p, w, n, forward, pixel, ln.x, Y, cur, prev);
            prev = cur;
            ++pixel;
        }
        return;
    }

    const uint colorMode = Vdp1CmdColorMode(df);
    const bool endCodeDisable = Vdp1CmdEndCodeDisable(df);
    const bool transparentPixelDisable = Vdp1CmdTransparentPixelDisable(df);
    const uint charSizeH = max(Vdp1Lo16(cmd.charSize), 1u);
    int uStart = 0;
    int uEnd = int(charSizeH) - 1;
    if (Vdp1CmdFlipH(df)) {
        const int t = uStart;
        uStart = uEnd;
        uEnd = t;
    }
    const bool hss = Vdp1CmdHighSpeedShrink(df) && ln.dmaj < charSizeH - 1; // decided natively
    TextureStepper u = MakeTextureStepper(ln.dmaj + 1, uStart, uEnd, hss, Vdp1RasEvenOddCoordSelect(rf) ? 1 : 0);
    TextureSkipPixels(u, skip);
    const int v0 = u.value;
    const uint v = Vdp1NxSpanTexV(span.flags);
    TexelState texel;
    texel.color = 0;
    texel.transparent = true;
    texel.hasEndCode = false;
    texel.endCodeCount = hss ? kIntMin : 0;
    ReadTexel(cmd, colorMode, endCodeDisable, uint(v0), v, charSizeH, texel);
    uint count = 0; // native texel steps (catch-up loops)
    uint read = 0;  // texels read
    bool ended = false;
    for (LineStep(ln); LineCanStep(ln) && !ended; LineStep(ln)) {
        const uint P = skip + ln.step - 1;
        while (TextureShouldStepTexel(u)) {
            TextureStepTexel(u);
            ++count;
        }
        const uint next = NextTexelCount(u, count);
        PixelStates cur = NoStates();
        for (uint s = 0; s < n; ++s) {
            const uint K = s == 0 ? count
                                  : clamp(PhaseCount(uint(u.num), uint(u.den), n, P, s, !forward), count, max(count, next));
            while (read < K) {
                ++read;
                ReadTexel(cmd, colorMode, endCodeDisable, uint(v0 + u.inc * int(read)), v, charSizeH, texel);
                if (texel.endCodeCount == 2) {
                    break;
                }
            }
            if (texel.endCodeCount == 2) {
                ended = true; // the line ends before this sub-step
                break;
            }
            cur.drawn[s] = !(texel.hasEndCode || (texel.transparent && !transparentPixelDisable));
            cur.color[s] = PlotColor(p, texel.color, SubGouraudBlend(gouraud, texel.color));
            cur.count = s + 1;
            if (p.gouraudEnable) {
                SubGouraudStep(gouraud);
            }
        }
        TextureStepPixel(u);
        EmitRectBlock(p, w, n, forward, pixel, ln.x, Y, cur, prev);
        prev = cur;
        ++pixel;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Quad

void RasterQuad(NxSpan span, CommandParams cmd, inout SlotWriter w, uint n) {
    const NxPlotter p = MakePlotter(cmd, n);
    const uint df = cmd.drawFlags;
    const uint rf = cmd.rasterFlags;
    // The N x extent of the system clip area: native pixel x covers N x columns N x .. N x + N - 1
    const int maxX = int(n) * (p.clip.sysH + 1) - 1;
    const int maxY = int(n) * (p.clip.sysV + 1) - 1;
    LineStepper ln = MakeLineStepperShifted(asint(span.x0), asint(span.y0), asint(span.x1), asint(span.y1),
                                            Vdp1NxSpanAntiAlias(span.flags), 0);
    const uint skip = LineSystemClip(ln, maxX, maxY);
    GouraudStepper gouraud = MakeGouraudStepper(1, 0, 0);
    if (p.gouraudEnable) {
        gouraud = MakeGouraudStepper(ln.dmaj + 1, Vdp1Lo16(span.gouraudLR), Vdp1Hi16(span.gouraudLR));
        GouraudSkip(gouraud, skip);
    }

    bool aa = false;
    if (!Vdp1CmdTextured(df)) {
        const uint color = cmd.color & 0xFFFF;
        for (LineStep(ln); LineCanStep(ln); aa = LineStep(ln)) {
            const uint c = PlotColor(p, color, GouraudBlend(gouraud, color));
            NxPlot(p, w, ln.x, ln.y, 2 * (ln.step - 1), c);
            if (aa) {
                NxPlot(p, w, ln.x + ln.aaXInc, ln.y + ln.aaYInc, 2 * (ln.step - 1) + 1, c);
            }
            if (p.gouraudEnable) {
                GouraudStep(gouraud);
            }
        }
        return;
    }

    const uint colorMode = Vdp1CmdColorMode(df);
    const bool endCodeDisable = Vdp1CmdEndCodeDisable(df);
    const bool transparentPixelDisable = Vdp1CmdTransparentPixelDisable(df);
    const uint charSizeH = max(Vdp1Lo16(cmd.charSize), 1u);
    int uStart = 0;
    int uEnd = int(charSizeH) - 1;
    if (Vdp1CmdFlipH(df)) {
        const int t = uStart;
        uStart = uEnd;
        uEnd = t;
    }
    const bool hss = Vdp1CmdHighSpeedShrink(df) && ln.dmaj / n < charSizeH - 1; // the native-equivalent length
    TextureStepper u = MakeTextureStepper(ln.dmaj + 1, uStart, uEnd, hss, Vdp1RasEvenOddCoordSelect(rf) ? 1 : 0);
    TextureSkipPixels(u, skip);
    const uint v = Vdp1NxSpanTexV(span.flags);
    TexelState texel;
    texel.color = 0;
    texel.transparent = true;
    texel.hasEndCode = false;
    texel.endCodeCount = hss ? kIntMin : 0;
    ReadTexel(cmd, colorMode, endCodeDisable, uint(u.value), v, charSizeH, texel);

    for (LineStep(ln); LineCanStep(ln); aa = LineStep(ln)) {
        while (TextureShouldStepTexel(u)) {
            TextureStepTexel(u);
            ReadTexel(cmd, colorMode, endCodeDisable, uint(u.value), v, charSizeH, texel);
            if (texel.endCodeCount == 2) {
                break;
            }
        }
        if (texel.endCodeCount == 2) {
            return;
        }
        TextureStepPixel(u);
        if (!(texel.hasEndCode || (texel.transparent && !transparentPixelDisable))) {
            const uint c = PlotColor(p, texel.color, GouraudBlend(gouraud, texel.color & 0xFFFF));
            NxPlot(p, w, ln.x, ln.y, 2 * (ln.step - 1), c);
            if (aa) {
                NxPlot(p, w, ln.x + ln.aaXInc, ln.y + ln.aaYInc, 2 * (ln.step - 1) + 1, c);
            }
        }
        if (p.gouraudEnable) {
            GouraudStep(gouraud);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------

void RasterSpan(uint3 id) {
    const uint n = g_batch.scale;
    if (id.x >= g_batch.spanCount || n < 2 || n > kMaxNxScale) {
        return;
    }
    s_scale = n;
    uint spanCount, spanStride, cmdCount, cmdStride, fragCapacity, fragStride;
    g_spans.GetDimensions(spanCount, spanStride);
    g_commands.GetDimensions(cmdCount, cmdStride);
    g_fragments.GetDimensions(fragCapacity, fragStride);
    const uint spanIndex = g_batch.spanOffset + id.x;
    if (spanIndex >= spanCount) {
        return;
    }
    const NxSpan span = g_spans[spanIndex];
    const uint cmdIndex = g_batch.cmdOffset + span.cmdIndex;
    if (cmdIndex >= cmdCount) {
        return;
    }
    const CommandParams cmd = g_commands[cmdIndex];

    // The span's slots: up to the next span's first slot (the last span's: the sub-batch's slot count)
    const uint slotEnd = id.x + 1 < g_batch.spanCount && spanIndex + 1 < spanCount ? g_spans[spanIndex + 1].slotBase
                                                                                   : g_batch.fragmentCount;
    const uint limit = min(g_batch.fragmentCount, fragCapacity);
    const uint room = span.slotBase < limit ? limit - span.slotBase : 0;
    SlotWriter writer;
    writer.fragBase = span.slotBase;
    writer.next = 0;
    writer.end = min(slotEnd > span.slotBase ? slotEnd - span.slotBase : 0, room);

    switch (Vdp1NxSpanKind(span.flags)) {
    case kNxSpanRect: RasterRect(span, cmd, writer, n); break;
    case kNxSpanQuad: RasterQuad(span, cmd, writer, n); break;
    case kNxSpanReplicate:
        RasterNativeLine(cmd, asint(span.x0), asint(span.y0), asint(span.x1), asint(span.y1), span.gouraudLR,
                         Vdp1NxSpanTexV(span.flags), writer, false, 0);
        break;
    default: break;
    }

    // The slots after the last fragment
    ZeroSlotsUpTo(writer, writer.end);
}
