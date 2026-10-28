// Brimir: VDP1 CPU framebuffer write run, pass 2 of 3 (VDP1WriteFB as the front half recorded it; GPU stage 2): each
// record writes the bytes whose winner it is, so every byte gets the value of the run's last record that writes it, the
// in-order result of the reference's ApplyFbWrites. Winners are distinct per byte; adjacent words share a buffer uint,
// hence the atomic masked writes. See include/vdp1_fb_write.hlsli. Brimir only.
// Bindings (vdp1_bindings.hpp kFbWriteBindings): FbWrite records (3), FBRAM (19), winners (23). Push constants:
// FbWriteParams.

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] FbWriteParams g_params;

StructuredBuffer<FbWrite> g_fbWrites : register(t3);
RWByteAddressBuffer g_fbram : register(u3);
RWStructuredBuffer<uint> g_fbWinners : register(u7);

#include "vdp1_fb_write.hlsli"
#include "vdp1_fbram.hlsli"

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    FbWrite w;
    uint slots[4];
    bool writes[4];
    if (!FbWriteRecord(id.x, w, slots, writes)) {
        return;
    }
    const uint word = Vdp1FbWriteWord(w.address);
    const uint fb = Vdp1FbWriteFb(w.address);
    uint mainMask = 0;
    uint altMask = 0;
    [unroll] for (uint s = 0; s < 4; ++s) {
        if (writes[s] && g_fbWinners[slots[s]] == id.x + 1) {
            const uint byteMask = (s & 1) != 0 ? 0x00FFu : 0xFF00u;
            if (s < 2) {
                mainMask |= byteMask;
            } else {
                altMask |= byteMask;
            }
        }
    }
    if (mainMask != 0) {
        FbramWriteWordMasked(Vdp1MainSlice(fb), word, w.value, mainMask);
    }
    if (altMask != 0) {
        FbramWriteWordMasked(Vdp1AltSlice(fb), word, w.value, altMask);
    }
}
