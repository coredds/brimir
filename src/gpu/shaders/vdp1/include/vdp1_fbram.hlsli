// Brimir: 16-bit FBRAM word access on the GPU FBRAM buffer and the write half of VDP1PlotPixel (the per-word resolve's
// fold), written from the CPU reference (tests/unit/vdp1_raster_reference.cpp, Resolve, Erase and ApplyFbWrites).
// Brimir only.
//
// The including shader declares `RWByteAddressBuffer g_fbram` (binding 19) first. FBRAM word w of a slice is the half
// w & 1 of the buffer uint holding it, byte-swapped (vdp1_hw_params.hpp, "Framebuffer memory"); the accesses are the
// atomic ones of vdp1_fbram_words.hlsli.
#ifndef BRIMIR_VDP1_FBRAM_HLSLI
#define BRIMIR_VDP1_FBRAM_HLSLI

#include "util/bit_ops.hlsli"
#include "vdp1_apply_fragment.hlsli"
#include "vdp1_defs.hlsli"
#include "vdp1_fbram_words.hlsli"

BRIMIR_VDP1_WORD_ACCESS(Fbram, g_fbram)

uint FbramReadWord(uint slice, uint word) {
    return FbramReadWordAt(Vdp1FbramByteAddress(slice, word));
}

// The word held `oldValue` (and nobody else writes it): one atomic XOR with the difference, none when unchanged
void FbramReplaceWord(uint slice, uint word, uint oldValue, uint newValue) {
    FbramReplaceWordAt(Vdp1FbramByteAddress(slice, word), oldValue, newValue);
}

// The bits of `mask` (a 16-bit word mask) become those of `value`
void FbramWriteWordMasked(uint slice, uint word, uint value, uint mask) {
    FbramWriteWordMaskedAt(Vdp1FbramByteAddress(slice, word), value, mask);
}

#endif
