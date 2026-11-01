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
//   slots partition [0, fragmentCount), so the batch's fragment buffer needs no clearing;
// - the group's lowest and highest counted unit go into the batch's range accumulator (g_batchState; one atomic pair
//   per group), from which batch_range sizes the bucket and resolve dispatches.
// The line loop is shared with the N x raster (include/vdp1_raster_line.hlsli).
// Bindings (src/gpu/vdp1/vdp1_bindings.hpp kRasterBindings): commands (0), spans (1), VDP1 VRAM (2), fragments (16),
// unit counts (17), batch state (22). Push constants: BatchParams (scale is 1: stage 2 draws at native resolution).

#define VDP1_RASTER_NX 0

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] BatchParams g_batch;

StructuredBuffer<CommandParams> g_commands : register(t0);
StructuredBuffer<Span> g_spans : register(t1);
ByteAddressBuffer g_vram : register(t2);
RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWStructuredBuffer<uint> g_batchState : register(u6);

#include "vdp1_raster_line.hlsli"

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID) {
    RasterMain(id, tid);
}

void RasterSpan(uint3 id) {
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

    SlotWriter writer;
    writer.fragBase = span.fragBase;
    writer.next = 0;
    writer.end = 0;
    const uint limit = min(g_batch.fragmentCount, fragCapacity);
    const uint room = span.fragBase < limit ? limit - span.fragBase : 0;
    RasterNativeLine(cmd, asint(span.x0), asint(span.y0), asint(span.x1), asint(span.y1), span.gouraudLR,
                     Vdp1SpanTexV(span.stepTexV), writer, true, room);

    // The slots after the last fragment
    ZeroSlotsUpTo(writer, writer.end);
}
