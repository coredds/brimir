// Brimir: per-output-row NBG scroll state computed on the CPU exactly as the software renderer does (no Ymir
// equivalent). Host mirror: VDP2NBGLine in src/gpu/vdp2/vdp2_hw_params.hpp (offsets checked against the reflection),
// which documents the fields.
#ifndef BRIMIR_VDP2_NBG_LINE_HLSLI
#define BRIMIR_VDP2_NBG_LINE_HLSLI

struct NBGLine {
    int fracScrollX;
    int fracScrollY;
    int scrollIncH;
    uint mosaicSrcLine;
    uint prevLastVCellScroll;
    uint prevLineEndScrollX;
    uint prevLineEndScrollY;
    uint flags;
};

static const uint kNBGLinePrevLineEndValid = 1u << 0u;

#endif
