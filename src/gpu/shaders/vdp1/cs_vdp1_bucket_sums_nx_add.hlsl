// Brimir: N x VDP1 bucket sums, second level (GPU stage 3; research section 1.4). Brimir only.
//
// After cs_vdp1_bucket_sums_nx.hlsl each chunk of the range's block totals is its exclusive scan within the chunk and
// g_scanSums[N^2 * 4096 + c] the total of chunk c. Group c (N^2 groups, dispatched directly) adds the totals of chunks
// 0 .. c - 1 (each thread sums them; at most 15) to the range's blocks of chunk c, which makes the range's g_scanSums
// its exclusive prefix, as bucket_sums leaves the native range's.
// Bindings (vdp1_bindings.hpp kBucketSumsNxBindings): scan sums (20, Vdp1NxScanSumsCount(N)), batch state (22). Push
// constants: BatchParams (scale = N, 2 to 4).

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] BatchParams g_batch;

RWStructuredBuffer<uint> g_scanSums : register(u4);
RWStructuredBuffer<uint> g_batchState : register(u6);

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint scale = g_batch.scale;
    const uint chunk = gid.x;
    uint sums, words, stride;
    g_scanSums.GetDimensions(sums, stride);
    g_batchState.GetDimensions(words, stride);
    const uint blocks = Vdp1NxScanBlockCount(scale);
    if (scale < 2 || scale > kMaxNxScale || chunk == 0 || chunk >= scale * scale || blocks + chunk >= sums) {
        return;
    }
    const uint unitLo = words > kBatchStateUnitLo ? g_batchState[kBatchStateUnitLo] : 0;
    const uint unitHi = words > kBatchStateUnitHi ? g_batchState[kBatchStateUnitHi] : 0;
    const uint blockLo = max(unitLo >> 6, chunk * kNxScanChunkBlocks);
    const uint blockHi = min(min((unitHi + 63) >> 6, (chunk + 1) * kNxScanChunkBlocks), blocks);
    if (blockHi <= blockLo) {
        return;
    }
    uint prefix = 0;
    for (uint c = 0; c < chunk; ++c) {
        prefix += g_scanSums[blocks + c];
    }
    if (prefix == 0) {
        return;
    }
    for (uint i = blockLo + tid.x; i < blockHi; i += 64) {
        g_scanSums[i] += prefix;
    }
}
