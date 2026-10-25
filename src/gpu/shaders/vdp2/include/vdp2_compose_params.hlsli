// Derived from Ymir libs/ymir-core/res/shaders/src/vdp/vdp2_compose_params.hlsli at 8cdf0a53.
// Host mirror: VDP2ComposeParams in src/gpu/vdp2/vdp2_hw_params.hpp (offsets checked against the reflection).
#ifndef YMIR_VDP_VDP2_COMPOSE_PARAMS_HLSLI
#define YMIR_VDP_VDP2_COMPOSE_PARAMS_HLSLI

// See C++ code for documentation on the fields

// Brimir: std430 layout (StructuredBuffer under Vulkan): int3 aligns to 16, so colorOffsetB is at 32, the ratios at
// 44, backLineColorCalcRatios at 64 and the stride is 80 (Ymir's D3D12 host struct is packed, 68 bytes).
struct ComposeParams {
    uint colorCalcEnable;
    uint colorOffsetEnable;
    uint colorOffsetSelect;
    uint lineColorEnable;
    int3 colorOffsetA;
    // Brimir: shadow enable per layer (bits 0-5 by layer index as colorCalcEnable, bit 6 back screen), in the hole
    // after colorOffsetA.
    uint shadowEnable;
    int3 colorOffsetB;
    uint bgColorCalcRatios[5];
    uint backLineColorCalcRatios[2];
    // Brimir: explicit tail padding to the std430 stride.
    uint pad[2];
};

#endif
