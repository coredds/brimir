// VDP2 backgrounds: renders NBG0-3 and RBG0-1 into the layer image, the RBG line colours, and the colour calculation
// window.
//
// Derived from Ymir libs/ymir-core/res/shaders/src/vdp/cs_vdp2_render_bgs.hlsl at 8cdf0a53. Only what Vulkan 1.2
// (Brimir's baseline) needs is changed, each change marked "// Brimir:":
// - common params are a push-constant block; register numbers follow src/gpu/vdp2/vdp2_bindings.hpp;
// - CRAM colours are a StructuredBuffer<uint> of packed RGBA8 (no texel buffer);
// - the layer and RBG line colour images are declared rgba8ui and the colour calculation window r32ui (no extended
//   storage formats);
// - the 64-bit rotation arithmetic uses the 32-bit helpers of int64_emu.hlsli (no Int64 capability);
// - [numthreads(32, 1, 1)] dispatched with z = 7 instead of [numthreads(32, 1, 7)] (at most 128 invocations); id.z is
//   still the layer;
// - NBG and RBG page base addresses are read straight from the parameters instead of a pbaResized[16] copy;
// - rotation bases use a 512-line stride per parameter (Vdp2FrameRecord::rotBases), so the RBG line colour image and
//   the rotation bases cover 512 lines;
// - NBG scroll state comes per row from g_nbgLines (computed on the CPU as software does), see "NBG drawing";
// - bounds guards replace D3D12's robust out-of-range accesses (no robustness features): pixels outside HRes x VRes
//   write nothing and RBG line colours outside their image are dropped.
// Where Ymir differs from Brimir's software renderer (the reference), the software semantics are ported, each marked
// "// Brimir: matches software (sw:<lines>)": RBG line colours read the RGB555 table of CRAM word (address & 0x7FF);
// NBG scroll, mosaic, vertical cell scroll and character pattern delay; RBG mosaic.
//
// Bindings (set 0; tools/shaders/build_shaders.cmake shifts t by 0 and u by 16):
//   0  t0  g_layerRenderParams   StructuredBuffer<LayerRenderParams>  STORAGE_BUFFER_DYNAMIC
//   2  t2  g_vram                ByteAddressBuffer                    STORAGE_BUFFER
//   3  t3  g_cramColor           StructuredBuffer<uint>               STORAGE_BUFFER
//   4  t4  g_cramRotCoeff        ByteAddressBuffer                    STORAGE_BUFFER
//   5  t5  g_rotParamBases       StructuredBuffer<RotParamBase>       STORAGE_BUFFER
//   7  t7  g_nbgLines            StructuredBuffer<NBGLine>            STORAGE_BUFFER
//   9  t9  g_spriteAttrsIn       Texture2DArray<uint>                 SAMPLED_IMAGE
//  13  t13 g_nbgChars            StructuredBuffer<uint>               STORAGE_BUFFER
//  16  u0  g_layerOut            RWTexture2DArray<uint4> rgba8ui      STORAGE_IMAGE
//  18  u2  g_rbgLineColorOut     RWTexture2DArray<uint4> rgba8ui      STORAGE_IMAGE
//  19  u3  g_colorCalcWindowOut  RWTexture2D<uint> r32ui              STORAGE_IMAGE
// Push constants: CommonRenderParams (48 bytes). Dispatch: (HRes / 32, lines, 7).

#include "vdp2_common_params.hlsli"
#include "vdp2_nbg_line.hlsli"
#include "vdp2_render_params_layer.hlsli"
#include "vdp2_render_params_rotation.hlsli"

#include "vdp2_defs.hlsli"
#include "vdp2_nbg_chars.hlsli"

#include "int64_emu.hlsli"
#include "util/bit_ops.hlsli"
#include "util/data_ops.hlsli"

// Brimir: push constants instead of the b0 root constants.
[[vk::push_constant]] CommonRenderParams g_commonParams;

// Brimir: registers renumbered per vdp2_bindings.hpp.
StructuredBuffer<LayerRenderParams> g_layerRenderParams : register(t0);
ByteAddressBuffer g_vram : register(t2);
// Brimir: StructuredBuffer<uint> of packed RGBA8 instead of Buffer<uint4>.
StructuredBuffer<uint> g_cramColor : register(t3);
ByteAddressBuffer g_cramRotCoeff : register(t4);
StructuredBuffer<RotParamBase> g_rotParamBases : register(t5);
// Brimir: per-line NBG state, [nbg * kMaxResV + dispatch row] (Vdp2FrameRecord::nbgLines).
StructuredBuffer<NBGLine> g_nbgLines : register(t7);
Texture2DArray<uint> g_spriteAttrsIn : register(t9);
// Brimir: the NBG character table the fetch pass writes (vdp2_nbg_chars.hlsli).
StructuredBuffer<uint> g_nbgChars : register(t13);

// Brimir: explicit storage formats (DXC would infer Rgba32ui for uint4).
[[vk::image_format("rgba8ui")]] RWTexture2DArray<uint4> g_layerOut : register(u0);
[[vk::image_format("rgba8ui")]] RWTexture2DArray<uint4> g_rbgLineColorOut : register(u2);
[[vk::image_format("r32ui")]] RWTexture2D<uint> g_colorCalcWindowOut : register(u3);

// ---------------------------------------------------------------------------------------------------------------------
// Parameters

static const uint interlaceMode = BitExtract(g_commonParams.displayParams, 2, 2);
static const uint oddField = BitExtract(g_commonParams.displayParams, 4, 1);
static const bool exclusiveMonitor = BitTest(g_commonParams.displayParams, 5);
static const bool hiResH = BitTest(g_commonParams.displayParams, 8);
static const bool palMode = BitTest(g_commonParams.displayParams, 9);
static const uint hreso = BitExtract(g_commonParams.displayParams, 10, 3);
static const uint vreso = BitExtract(g_commonParams.displayParams, 13, palMode ? 2 : 1);
static const uint2 displayRes = uint2(
    kResolutionsH[hreso & 3u], // 3rd bit intentionally ignored
    exclusiveMonitor ? 480 : kResolutionsV[vreso]
);

static const bool coeffTableCRAM = BitTest(g_commonParams.rotParams, 0);
static const uint coeffDataAccess = BitExtract(g_commonParams.rotParams, 1, 4);
static const bool coeffDataPerDot = BitTest(g_commonParams.rotParams, 5);

static const bool deinterlace = BitTest(g_commonParams.enhancements, 0);

static const uint colorRAMMode = BitExtract(g_commonParams.displayParams, 6, 2);
static const uint kCRAMAddressMask = colorRAMMode == 1 ? 0x7FF : 0x3FF;

// Brimir: the frame's output size (bounds of the outputs).
static const uint2 outputSize = UnpackOutputSize(g_commonParams.outputSize);

// ---------------------------------------------------------------------------------------------------------------------
// Utilities

uint GetY(uint y, bool doubleDensityOnly) {
    const bool interlaced = doubleDensityOnly
        ? interlaceMode == kInterlaceModeDoubleDensity
        : interlaceMode >= kInterlaceModeSingleDensity;

    if (!deinterlace && interlaced && !exclusiveMonitor) {
        return (y << 1) | oddField;
    } else {
        return y;
    }
}

uint ReadVRAM4(uint address, uint nibble) {
    return Read4(g_vram, address & 0x7FFFF, nibble);
}

uint ReadVRAM8(uint address) {
    return Read8(g_vram, address & 0x7FFFF);
}

uint ReadVRAM16(uint address) {
    return Read16(g_vram, address & 0x7FFFE);
}

uint ReadVRAM32(uint address) {
    return Read32(g_vram, address & 0x7FFFC);
}

uint ReadCRAMRotCoeff16(uint address) {
    return Read16(g_cramRotCoeff, address & 0x7FE);
}

uint ReadCRAMRotCoeff32(uint address) {
    return Read32(g_cramRotCoeff, address & 0x7FC);
}

// ---------------------------------------------------------------------------------------------------------------------
// Windows

bool InsideWindow(GlobalWindowParams window, bool invert, uint2 pos) {
    int2 start = window.start;
    int2 end = window.end;

    // Read line window if enabled
    if (window.lineWindowTableEnable) {
        const uint address = window.lineWindowTableAddress + pos.y * 4;
        start.x = ReadVRAM16(address + 0);
        end.x = ReadVRAM16(address + 2);
    }

    start.x = SignExtend(start.x, 16);
    end.x = SignExtend(end.x, 16);
    start.y = SignExtend(start.y, 16);
    end.y = SignExtend(end.y, 16);

    // Some games set out-of-range window parameters and expect them to work.
    // It seems like window coordinates should be signed...
    //
    // Panzer Dragoon 2 Zwei:
    //   0000 to FFFE -> empty window
    //   FFFE to 02C0 -> full line
    //
    // Panzer Dragoon Saga:
    //   0000 to FFFF -> empty window
    //
    // Snatcher:
    //   FFFC to 0286 -> full line
    //
    // Handle these cases here
    if (start.x < 0) {
        start.x = 0;
    }
    if (end.x < 0) {
        if (start.x >= end.x) {
            start.x = 0x3FF;
        }
        end.x = 0;
    }

    // For normal screen modes, X coordinates don't use bit 0
    if (!hiResH) {
        start.x >>= 1;
        end.x >>= 1;
    }

    const int2 spos = int2(pos);
    const bool inside = all(spos >= start) && all(spos <= end);
    return inside != invert;
}

bool InsideSpriteWindow(bool invert, uint2 pos) {
    return BitTest(g_spriteAttrsIn[uint3(pos, 0)], kSpriteAttrBitShadowWindow) != invert;
}

bool InsideWindows(LayerWindowParams layerWindows, uint2 pos) {
    const bool windowLogicAND = layerWindows.windowLogicAnd;
    const bool window0Enable = layerWindows.window0Enable;
    const bool window0Invert = layerWindows.window0Invert;
    const bool window1Enable = layerWindows.window1Enable;
    const bool window1Invert = layerWindows.window1Invert;

    // If no windows are enabled, consider the pixel outside of windows
    if (!window0Enable && !window1Enable) {
        return false;
    }

    bool inside = windowLogicAND;
    if (window0Enable) {
        const bool insideW0 = InsideWindow(g_layerRenderParams[0].windows[0], window0Invert, pos);
        if (windowLogicAND) {
            inside = inside && insideW0;
        } else {
            inside = inside || insideW0;
        }
    }
    if (window1Enable) {
        const bool insideW1 = InsideWindow(g_layerRenderParams[0].windows[1], window1Invert, pos);
        if (windowLogicAND) {
            inside = inside && insideW1;
        } else {
            inside = inside || insideW1;
        }
    }

    return inside;
}

bool InsideWindows(LayerWindowParamsS layerWindows, uint2 pos) {
    const bool windowLogicAND = layerWindows.base.windowLogicAnd;
    const bool window0Enable = layerWindows.base.window0Enable;
    const bool window0Invert = layerWindows.base.window0Invert;
    const bool window1Enable = layerWindows.base.window1Enable;
    const bool window1Invert = layerWindows.base.window1Invert;
    const bool spriteWindowEnable = layerWindows.spriteWindowEnable;
    const bool spriteWindowInvert = layerWindows.spriteWindowInvert;

    // If no windows are enabled, consider the pixel outside of windows
    if (!window0Enable && !window1Enable && !spriteWindowEnable) {
        return false;
    }

    bool inside = windowLogicAND;
    if (window0Enable) {
        const bool insideW0 = InsideWindow(g_layerRenderParams[0].windows[0], window0Invert, pos);
        if (windowLogicAND) {
            inside = inside && insideW0;
        } else {
            inside = inside || insideW0;
        }
    }
    if (window1Enable) {
        const bool insideW1 = InsideWindow(g_layerRenderParams[0].windows[1], window1Invert, pos);
        if (windowLogicAND) {
            inside = inside && insideW1;
        } else {
            inside = inside || insideW1;
        }
    }
    if (spriteWindowEnable) {
        const bool insideSW = InsideSpriteWindow(spriteWindowInvert, pos);
        if (windowLogicAND) {
            inside = inside && insideSW;
        } else {
            inside = inside || insideSW;
        }
    }

    return inside;
}

// ---------------------------------------------------------------------------------------------------------------------
// Colors and CRAM

uint4 FetchCRAMColor(uint cramOffset, uint colorIndex) {
    const uint cramAddress = (cramOffset + colorIndex) & kCRAMAddressMask;
    return UnpackRGBA8(g_cramColor[cramAddress]); // Brimir: packed RGBA8
}

uint4 Color555(uint val16) {
    return uint4(
        BitExtract(val16, 0u, 5u) << 3u,
        BitExtract(val16, 5u, 5u) << 3u,
        BitExtract(val16, 10u, 5u) << 3u,
        BitExtract(val16, 15u, 1u)
    );
}

uint4 Color888(uint val32) {
    return uint4(
        BitExtract(val32, 0u, 8u),
        BitExtract(val32, 8u, 8u),
        BitExtract(val32, 16u, 8u),
        BitExtract(val32, 31u, 1u)
    );
}

// ---------------------------------------------------------------------------------------------------------------------
// Special color calculation bits

bool IsSpecialColorCalcMatch(uint specFuncSelect, uint specColorCode) {
    return BitTest(g_layerRenderParams[0].specialFunctionCodes, specFuncSelect * 8 + specColorCode);
}

bool GetSpecialColorCalcFlag(const BaseBGParams params, uint specColorCode, bool specColorCalc, bool colorMSB) {
    const bool colorCalcEnable = params.colorCalcEnable;
    if (!colorCalcEnable) {
        return false;
    }

    const uint specColorCalcMode = params.specialColorCalcMode;
    switch (specColorCalcMode) {
        case kSpecColorCalcModeScreen:
            return colorCalcEnable;
        case kSpecColorCalcModeCharacter:
            return specColorCalc;
        case kSpecColorCalcModeDot:
            return specColorCalc && IsSpecialColorCalcMatch(params.specialFunctionSelect, specColorCode);
        case kSpecColorCalcModeColorMSB:
            return colorMSB;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rotation parameter table fetching

struct RotTable {
    // Screen start coordinates (signed 13.10 fixed point)
    int Xst, Yst, Zst;

    // Screen vertical coordinate increments (signed 3.10 fixed point)
    int deltaXst, deltaYst;

    // Screen horizontal coordinate increments (signed 3.10 fixed point)
    int deltaX, deltaY;

    // Rotation matrix parameters (signed 4.10 fixed point)
    int A, B, C, D, E, F;

    // Viewpoint coordinates (signed 14-bit integer)
    int Px, Py, Pz;

    // Center point coordinates (signed 14-bit integer)
    int Cx, Cy, Cz;

    // Horizontal shift (signed 14.10 fixed point)
    int Mx, My;

    // Scaling coefficients (signed 8.16 fixed point)
    int kx, ky;

    // Coefficient table parameters
    uint KAst; // Coefficient table start address (unsigned 16.10 fixed point)
    int dKAst; // Coefficient table vertical increment (signed 10.10 fixed point)
    int dKAx; // Coefficient table horizontal increment (signed 10.10 fixed point)
};

struct RotCoefficient {
    int value; // coefficient value, scaled to 16 fractional bits
    uint lineColorData;
    bool transparent;
};

RotTable ReadRotTable(const uint address) {
    RotTable table;

    table.Xst = SignExtend(ReadVRAM32(address + 0x00) >> 6, 23);
    table.Yst = SignExtend(ReadVRAM32(address + 0x04) >> 6, 23);
    table.Zst = SignExtend(ReadVRAM32(address + 0x08) >> 6, 23);

    table.deltaXst = SignExtend(ReadVRAM32(address + 0x0C) >> 6, 13);
    table.deltaYst = SignExtend(ReadVRAM32(address + 0x10) >> 6, 13);

    table.deltaX = SignExtend(ReadVRAM32(address + 0x14) >> 6, 13);
    table.deltaY = SignExtend(ReadVRAM32(address + 0x18) >> 6, 13);

    table.A = SignExtend(ReadVRAM32(address + 0x1C) >> 6, 14);
    table.B = SignExtend(ReadVRAM32(address + 0x20) >> 6, 14);
    table.C = SignExtend(ReadVRAM32(address + 0x24) >> 6, 14);
    table.D = SignExtend(ReadVRAM32(address + 0x28) >> 6, 14);
    table.E = SignExtend(ReadVRAM32(address + 0x2C) >> 6, 14);
    table.F = SignExtend(ReadVRAM32(address + 0x30) >> 6, 14);

    table.Px = SignExtend(ReadVRAM16(address + 0x34), 14);
    table.Py = SignExtend(ReadVRAM16(address + 0x36), 14);
    table.Pz = SignExtend(ReadVRAM16(address + 0x38), 14);

    table.Cx = SignExtend(ReadVRAM16(address + 0x3C), 14);
    table.Cy = SignExtend(ReadVRAM16(address + 0x3E), 14);
    table.Cz = SignExtend(ReadVRAM16(address + 0x40), 14);

    table.Mx = SignExtend(ReadVRAM32(address + 0x44) >> 6, 24);
    table.My = SignExtend(ReadVRAM32(address + 0x48) >> 6, 24);

    table.kx = SignExtend(ReadVRAM32(address + 0x4C), 24);
    table.ky = SignExtend(ReadVRAM32(address + 0x50), 24);

    table.KAst = ReadVRAM32(address + 0x54) >> 6;
    table.dKAst = SignExtend(ReadVRAM32(address + 0x58) >> 6, 20);
    table.dKAx = SignExtend(ReadVRAM32(address + 0x5C) >> 6, 20);

    return table;
}

bool CanFetchCoefficient(const uint coeffDataSize, uint coeffAddress) {
    if (coeffTableCRAM) {
        return true;
    }

    if (!coeffDataPerDot) {
        return true;
    }

    const uint offset = coeffAddress >> 10u;
    const uint address = (offset * 4) >> coeffDataSize;
    const uint bank = BitExtract(address, 17, 2);
    return BitTest(coeffDataAccess, bank);
}

RotCoefficient ReadRotCoefficient(const uint coeffDataSize, const uint coeffDataMode, uint coeffAddress) {
    const uint offset = coeffAddress >> 10;

    RotCoefficient coeff;

    // Force coefficient to 0 if it cannot be read in per-dot mode
    if (!CanFetchCoefficient(coeffDataSize, coeffAddress)) {
        coeff.value = 0;
        coeff.lineColorData = 0;
        coeff.transparent = true;
        return coeff;
    }

    if (coeffDataSize == 1) {
        // One-word coefficient data
        const uint address = offset * 2;
        const uint data = coeffTableCRAM ? ReadCRAMRotCoeff16(address) : ReadVRAM16(address);
        coeff.value = SignExtend(data, 15);
        coeff.lineColorData = 0;
        coeff.transparent = BitTest(data, 15);

        if (coeffDataMode == kCoeffDataModeViewpointX) {
            coeff.value <<= 14;
        } else {
            coeff.value <<= 6;
        }
    } else {
        // Two-word coefficient data
        const uint address = offset * 4;
        const uint data = coeffTableCRAM ? ReadCRAMRotCoeff32(address) : ReadVRAM32(address);
        coeff.value = SignExtend(data, 24);
        coeff.lineColorData = BitExtract(data, 24, 7);
        coeff.transparent = BitTest(data, 31);

        if (coeffDataMode == kCoeffDataModeViewpointX) {
            coeff.value <<= 8;
        }
    }

    return coeff;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rotation parameter calculation

uint2 CalcRotationScreenCoords(uint2 pos, uint index) {
    const RotParamBase base = g_rotParamBases[index * kRotParamBaseStride + pos.y]; // Brimir: 512-line stride
    const uint coeffParamsOffset = 6 + index * 5;
    const bool coeffTableEnable = BitTest(g_commonParams.rotParams, coeffParamsOffset + 0);
    const uint coeffDataSize = BitExtract(g_commonParams.rotParams, coeffParamsOffset + 1, 1);
    const uint coeffDataMode = BitExtract(g_commonParams.rotParams, coeffParamsOffset + 2, 2);

    const RotTable t = ReadRotTable(base.tableAddress);

    int Tx, Ty, Tz;

    // Common terms for Xsp and Ysp (14.10)
    // 10 - 0 = 10 frac bits
    // 23 - 14 = 23 total bits
    // expand to 10 frac bits
    // 10 - 10 = 10 frac bits
    // 23 - 24 = 24 total bits
    const int Xst = base.Xst;
    const int Yst = base.Yst;
    const int Zst = t.Zst;
    Tx = Xst - (t.Px << 10);
    Ty = Yst - (t.Py << 10);
    Tz = Zst - (t.Pz << 10);

    // Transformed starting screen coordinates (18.10)
    // 10*(10-10) + 10*(10-10) + 10*(10-10) = 20 frac bits
    // 14*(23-24) + 14*(23-24) + 14*(23-24) = 38 total bits
    // reduce to 10 frac bits
    // Brimir: int((int64_t(a0) * b0 + int64_t(a1) * b1 + int64_t(a2) * b2) >> 10) in 32-bit arithmetic (no Int64).
    const int Xsp = RotDotShr10(t.A, Tx, t.B, Ty, t.C, Tz);
    const int Ysp = RotDotShr10(t.D, Tx, t.E, Ty, t.F, Tz);

    // Transformed view coordinates (18.10)
    // 10*(0-0) + 10*(0-0) + 10*(0-0) + 10 + 10 = 10+10+10 + 10+10 = 10 frac bits
    // 14*(14-14) + 14*(14-14) + 14*(14-14) + 24 + 24 = 28+28+28 + 24+24 = 28 total bits
    Tx = t.Px - t.Cx;
    Ty = t.Py - t.Cy;
    Tz = t.Pz - t.Cz;
    int /***/ Xp = t.A * Tx + t.B * Ty + t.C * Tz + (t.Cx << 10) + t.Mx;
    const int Yp = t.D * Tx + t.E * Ty + t.F * Tz + (t.Cy << 10) + t.My;

    // Screen coordinate increments per Hcnt (7.10)
    // 10*10 + 10*10 = 20 + 20 = 20 frac bits
    // 14*13 + 14*13 = 27 + 27 = 27 total bits
    // reduce to 10 frac bits
    const int scrXIncH = (t.A * t.deltaX + t.B * t.deltaY) >> 10;
    const int scrYIncH = (t.D * t.deltaX + t.E * t.deltaY) >> 10;

    int kx = t.kx;
    int ky = t.ky;

    if (coeffTableEnable) {
        // Current coefficient address (16.10)
        const uint KAxofs = coeffDataPerDot ? pos.x * t.dKAx : 0;
        const uint KA = base.KA + KAxofs;

        // Read and apply rotation coefficient
        const RotCoefficient coeff = ReadRotCoefficient(coeffDataSize, coeffDataMode, KA);

        switch (coeffDataMode) {
            case kCoeffDataModeScaleCoeffXY:
                kx = ky = coeff.value;
                break;
            case kCoeffDataModeScaleCoeffX:
                kx = coeff.value;
                break;
            case kCoeffDataModeScaleCoeffY:
                ky = coeff.value;
                break;
            case kCoeffDataModeViewpointX:
                Xp = coeff.value << 2;
                break;
        }
    }

    // Current screen coordinates (18.10)
    const int scrX = Xsp + pos.x * scrXIncH;
    const int scrY = Ysp + pos.x * scrYIncH;

    // Resulting screen coordinates (26.0)
    // (16*10) + 10 = 26 + 10 frac bits
    // (24*28) + 28 = 52 + 28 total bits
    // reduce 26 to 10 frac bits
    // = 10 + 10 = 10 frac bits
    // = 36 + 28 = 36 total bits
    // remove frac bits from result = 26 total bits
    // Brimir: uint((((int64_t(k) * scr) >> 16) + p) >> 10) in 32-bit arithmetic (no Int64).
    return uint2(
        RotCoordShr(kx, scrX, Xp),
        RotCoordShr(ky, scrY, Yp)
    );
}

RotCoefficient CalcRotationCoefficient(uint2 pos, uint index) {
    const RotParamBase base = g_rotParamBases[index * kRotParamBaseStride + pos.y]; // Brimir: 512-line stride
    const uint coeffParamsOffset = 6 + index * 5;
    const bool coeffTableEnable = BitTest(g_commonParams.rotParams, coeffParamsOffset + 0);

    RotCoefficient coeff;
    if (coeffTableEnable) {
        // Current coefficient address (16.10)
        const int dKAx = SignExtend(ReadVRAM32(base.tableAddress + 0x5C) >> 6, 20);
        const uint KAxofs = coeffDataPerDot ? pos.x * dKAx : 0;
        const uint KA = base.KA + KAxofs;

        // Read and apply rotation coefficient
        const uint coeffDataSize = BitExtract(g_commonParams.rotParams, coeffParamsOffset + 1, 1);
        const uint coeffDataMode = BitExtract(g_commonParams.rotParams, coeffParamsOffset + 2, 2);
        coeff = ReadRotCoefficient(coeffDataSize, coeffDataMode, KA);
    } else {
        coeff.value = 0;
        coeff.lineColorData = 0;
        coeff.transparent = true;
    }

    return coeff;
}

// ---------------------------------------------------------------------------------------------------------------------
// Character fetching

struct Character {
    uint charNum;
    uint palNum;
    bool specColorCalc;
    bool specPriority;
    bool flipH;
    bool flipV;
};
static const Character kBlankCharacter = (Character) 0;

Character ExtractOneWordCharacter(const BaseBGParams params, uint charData) {
    const bool extChar = params.extChar;
    const uint cellSizeShift = params.cellSizeShift;

    // Character number bit range from the 1-word character pattern data (charData)
    const uint baseCharNumMask = extChar ? 0xFFF : 0x3FF;
    const uint baseCharNumPos = 2 * cellSizeShift;

    // Upper character number bit range from the supplementary character number (bgParams.supplCharNum)
    const uint supplCharNumStart = 2 * cellSizeShift + (extChar ? 2 : 0);
    const uint supplCharNumMask = 0x1Fu >> supplCharNumStart;
    const uint supplCharNumPos = 10 + supplCharNumStart;
    // The lower bits are always in range 0..1 and only used if cellSizeShift == true

    const uint baseCharNum = charData & baseCharNumMask;
    const uint supplCharNum = (params.supplScrollCharNum >> supplCharNumStart) & supplCharNumMask;

    Character ch;
    ch.charNum = (baseCharNum << baseCharNumPos) | (supplCharNum << supplCharNumPos);
    if (cellSizeShift > 0) {
        ch.charNum |= BitExtract(params.supplScrollCharNum, 0, 2);
    }
    if (params.colorFormat != kColorFormatPalette16) {
        ch.palNum = BitExtract(charData, 12, 3) << 8;
    } else {
        ch.palNum = (BitExtract(charData, 12, 4) | params.supplPalNum) << 4;
    }
    ch.specColorCalc = params.supplSpecialColorCalc;
    ch.specPriority = params.supplSpecialPriority;
    ch.flipH = !extChar && BitTest(charData, 10);
    ch.flipV = !extChar && BitTest(charData, 11);
    return ch;
}

Character FetchTwoWordCharacter(const BaseBGParams params, uint pageAddress, uint charIndex) {
    const uint charAddress = pageAddress + charIndex * 4;
    const uint charBank = BitExtract(charAddress, 17, 2);

    if (!BitTest(params.patNameAccess, charBank)) {
        return kBlankCharacter;
    }

    const uint charData = ReadVRAM32(charAddress);

    Character ch;
    ch.charNum = BitExtract(charData, 0, 15);
    ch.palNum = BitExtract(charData, 16, 7) << 4;
    ch.specColorCalc = BitTest(charData, 28);
    ch.specPriority = BitTest(charData, 29);
    ch.flipH = BitTest(charData, 30);
    ch.flipV = BitTest(charData, 31);
    return ch;
}

Character FetchOneWordCharacter(const BaseBGParams params, uint pageAddress, uint charIndex) {
    const uint charAddress = pageAddress + charIndex * 2;
    const uint charBank = BitExtract(charAddress, 17, 2);
    if (!BitTest(params.patNameAccess, charBank)) {
        return kBlankCharacter;
    }

    const uint charData = ReadVRAM16(charAddress);
    return ExtractOneWordCharacter(params, charData);
}

// ---------------------------------------------------------------------------------------------------------------------
// Pixel fetching

uint4 FetchPixel(const BaseBGParams params, uint baseAddress, uint2 dotPos, uint linePitch, bool applyVRAMDelay, uint palNum, bool specColorCalc, uint specPriority) {
    const uint charPatAccess = params.charPatAccess;
    const uint colorFormat = params.colorFormat;
    const uint cramOffset = params.cramOffset;
    const uint bgPriorityNum = params.priorityNumber;
    const uint bgPriorityMode = params.priorityMode;
    const bool enableTransparency = params.enableTransparency;

    const uint dotOffset = dotPos.x + dotPos.y * linePitch;

    uint colorData;
    uint4 outColor;
    bool outTransparent;
    bool outSpecColorCalc;
    if (colorFormat == kColorFormatPalette16) {
        const uint dotAddress = baseAddress + (dotOffset >> 1);
        const uint dotBank = BitExtract(dotAddress, 17, 2);
        const uint vramAccessOffset = applyVRAMDelay ? (BitExtract(params.vramDataOffset, dotBank, 1) << 3) : 0;
        const uint dotData = BitTest(charPatAccess, dotBank) ? ReadVRAM4(dotAddress + vramAccessOffset, ~dotPos.x & 1) : 0;
        const uint colorIndex = palNum | dotData;
        colorData = BitExtract(dotData, 1, 3);
        outColor = FetchCRAMColor(cramOffset, colorIndex);
        outTransparent = enableTransparency && dotData == 0;
        outSpecColorCalc = GetSpecialColorCalcFlag(params, colorData, specColorCalc, BitTest(outColor.a, 0));

    } else if (colorFormat == kColorFormatPalette256) {
        const uint dotAddress = baseAddress + dotOffset;
        const uint dotBank = BitExtract(dotAddress, 17, 2);
        const uint vramAccessOffset = applyVRAMDelay ? (BitExtract(params.vramDataOffset, dotBank, 1) << 3) : 0;
        const uint dotData = BitTest(charPatAccess, dotBank) ? ReadVRAM8(dotAddress + vramAccessOffset) : 0;
        const uint colorIndex = (palNum & 0x700) | dotData;
        colorData = BitExtract(dotData, 1, 3);
        outColor = FetchCRAMColor(cramOffset, colorIndex);
        outTransparent = enableTransparency && dotData == 0;
        outSpecColorCalc = GetSpecialColorCalcFlag(params, colorData, specColorCalc, BitTest(outColor.a, 0));

    } else if (colorFormat == kColorFormatPalette2048) {
        const uint dotAddress = baseAddress + (dotOffset << 1);
        const uint dotBank = BitExtract(dotAddress, 17, 2);
        const uint vramAccessOffset = applyVRAMDelay ? (BitExtract(params.vramDataOffset, dotBank, 1) << 3) : 0;
        const uint dotData = BitTest(charPatAccess, dotBank) ? ReadVRAM16(dotAddress + vramAccessOffset) : 0;
        const uint colorIndex = dotData & 0x7FF;
        colorData = BitExtract(dotData, 1, 3);
        outColor = FetchCRAMColor(cramOffset, colorIndex);
        outTransparent = enableTransparency && (dotData & 0x7FF) == 0;
        outSpecColorCalc = GetSpecialColorCalcFlag(params, colorData, specColorCalc, BitTest(outColor.a, 0));

    } else if (colorFormat == kColorFormatRGB555) {
        const uint dotAddress = baseAddress + (dotOffset << 1);
        const uint dotBank = BitExtract(dotAddress, 17, 2);
        const uint vramAccessOffset = applyVRAMDelay ? (BitExtract(params.vramDataOffset, dotBank, 1) << 3) : 0;
        const uint dotData = BitTest(charPatAccess, dotBank) ? ReadVRAM16(dotAddress + vramAccessOffset) : 0;
        outColor = Color555(dotData);
        outTransparent = enableTransparency && outColor.w == 0;
        outSpecColorCalc = GetSpecialColorCalcFlag(params, 7, specColorCalc, true);

    } else if (colorFormat == kColorFormatRGB888) {
        const uint dotAddress = baseAddress + (dotOffset << 2);
        const uint dotBank = BitExtract(dotAddress, 17, 2);
        const uint vramAccessOffset = applyVRAMDelay ? (BitExtract(params.vramDataOffset, dotBank, 1) << 3) : 0;
        const uint dotData = BitTest(charPatAccess, dotBank) ? ReadVRAM32(dotAddress + vramAccessOffset) : 0;
        outColor = Color888(dotData);
        outTransparent = enableTransparency && outColor.w == 0;
        outSpecColorCalc = GetSpecialColorCalcFlag(params, 7, specColorCalc, true);

    } else {
        colorData = 0;
        outColor = uint4(0, 0, 0, 0);
        outTransparent = true;
        outSpecColorCalc = false;
    }

    if (outTransparent) {
        return kTransparentPixel;
    }

    uint outPriority = bgPriorityNum;
    if (bgPriorityMode == kPriorityModeCharacter) {
        outPriority &= ~1;
        outPriority |= specPriority;
    } else if (bgPriorityMode == kPriorityModeDot) {
        outPriority &= ~1;
        if (specPriority != 0 && colorFormat < kColorFormatRGB555) {
            outPriority |= IsSpecialColorCalcMatch(params.specialFunctionSelect, colorData);
        }
    }

    const uint outPaletteFormat = colorFormat <= kColorFormatPalette2048 ? 1u : 0u;

    return uint4(
        outColor.rgb,
        (outPaletteFormat << kPixelAttrBitPaletteFormat) |
        (outSpecColorCalc << kPixelAttrBitSpecColorCalc) |
        outPriority
    );
}

// Brimir: matches software (sw:5298-5335): no cell swap for delayed two-word 2x2 characters (Ymir swaps the cells
// instead of delaying the character; the delay is DrawNBGScrollPixel's).
uint4 FetchCharacterPixel(const BaseBGParams params, Character ch, uint2 dotPos, uint cellIndex) {
    const uint cellSizeShift = params.cellSizeShift;
    const uint colorFormat = params.colorFormat;

    if (ch.flipH) {
        dotPos.x ^= 7;
        if (cellSizeShift > 0) {
            cellIndex ^= 1;
        }
    }
    if (ch.flipV) {
        dotPos.y ^= 7;
        if (cellSizeShift > 0) {
            cellIndex ^= 2;
        }
    }

    // Adjust cell index based on color format
    if (colorFormat == kColorFormatRGB888) {
        cellIndex <<= 3;
    } else if (colorFormat == kColorFormatRGB555) {
        cellIndex <<= 2;
    } else if (colorFormat != kColorFormatPalette16) {
        cellIndex <<= 1;
    }

    const uint baseAddress = (ch.charNum + cellIndex) << 5;
    return FetchPixel(params, baseAddress, dotPos, 8, false, ch.palNum, ch.specColorCalc, ch.specPriority);
}

uint4 FetchBitmapPixel(const BaseBGParams params, uint bitmapBaseAddress, uint2 scrollPos) {
    const uint2 bitmapSize = params.bitmapSize;

    const uint2 dotPos = scrollPos & (bitmapSize - 1u);
    const uint palNum = params.supplPalNum;
    const bool specColorCalc = params.supplSpecialColorCalc;
    const uint specPriority = params.supplSpecialPriority;

    return FetchPixel(params, bitmapBaseAddress, dotPos, bitmapSize.x, true, palNum, specColorCalc, specPriority);
}

// Brimir: where a scroll BG pixel's character is (FetchScrollBGPixel split in two, so that NBG character delays can
// fetch the character of another position).
struct CharLocation {
    uint pageAddress;
    uint charIndex; // within the page, as software's VRAMFetcher::lastCharIndex (sw:5181)
    uint cellX;     // cell column within a 2x2 character
    uint cellIndex;
    uint2 dotPos;
    uint bank;      // VRAM bank of the page base address
};

// Brimir: the page base addresses are read from g_layerRenderParams (nbg[index] or rbg[index] with rotSel) instead of
// a uint[16] argument, which needed a private pbaResized[16] copy for NBGs.
CharLocation LocateScrollBGChar(const BaseBGParams params, uint2 scrollPos, uint2 pageShift, bool rot, uint index,
                                uint rotSel) {
    const uint planeShift = rot ? 2 : 1;
    const uint planeMask = (1u << planeShift) - 1u;

    const bool twoWordChar = params.twoWordChar;
    const uint cellSizeShift = params.cellSizeShift;
    const uint pageSize = kPageSizes[cellSizeShift][twoWordChar ? 1 : 0];

    const uint2 planePos = (scrollPos >> (pageShift + 9)) & planeMask;
    const uint plane = planePos.x | (planePos.y << planeShift);
    const uint pageBaseAddress = rot ? g_layerRenderParams[0].rbg[index].pageBaseAddresses[rotSel][plane]
                                     : g_layerRenderParams[0].nbg[index].pageBaseAddresses[plane];

    // HACK: apply data access shift
    // Not entirely correct, but fixes problems with World Heroes Perfect's demo screen
    const uint bank = BitExtract(pageBaseAddress, 17, 2);
    if (BitTest(params.vramDataOffset, bank)) {
        scrollPos.x += 8;
    }

    const uint2 pagePos = (scrollPos >> 9) & pageShift;
    const uint page = pagePos.x + (pagePos.y << 1);
    const uint pageOffset = page << pageSize;
    const uint pageAddress = pageBaseAddress + pageOffset;

    // HACK: work around FXC bug that produces invalid code for the line below:
    //   const uint2 charPatPos = ((scrollPos >> 3) & 0x3F) >> cellSizeShift;
    // When cellSizeShift is derived from a masked/shifted value, the compiler merges the two shifts into one:
    //   const uint2 charPatPos = (scrollPos >> (3 + cellSizeShift) & 0x3F;
    // See https://shader-playground.timjones.io/1df21a52a4e485bd355e1c9bab45bbd8
    const uint2 baseCharPatPos = (scrollPos >> 3) & 0x3F;
    const uint2 charPatPos = cellSizeShift != 0 ? (baseCharPatPos >> 1) : baseCharPatPos;
    const uint charIndex = charPatPos.x + (charPatPos.y << (6 - cellSizeShift));

    const uint2 cellPos = (scrollPos >> 3) & cellSizeShift;

    CharLocation loc;
    loc.pageAddress = pageAddress;
    loc.charIndex = charIndex;
    loc.cellX = cellPos.x;
    loc.cellIndex = cellPos.x + (cellPos.y << 1);
    loc.dotPos = scrollPos & 7;
    loc.bank = bank;
    return loc;
}

Character FetchCharacter(const BaseBGParams params, CharLocation loc) {
    if (params.twoWordChar != 0) {
        return FetchTwoWordCharacter(params, loc.pageAddress, loc.charIndex);
    } else {
        return FetchOneWordCharacter(params, loc.pageAddress, loc.charIndex);
    }
}

uint4 FetchScrollBGPixel(const BaseBGParams params, uint2 scrollPos, uint2 pageShift, bool rot, uint index,
                         uint rotSel) {
    const CharLocation loc = LocateScrollBGChar(params, scrollPos, pageShift, rot, index, rotSel);
    return FetchCharacterPixel(params, FetchCharacter(params, loc), loc.dotPos, loc.cellIndex);
}

// Brimir: RBG `index` and `rotSel` instead of its pageBaseAddresses[rotSel].
uint4 FetchScrollRBGPixel(const BaseBGParams params, uint2 scrollPos, uint2 pageShift, uint index, uint rotSel) {
    return FetchScrollBGPixel(params, scrollPos, pageShift, true, index, rotSel);
}

// ---------------------------------------------------------------------------------------------------------------------
// NBG drawing
//
// Brimir: matches software (sw:2829-2911, 4516-4698, 5152-5219) with the per-line NBG state the front half computes as
// software does (VDP2NBGLine; Ymir derives the scroll from the parameters here, bgs:889-956):
// - scroll: fracScrollX/Y and scrollIncH of the row, plus the scroll amounts of the batch (software adds the register
//   values at draw time);
// - horizontal mosaic: pixel x is the pixel at x - x % mosaicH, window included (sw:4553-4567, 4657-4671); vertical
//   mosaic: the row repeats the layer output of mosaicSrcLine, drawn here with that row's state or loaded from the
//   layer image if an earlier batch drew it (sw:2889-2892);
// - vertical cell scroll: the value of the k-th table read, k the cell column changes since x = 0, with software's
//   repeat and delay rules; reads before the first are the previous line's last value (sw:4526-4574); with mosaic,
//   the first value applies to the whole line;
// - character pattern delay, and vertical cell scroll on 2x2 characters: the pixel shows the character software's
//   fetcher shows there (sw:4549-4601, 5193-5215), which the fetch pass (cs_vdp2_nbg_fetch.hlsl) stores per pixel in
//   g_nbgChars, windows included; the left edge's delayed character is the fetcher's nextChar, decoded on the CPU.

uint NBGWindowY(uint row) {
    uint y = GetY(row, true);
    if (deinterlace && interlaceMode == kInterlaceModeSingleDensity) {
        y >>= 1;
    }
    return y;
}

uint NBGVCellScrollY(const NBGParams params, NBGLine nbgLine, uint index, uint x) {
    if (index > 1 || !params.vcellScrollEnable) {
        return 0;
    }

    // Table reads after the line's first one: one per cell column change between consecutive pixels (the increment is
    // below 8 pixels, so at most one per pixel)
    uint reads = 0;
    if (!params.base.mosaicEnable) {
        const uint fracScrollX = uint(nbgLine.fracScrollX) + params.scrollAmount.x;
        reads = ((fracScrollX & 0x7FF) + x * uint(nbgLine.scrollIncH)) >> 11;
    }

    int offset = int(reads);
    if (params.vcellScrollRepeat && offset > 0) {
        --offset;
    }
    if (params.vcellScrollDelay) {
        --offset;
    }
    if (offset < 0) {
        return nbgLine.prevLastVCellScroll;
    }

    const uint vcellScrollTableAddress = BitExtract(g_commonParams.vcellScroll, 0, 19);
    const uint vcellScrollInc = BitExtract(g_commonParams.vcellScroll, 19, 3) << 2u;
    const uint vcellAddress = vcellScrollTableAddress + params.vcellScrollOffset + uint(offset) * vcellScrollInc;
    return BitExtract(ReadVRAM32(vcellAddress), 8, 19);
}

uint2 NBGScrollPos(const NBGParams params, NBGLine nbgLine, uint index, uint x) {
    const uint fracScrollX = uint(nbgLine.fracScrollX) + params.scrollAmount.x + x * uint(nbgLine.scrollIncH);
    const uint fracScrollY = uint(nbgLine.fracScrollY) + params.scrollAmount.y + NBGVCellScrollY(params, nbgLine, index, x);
    return uint2(fracScrollX >> 8, fracScrollY >> 8);
}

// Brimir: NBG `index` instead of its pageBaseAddresses[4] (no pbaResized[16] copy).
CharLocation LocateNBGChar(const NBGParams params, uint2 scrollPos, uint index) {
    return LocateScrollBGChar(params.base, scrollPos, params.base.pageShift, false, index, 0);
}

CharLocation LocateNBGCharAt(const NBGParams params, NBGLine nbgLine, uint index, uint x) {
    return LocateNBGChar(params, NBGScrollPos(params, nbgLine, index, x), index);
}

// A ymir::vdp::Character::u32 (the two-word pattern name layout) as a Character.
Character UnpackCharacter(uint value) {
    Character ch;
    ch.charNum = BitExtract(value, 0, 15);
    ch.palNum = BitExtract(value, 16, 7) << 4;
    ch.specColorCalc = BitTest(value, 28);
    ch.specPriority = BitTest(value, 29);
    ch.flipH = BitTest(value, 30);
    ch.flipV = BitTest(value, 31);
    return ch;
}

uint4 DrawNBGScrollPixel(const NBGParams params, NBGLine nbgLine, uint index, uint row, uint x) {
    const CharLocation loc = LocateNBGCharAt(params, nbgLine, index, x);
    if (NBGUsesCharTable(params, index)) {
        // The character the fetcher shows at x, from the fetch pass (cs_vdp2_nbg_fetch.hlsl). The dot and cell are the
        // pixel's own (sw:5193-5229).
        const Character ch = UnpackCharacter(g_nbgChars[NBGCharIndex(index, row, x)]);
        return FetchCharacterPixel(params.base, ch, loc.dotPos, loc.cellIndex);
    }
    // A character is fetched when the index changes, and within a run of one index the scroll position moves by less
    // than a page: the character at x, except when a window hides 512 dots or more (exception W5).
    return FetchCharacterPixel(params.base, FetchCharacter(params.base, loc), loc.dotPos, loc.cellIndex);
}

uint4 DrawNBG(uint2 pos, // dispatch coordinates
              uint index // NBG index (0 to 3)
             ) {
    const NBGParams params = g_layerRenderParams[0].nbg[index];
    if (!params.base.enabled) {
        return kTransparentPixel;
    }

    uint row = pos.y;
    NBGLine nbgLine = g_nbgLines[index * kMaxResV + row];
    if (nbgLine.mosaicSrcLine != row) {
        // Vertical mosaic: the row repeats an earlier row's layer output
        row = nbgLine.mosaicSrcLine;
        if (row < g_commonParams.startY) {
            return g_layerOut[uint3(pos.x, GetY(row, false), index)];
        }
        nbgLine = g_nbgLines[index * kMaxResV + row];
    }
    const uint windowY = NBGWindowY(row);

    const uint mosaicH = params.base.mosaicEnable ? BitExtract(g_commonParams.layerParams, 14, 4) + 1 : 1;
    const uint x = pos.x - pos.x % mosaicH;
    if (InsideWindows(params.base.windowParams, uint2(x, windowY))) {
        return kTransparentPixel;
    }

    if (params.base.bitmap) {
        return FetchBitmapPixel(params.base, params.base.bitmapBaseAddress, NBGScrollPos(params, nbgLine, index, x));
    }
    return DrawNBGScrollPixel(params, nbgLine, index, row, x);
}

// ---------------------------------------------------------------------------------------------------------------------
// RBG drawing

uint SelectRotationParameter(const RBGParams params, uint2 pos) {
    const uint rotParamMode = BitExtract(g_commonParams.layerParams, 22, 2);
    switch (rotParamMode) {
        case kRotParamModeA:
            return kRotParamA;
        case kRotParamModeB:
            return kRotParamB;
        case kRotParamModeCoeff:{
                const bool coeffTableEnable = BitTest(g_commonParams.rotParams, 6);
                if (!coeffTableEnable) {
                    return kRotParamA;
                }
                const bool transparent = CalcRotationCoefficient(pos, 0).transparent;
                return transparent ? kRotParamB : kRotParamA;
            }
        case kRotParamModeWindow:
            return InsideWindows(g_layerRenderParams[0].rotWindows, uint2(pos.x, GetY(pos.y, false))) ? kRotParamB : kRotParamA;
    }
    return kRotParamA; // shouldn't happen
}

void StoreRotationLineColorData(uint2 pos, uint2 rotPos, uint index, uint rotSel) {
    const bool lineColorEnabled = BitTest(g_commonParams.layerParams, 12 + index);
    if (!lineColorEnabled) {
        return;
    }

    const uint rotParamMode = BitExtract(g_commonParams.layerParams, 22, 2);
    const bool hasRBG1 = BitTest(g_commonParams.layerParams, 13);

    bool useCoeffLineColor = false;
    uint coeffSel;

    switch (rotParamMode) {
        case kRotParamModeA:
            useCoeffLineColor = rotSel == kRotParamA;
            coeffSel = kRotParamA;
            break;
        case kRotParamModeB:
            useCoeffLineColor = rotSel == kRotParamB;
            coeffSel = hasRBG1 ? kRotParamA : kRotParamB;
            break;
        case kRotParamModeCoeff:
            useCoeffLineColor = true;
            coeffSel = kRotParamA;
            break;
        case kRotParamModeWindow:
            useCoeffLineColor = true;
            coeffSel = hasRBG1 ? kRotParamA : rotSel;
            break;
    }

    const bool lineColorPerLine = g_layerRenderParams[0].lineScreenParams.perLine;
    const uint lineColorBaseAddress = g_layerRenderParams[0].lineScreenParams.baseAddress;

    const uint lineColorY = lineColorPerLine ? pos.y : 0;
    const uint lineColorAddress = lineColorBaseAddress + lineColorY * 2;

    uint cramAddress = ReadVRAM16(lineColorAddress);

    if (useCoeffLineColor) {
        const uint coeffParamsOffset = 6 + coeffSel * 5;
        const bool coeffTableEnable = BitTest(g_commonParams.rotParams, coeffParamsOffset + 0);
        const bool coeffUseLineColorData = BitTest(g_commonParams.rotParams, coeffParamsOffset + 4);
        if (coeffTableEnable && coeffUseLineColorData) {
            const uint baseLineColorData = BitExtract(cramAddress, 7, 4);
            const uint lineColorData = CalcRotationCoefficient(rotPos.xy, coeffSel).lineColorData;
            cramAddress = (baseLineColorData << 7) | lineColorData;
        }
    }

    // Brimir: matches software (sw:2234, 2270, 2653-2654, 4923-4930): the line colour is the RGB555 colour of CRAM word
    // (address & 0x7FF) in every CRAM mode (Ymir reads the mode-dependent colour cache and returns 0 from 2048 up).
    // Writes outside the image (352 x 512) are dropped, as D3D12 does (no robust access).
    if (pos.x < kMaxNormalResH && pos.y < kMaxResV) {
        g_rbgLineColorOut[uint3(pos.xy, index)] = UnpackRGBA8(g_cramColor[kCRAM555TableBase + (cramAddress & 0x7FF)]);
    }
}

// Brimir: linePos is where the pixel's line colour goes (the repeated pixel under mosaic); pos is the pixel drawn.
uint4 DrawScrollRBG(uint2 pos, uint2 linePos, uint index, uint rotSel, uint2 scrollPos) {
    const RBGParams params = g_layerRenderParams[0].rbg[index];

    // Brimir: mosaic is applied by DrawRBG (sw:4713-4733).
    const uint2 rotPos = pos;

    const RBGParams rotParams = g_layerRenderParams[0].rbg[rotSel];
    const uint2 pageShift = rotParams.base.pageShift;

    // Determine maximum coordinates and screen over process
    const uint screenOverProcess = rotParams.screenOverProcess;
    const bool usingFixed512 = screenOverProcess == kScreenOverProcessFixed512;
    const bool usingRepeat = screenOverProcess == kScreenOverProcessRepeat;
    const uint2 scrollSize = usingFixed512
        ? uint2(512, 512)
        : uint2(512 * 4, 512 * 4) << pageShift;

    if (all(scrollPos < scrollSize) || usingRepeat) {
        StoreRotationLineColorData(linePos, rotPos, index, rotSel);

        // Brimir: index and rotSel, not pageBaseAddresses[rotSel]
        return FetchScrollRBGPixel(params.base, scrollPos, pageShift, index, rotSel);
    }

    // Out of bounds

    if (screenOverProcess == kScreenOverProcessRepeatChar) {
        StoreRotationLineColorData(linePos, rotPos, index, rotSel);

        const uint2 dotPos = scrollPos & 7;
        Character ch = ExtractOneWordCharacter(params.base, rotParams.screenOverPatternName);
        return FetchCharacterPixel(params.base, ch, dotPos, 0);
    }

    return kTransparentPixel;
}

uint4 DrawBitmapRBG(uint2 pos, uint index, uint rotSel, uint2 scrollPos) {
    const RBGParams params = g_layerRenderParams[0].rbg[index];
    const uint screenOverProcess = params.screenOverProcess;

    // Brimir: matches software (sw:4814-4881): bitmap RBGs have no mosaic.
    const uint2 rotPos = pos;

    const uint2 pageShift = g_layerRenderParams[0].rbg[rotSel].base.pageShift;

    // Determine maximum coordinates and screen over process
    const bool usingFixed512 = screenOverProcess == kScreenOverProcessFixed512;
    const bool usingRepeat = screenOverProcess == kScreenOverProcessRepeat;
    const uint2 scrollSize = usingFixed512
        ? uint2(512, 512)
        : uint2(512 * 4, 512 * 4) << pageShift;

    if (all(scrollPos < scrollSize) || usingRepeat) {
        StoreRotationLineColorData(pos, rotPos, index, rotSel);

        return FetchBitmapPixel(params.base, g_layerRenderParams[0].rbg[rotSel].base.bitmapBaseAddress, scrollPos);
    }

    return kTransparentPixel;
}

uint4 DrawRBG(uint2 pos, // pixel coordinates
              uint index // RBG index (0 to 1)
             ) {
    const RBGParams params = g_layerRenderParams[0].rbg[index];
    if (!params.base.enabled) {
        return kTransparentPixel;
    }

    // Brimir: matches software (sw:4713-4733): with mosaic, a scroll RBG repeats every mosaicH-th pixel in RBG
    // coordinates, whole (window and coefficient transparency included); in hi-res modes the repeated output pixels
    // copy the odd output pixel of that pixel. (Ymir quantizes only the coefficient position of line colours.) The
    // repeated pixel's line colour is that pixel's; software keeps the line colour it last stored at the repeated x.
    uint2 linePos = pos;
    if (params.base.mosaicEnable && !params.base.bitmap) {
        const uint mosaicH = BitExtract(g_commonParams.layerParams, 14, 4) + 1;
        const uint rbgX = hiResH ? pos.x >> 1 : pos.x;
        const uint baseX = rbgX - rbgX % mosaicH;
        if (baseX != rbgX) {
            pos.x = hiResH ? (baseX << 1) | 1 : baseX;
        }
    }

    if (InsideWindows(params.base.windowParams, uint2(pos.x, GetY(pos.y, false)))) {
        return kTransparentPixel;
    }

    if (hiResH) {
        pos.x >>= 1;
        linePos.x >>= 1;
    }
    if (deinterlace && interlaceMode >= kInterlaceModeSingleDensity) {
        pos.y >>= 1;
        linePos.y >>= 1;
    }

    const uint rotSel = index == 0 ? SelectRotationParameter(params, pos) : kRotParamB;
    const RotCoefficient rotCoeff = CalcRotationCoefficient(pos, rotSel);

    // Handle transparent pixels in coefficient table
    const uint coeffParamsOffset = 6 + rotSel * 5;
    const bool coeffTableEnable = BitTest(g_commonParams.rotParams, coeffParamsOffset + 0);
    if (coeffTableEnable && rotCoeff.transparent) {
        return kTransparentPixel;
    }

    const uint2 screenCoords = CalcRotationScreenCoords(pos, rotSel);
    return params.base.bitmap
        ? DrawBitmapRBG(pos, index, rotSel, screenCoords)
        : DrawScrollRBG(pos, linePos, index, rotSel, screenCoords);
}

// ---------------------------------------------------------------------------------------------------------------------
// Color calculation window

bool InsideColorCalcWindow(uint2 pos) {
    LayerWindowParamsS ccWindows;
    ccWindows.base.windowLogicAnd = BitTest(g_commonParams.windows, 5);
    ccWindows.base.window0Enable = BitTest(g_commonParams.windows, 6);
    ccWindows.base.window0Invert = BitTest(g_commonParams.windows, 7);
    ccWindows.base.window1Enable = BitTest(g_commonParams.windows, 8);
    ccWindows.base.window1Invert = BitTest(g_commonParams.windows, 9);
    ccWindows.spriteWindowEnable = BitTest(g_commonParams.windows, 10);
    ccWindows.spriteWindowInvert = BitTest(g_commonParams.windows, 11);
    pos.y = GetY(pos.y, false);
    return InsideWindows(ccWindows, pos);
}

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

// Brimir: 32x1x1 (at most 128 invocations per workgroup), dispatched with z = 7; id.z is the layer as before.
[numthreads(32, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint2 drawCoord = uint2(id.x, id.y + g_commonParams.startY);
    const uint3 outCoord = uint3(drawCoord.x, GetY(drawCoord.y, false), id.z);
    // Brimir: nothing outside the frame (HRes x VRes) is written (no robust access).
    if (any(outCoord.xy >= outputSize)) {
        return;
    }
    if (id.z <= 3) {
        g_layerOut[outCoord] = DrawNBG(drawCoord, id.z);
    } else if (id.z <= 5) {
        g_layerOut[outCoord] = DrawRBG(drawCoord, id.z - 4);
    } else if (id.z == 6) {
        g_colorCalcWindowOut[outCoord.xy] = InsideColorCalcWindow(drawCoord) ? 1u : 0u;
    }
}
