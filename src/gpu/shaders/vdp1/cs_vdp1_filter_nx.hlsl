// Brimir: N x VDP1 filter pass of a mixed sub-batch (GPU stage 3, the overwrite-class fast path's hybrid; research
// section 1.6). Brimir only.
//
// In a hybrid sub-batch (BatchParams::drawFB kBatchHybrid, vdp1_hw_params.hpp: one pixel depth and transparent-meshes
// setting) some fragments are overwrite-class (Vdp1NxFragOverwrite) and the others read what they write (MSB-on,
// shadow, half-transparency) or write the mesh framebuffer. An overwrite-class fragment replaces every bit any fragment
// of the sub-batch writes at its word (8 bpp: its byte; with transparent meshes on, the only setting with mesh
// framebuffer writes, it also clears the mesh word's), so applying a unit's fragments in slot order equals applying its
// last overwrite-class fragment (the winner, per byte at 8 bpp) and the fragments after it. raster_nx kept the winners'
// slot + 1 in g_unitOffsets (Vdp1NxWinnerIndex); this pass counts the fragments that matter (the winner and the later
// ones) for the ordered passes that follow (bucket .. resolve_nx) and marks the others dead (the live bit cleared), so
// scatter and resolve skip them. The bucket pass then overwrites the first half of g_unitOffsets.
// Thread t of group g takes slots 256 g + t + 64 k (k < 4), as scatter.
// Bindings (vdp1_bindings.hpp kFilterNxBindings): fragments (16), unit counts (17), unit offsets (21, the winners).
// Push constants: BatchParams (fragmentCount).

#include "vdp1_defs.hlsli"
#include "vdp1_overwrite_nx.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] BatchParams g_batch;

RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWStructuredBuffer<uint> g_unitOffsets : register(u5);

static const uint kSlotsPerThread = 4;

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint fragCapacity, counts, offsets, stride;
    g_fragments.GetDimensions(fragCapacity, stride);
    g_unitCounts.GetDimensions(counts, stride);
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
        const uint unit = Vdp1NxFragUnit(target);
        const uint native = Vdp1NxFragNativeTarget(target);
        const uint index = Vdp1NxWinnerIndex(unit, native, g_batch.scale);
        if (unit >= counts || index >= offsets) {
            continue;
        }
        const uint winner = g_unitOffsets[index];
        const bool keep = Vdp1NxFragOverwrite(native) ? slot + 1 == winner : slot + 1 > winner;
        if (keep) {
            InterlockedAdd(g_unitCounts[unit], 1);
        } else {
            g_fragments[slot].target = target & ~kFragLiveBit;
        }
    }
}
