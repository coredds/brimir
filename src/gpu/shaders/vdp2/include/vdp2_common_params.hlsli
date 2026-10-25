// Derived from Ymir libs/ymir-core/res/shaders/src/vdp/vdp2_common_params.hlsli at 8cdf0a53.
// Host mirror: VDP2CommonRenderParams in src/gpu/vdp2/vdp2_hw_params.hpp (offsets checked against the reflection).
#ifndef YMIR_VDP_VDP2_COMMON_PARAMS_HLSLI
#define YMIR_VDP_VDP2_COMMON_PARAMS_HLSLI

// See C++ code for documentation on the fields

struct CommonRenderParams {
    uint startY;
    uint displayParams;
    uint layerParams;
    uint rotParams;
    uint spriteParams;
    // Brimir: latched TVMD.BDCLMD (VDP2Regs::borderColorModeLatch), used by software for display-off lines. It fills
    // the std430 hole before the 8-byte aligned uint2.
    uint borderColorModeLatch;
    uint2 spritePriosRatios;
    uint vcellScroll;
    uint windows;
    uint enhancements;
    // Brimir: explicit tail padding to the std430 size (48, a multiple of the uint2 alignment).
    uint pad;
};

#endif
