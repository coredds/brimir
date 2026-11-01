// Brimir: N x VDP1 rebuild pass (GPU stage 3; research section 2): the N x surface's slices of sliceMask become their
// native words (entering GPU VDP1, reset, state load, scale change, slice uploads). Brimir only.
//
// Thread x of group (g, s) takes buffer uint u = 64 g + x of N x slice s (when bit s of sliceMask is set): its two N x
// words 2u and 2u + 1 are the native words 2u / N^2 and (2u + 1) / N^2 of the slice. One plain store per uint, every
// uint written by one thread.
// Dispatch: Vdp1RebuildNxGroups(N) x 8 groups (N^2 * 1024 x 8).
// Bindings (vdp1_bindings.hpp kRebuildNxBindings): FBRAM (19, read), N x surface (24). Push constants: RebuildNxParams
// (scale = N, 2 to 4).

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] RebuildNxParams g_params;

RWByteAddressBuffer g_fbram : register(u3);
RWByteAddressBuffer g_fbramNx : register(u8);

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint scale = g_params.scale;
    const uint slice = gid.y;
    if (scale < 2 || scale > kMaxNxScale || slice >= 8 || ((g_params.sliceMask >> slice) & 1) == 0) {
        return;
    }
    const uint sub = scale * scale;
    const uint u = gid.x * 64 + tid.x;
    const uint sliceUints = Vdp1NxFbramSliceSize(scale) / 4;
    uint nxSize, fbramSize;
    g_fbramNx.GetDimensions(nxSize);
    g_fbram.GetDimensions(fbramSize);
    const uint at = Vdp1NxFbramByteAddress(slice, 2 * u, scale);
    if (u >= sliceUints || at + 4 > nxSize || (slice + 1) * kFbramSliceSize > fbramSize) {
        return;
    }
    const uint w0 = (2 * u) / sub;
    const uint w1 = (2 * u + 1) / sub;
    const uint base = slice * kFbramSliceSize;
    // Native words w0 and w1 as stored (big-endian halves): their 16-bit halves of the FBRAM uints, unswapped
    const uint h0 = (g_fbram.Load(base + ((w0 * 2) & ~3u)) >> ((w0 & 1) * 16)) & 0xFFFF;
    const uint h1 = (g_fbram.Load(base + ((w1 * 2) & ~3u)) >> ((w1 & 1) * 16)) & 0xFFFF;
    g_fbramNx.Store(at, h0 | (h1 << 16));
}
