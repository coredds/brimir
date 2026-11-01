// Brimir: 16-bit word access on a buffer in FBRAM's layout (the GPU FBRAM and the N x surface, vdp1_hw_params.hpp): the
// word at byte address a (even) is the half (a & 2) * 8 of the buffer uint at a & ~3, big-endian bytes. Two words share a
// uint and may belong to different threads, so every access is atomic: a read is an InterlockedOr with 0 and a write
// changes only its own half (InterlockedXor with the difference when the old value is known, as in the resolve, which
// owns its words; InterlockedAnd + InterlockedOr otherwise). Brimir only.
//
// BRIMIR_VDP1_WORD_ACCESS(P, BUF) defines P##ReadWordAt, P##ReplaceWordAt and P##WriteWordMaskedAt on the
// RWByteAddressBuffer BUF (declared before by the including shader).
#ifndef BRIMIR_VDP1_FBRAM_WORDS_HLSLI
#define BRIMIR_VDP1_FBRAM_WORDS_HLSLI

#include "util/bit_ops.hlsli"

#define BRIMIR_VDP1_WORD_ACCESS(P, BUF)                                                                                \
    uint P##ReadWordAt(uint address) {                                                                                 \
        uint whole;                                                                                                    \
        BUF.InterlockedOr(address & ~3u, 0, whole);                                                                    \
        return ByteSwap16(whole >> ((address & 2u) * 8u));                                                             \
    }                                                                                                                  \
    /* the word held oldValue (and nobody else writes it): one atomic XOR with the difference, none when unchanged */  \
    void P##ReplaceWordAt(uint address, uint oldValue, uint newValue) {                                                \
        const uint diff = (oldValue ^ newValue) & 0xFFFF;                                                              \
        if (diff != 0) {                                                                                               \
            BUF.InterlockedXor(address & ~3u, ByteSwap16(diff) << ((address & 2u) * 8u));                              \
        }                                                                                                              \
    }                                                                                                                  \
    /* the bits of mask (a 16-bit word mask) become those of value */                                                  \
    void P##WriteWordMaskedAt(uint address, uint value, uint mask) {                                                   \
        const uint shift = (address & 2u) * 8u;                                                                        \
        const uint at = address & ~3u;                                                                                 \
        const uint m = ByteSwap16(mask & 0xFFFF) << shift;                                                             \
        BUF.InterlockedAnd(at, ~m);                                                                                    \
        const uint bits = (ByteSwap16(value & mask & 0xFFFF) << shift) & m;                                            \
        if (bits != 0) {                                                                                               \
            BUF.InterlockedOr(at, bits);                                                                               \
        }                                                                                                              \
    }

#endif
