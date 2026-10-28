// Brimir: VDP1 CPU framebuffer write pass (VDP1WriteFB as the front half recorded it; GPU stage 2). Written from the
// CPU reference's ApplyFbWrites (tests/unit/vdp1_raster_reference.cpp). Brimir only.
//
// The records [offset, offset + count) apply in order: each replaces the bits of `mask` of its word of main framebuffer
// fb (and of the alternate field when it mirrors); the mesh framebuffers are left alone. One group of 64 threads takes
// the records in chunks of 64, one per thread. Within a chunk a record writes only the bits no later record of the
// chunk writes on the same word (of main; of the alternate field among the mirroring records), so the chunk's writes
// are disjoint and commute, and the result is the in-order one; a barrier separates the chunks.
// Bindings (vdp1_bindings.hpp kFbWriteBindings): FbWrite records (3), FBRAM (19). Push constants: FbWriteParams.

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] FbWriteParams g_params;

StructuredBuffer<FbWrite> g_fbWrites : register(t3);
globallycoherent RWByteAddressBuffer g_fbram : register(u3);

#include "vdp1_fbram.hlsli"

// 768 bytes (vdp1_bindings.hpp kFbWriteSharedMemorySize)
groupshared uint gs_address[64];
groupshared uint gs_value[64];
groupshared uint gs_mask[64];

[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_GroupThreadID) {
    const uint t = tid.x;
    uint records, stride;
    g_fbWrites.GetDimensions(records, stride);
    const uint first = min(g_params.offset, records);
    const uint end = min(first + min(g_params.count, records - first), records);

    for (uint base = first; base < end; base += 64) {
        const uint index = base + t;
        const bool mine = index < end;
        if (mine) {
            const FbWrite w = g_fbWrites[index];
            gs_address[t] = w.address;
            gs_value[t] = w.value;
            gs_mask[t] = w.mask & 0xFFFF;
        }
        GroupMemoryBarrierWithGroupSync();
        if (mine) {
            const uint address = gs_address[t];
            const uint key = address & 0x3FFFF; // word | fb
            const bool mirror = Vdp1FbWriteMirrorAlt(address);
            uint mainMask = gs_mask[t];
            uint altMask = mirror ? gs_mask[t] : 0;
            const uint chunk = min(64u, end - base);
            for (uint later = t + 1; later < chunk; ++later) {
                const uint other = gs_address[later];
                if ((other & 0x3FFFF) == key) {
                    mainMask &= ~gs_mask[later];
                    if (Vdp1FbWriteMirrorAlt(other)) {
                        altMask &= ~gs_mask[later];
                    }
                }
            }
            const uint word = Vdp1FbWriteWord(address);
            const uint fb = Vdp1FbWriteFb(address);
            if (mainMask != 0) {
                FbramWriteWordMasked(Vdp1MainSlice(fb), word, gs_value[t], mainMask);
            }
            if (altMask != 0) {
                FbramWriteWordMasked(Vdp1AltSlice(fb), word, gs_value[t], altMask);
            }
        }
        AllMemoryBarrierWithGroupSync();
    }
}
