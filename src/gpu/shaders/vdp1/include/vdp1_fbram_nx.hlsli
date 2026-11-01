// Brimir: the N x surface (GPU stage 3; vdp1_hw_params.hpp "N x surface and N x fragments"): word access and block
// writes. The including shader declares `RWByteAddressBuffer g_fbramNx` (binding 24) first. Native word w of a slice is
// the N x N block of N x words w * N^2 .. w * N^2 + N^2 - 1, stored as FBRAM stores its words (vdp1_fbram_words.hlsli).
// Brimir only.
#ifndef BRIMIR_VDP1_FBRAM_NX_HLSLI
#define BRIMIR_VDP1_FBRAM_NX_HLSLI

#include "util/bit_ops.hlsli"
#include "vdp1_defs.hlsli"
#include "vdp1_fbram_words.hlsli"

BRIMIR_VDP1_WORD_ACCESS(Nx, g_fbramNx)

// Bytes of the N x surface buffer
uint NxFbramSize() {
    uint size;
    g_fbramNx.GetDimensions(size);
    return size;
}

// The bits of `mask` of every N x word of native word `word`'s block in slice `slice` become those of `value` (nothing
// when the block lies outside the buffer). A buffer uint holding words of two blocks (N = 3) is written atomically
// (InterlockedAnd + InterlockedOr on the block's half). A uint inside the block with a full mask is written with Store
// when `plain` (erase: only threads of the same native word write it, all with the same value), else with
// InterlockedExchange (CPU writes: records of one word write it concurrently with masked atomics, all towards the same
// final value).
void NxWriteBlock(uint slice, uint word, uint value, uint mask, uint scale, bool plain) {
    const uint sub = scale * scale;
    const uint first = Vdp1NxFbramByteAddress(slice, word * sub, scale);
    const uint end = first + sub * 2;
    if (end > NxFbramSize() || end <= first) {
        return;
    }
    const uint m16 = ByteSwap16(mask & 0xFFFF);
    const uint bits16 = ByteSwap16(value & mask & 0xFFFF);
    for (uint at = first & ~3u; at < end; at += 4) {
        const bool lo = at >= first;  // the word at `at`
        const bool hi = at + 2 < end; // the word at `at` + 2 (at + 2 >= first: first is even)
        const uint m = (lo ? m16 : 0u) | (hi ? m16 << 16 : 0u);
        const uint bits = (lo ? bits16 : 0u) | (hi ? bits16 << 16 : 0u);
        if (m == 0xFFFFFFFFu) {
            if (plain) {
                g_fbramNx.Store(at, bits);
            } else {
                uint old;
                g_fbramNx.InterlockedExchange(at, bits, old);
            }
        } else if (m != 0) {
            g_fbramNx.InterlockedAnd(at, ~m);
            if (bits != 0) {
                g_fbramNx.InterlockedOr(at, bits);
            }
        }
    }
}

#endif
