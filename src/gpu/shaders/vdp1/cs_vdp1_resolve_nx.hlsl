// Brimir: N x VDP1 resolve pass (GPU stage 3; research section 1.4). The native ordered resolve
// (include/vdp1_resolve.hlsli: the same dispatch, small and large paths, applied in key order) with the N x unit decode,
// on the N x surface; tests/unit/vdp1_nx_reference.cpp Resolve is the specification. Brimir only.
//
// The unit is u = (native unit) * N^2 + j * N + i: native unit u / N^2 (word & 0x1FFFF, field >> 17) and sub-texel
// sub = u % N^2. Its words are N x word word * N^2 + sub of the main (field 0) or alternate (field 1) slice of the draw
// framebuffer and of the field's mesh slice. The fragments' N x targets carry the native flags in bits 28-22, which
// ApplyFragment reads in the native layout (Vdp1NxFragNativeTarget). An N x range of up to N^2 * 262,144 units is
// resolved in up to N^2 rounds.
// Bindings (vdp1_bindings.hpp kResolveNxBindings): fragments (16), unit counts (17), bucket (18), scan sums (20), unit
// offsets (21), batch state (22), N x surface (24). Push constants: BatchParams (drawFB, scale = N, 2 to 4).

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] BatchParams g_batch;

RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWStructuredBuffer<uint> g_bucket : register(u2);
RWStructuredBuffer<uint> g_scanSums : register(u4);
RWStructuredBuffer<uint> g_unitOffsets : register(u5);
RWStructuredBuffer<uint> g_batchState : register(u6);
RWByteAddressBuffer g_fbramNx : register(u8);

#include "vdp1_apply_fragment.hlsli"
#include "vdp1_fbram_nx.hlsli"

uint ResolveScale() {
    return g_batch.scale >= 2 && g_batch.scale <= kMaxNxScale ? g_batch.scale : 0;
}

uint ResolveUnitLimit() {
    return Vdp1NxUnitCount(ResolveScale());
}

void ResolveUnitWords(uint unit, uint fb, out uint mainAddress, out uint meshAddress) {
    const uint scale = max(ResolveScale(), 1u);
    const uint sub = scale * scale;
    const uint native = unit / sub;
    const uint index = (native & kWordIndexMask) * sub + (unit - native * sub);
    const uint alt = (native >> 17) & 1;
    mainAddress = Vdp1NxFbramByteAddress(alt != 0 ? Vdp1AltSlice(fb) : Vdp1MainSlice(fb), index, scale);
    meshAddress = Vdp1NxFbramByteAddress(Vdp1MeshSlice(alt, fb), index, scale);
}

uint ResolveReadWord(uint address) {
    return address + 2 <= NxFbramSize() ? NxReadWordAt(address) : 0;
}

void ResolveReplaceWord(uint address, uint oldValue, uint newValue) {
    if (address + 2 <= NxFbramSize()) {
        NxReplaceWordAt(address, oldValue, newValue);
    }
}

void ResolveApply(inout uint m, inout uint s, uint target, uint color) {
    ApplyFragment(m, s, Vdp1NxFragNativeTarget(target), color);
}

#include "vdp1_resolve.hlsli"
