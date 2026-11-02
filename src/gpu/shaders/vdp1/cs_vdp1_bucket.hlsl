// Brimir: VDP1 bucket pass, first level of the exclusive scan of the per-unit fragment counts (GPU stage 2; research
// section 4.3). Brimir only.
//
// Dispatched indirectly (g_batchState kBatchStateBucketArgs: x = min(blocks, 4096), y = ceil(blocks / 4096), 1; y is 1
// for a native range): group (x, y) scans the counts of block b = unitLo / 64 + x + 4096 y of the batch's unit range,
// units [64 b, 64 b + 64): g_unitOffsets[u] = the counts of the block's units before u, g_scanSums[b] = the block's
// total. An N x range (GPU stage 3) has up to 65,536 blocks, beyond the 65,535 groups of one dimension.
// cs_vdp1_bucket_sums.hlsl (N x: cs_vdp1_bucket_sums_nx*.hlsl) then turns the range's g_scanSums into the blocks'
// exclusive prefix, so unit u's bucket starts at g_scanSums[u / 64] + g_unitOffsets[u] (the scatter and resolve passes
// add the two). Blocks outside the range hold no fragment and are not used. A shared-memory scan (no subgroup
// operations: Vulkan 1.2 guarantees only basic ones).
// Bindings (vdp1_bindings.hpp kBucketBindings): unit counts (17), scan sums (20), unit offsets (21), batch state (22).
// No push constants.

#include "vdp1_defs.hlsli"

RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWStructuredBuffer<uint> g_scanSums : register(u4);
RWStructuredBuffer<uint> g_unitOffsets : register(u5);
RWStructuredBuffer<uint> g_batchState : register(u6);

groupshared uint gs_scan[64];

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint t = tid.x;
    uint counts, offsets, sums, words, stride;
    g_unitCounts.GetDimensions(counts, stride);
    g_unitOffsets.GetDimensions(offsets, stride);
    g_scanSums.GetDimensions(sums, stride);
    g_batchState.GetDimensions(words, stride);
    const uint unitHi = words > kBatchStateUnitHi ? g_batchState[kBatchStateUnitHi] : 0;
    const uint block = (words > kBatchStateUnitLo ? g_batchState[kBatchStateUnitLo] >> 6 : 0) + gid.x +
                       gid.y * kScanBlockCount;
    const uint unit = block * 64 + t;
    const bool inRange = block < (unitHi + 63) >> 6 && unit < kMaxNxUnitCount;

    const uint count = inRange && unit < counts ? g_unitCounts[unit] : 0;
    gs_scan[t] = count;
    GroupMemoryBarrierWithGroupSync();
    // Hillis-Steele inclusive scan
    for (uint offset = 1; offset < 64; offset <<= 1) {
        const uint add = t >= offset ? gs_scan[t - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_scan[t] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    if (inRange && unit < offsets) {
        g_unitOffsets[unit] = gs_scan[t] - count;
    }
    if (t == 63 && inRange && block < sums) {
        g_scanSums[block] = gs_scan[63];
    }
}
