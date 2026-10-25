// Derived from Ymir libs/ymir-core/res/shaders/src/vdp/vdp2_window_params.hlsli at 8cdf0a53 (unchanged).
// Host mirror: src/gpu/vdp2/vdp2_hw_params.hpp (offsets checked against the reflection).
#ifndef YMIR_VDP_VDP2_WINDOW_PARAMS_HLSLI
#define YMIR_VDP_VDP2_WINDOW_PARAMS_HLSLI

// See C++ code for documentation on the fields

struct GlobalWindowParams {
    uint2 start;
    uint2 end;
    uint lineWindowTableAddress;
    bool lineWindowTableEnable;
};

struct LayerWindowParams {
    bool windowLogicAnd;
    bool window0Enable;
    bool window0Invert;
    bool window1Enable;
    bool window1Invert;
};

struct LayerWindowParamsS {
    LayerWindowParams base;
    bool spriteWindowEnable;
    bool spriteWindowInvert;
};

#endif
