// Brimir: the per-pixel NBG character table (no Ymir equivalent). cs_vdp2_nbg_fetch.hlsl writes it, the backgrounds
// shader reads it: for each NBG, dispatch row and fetching pixel x (not windowed, the first pixel of its mosaic block),
// the character software's fetcher shows there (VRAMFetcher::currChar, sw:5193-5215) as ymir::vdp::Character::u32.
// Laid out [nbg][row][x], kMaxResV rows of kMaxResH entries per NBG. Host mirror: kNBGCharCount and NBGUsesCharTable in
// src/gpu/vdp2/vdp2_bindings.hpp. Include after vdp2_defs.hlsli and vdp2_render_params_layer.hlsli.
#ifndef BRIMIR_VDP2_NBG_CHARS_HLSLI
#define BRIMIR_VDP2_NBG_CHARS_HLSLI

uint NBGCharIndex(uint nbg, uint row, uint x) {
    return (nbg * kMaxResV + row) * kMaxResH + x;
}

// The NBGs whose characters come from the table: scroll NBGs with a character pattern delay in some bank, and NBG0/1
// with vertical cell scroll on 2x2 characters without mosaic (the vertical position changes between the two cell
// columns of a character, so the character of a run is its first fetch's).
bool NBGUsesCharTable(const NBGParams params, uint index) {
    if (!params.base.enabled || params.base.bitmap) {
        return false;
    }
    if (params.base.charPatDelay != 0) {
        return true;
    }
    return index <= 1 && params.vcellScrollEnable && !params.base.mosaicEnable && params.base.cellSizeShift != 0;
}

#endif
