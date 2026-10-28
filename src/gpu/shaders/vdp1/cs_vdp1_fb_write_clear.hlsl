// Brimir: VDP1 CPU framebuffer write run, pass 3 of 3: each record zeroes the winner slots of the bytes it wrote
// (every record of a slot stores the same 0), so the winners buffer is zero for the next run. See
// include/vdp1_fb_write.hlsli. Brimir only.
// Bindings (vdp1_bindings.hpp kFbWriteClearBindings): FbWrite records (3), winners (23). Push constants: FbWriteParams.

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] FbWriteParams g_params;

StructuredBuffer<FbWrite> g_fbWrites : register(t3);
RWStructuredBuffer<uint> g_fbWinners : register(u7);

#include "vdp1_fb_write.hlsli"

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    FbWrite w;
    uint slots[4];
    bool writes[4];
    if (!FbWriteRecord(id.x, w, slots, writes)) {
        return;
    }
    [unroll] for (uint s = 0; s < 4; ++s) {
        if (writes[s]) {
            g_fbWinners[slots[s]] = 0;
        }
    }
}
