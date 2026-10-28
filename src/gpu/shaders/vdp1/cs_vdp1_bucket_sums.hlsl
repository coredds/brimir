// Brimir: VDP1 bucket pass, second level: the exclusive scan of the block totals cs_vdp1_bucket.hlsl wrote, in place
// (GPU stage 2; research section 4.3). Brimir only.
//
// One group of 64 threads over kScanBlockCount = 4096 sums: thread t adds up sums [64 t, 64 t + 64), the group scans the
// 64 totals in shared memory, and thread t rewrites its sums as their exclusive prefix.
// Bindings (vdp1_bindings.hpp kBucketSumsBindings): scan sums (20). No push constants.

#include "vdp1_defs.hlsli"

RWStructuredBuffer<uint> g_scanSums : register(u4);

static const uint kBlocks = kUnitCount / 64;

groupshared uint gs_scan[64];

[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_GroupThreadID) {
    const uint t = tid.x;
    uint sums, stride;
    g_scanSums.GetDimensions(sums, stride);
    const uint first = t * 64;
    const uint last = min(first + 64, min(kBlocks, sums));

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
