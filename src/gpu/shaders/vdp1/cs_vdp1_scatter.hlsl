// Brimir: VDP1 scatter pass (GPU stage 2; research section 4.3). Brimir only.
//
// Each live fragment slot i < fragmentCount writes i into its unit's bucket: position g_scanSums[unit / 64] +
// g_unitOffsets[unit], the offset advanced atomically, so a bucket's order is arbitrary (the resolve sorts it by slot,
// the order key). After the pass g_unitOffsets[unit] is the end of the unit's bucket within its block.
// Group g handles the kScatterSlotsPerGroup (256) slots from 256 g, thread t the slots 256 g + t + 64 k (k < 4).
// Bindings (vdp1_bindings.hpp kScatterBindings): fragments (16), bucket (18), scan sums (20), unit offsets (21).
// Push constants: BatchParams (fragmentCount).

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] BatchParams g_batch;

RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_bucket : register(u2);
RWStructuredBuffer<uint> g_scanSums : register(u4);
RWStructuredBuffer<uint> g_unitOffsets : register(u5);

static const uint kSlotsPerThread = 4;

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint fragCapacity, bucketCapacity, sums, offsets, stride;
    g_fragments.GetDimensions(fragCapacity, stride);
    g_bucket.GetDimensions(bucketCapacity, stride);
    g_scanSums.GetDimensions(sums, stride);
    g_unitOffsets.GetDimensions(offsets, stride);
    const uint count = min(g_batch.fragmentCount, fragCapacity);

    for (uint k = 0; k < kSlotsPerThread; ++k) {
        const uint slot = gid.x * (64 * kSlotsPerThread) + k * 64 + tid.x;
        if (slot >= count) {
            break;
        }
        const uint target = g_fragments[slot].target;
        if (!Vdp1FragLive(target)) {
            continue;
        }
        const uint unit = Vdp1FragUnit(target);
        if (unit >= offsets || (unit >> 6) >= sums) {
            continue;
        }
        uint within;
        InterlockedAdd(g_unitOffsets[unit], 1, within);
        const uint position = g_scanSums[unit >> 6] + within;
        if (position < bucketCapacity) {
            g_bucket[position] = slot;
        }
    }
}
