// VDP2 sprite layer: renders the VDP1 framebuffer (sprite and transparent-mesh layers) into the layer image and the
// sprite attribute image.
//
// Derived from Ymir libs/ymir-core/res/shaders/src/vdp/cs_vdp2_render_sprite.hlsl at 8cdf0a53. Only what Vulkan 1.2
// (Brimir's baseline) needs is changed, each change marked "// Brimir:":
// - common params are a push-constant block; register numbers follow src/gpu/vdp2/vdp2_bindings.hpp;
// - CRAM colours are a StructuredBuffer<uint> of packed RGBA8 (no texel buffer);
// - the layer image is declared rgba8ui and the sprite attributes r32ui (no extended storage formats);
// - bounds guards replace D3D12's robust out-of-range accesses (no robustness features): pixels outside
//   HRes x VRes and layers beyond the mesh layer write nothing, and framebuffer reads beyond the buffer return 0.
// Where Ymir's sprite layer differs from Brimir's software renderer (the reference), the software semantics are
// ported, each marked "// Brimir: matches software (sw:<lines>)": framebuffer offsets wrap within the framebuffer,
// and under deinterlacing rotated sprites use the line's coordinates and the alt fields read the framebuffers software
// reads.
//
// Bindings (set 0; tools/shaders/build_shaders.cmake shifts t by 0 and u by 16):
//   0  t0  g_layerRenderParams  StructuredBuffer<LayerRenderParams>  STORAGE_BUFFER_DYNAMIC
//   2  t2  g_vram               ByteAddressBuffer                    STORAGE_BUFFER
//   3  t3  g_cramColor          StructuredBuffer<uint>               STORAGE_BUFFER
//   5  t5  g_rotParamBases      StructuredBuffer<RotParamBase>       STORAGE_BUFFER
//   6  t6  g_spriteFB           ByteAddressBuffer                    STORAGE_BUFFER
//  16  u0  g_layerOut           RWTexture2DArray<uint4> rgba8ui      STORAGE_IMAGE
//  17  u1  g_spriteAttrsOut     RWTexture2DArray<uint> r32ui         STORAGE_IMAGE
// Push constants: CommonRenderParams (48 bytes). Dispatch: (HRes / 32, lines, 1 + transparent meshes).

#include "vdp2_common_params.hlsli"
#include "vdp2_render_params_layer.hlsli"
#include "vdp2_render_params_rotation.hlsli"

#include "vdp2_defs.hlsli"

#include "util/bit_ops.hlsli"
#include "util/data_ops.hlsli"

// Brimir: push constants instead of the b0 root constants.
[[vk::push_constant]] CommonRenderParams g_commonParams;

// Brimir: registers renumbered per vdp2_bindings.hpp.
StructuredBuffer<LayerRenderParams> g_layerRenderParams : register(t0);
ByteAddressBuffer g_vram : register(t2);
// Brimir: StructuredBuffer<uint> of packed RGBA8 instead of Buffer<uint4>.
StructuredBuffer<uint> g_cramColor : register(t3);
StructuredBuffer<RotParamBase> g_rotParamBases : register(t5);
ByteAddressBuffer g_spriteFB : register(t6);

// Brimir: explicit storage formats (DXC would infer Rgba32ui for uint4).
[[vk::image_format("rgba8ui")]] RWTexture2DArray<uint4> g_layerOut : register(u0);
[[vk::image_format("r32ui")]] RWTexture2DArray<uint> g_spriteAttrsOut : register(u1);

// ---------------------------------------------------------------------------------------------------------------------
// Parameters

static const uint interlaceMode = BitExtract(g_commonParams.displayParams, 2, 2);
static const uint oddField = BitExtract(g_commonParams.displayParams, 4, 1);
static const bool exclusiveMonitor = BitTest(g_commonParams.displayParams, 5);
static const bool hiResH = BitTest(g_commonParams.displayParams, 8);

static const bool rotate = BitTest(g_commonParams.spriteParams, 0);
static const bool pixel8Bits = BitTest(g_commonParams.spriteParams, 1);
static const uint type = BitExtract(g_commonParams.spriteParams, 2, 4);
static const uint2 fbSize = uint2(
    512u << BitExtract(g_commonParams.spriteParams, 6, 1),
    256u << BitExtract(g_commonParams.spriteParams, 7, 1)
);
static const bool inHalfResH = BitTest(g_commonParams.spriteParams, 8);
static const bool outHalfResH = BitTest(g_commonParams.spriteParams, 9);
static const bool mixedFormat = BitTest(g_commonParams.spriteParams, 10);
static const bool useSpriteWindow = BitTest(g_commonParams.spriteParams, 20);
static const uint spriteDisplayFB = BitExtract(g_commonParams.spriteParams, 23, 1);

static const bool deinterlace = BitTest(g_commonParams.enhancements, 0);

static const uint colorRAMMode = BitExtract(g_commonParams.displayParams, 6, 2);
static const uint kCRAMAddressMask = colorRAMMode == 1 ? 0x7FF : 0x3FF;

static const uint kSpriteFBBaseOffset = spriteDisplayFB * kVDP1FBRAMSize;
static const uint kDeinterlaceFBBaseOffset = kVDP1FBRAMSize * 2;
static const uint kVDP1MeshFBOffset = kVDP1FBRAMSize * 2 * 2;

// Brimir: the frame's output size (bounds of the outputs).
static const uint2 outputSize = UnpackOutputSize(g_commonParams.outputSize);

// ---------------------------------------------------------------------------------------------------------------------
// Utilities

uint GetY(uint y) {
    if (!deinterlace && interlaceMode >= kInterlaceModeSingleDensity && !exclusiveMonitor) {
        return (y << 1) | oddField;
    } else {
        return y;
    }
}

// Brimir: reads beyond the framebuffer buffer return 0, as D3D12 raw-buffer reads do (Ymir relies on it: rotated or
// half-resolution coordinates can address past the slices).
uint LoadSpriteFB(uint address) {
    return address < kVDP1FBRAMBufferSize ? g_spriteFB.Load(address) : 0;
}

// Brimir: matches software (sw:5522, 5528, 2766): framebuffer offsets wrap within the 256 KiB framebuffer
// (& 0x3FFFF, & 0x3FFFE) before the slice base is added, instead of reading into the next slice.
uint ReadFB8(uint address, uint sliceBase) {
    address = (address & 0x3FFFF) + sliceBase;
    return BitExtract(LoadSpriteFB(address & ~3), (address & 3) * 8, 8); // Brimir: LoadSpriteFB
}

uint ReadFB16(uint address, uint sliceBase) {
    address = (address & 0x3FFFE) + sliceBase;
    return ByteSwap16(LoadSpriteFB(address & ~3) >> ((address & 2) * 8)); // Brimir: LoadSpriteFB
}

uint ReadSprite8(uint address, uint field) {
    return ReadFB8(address, kSpriteFBBaseOffset + field * kDeinterlaceFBBaseOffset);
}

uint ReadSprite16(uint address, uint field) {
    return ReadFB16(address, kSpriteFBBaseOffset + field * kDeinterlaceFBBaseOffset);
}

uint ReadMesh8(uint address, uint field) {
    return ReadFB8(address, kVDP1MeshFBOffset + kSpriteFBBaseOffset + field * kDeinterlaceFBBaseOffset);
}

uint ReadMesh16(uint address, uint field) {
    return ReadFB16(address, kVDP1MeshFBOffset + kSpriteFBBaseOffset + field * kDeinterlaceFBBaseOffset);
}

uint ReadVRAM16(uint address) {
    return Read16(g_vram, address & 0x7FFFE);
}

uint ReadVRAM32(uint address) {
    return Read32(g_vram, address & 0x7FFFC);
}

// ---------------------------------------------------------------------------------------------------------------------
// Colors and CRAM

uint4 FetchCRAMColor(uint cramOffset, uint colorIndex) {
    const uint cramAddress = (cramOffset + colorIndex) & kCRAMAddressMask;
    return UnpackRGBA8(g_cramColor[cramAddress]); // Brimir: packed RGBA8
}

uint4 Color555(uint val16) {
    return uint4(
        ((val16 >> 0) & 0x1F) << 3,
        ((val16 >> 5) & 0x1F) << 3,
        ((val16 >> 10) & 0x1F) << 3,
        (val16 >> 15) & 1
    );
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

bool InsideWindows(uint2 pos) {
    const bool windowLogicAND = BitTest(g_commonParams.windows, 0);
    const bool window0Enable = BitTest(g_commonParams.windows, 1);
    const bool window0Invert = BitTest(g_commonParams.windows, 2);
    const bool window1Enable = BitTest(g_commonParams.windows, 3);
    const bool window1Invert = BitTest(g_commonParams.windows, 4);

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

// ---------------------------------------------------------------------------------------------------------------------
// Rotation parameter calculation

uint2 CalcRotationSpriteCoordinates(uint2 pos) {
    const RotParamBase base = g_rotParamBases[pos.y];

    const int Xst = SignExtend(ReadVRAM32(base.tableAddress + 0x00) >> 6, 23);
    const int Yst = SignExtend(ReadVRAM32(base.tableAddress + 0x04) >> 6, 23);

    const int deltaXst = SignExtend(ReadVRAM32(base.tableAddress + 0x0C) >> 6, 13);
    const int deltaYst = SignExtend(ReadVRAM32(base.tableAddress + 0x10) >> 6, 13);

    const int deltaX = SignExtend(ReadVRAM32(base.tableAddress + 0x14) >> 6, 13);
    const int deltaY = SignExtend(ReadVRAM32(base.tableAddress + 0x18) >> 6, 13);

    // Current sprite coordinates (13.10)
    // 10 + 0*10 + 0*10 = 10 + 10 + 10 = 10 frac bits
    // 23 + 10*13 + 9*13 = 23 + 23 + 22 = 23 total bits
    return uint2(
        (Xst + pos.y * deltaXst + pos.x * deltaX) >> 10,
        (Yst + pos.y * deltaYst + pos.x * deltaY) >> 10
    );
}

// ---------------------------------------------------------------------------------------------------------------------
// Sprite data

struct SpriteData {
    uint colorData; // DC10-0
    uint colorCalcRatio; // CC2-0
    uint priority; // PR2-0
    bool shadowOrWindow; // SD
    uint special; // Color data special patterns
};

uint GetSpecialPattern(uint rawData, uint colorDataBits) {
    // Normal shadow pattern (LSB = 0, rest of the color data bits = 1)
    const uint kNormalShadowValue = (1u << colorDataBits) - 2u;

    if ((rawData & 0x7FFF) == 0) {
        return kSpriteDataTransparent;
    } else if (BitExtract(rawData, 0, colorDataBits) == kNormalShadowValue) {
        return kSpriteDataShadow;
    } else {
        return kSpriteDataNormal;
    }
}

SpriteData FetchSpriteData(uint fbAddr, uint field, bool meshLayer) {
    // Adjust offset based on VDP1 data size.
    // The majority of games actually set the sprite readout size to match the VDP1 sprite data size, but there's
    // *always* an exception...
    // 8-bit VDP1 data vs. 16-bit readout: NBA Live 98
    // 16-bit VDP1 data vs. 8-bit readout: I Love Donald Duck
    uint rawData;
    if (pixel8Bits) {
        rawData = meshLayer ? ReadMesh8(fbAddr, field) : ReadSprite8(fbAddr, field);
        if (type < 8 && (!meshLayer || rawData != 0)) {
            rawData |= 0xFF00;
        }
    } else {
        fbAddr <<= 1;
        rawData = meshLayer ? ReadMesh16(fbAddr, field) : ReadSprite16(fbAddr, field);
    }

    // Sprite types 0-7 are 16-bit, 8-15 are 8-bit
    //  type |15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0|
    //    0  |PR1-0  |CC2-0      |DC10-0                                    |
    //    1  |PR2-0      |CC1-0  |DC10-0                                    |
    //    2  |SD |PR0|CC2-0      |DC10-0                                    |
    //    3  |SD |PR1-0  |CC1-0  |DC10-0                                    |
    //    4  |SD |PR1-0  |CC2-0      |DC9-0                                 |
    //    5  |SD |PR2-0      |CC0|DC10-0                                    |
    //    6  |SD |PR2-0      |CC1-0  |DC9-0                                 |
    //    7  |SD |PR2-0      |CC2-0      |DC8-0                             |
    //  type                                 | 7   6   5   4   3   2   1   0|
    //    8                                  |PR0|DC6-0                     |
    //    9                                  |PR0|CC0|DC5-0                 |
    //    A                                  |PR1-0  |DC5-0                 |
    //    B                                  |CC1-0  |DC5-0                 |
    //    C                                  |SP0|DC6-0                     |
    //    D                                  |SP0|SC0|DC5-0                 |
    //    E                                  |SP1-0  |DC5-0                 |
    //    F                                  |SC1-0  |DC5-0                 |
    //
    // SD: shadow/sprite window
    // PR: priority
    // CC: color calculation ratio
    // DC: dot color data
    // SP: shared PR+DC
    // SC: shared CC+DC

    SpriteData data;
    switch (type) {
        case 0x0:
            data.colorData = BitExtract(rawData, 0, 11);
            data.colorCalcRatio = BitExtract(rawData, 11, 3);
            data.priority = BitExtract(rawData, 14, 2);
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 11);
            break;

        case 0x1:
            data.colorData = BitExtract(rawData, 0, 11);
            data.colorCalcRatio = BitExtract(rawData, 11, 2);
            data.priority = BitExtract(rawData, 13, 3);
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 11);
            break;

        case 0x2:
            data.colorData = BitExtract(rawData, 0, 11);
            data.colorCalcRatio = BitExtract(rawData, 11, 3);
            data.priority = BitExtract(rawData, 14, 1);
            data.shadowOrWindow = BitTest(rawData, 15);
            data.special = GetSpecialPattern(rawData, 11);
            break;

        case 0x3:
            data.colorData = BitExtract(rawData, 0, 11);
            data.colorCalcRatio = BitExtract(rawData, 11, 2);
            data.priority = BitExtract(rawData, 13, 2);
            data.shadowOrWindow = BitTest(rawData, 15);
            data.special = GetSpecialPattern(rawData, 11);
            break;

        case 0x4:
            data.colorData = BitExtract(rawData, 0, 10);
            data.colorCalcRatio = BitExtract(rawData, 10, 3);
            data.priority = BitExtract(rawData, 13, 2);
            data.shadowOrWindow = BitTest(rawData, 15);
            data.special = GetSpecialPattern(rawData, 10);
            break;

        case 0x5:
            data.colorData = BitExtract(rawData, 0, 11);
            data.colorCalcRatio = BitExtract(rawData, 11, 1);
            data.priority = BitExtract(rawData, 12, 3);
            data.shadowOrWindow = BitTest(rawData, 15);
            data.special = GetSpecialPattern(rawData, 11);
            break;

        case 0x6:
            data.colorData = BitExtract(rawData, 0, 10);
            data.colorCalcRatio = BitExtract(rawData, 10, 2);
            data.priority = BitExtract(rawData, 12, 3);
            data.shadowOrWindow = BitTest(rawData, 15);
            data.special = GetSpecialPattern(rawData, 10);
            break;

        case 0x7:
            data.colorData = BitExtract(rawData, 0, 9);
            data.colorCalcRatio = BitExtract(rawData, 9, 3);
            data.priority = BitExtract(rawData, 12, 3);
            data.shadowOrWindow = BitTest(rawData, 15);
            data.special = GetSpecialPattern(rawData, 9);
            break;

        case 0x8:
            data.colorData = BitExtract(rawData, 0, 7);
            data.colorCalcRatio = 0;
            data.priority = BitExtract(rawData, 7, 1);
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 7);
            break;

        case 0x9:
            data.colorData = BitExtract(rawData, 0, 6);
            data.colorCalcRatio = BitExtract(rawData, 6, 1);
            data.priority = BitExtract(rawData, 7, 1);
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 6);
            break;

        case 0xA:
            data.colorData = BitExtract(rawData, 0, 6);
            data.colorCalcRatio = 0;
            data.priority = BitExtract(rawData, 6, 2);
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 6);
            break;

        case 0xB:
            data.colorData = BitExtract(rawData, 0, 6);
            data.colorCalcRatio = BitExtract(rawData, 6, 2);
            data.priority = 0;
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 6);
            break;

        case 0xC:
            data.colorData = BitExtract(rawData, 0, 8);
            data.colorCalcRatio = 0;
            data.priority = BitExtract(rawData, 7, 1);
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 8);
            break;

        case 0xD:
            data.colorData = BitExtract(rawData, 0, 8);
            data.colorCalcRatio = BitExtract(rawData, 6, 1);
            data.priority = BitExtract(rawData, 7, 1);
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 8);
            break;

        case 0xE:
            data.colorData = BitExtract(rawData, 0, 8);
            data.colorCalcRatio = 0;
            data.priority = BitExtract(rawData, 6, 2);
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 8);
            break;

        case 0xF:
            data.colorData = BitExtract(rawData, 0, 8);
            data.colorCalcRatio = BitExtract(rawData, 6, 2);
            data.priority = 0;
            data.shadowOrWindow = false;
            data.special = GetSpecialPattern(rawData, 8);
            break;
    }
    return data;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sprite layer rendering

struct SpriteOutput {
    uint4 layer;
    uint colorCalcRatio;
    bool colorMSB;
    bool shadowOrWindow;
    uint special;
};

// index 0 = sprite
// index 1 = transparent meshes
SpriteOutput DrawSprite(uint2 pos, uint2 outPos, uint index) {
    const bool meshLayer = index == 1;

    SpriteOutput output = { kTransparentPixel, 0, false, false, false };

    // Brimir: matches software (sw:2558-2563, 2685-2690): when deinterlacing, pos.y is (line << 1) | alt field (the
    // front half doubles rows outside exclusive monitor modes only). Both fields of a line use the line's rotation
    // coordinates (sw:2335, 2697); the alt field reads the alt sprite and mesh framebuffers in double density only.
    // (Ymir indexes the rotation bases by the doubled row and uses field 0 for rotated sprites.)
    uint fieldLine = pos.y;
    uint altField = 0;
    if (deinterlace && interlaceMode >= kInterlaceModeSingleDensity && !exclusiveMonitor) {
        altField = pos.y & 1;
        fieldLine >>= 1;
    }
    const uint field = interlaceMode == kInterlaceModeDoubleDensity ? altField : 0;

    uint2 spritePos;
    if (rotate) {
        spritePos = CalcRotationSpriteCoordinates(uint2(pos.x, fieldLine));
        if (any(spritePos >= fbSize)) {
            return output;
        }
    } else {
        spritePos = uint2(pos.x, fieldLine);
        if (inHalfResH) {
            spritePos.x <<= 1;
        } else if (outHalfResH) {
            spritePos.x >>= 1;
        }
    }
    const uint fbAddr = spritePos.x + spritePos.y * fbSize.x;

    if (InsideWindows(outPos)) {
        return output;
    }

    if (mixedFormat) {
        const uint spriteDataValue = meshLayer ? ReadMesh16(fbAddr << 1, field) : ReadSprite16(fbAddr << 1, field);
        if (BitTest(spriteDataValue, 15)) {
            // RGB data

            // Transparent if:
            // - Using byte-sized sprite types (0x8 to 0xF) and the lower 8 bits are all zero
            // - Using word-sized sprite types that have the shadow/sprite window bit (types 0x2 to 0x7), sprite
            //   window is enabled, and the lower 15 bits are all zero
            if (type >= 8) {
                if (BitExtract(spriteDataValue, 0, 8) == 0) {
                    return output;
                }
            } else if (type >= 2) {
                if (useSpriteWindow && BitExtract(spriteDataValue, 0, 15) == 0) {
                    return output;
                }
            }

            const uint4 outColor = Color555(spriteDataValue);
            const uint outPriority = BitExtract(g_commonParams.spritePriosRatios.x, 0, 3);

            output.colorCalcRatio = BitExtract(g_commonParams.spritePriosRatios, 0 * 8 + 3, 5);
            output.colorMSB = true;
            output.layer = uint4(outColor.rgb, outPriority);
            return output;
        }
    }

    // Palette data
    const SpriteData spriteData = FetchSpriteData(fbAddr, field, meshLayer);

    // Handle sprite window
    const bool spriteWindowEnabled = BitTest(g_commonParams.spriteParams, 21);
    const bool spriteWindowInverted = BitTest(g_commonParams.spriteParams, 22);
    if (useSpriteWindow && spriteWindowEnabled && spriteData.shadowOrWindow != spriteWindowInverted) {
        output.layer = kTransparentPixel;
        output.shadowOrWindow = true;
        return output;
    }

    const uint colorDataOffset = BitExtract(g_commonParams.spriteParams, 17, 3) << 8;
    const uint colorIndex = colorDataOffset + spriteData.colorData;
    const uint4 outColor = FetchCRAMColor(0, colorIndex);
    const bool outTransparent = spriteData.special == kSpriteDataTransparent;
    const uint outPriority = outTransparent && !spriteData.shadowOrWindow
        ? 0
        : BitExtract(g_commonParams.spritePriosRatios, spriteData.priority * 8, 3);

    output.layer = uint4(outColor.rgb,
        outPriority |
        (1u << kPixelAttrBitSpecColorCalc)
    );
    output.colorCalcRatio = BitExtract(g_commonParams.spritePriosRatios, spriteData.colorCalcRatio * 8 + 3, 5);
    output.colorMSB = outColor.a != 0;
    output.shadowOrWindow = spriteData.shadowOrWindow;
    output.special = spriteData.special;
    return output;
}

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

// TODO: 32 threads might be suboptimal on AMD GPUs
[numthreads(32, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint2 drawCoord = uint2(id.x, id.y + g_commonParams.startY);
    const uint2 outCoord = uint2(drawCoord.x, GetY(drawCoord.y));
    // Brimir: nothing outside the frame (HRes x VRes) or beyond the mesh layer is written (no robust access).
    if (any(outCoord >= outputSize) || id.z > 1) {
        return;
    }
    const SpriteOutput output = DrawSprite(drawCoord, outCoord.xy, id.z);
    g_layerOut[uint3(outCoord.xy, id.z + 6)] = output.layer;
    g_spriteAttrsOut[uint3(outCoord.xy, id.z)] =
        output.colorCalcRatio |
        ((output.colorMSB ? 1u : 0u) << kSpriteAttrBitColorMSB) |
        ((output.shadowOrWindow ? 1u : 0u) << kSpriteAttrBitShadowWindow) |
        (output.special << kSpriteAttrBitSpecial);
}
