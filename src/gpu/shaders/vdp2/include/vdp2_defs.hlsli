// Derived from Ymir libs/ymir-core/res/shaders/src/vdp/vdp2_defs.hlsli at 8cdf0a53. Brimir additions are at the end,
// marked "// Brimir:".
#ifndef YMIR_VDP_VDP2_DEFS_HLSLI
#define YMIR_VDP_VDP2_DEFS_HLSLI

static const uint kVDP1FBRAMSize = 256 * 1024;

static const uint kResolutionsH[4] = { 320, 352, 640, 704 };
static const uint kResolutionsV[4] = { 224, 240, 256, 256 };

static const uint kInterlaceModeNone = 0;
static const uint kInterlaceModeInvalid = 1;
static const uint kInterlaceModeSingleDensity = 2;
static const uint kInterlaceModeDoubleDensity = 3;

static const uint kColorFormatPalette16 = 0;
static const uint kColorFormatPalette256 = 1;
static const uint kColorFormatPalette2048 = 2;
static const uint kColorFormatRGB555 = 3;
static const uint kColorFormatRGB888 = 4;

static const uint kPriorityModeScreen = 0;
static const uint kPriorityModeCharacter = 1;
static const uint kPriorityModeDot = 2;

static const uint kSpecColorCalcModeScreen = 0;
static const uint kSpecColorCalcModeCharacter = 1;
static const uint kSpecColorCalcModeDot = 2;
static const uint kSpecColorCalcModeColorMSB = 3;

static const uint kPageSizes[2][2] = { { 13, 14 }, { 11, 12 } };

static const uint kMaxNormalResH = 352;
static const uint kMaxNormalResV = 256;

static const uint kMaxResH = 704;
static const uint kMaxResV = 512;

static const uint kRotParamLinePitch = kMaxNormalResH;
static const uint kRotParamEntryStride = kRotParamLinePitch * kMaxNormalResV;

static const uint kRotParamA = 0;
static const uint kRotParamB = 1;

static const uint kRotParamModeA = 0;
static const uint kRotParamModeB = 1;
static const uint kRotParamModeCoeff = 2;
static const uint kRotParamModeWindow = 3;

static const uint kCoeffDataModeScaleCoeffXY = 0;
static const uint kCoeffDataModeScaleCoeffX = 1;
static const uint kCoeffDataModeScaleCoeffY = 2;
static const uint kCoeffDataModeViewpointX = 3;

static const uint kScreenOverProcessRepeat = 0;
static const uint kScreenOverProcessRepeatChar = 1;
static const uint kScreenOverProcessTransparent = 2;
static const uint kScreenOverProcessFixed512 = 3;

static const uint kPixelAttrBitPaletteFormat = 6;
static const uint kPixelAttrBitSpecColorCalc = 7;

static const uint kSpriteAttrBitColorCalcRatio = 0; // bits 0 to 4
static const uint kSpriteAttrBitColorMSB = 5;
static const uint kSpriteAttrBitShadowWindow = 6;
static const uint kSpriteAttrBitSpecial = 7; // bits 7 to 8

static const uint kSpriteDataNormal = 0; // Any other value
static const uint kSpriteDataShadow = 1; // Normal shadow pattern (DC=0b...11110)
static const uint kSpriteDataTransparent = 2; // Raw 16-bit value is 0x0000

static const uint kColorGradScreenSprite = 0;
static const uint kColorGradScreenRBG0 = 1;
static const uint kColorGradScreenNBG0_RBG0 = 2;
static const uint kColorGradScreenNBG1_EXBG = 4;
static const uint kColorGradScreenNBG2 = 5;
static const uint kColorGradScreenNBG3 = 6;

static const uint kLayerIndexNBG0 = 0;
static const uint kLayerIndexNBG1 = 1;
static const uint kLayerIndexNBG2 = 2;
static const uint kLayerIndexNBG3 = 3;
static const uint kLayerIndexRBG0 = 4;
static const uint kLayerIndexRBG1 = 5;
static const uint kLayerIndexSprite = 6;
static const uint kLayerIndexMesh = 7;

static const uint4 kTransparentPixel = uint4(0, 0, 0, 0);

// ---------------------------------------------------------------------------------------------------------------------
// Brimir additions (Vulkan port)

// Brimir: rotation parameter bases are indexed [param * kRotParamBaseStride + line] with 512 lines per parameter, the
// layout of Vdp2FrameRecord::rotBases (exclusive-monitor lines fit). Ymir's stride is kMaxNormalResV.
static const uint kRotParamBaseStride = kMaxResV;

// Brimir: LNCL and BACK share one buffer of packed RGBA8 colours, LNCL at [y] and BACK at [kBackScreenOffset + y], the
// layout of Vdp2FrameRecord::lnclBack. Ymir's offset is kMaxNormalResV.
static const uint kBackScreenOffset = kMaxResV;

// Brimir: the VDP1 framebuffer buffer holds eight kVDP1FBRAMSize slices (display FB 0/1 of main, alt, mesh0, mesh1).
// Reads beyond it return 0, as Ymir's D3D12 raw-buffer reads do (Vulkan has no robust access in the baseline).
static const uint kVDP1FBRAMBufferSize = kVDP1FBRAMSize * 8;

// Brimir: entries in the CRAM colour buffer (StructuredBuffer<uint>, one packed RGBA8 colour per entry).
static const uint kCRAMColorEntries = 2048;

// Brimir: CRAM colours and LNCL/BACK are StructuredBuffer<uint> of packed RGBA8 (ColorR8G8B8A8, r in the low byte)
// instead of Buffer<uint4> views of R8G8B8A8_UINT: no texel buffer and no Rgba32ui format declaration.
uint4 UnpackRGBA8(uint color) {
    return uint4(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, color >> 24);
}

// Brimir: the output size the back half pushes (CommonRenderParams::outputSize): HRes, VRes, clamped to the output
// images and the composite buffer (kMaxResH x kMaxResV) so a bad value cannot address beyond them.
uint2 UnpackOutputSize(uint outputSize) {
    return min(uint2(outputSize & 0xFFFF, outputSize >> 16), uint2(kMaxResH, kMaxResV));
}

#endif
