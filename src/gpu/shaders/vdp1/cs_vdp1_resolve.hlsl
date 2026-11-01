// Brimir: VDP1 resolve pass, the per-FBRAM-word ordered resolve (GPU stage 2; research section 4.3). Written from the
// CPU reference's Resolve (tests/unit/vdp1_raster_reference.cpp). Brimir only.
//
// The unit is an FBRAM word of the draw framebuffer, field u >> 17: word u & 0x1FFFF of the main framebuffer (field 0)
// or the alternate one (field 1), with the mesh word of its field. The ordered resolve itself (the dispatch, the small
// and large paths) is include/vdp1_resolve.hlsli, shared with the N x resolve.
// Bindings (vdp1_bindings.hpp kResolveBindings): fragments (16), unit counts (17), bucket (18), FBRAM (19), scan sums
// (20), unit offsets (21), batch state (22). Push constants: BatchParams (drawFB).

#define VDP1_RESOLVE_NATIVE 1

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] BatchParams g_batch;

RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWStructuredBuffer<uint> g_bucket : register(u2);
RWByteAddressBuffer g_fbram : register(u3);
RWStructuredBuffer<uint> g_scanSums : register(u4);
RWStructuredBuffer<uint> g_unitOffsets : register(u5);
RWStructuredBuffer<uint> g_batchState : register(u6);

#include "vdp1_fbram.hlsli"

uint ResolveUnitLimit() {
    return kUnitCount;
}

void ResolveUnitWords(uint unit, uint fb, out uint mainAddress, out uint meshAddress) {
    const uint word = unit & kWordIndexMask;
    const uint alt = (unit >> 17) & 1;
    mainAddress = Vdp1FbramByteAddress(alt != 0 ? Vdp1AltSlice(fb) : Vdp1MainSlice(fb), word);
    meshAddress = Vdp1FbramByteAddress(Vdp1MeshSlice(alt, fb), word);
}

uint ResolveReadWord(uint address) {
    return FbramReadWordAt(address);
}

void ResolveReplaceWord(uint address, uint oldValue, uint newValue) {
    FbramReplaceWordAt(address, oldValue, newValue);
}

void ResolveApply(inout uint m, inout uint s, uint target, uint color) {
    ApplyFragment(m, s, target, color);
}

#include "vdp1_resolve.hlsli"
