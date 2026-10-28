// Brimir: 16-bit FBRAM word access on the GPU FBRAM buffer and the write half of VDP1PlotPixel (the per-word resolve's
// fold), written from the CPU reference (tests/unit/vdp1_raster_reference.cpp, Resolve, Erase and ApplyFbWrites).
// Brimir only.
//
// The including shader declares `RWByteAddressBuffer g_fbram` (binding 19) first. FBRAM word w of a slice is the half
// w & 1 of the buffer uint holding it, byte-swapped (vdp1_hw_params.hpp, "Framebuffer memory"). Two FBRAM words share a
// uint and may belong to different threads, so every access to the buffer is atomic: a read is an InterlockedOr with 0
// and a write changes only its own half (InterlockedXor with the difference when the old value is known, as in the
// resolve, which owns its words; InterlockedAnd + InterlockedOr otherwise).
#ifndef BRIMIR_VDP1_FBRAM_HLSLI
#define BRIMIR_VDP1_FBRAM_HLSLI

#include "util/bit_ops.hlsli"
#include "vdp1_defs.hlsli"

// Byte address of the buffer uint holding the word
uint FbramUintAddress(uint slice, uint word) {
    return Vdp1FbramByteAddress(slice, word) & ~3u;
}

uint FbramReadWord(uint slice, uint word) {
    uint whole;
    g_fbram.InterlockedOr(FbramUintAddress(slice, word), 0, whole);
    return ByteSwap16(whole >> Vdp1FbWordShift(word));
}

// The word held `oldValue` (and nobody else writes it): one atomic XOR with the difference, none when unchanged
void FbramReplaceWord(uint slice, uint word, uint oldValue, uint newValue) {
    const uint diff = (oldValue ^ newValue) & 0xFFFF;
    if (diff != 0) {
        g_fbram.InterlockedXor(FbramUintAddress(slice, word), ByteSwap16(diff) << Vdp1FbWordShift(word));
    }
}

// The bits of `mask` (a 16-bit word mask) become those of `value`
void FbramWriteWordMasked(uint slice, uint word, uint value, uint mask) {
    const uint shift = Vdp1FbWordShift(word);
    const uint address = FbramUintAddress(slice, word);
    const uint m = ByteSwap16(mask & 0xFFFF) << shift;
    g_fbram.InterlockedAnd(address, ~m);
    const uint bits = (ByteSwap16(value & mask & 0xFFFF) << shift) & m;
    if (bits != 0) {
        g_fbram.InterlockedOr(address, bits);
    }
}

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
