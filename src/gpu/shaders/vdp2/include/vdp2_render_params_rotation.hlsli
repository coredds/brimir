// Derived from Ymir libs/ymir-core/res/shaders/src/vdp/vdp2_render_params_rotation.hlsli at 8cdf0a53 (unchanged).
// Host mirror: VDP2RotParamBase in src/gpu/vdp2/vdp2_hw_params.hpp (offsets checked against the reflection).
#ifndef YMIR_VDP_VDP2_RENDER_PARAMS_ROTATION_HLSLI
#define YMIR_VDP_VDP2_RENDER_PARAMS_ROTATION_HLSLI

struct RotParamBase {
    uint tableAddress;
    int Xst, Yst;
    uint KA;
};

#endif
