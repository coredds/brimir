// Brimir: layout probe of the N x VDP1 GPU structs (GPU stage 3). It declares the N x span buffer, the N x surface and
// every N x push-constant struct, so tools/shaders/build_shaders.cmake reflects their std430 layouts into
// generated/vdp1_nx_layout_probe.layout.h, which tests/unit/test_gpu_vdp1_params.cpp compares with the host structs of
// src/gpu/vdp1/vdp1_hw_params.hpp (Vdp1NxSpan, Vdp1EraseNxPushConstants, Vdp1FbWriteNxPushConstants,
// Vdp1RebuildNxPushConstants). It also evaluates the N x accessors of vdp1_defs.hlsli, so that test can run them against
// the C++ copies on random words.
//
// Thread i (i < batch.spanCount) reads element i of the N x span and fragment buffers and a word of the N x surface and
// writes kVdp1NxProbeWords words of results into g_unitCounts at i * kVdp1NxProbeWords (the table in the test,
// NxProbeExpected).
// Bindings (src/gpu/vdp1/vdp1_bindings.hpp kNxLayoutProbeBindings): N x spans (1), fragments (16), results (17), N x
// surface (24). Push constants: NxProbePush, the N x push-constant structs after BatchParams.

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

static const uint kVdp1NxProbeWords = 30;

struct NxProbePush {
    BatchParams batch;
    EraseNxParams erase;
    FbWriteNxParams fbWrite;
    RebuildNxParams rebuild;
};
[[vk::push_constant]] NxProbePush g_probe;

StructuredBuffer<NxSpan> g_spans : register(t1);
RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWByteAddressBuffer g_fbramNx : register(u8);

#define EMIT(value) g_unitCounts[o + k++] = (value)

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_probe.batch.spanCount) {
        return;
    }
    const uint i = id.x;
    const uint o = i * kVdp1NxProbeWords;
    uint k = 0;

    const NxSpan s = g_spans[i];
    const Fragment f = g_fragments[i];
    const uint scale = g_probe.batch.scale;

    // 0-7: raw words
    EMIT(s.x0);
    EMIT(s.y0);
    EMIT(s.x1);
    EMIT(s.y1);
    EMIT(s.cmdIndex);
    EMIT(s.slotBase);
    EMIT(s.gouraudLR);
    EMIT(s.flags);
    // 8-12: flags
    EMIT(Vdp1NxSpanKind(s.flags));
    EMIT(Vdp1NxSpanAntiAlias(s.flags));
    EMIT(Vdp1NxSpanSubRow(s.flags));
    EMIT(Vdp1NxSpanTexV(s.flags));
    EMIT(Vdp1MakeNxSpanFlags(Vdp1NxSpanKind(s.flags), Vdp1NxSpanAntiAlias(s.flags), Vdp1NxSpanSubRow(s.flags),
                             Vdp1NxSpanTexV(s.flags)));
    // 13-16: N x targets
    EMIT(Vdp1NxFragUnit(f.target));
    EMIT(Vdp1NxFragNativeTarget(f.target));
    EMIT(Vdp1MakeNxFragmentTarget(f.color, f.target));
    EMIT(Vdp1MakeNxFragmentTarget(Vdp1NxFragUnit(f.target), Vdp1NxFragNativeTarget(f.target)));
    // 17-22: N x sizes and addresses
    EMIT(Vdp1NxUnit(f.target & kFragUnitMask, f.color & 15, scale));
    EMIT(Vdp1NxUnitCount(scale));
    EMIT(Vdp1NxFbramSliceSize(scale));
    EMIT(Vdp1NxFbramBufferSize(scale));
    EMIT(Vdp1NxFbramByteAddress(s.cmdIndex, s.slotBase, scale));
    EMIT(Vdp1NxScanBlockCount(scale));
    // 23: the N x surface, a word (masked to the buffer)
    uint size;
    g_fbramNx.GetDimensions(size);
    EMIT(size >= 4 ? g_fbramNx.Load(((i * 4) % size) & ~3u) : 0);
    // 24-29: the push constants
    EMIT(g_probe.erase.erase.x1 + g_probe.erase.erase.x3 + g_probe.erase.erase.y1 + g_probe.erase.erase.y3);
    EMIT(g_probe.erase.erase.value ^ g_probe.erase.erase.offsetShift ^ g_probe.erase.erase.writeLimit ^
         g_probe.erase.erase.flags);
    EMIT(g_probe.erase.scale);
    EMIT(g_probe.fbWrite.run.offset + g_probe.fbWrite.run.count);
    EMIT(g_probe.fbWrite.scale);
    EMIT((g_probe.rebuild.sliceMask * 3) ^ g_probe.rebuild.scale ^ g_probe.batch.spanOffset ^ g_probe.batch.cmdOffset ^
         g_probe.batch.fragmentCount ^ g_probe.batch.drawFB);
}
