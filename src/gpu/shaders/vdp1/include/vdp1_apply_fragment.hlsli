// Brimir: the write half of VDP1PlotPixel (the per-word resolve's fold), written from the CPU reference
// (tests/unit/vdp1_raster_reference.cpp, Resolve). No buffer access: the native and N x resolves share it. Brimir only.
#ifndef BRIMIR_VDP1_APPLY_FRAGMENT_HLSLI
#define BRIMIR_VDP1_APPLY_FRAGMENT_HLSLI

#include "vdp1_defs.hlsli"

// The write half of VDP1PlotPixel for one fragment on its word's (main, mesh) pair (reference Resolve): MSB-on sets bit
// 15 of the main word; 8 bpp writes a byte (the even byte is the high one); 16 bpp applies the colour calculation with
// the destination read from main. A write to the mesh framebuffer replaces the mesh word (byte); a main write with
// transparent meshes on clears it.
void ApplyFragment(inout uint m, inout uint s, uint target, uint color) {
    if (Vdp1FragMsbOn(target)) {
        m |= 0x8000;
        return;
    }
    const bool toMesh = Vdp1FragMesh(target);
    const bool transparentMeshes = Vdp1FragTransparentMeshes(target);
    if (Vdp1FragPixel8(target)) {
        const uint shift = Vdp1FragOddByte(target) ? 0 : 8;
        const uint keep = ~(0xFFu << shift) & 0xFFFF;
        const uint byteBits = (color & 0xFF) << shift;
        if (toMesh) {
            s = (s & keep) | byteBits;
        } else {
            m = (m & keep) | byteBits;
            if (transparentMeshes) {
                s &= keep;
            }
        }
        return;
    }

    const uint src = color & 0xFFFF;
    uint dst = m;
    const uint sr = src & 0x1F, sg = (src >> 5) & 0x1F, sb = (src >> 10) & 0x1F;
    const uint dr = dst & 0x1F, dg = (dst >> 5) & 0x1F, db = (dst >> 10) & 0x1F;
    const bool dmsb = (dst & 0x8000) != 0;
    switch (Vdp1FragColorCalc(target)) {
    case 0: // replace
        dst = src;
        break;
    case 1: // shadow
        if (dmsb) {
            dst = (dr >> 1) | ((dg >> 1) << 5) | ((db >> 1) << 10) | 0x8000;
        }
        break;
    case 2: // half-luminance
        dst = (sr >> 1) | ((sg >> 1) << 5) | ((sb >> 1) << 10) | (src & 0x8000);
        break;
    default: // half-transparency
        if (dmsb) {
            dst = ((sr + dr) >> 1) | (((sg + dg) >> 1) << 5) | (((sb + db) >> 1) << 10) | 0x8000;
        } else {
            dst = src;
        }
        break;
    }
    if (toMesh) {
        s = dst;
    } else {
        m = dst;
        if (transparentMeshes) {
            s = 0;
        }
    }
}

#endif
