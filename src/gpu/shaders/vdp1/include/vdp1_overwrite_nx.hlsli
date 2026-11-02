// Brimir: the overwrite-class fast path of the N x passes (GPU stage 3; research section 1.6): BatchParams::drawFB bits
// of an N x sub-batch (vdp1_hw_params.hpp kBatchDrawFbMask, kBatchOverwrite*, kBatchHybrid) and the per-fragment class.
// Only the N x shaders include it (the native SPIR-V stays as it was). Brimir only.
#ifndef BRIMIR_VDP1_OVERWRITE_NX_HLSLI
#define BRIMIR_VDP1_OVERWRITE_NX_HLSLI

#include "vdp1_defs.hlsli"

static const uint kBatchDrawFbMask = 1;
static const uint kBatchOverwrite = 2;
static const uint kBatchOverwriteOddBytes = 4;
static const uint kBatchHybrid = 8;

// Whether the fragment (native-layout target) replaces the bits it writes whatever they held (ApplyFragment without a
// read of the destination): no MSB-on, no mesh framebuffer write, 8 bpp or a 16 bpp replace or half-luminance
bool Vdp1NxFragOverwrite(uint nativeTarget) {
    if (Vdp1FragMsbOn(nativeTarget) || Vdp1FragMesh(nativeTarget)) {
        return false;
    }
    const uint calc = Vdp1FragColorCalc(nativeTarget);
    return Vdp1FragPixel8(nativeTarget) || calc == 0 || calc == 2;
}

// The word of the unit offsets that holds the winner of the byte a fragment writes: `unit` (16 bpp; 8 bpp even bytes;
// MSB-on, which sets bit 15, the even byte) or Vdp1NxUnitCount(scale) + unit (8 bpp odd bytes; the second half of the
// N x unit offsets buffer, vdp1_bindings.hpp Vdp1NxUnitOffsetsBufferSize)
uint Vdp1NxWinnerIndex(uint unit, uint nativeTarget, uint scale) {
    const bool odd = Vdp1FragPixel8(nativeTarget) && Vdp1FragOddByte(nativeTarget) && !Vdp1FragMsbOn(nativeTarget);
    return odd ? Vdp1NxUnitCount(scale) + unit : unit;
}

#endif
