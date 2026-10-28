// Brimir: VDP1 bucket pass, first level of the exclusive scan of the per-unit fragment counts (GPU stage 2; research
// section 4.3). Brimir only.
//
// Group g (kScanBlockCount = 4096 groups) scans the counts of units [64 g, 64 g + 64): g_unitOffsets[u] = the counts of
// the block's units before u, g_scanSums[g] = the block's total. cs_vdp1_bucket_sums.hlsl then turns g_scanSums into the
// blocks' exclusive prefix, so unit u's bucket starts at g_scanSums[u / 64] + g_unitOffsets[u] (the scatter and resolve
// passes add the two). A shared-memory scan (no subgroup operations: Vulkan 1.2 guarantees only basic ones).
// Bindings (vdp1_bindings.hpp kBucketBindings): unit counts (17), scan sums (20), unit offsets (21). No push constants.

#include "vdp1_defs.hlsli"

RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWStructuredBuffer<uint> g_scanSums : register(u4);
RWStructuredBuffer<uint> g_unitOffsets : register(u5);

groupshared uint gs_scan[64];

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint t = tid.x;
    const uint unit = gid.x * 64 + t;
    uint counts, offsets, sums, stride;
    g_unitCounts.GetDimensions(counts, stride);
    g_unitOffsets.GetDimensions(offsets, stride);
    g_scanSums.GetDimensions(sums, stride);

    const uint count = unit < kUnitCount && unit < counts ? g_unitCounts[unit] : 0;
    gs_scan[t] = count;
    GroupMemoryBarrierWithGroupSync();
    // Hillis-Steele inclusive scan
    for (uint offset = 1; offset < 64; offset <<= 1) {
        const uint add = t >= offset ? gs_scan[t - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_scan[t] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    if (unit < kUnitCount && unit < offsets) {
        g_unitOffsets[unit] = gs_scan[t] - count;
    }
    if (t == 63 && gid.x < sums) {
        g_scanSums[gid.x] = gs_scan[63];
    }
}
