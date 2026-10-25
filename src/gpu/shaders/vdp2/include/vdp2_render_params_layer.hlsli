// Derived from Ymir libs/ymir-core/res/shaders/src/vdp/vdp2_render_params_layer.hlsli at 8cdf0a53 (unchanged).
// Host mirror: VDP2LayerRenderParams in src/gpu/vdp2/vdp2_hw_params.hpp (offsets checked against the reflection).
#ifndef YMIR_VDP_VDP2_RENDER_PARAMS_LAYER_HLSLI
#define YMIR_VDP_VDP2_RENDER_PARAMS_LAYER_HLSLI

#include "vdp2_window_params.hlsli"

// See C++ code for documentation on the fields

struct BaseBGParams {
    bool enabled;
    bool enableTransparency;
    bool bitmap;
    uint priorityNumber;
    uint priorityMode;
    uint specialFunctionSelect;
    uint cellSizeShift;
    uint colorFormat;
    uint cramOffset;
    uint supplScrollCharNum;
    uint supplPalNum;
    uint supplSpecialColorCalc;
    uint supplSpecialPriority;
    bool mosaicEnable;
    bool colorCalcEnable;
    bool extChar;
    bool twoWordChar;
    uint patNameAccess;
    uint charPatAccess;
    uint charPatDelay;
    uint vramDataOffset;
    uint specialColorCalcMode;
    uint2 pageShift;
    uint2 bitmapSize;
    uint bitmapBaseAddress;
    LayerWindowParamsS windowParams;
};

struct NBGParams {
    BaseBGParams base;
    uint2 scrollAmount;
    uint2 scrollInc;
    uint pageBaseAddresses[4];
    bool vcellScrollEnable;
    bool lineScrollXEnable;
    bool lineScrollYEnable;
    bool lineZoomEnable;
    uint lineScrollInterval;
    uint lineScrollTableAddress;
    uint vcellScrollOffset;
    uint vcellScrollDelay;
    uint vcellScrollRepeat;
    uint scrollYIncBase;
};

struct RBGParams {
    BaseBGParams base;
    uint screenOverProcess;
    uint screenOverPatternName;
    uint pageBaseAddresses[2][16];
};

struct LineBackScreenParams {
    uint baseAddress;
    bool perLine;
};

struct LayerRenderParams {
    NBGParams nbg[4];
    RBGParams rbg[2];

    GlobalWindowParams windows[2];
    LayerWindowParams rotWindows;

    LineBackScreenParams lineScreenParams;
    LineBackScreenParams backScreenParams;

    uint specialFunctionCodes;
};

#endif
