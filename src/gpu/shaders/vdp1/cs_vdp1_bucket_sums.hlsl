// Brimir: VDP1 bucket pass, second level: the exclusive scan of the block totals cs_vdp1_bucket.hlsl wrote for the
// batch's unit range, in place (GPU stage 2; research section 4.3). Brimir only.
//
// One group of 64 threads over the range's blocks [unitLo / 64, ceil(unitHi / 64)) (at most kScanBlockCount = 4096):
// thread t adds up its share of consecutive sums, the group scans the 64 totals in shared memory, and thread t
// rewrites its sums as their exclusive prefix. An empty range scans nothing.
// Bindings (vdp1_bindings.hpp kBucketSumsBindings): scan sums (20), batch state (22). No push constants.

#include "vdp1_defs.hlsli"

RWStructuredBuffer<uint> g_scanSums : register(u4);
RWStructuredBuffer<uint> g_batchState : register(u6);

static const uint kBlocks = kUnitCount / 64;

groupshared uint gs_scan[64];

[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_GroupThreadID) {
    const uint t = tid.x;
    uint sums, words, stride;
    g_scanSums.GetDimensions(sums, stride);
    g_batchState.GetDimensions(words, stride);
    const uint unitLo = words > kBatchStateUnitLo ? g_batchState[kBatchStateUnitLo] : 0;
    const uint unitHi = words > kBatchStateUnitHi ? g_batchState[kBatchStateUnitHi] : 0;
    const uint blockLo = unitLo >> 6;
    const uint blockHi = min((unitHi + 63) >> 6, min(kBlocks, sums));
    const uint blocks = blockHi > blockLo ? blockHi - blockLo : 0;
    const uint share = (blocks + 63) / 64;
    const uint first = min(blockLo + t * share, blockLo + blocks);
    const uint last = min(first + share, blockLo + blocks);

    uint total = 0;
    for (uint i = first; i < last; ++i) {
        total += g_scanSums[i];
    }
    gs_scan[t] = total;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1; offset < 64; offset <<= 1) {
        const uint add = t >= offset ? gs_scan[t - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_scan[t] += add;
        GroupMemoryBarrierWithGroupSync();
    }

    uint running = gs_scan[t] - total;
    for (uint j = first; j < last; ++j) {
        const uint value = g_scanSums[j];
        g_scanSums[j] = running;
        running += value;
    }
}
