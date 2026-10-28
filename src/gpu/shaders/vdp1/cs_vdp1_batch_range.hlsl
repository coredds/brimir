// Brimir: VDP1 batch range pass (GPU stage 2). Brimir only.
//
// Thread 0 turns the raster pass's accumulator (InterlockedMax of ~unit and of unit + 1 over the batch's fragments;
// zero when there is none) into the batch's unit range [unitLo, unitHi) and the indirect dispatch arguments of the
// bucket pass (one group per 64-unit block of the range) and the resolve pass (min(unitHi - unitLo, 4096) groups: as many
// as possible, so the units of one group lie far apart),
// zero groups for an empty batch, then resets the accumulator for the next batch.
// Bindings (vdp1_bindings.hpp kBatchRangeBindings): batch state (22). No push constants.

#include "vdp1_defs.hlsli"

RWStructuredBuffer<uint> g_batchState : register(u6);

[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_GroupThreadID) {
    uint words, stride;
    g_batchState.GetDimensions(words, stride);
    if (tid.x != 0 || words <= kBatchStateAccHi) {
        return;
    }
    const uint accHi = g_batchState[kBatchStateAccHi];
    uint hi = min(accHi, kUnitCount);
    uint lo = ~g_batchState[kBatchStateAccLoInv];
    if (accHi == 0 || lo >= hi) {
        lo = 0;
        hi = 0;
    }
    const uint blockLo = lo >> 6;
    const uint blockHi = (hi + 63) >> 6;
    g_batchState[kBatchStateBucketArgs + 0] = blockHi - blockLo;
    g_batchState[kBatchStateBucketArgs + 1] = 1;
    g_batchState[kBatchStateBucketArgs + 2] = 1;
    g_batchState[kBatchStateResolveArgs + 0] = min(hi - lo, kUnitCount / 64);
    g_batchState[kBatchStateResolveArgs + 1] = 1;
    g_batchState[kBatchStateResolveArgs + 2] = 1;
    g_batchState[kBatchStateUnitLo] = lo;
    g_batchState[kBatchStateUnitHi] = hi;
    g_batchState[kBatchStateAccLoInv] = 0;
    g_batchState[kBatchStateAccHi] = 0;
}
