// Brimir: N x VDP1 resolve of an overwrite-class sub-batch (GPU stage 3, research section 1.6: the fast path). Brimir only.
//
// In an overwrite-class sub-batch (BatchParams::drawFB kBatchOverwrite, vdp1_hw_params.hpp) every fragment replaces the
// bits it writes regardless of what is there (no MSB-on, 16 bpp replace or half-luminance, or 8 bpp; no mesh
// framebuffer write; one pixel depth and transparent-meshes setting), so applying a unit's fragments in slot order
// (resolve_nx) leaves the same words as applying only its last one. raster_nx kept that slot + 1 per N x unit in
// g_unitCounts (8 bpp: the even bytes there and the odd bytes in the second half of g_unitOffsets,
// kBatchOverwriteOddBytes, Vdp1NxWinnerIndex); this pass
// applies it with the write half of VDP1PlotPixel (ApplyFragment) and clears the entries, so the counts (and the odd-byte
// winners) are zero again for the next sub-batch. No bucket, scatter or sort.
//
// Dispatched like resolve_nx (g_batchState kBatchStateResolveArgs over the range [unitLo, unitHi), units in rounds of
// 64 G per group), one thread per unit. The unit's words are resolve_nx's (main or alternate slice of the draw
// framebuffer and the field's mesh slice), written with a compare-exchange per uint (a uint holds two N x words).
// Bindings (vdp1_bindings.hpp kResolveFastNxBindings): fragments (16), unit counts (17), unit offsets (21), batch state
// (22), N x surface (24). Push constants: BatchParams (drawFB, scale = N, 2 to 4).

#include "vdp1_defs.hlsli"
#include "vdp1_overwrite_nx.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] BatchParams g_batch;

RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWStructuredBuffer<uint> g_unitOffsets : register(u5);
RWStructuredBuffer<uint> g_batchState : register(u6);
RWByteAddressBuffer g_fbramNx : register(u8);

#include "vdp1_apply_fragment.hlsli"
#include "vdp1_fbram_nx.hlsli"

// Applies the fragment at slot `winner` - 1 (0: none)
void ApplyWinner(inout uint m, inout uint s, uint winner) {
    uint capacity, stride;
    g_fragments.GetDimensions(capacity, stride);
    if (winner == 0 || winner - 1 >= capacity) {
        return;
    }
    const Fragment f = g_fragments[winner - 1];
    if (Vdp1FragLive(f.target)) {
        ApplyFragment(m, s, Vdp1NxFragNativeTarget(f.target), f.color);
    }
}

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint scale = g_batch.scale >= 2 && g_batch.scale <= kMaxNxScale ? g_batch.scale : 0;
    uint words, stride, counts, offsets;
    g_batchState.GetDimensions(words, stride);
    g_unitCounts.GetDimensions(counts, stride);
    g_unitOffsets.GetDimensions(offsets, stride);
    if (scale == 0 || words <= kBatchStateUnitHi) {
        return;
    }
    const uint unitLo = g_batchState[kBatchStateUnitLo];
    const uint unitHi = min(g_batchState[kBatchStateUnitHi], Vdp1NxUnitCount(scale));
    const uint groups = g_batchState[kBatchStateResolveArgs];
    const uint fb = g_batch.drawFB & kBatchDrawFbMask;
    const bool oddBytes = (g_batch.drawFB & kBatchOverwriteOddBytes) != 0;
    const uint sub = scale * scale;
    const uint size = NxFbramSize();
    for (uint unit = unitLo + tid.x * groups + gid.x; unit < unitHi && groups != 0; unit += 64 * groups) {
        const uint oddIndex = Vdp1NxUnitCount(scale) + unit;
        const uint even = unit < counts ? g_unitCounts[unit] : 0;
        const uint odd = oddBytes && oddIndex < offsets ? g_unitOffsets[oddIndex] : 0;
        if (even == 0 && odd == 0) {
            continue;
        }
        const uint native = unit / sub;
        const uint index = (native & kWordIndexMask) * sub + (unit - native * sub);
        const uint alt = (native >> 17) & 1;
        const uint mainAddress = Vdp1NxFbramByteAddress(alt != 0 ? Vdp1AltSlice(fb) : Vdp1MainSlice(fb), index, scale);
        const uint meshAddress = Vdp1NxFbramByteAddress(Vdp1MeshSlice(alt, fb), index, scale);
        const bool mainIn = mainAddress + 2 <= size;
        const bool meshIn = meshAddress + 2 <= size;
        const uint m0 = mainIn ? NxReadWordAt(mainAddress) : 0;
        const uint s0 = meshIn ? NxReadWordAt(meshAddress) : 0;
        uint m = m0;
        uint s = s0;
        ApplyWinner(m, s, even); // a word's two 8 bpp bytes are disjoint: either order
        ApplyWinner(m, s, odd);
        if (mainIn) {
            NxReplaceWordAt(mainAddress, m0, m);
        }
        if (meshIn) {
            NxReplaceWordAt(meshAddress, s0, s);
        }
        if (even != 0) {
            g_unitCounts[unit] = 0;
        }
        if (odd != 0) {
            g_unitOffsets[oddIndex] = 0;
        }
    }
}
