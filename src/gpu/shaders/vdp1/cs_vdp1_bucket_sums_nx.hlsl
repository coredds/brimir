// Brimir: N x VDP1 bucket sums, first level (GPU stage 3; research section 1.4). Brimir only.
//
// An N x range has up to N^2 * 4096 block totals (cs_vdp1_bucket.hlsl), beyond one group's 4096 (cs_vdp1_bucket_sums
// .hlsl). The blocks are cut into N^2 chunks of kNxScanChunkBlocks (4096): blocks [4096 c, 4096 c + 4096). Group c
// (N^2 groups, dispatched directly) scans the range's blocks of chunk c in place, exclusive within the chunk, as
// bucket_sums scans the native range (thread t adds up its share of consecutive sums, the group scans the 64 totals in
// shared memory, thread t rewrites its sums), and writes the chunk's total to g_scanSums[N^2 * 4096 + c] (0 for a
// chunk outside the range). cs_vdp1_bucket_sums_nx_add.hlsl then adds the totals of the chunks before each chunk.
// Bindings (vdp1_bindings.hpp kBucketSumsNxBindings): scan sums (20, Vdp1NxScanSumsCount(N)), batch state (22). Push
// constants: BatchParams (scale = N, 2 to 4).

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] BatchParams g_batch;

RWStructuredBuffer<uint> g_scanSums : register(u4);
RWStructuredBuffer<uint> g_batchState : register(u6);

groupshared uint gs_scan[64];

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint t = tid.x;
    const uint scale = g_batch.scale;
    const uint chunk = gid.x;
    uint sums, words, stride;
    g_scanSums.GetDimensions(sums, stride);
    g_batchState.GetDimensions(words, stride);
    const uint blocks = Vdp1NxScanBlockCount(scale);
    if (scale < 2 || scale > kMaxNxScale || chunk >= scale * scale || blocks + chunk >= sums) {
        return;
    }
    const uint unitLo = words > kBatchStateUnitLo ? g_batchState[kBatchStateUnitLo] : 0;
    const uint unitHi = words > kBatchStateUnitHi ? g_batchState[kBatchStateUnitHi] : 0;
    const uint blockLo = max(unitLo >> 6, chunk * kNxScanChunkBlocks);
    const uint blockHi = min(min((unitHi + 63) >> 6, (chunk + 1) * kNxScanChunkBlocks), blocks);
    const uint count = blockHi > blockLo ? blockHi - blockLo : 0;
    const uint share = (count + 63) / 64;
    const uint first = min(blockLo + t * share, blockLo + count);
    const uint last = min(first + share, blockLo + count);

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
    if (t == 63) {
        g_scanSums[blocks + chunk] = gs_scan[63];
    }
}
