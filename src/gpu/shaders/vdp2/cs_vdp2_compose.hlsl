// VDP2 compositor: combines the layer images, line/back screens and sprite attributes into the output frame.
//
// Derived from Ymir libs/ymir-core/res/shaders/src/vdp/cs_vdp2_compose.hlsl at 8cdf0a53. Only what Vulkan 1.2
// (Brimir's baseline) needs is changed, each change marked "// Brimir:":
// - common params are a push-constant block; register numbers follow src/gpu/vdp2/vdp2_bindings.hpp;
// - LNCL/BACK are a StructuredBuffer<uint> of packed RGBA8 (no texel buffer) with BACK at kMaxResV + y
//   (Vdp2FrameRecord::lnclBack);
// - the output is packed XBGR (r | g << 8 | b << 16 | 0xFF000000, the software framebuffer's order) in a
//   RWStructuredBuffer<uint> at y * HRes + x instead of a float4 UNORM image;
// - bounds guards replace D3D12's robust out-of-range accesses (no robustness features): pixels outside HRes x VRes
//   write nothing and RBG line colour reads outside their image return 0.
// Where Ymir's compositor differs from Brimir's software renderer (the reference), the software semantics are ported,
// each marked "// Brimir: matches software (sw:<lines>)" (vdp_renderer_sw.cpp; stage1b-port-inventory.md section 4.1):
// per-layer shadow enable, the extended colour calculation layer-1 enable, the back screen ratio, LNCL/BACK per field
// line with the display-off border colour latch, and the transparent mesh layer.
//
// Bindings (set 0; tools/shaders/build_shaders.cmake shifts t by 0 and u by 16):
//   1  t1   g_composeParams      StructuredBuffer<ComposeParams>  STORAGE_BUFFER_DYNAMIC
//   8  t8   g_lnclBackIn         StructuredBuffer<uint>           STORAGE_BUFFER
//   9  t9   g_spriteAttrsIn      Texture2DArray<uint>             SAMPLED_IMAGE
//  10  t10  g_layerIn            Texture2DArray<uint4>            SAMPLED_IMAGE
//  11  t11  g_rbgLineColorIn     Texture2DArray<uint4>            SAMPLED_IMAGE
//  12  t12  g_colorCalcWindowIn  Texture2D<uint>                  SAMPLED_IMAGE
//  20  u4   g_compositeOut       RWStructuredBuffer<uint>         STORAGE_BUFFER
// Push constants: CommonRenderParams (48 bytes). Dispatch: (HRes / 32, lines, 1).

#include "vdp2_common_params.hlsli"
#include "vdp2_compose_params.hlsli"

#include "vdp2_defs.hlsli"

#include "util/bit_ops.hlsli"
#include "util/data_ops.hlsli"

// Brimir: push constants instead of the b0 root constants.
[[vk::push_constant]] CommonRenderParams g_commonParams;

// Brimir: registers renumbered per vdp2_bindings.hpp.
StructuredBuffer<ComposeParams> g_composeParams : register(t1);
Texture2DArray<uint4> g_layerIn : register(t10);
// Brimir: StructuredBuffer<uint> of packed RGBA8 instead of Buffer<uint4>.
StructuredBuffer<uint> g_lnclBackIn : register(t8);
Texture2DArray<uint4> g_rbgLineColorIn : register(t11);
Texture2DArray<uint> g_spriteAttrsIn : register(t9);
Texture2D<uint> g_colorCalcWindowIn : register(t12);

// Brimir: packed XBGR words, pitch HRes, instead of RWTexture2D<float4>.
RWStructuredBuffer<uint> g_compositeOut : register(u4);

// ---------------------------------------------------------------------------------------------------------------------
// Definitions

static const uint kBGLayerNBG0 = 0;
static const uint kBGLayerNBG1 = 1;
static const uint kBGLayerNBG2 = 2;
static const uint kBGLayerNBG3 = 3;
static const uint kBGLayerRBG0 = 4;
static const uint kBGLayerRBG1 = 5;
static const uint kBGLayerSprite = 6;
static const uint kBGLayerMesh = 7;
static const uint kBGLayerInvalid = 8;

static const uint kLayerSprite = 0;
static const uint kLayerRBG0 = 1;
static const uint kLayerNBG0_RBG1 = 2;
static const uint kLayerNBG1_EXBG = 3;
static const uint kLayerNBG2 = 4;
static const uint kLayerNBG3 = 5;
static const uint kLayerBack = 6;
static const uint kLayerLine = 7; // not used in the stack, but referenced by parameters
static const uint kLayerMesh = 8; // not used in the stack, but referenced by parameters

static const uint kSpriteCCCondPriorityLE = 0;
static const uint kSpriteCCCondPriorityEQ = 1;
static const uint kSpriteCCCondPriorityGE = 2;
static const uint kSpriteCCCondColorMSB = 3;

// ---------------------------------------------------------------------------------------------------------------------
// Parameters

static const uint interlaceMode = BitExtract(g_commonParams.displayParams, 2, 2);
static const uint oddField = BitExtract(g_commonParams.displayParams, 4, 1);
static const bool exclusiveMonitor = BitTest(g_commonParams.displayParams, 5);
static const uint cramMode = BitExtract(g_commonParams.displayParams, 6, 2);
static const bool hiResH = BitExtract(g_commonParams.displayParams, 10, 3) & 2;
static const bool normalTVMode = BitExtract(g_commonParams.displayParams, 10, 3) < 2;
static const bool firstInterlacedFrame = BitTest(g_commonParams.displayParams, 17);

static const bool colorGradEnable = BitTest(g_commonParams.layerParams, 28);

static const bool deinterlace = BitTest(g_commonParams.enhancements, 0);
static const bool transparentMeshes = BitTest(g_commonParams.enhancements, 1);

// Brimir: the frame's output size (bounds and pitch of the composite).
static const uint2 outputSize = UnpackOutputSize(g_commonParams.outputSize);

// ---------------------------------------------------------------------------------------------------------------------
// Utilities

bool IsInterlaced() {
    return interlaceMode >= kInterlaceModeSingleDensity && !exclusiveMonitor;
}

uint GetLoResInputX(uint x) {
    if (hiResH) {
        return x >> 1u;
    } else {
        return x;
    }
}

// Brimir: matches software (sw:2528, 2645-2664, 3909): LNCL/BACK and the RBG line colours are per VDP line, which both
// fields of a line share. y is the output row: (line << 1) | field in interlaced modes, with or without deinterlacing.
// (Ymir's GetLoResInputY kept the output row in single-density interlace without deinterlacing.)
uint GetLineIndex(uint y) {
    return IsInterlaced() ? y >> 1u : y;
}

uint GetOutputY(uint y) {
    if (!deinterlace && IsInterlaced()) {
        return (y << 1u) | oddField;
    } else {
        return y;
    }
}

bool IsBGLayerEnabled(uint bgLayer) {
    return BitTest(g_commonParams.layerParams, bgLayer + 6);
}

bool IsLayerEnabled(uint layer) {
    return BitTest(g_commonParams.layerParams, layer);
}

uint GetBGLayerIndex(uint layer) {
    switch (layer) {
        case kLayerSprite:
            return kBGLayerSprite;
        case kLayerMesh:
            return kBGLayerMesh;
        case kLayerRBG0:
            return kBGLayerRBG0;
        case kLayerNBG0_RBG1:
            return IsBGLayerEnabled(kBGLayerRBG1) ? kBGLayerRBG1 : kBGLayerNBG0;
        case kLayerNBG1_EXBG:
            return kBGLayerNBG1;
        case kLayerNBG2:
            return kBGLayerNBG2;
        case kLayerNBG3:
            return kBGLayerNBG3;
        default:
            return kBGLayerInvalid; // go out of bounds intentionally to read blanks
    }
}

uint GetColorGradScreenLayerIndex(uint colorGradScreen) {
    switch (colorGradScreen) {
        case kColorGradScreenNBG0_RBG0:
            return kLayerNBG0_RBG1;
        case kColorGradScreenNBG1_EXBG:
            return kLayerNBG1_EXBG;
        case kColorGradScreenNBG2:
            return kLayerNBG2;
        case kColorGradScreenNBG3:
            return kLayerNBG3;
        case kColorGradScreenRBG0:
            return kLayerRBG0;
        case kColorGradScreenSprite:
            return kLayerSprite;
        default:
            return kLayerBack;
    }
}

bool IsColorCalcEnabled(uint layer, uint2 pos) {
    const bool enabled = BitTest(g_composeParams[0].colorCalcEnable, layer);
    if (layer >= kLayerBack) {
        // Back and line screen layers use the enable bit alone
        return enabled;
    }
    if (!enabled) {
        // Color calculation is disabled for this layer
        return false;
    }
    if (g_colorCalcWindowIn[pos] != 0) {
        return false;
    }
    const bool restrictedColorCalc = BitTest(g_commonParams.layerParams, 24);
    if (layer == kLayerSprite) {
        // Sprites use condition modes based on priority or color MSB
        const uint layerAttrs = g_layerIn[uint3(pos, kLayerIndexSprite)].a;
        if (restrictedColorCalc && BitTest(layerAttrs, kPixelAttrBitSpecColorCalc)) {
            return false;
        }
        const uint attrs = g_spriteAttrsIn[uint3(pos, 0)];
        const uint priority = BitExtract(layerAttrs, 0, 3);
        const uint value = BitExtract(g_commonParams.spriteParams, 12, 3);
        const uint cond = BitExtract(g_commonParams.spriteParams, 15, 2);
        switch (cond) {
            case kSpriteCCCondPriorityLE:
                return priority <= value;
            case kSpriteCCCondPriorityEQ:
                return priority == value;
            case kSpriteCCCondPriorityGE:
                return priority >= value;
            case kSpriteCCCondColorMSB:
                return BitTest(attrs, kSpriteAttrBitColorMSB);
        }
        return false;
    }
    // BG layers use the per-pixel special color calculation flag
    const uint bgLayer = GetBGLayerIndex(layer);
    const uint attrs = g_layerIn[uint3(pos.xy, bgLayer)].a;
    if (restrictedColorCalc && BitTest(attrs, kPixelAttrBitPaletteFormat)) {
        return false;
    }
    return BitTest(attrs, kPixelAttrBitSpecColorCalc);
}

// Brimir: matches software (sw:4039-4072, 4227): the extended colour calculation enable of layer 1 checks the layer's
// enable bit and restricted colour calculation (and the sprite condition), not the colour calculation window or the
// per-pixel special colour calculation bit (Ymir uses IsColorCalcEnabled).
bool IsLayer1ColorCalcEnabled(uint layer, uint2 pos) {
    const bool enabled = BitTest(g_composeParams[0].colorCalcEnable, layer);
    if (!enabled || layer >= kLayerBack) {
        return enabled;
    }
    const bool restrictedColorCalc = BitTest(g_commonParams.layerParams, 24);
    if (layer == kLayerSprite) {
        const uint layerAttrs = g_layerIn[uint3(pos, kLayerIndexSprite)].a;
        if (restrictedColorCalc && BitTest(layerAttrs, kPixelAttrBitSpecColorCalc)) {
            return false;
        }
        const uint attrs = g_spriteAttrsIn[uint3(pos, 0)];
        const uint priority = BitExtract(layerAttrs, 0, 3);
        const uint value = BitExtract(g_commonParams.spriteParams, 12, 3);
        const uint cond = BitExtract(g_commonParams.spriteParams, 15, 2);
        switch (cond) {
            case kSpriteCCCondPriorityLE:
                return priority <= value;
            case kSpriteCCCondPriorityEQ:
                return priority == value;
            case kSpriteCCCondPriorityGE:
                return priority >= value;
            case kSpriteCCCondColorMSB:
                return BitTest(attrs, kSpriteAttrBitColorMSB);
        }
        return false;
    }
    const uint attrs = g_layerIn[uint3(pos.xy, GetBGLayerIndex(layer))].a;
    return !(restrictedColorCalc && BitTest(attrs, kPixelAttrBitPaletteFormat));
}

// Brimir: matches software (sw:4325-4344): the mesh layer's colour calculation uses the sprite enable and condition
// with the mesh pixel's priority and colour MSB, without the colour calculation window or restricted colour
// calculation (Ymir tests the kLayerMesh enable bit, which is never set).
bool IsMeshColorCalcEnabled(uint meshPriority, uint2 pos) {
    if (!BitTest(g_commonParams.spriteParams, 11)) {
        return false;
    }
    const uint value = BitExtract(g_commonParams.spriteParams, 12, 3);
    const uint cond = BitExtract(g_commonParams.spriteParams, 15, 2);
    switch (cond) {
        case kSpriteCCCondPriorityLE:
            return meshPriority <= value;
        case kSpriteCCCondPriorityEQ:
            return meshPriority == value;
        case kSpriteCCCondPriorityGE:
            return meshPriority >= value;
        case kSpriteCCCondColorMSB:
            return BitTest(g_spriteAttrsIn[uint3(pos, 1)], kSpriteAttrBitColorMSB);
    }
    return false;
}

bool IsLineColorEnabled(uint layer, uint2 pos) {
    return BitTest(g_composeParams[0].lineColorEnable, layer);
}

// pos is the output position.
uint3 GetLineColor(uint layer, uint2 pos) {
    if (layer == kLayerRBG0 || (layer == kLayerNBG0_RBG1 && IsBGLayerEnabled(kBGLayerRBG1))) {
        const uint y = GetLineIndex(pos.y);
        // Brimir: reads outside the 352 x 512 image return 0, as D3D12 loads do (no robust access).
        const uint x = GetLoResInputX(pos.x);
        if (x >= kMaxNormalResH || y >= kMaxResV) {
            return uint3(0, 0, 0);
        }
        return g_rbgLineColorIn[uint3(x, y, layer - kLayerRBG0)].rgb;
    }
    return UnpackRGBA8(g_lnclBackIn[GetLineIndex(pos.y)]).rgb; // Brimir: packed RGBA8
}

int GetColorCalcRatio(uint layer, uint2 pos) {
    switch (layer) {
        case kLayerSprite:
            return BitExtract(g_spriteAttrsIn[uint3(pos, 0)], kSpriteAttrBitColorCalcRatio, 5);
        case kLayerRBG0:
        case kLayerNBG0_RBG1:
        case kLayerNBG1_EXBG:
        case kLayerNBG2:
        case kLayerNBG3:
            return g_composeParams[0].bgColorCalcRatios[layer - kLayerRBG0];
        case kLayerBack:
            // Brimir: matches software (sw:4286): the back screen uses its own ratio (Ymir picks the line screen ratio
            // when the back screen's colour calculation enable is set).
            return g_composeParams[0].backLineColorCalcRatios[0];
        default:
            return 31;
    }
}

bool IsColorOffsetEnabled(uint layer) {
    return BitTest(g_composeParams[0].colorOffsetEnable, layer);
}

int3 GetColorOffset(uint layer) {
    const bool selB = BitTest(g_composeParams[0].colorOffsetSelect, layer);
    return selB ? g_composeParams[0].colorOffsetB : g_composeParams[0].colorOffsetA;
}

uint4 GetLayerOutput(uint layer, uint2 pos) {
    switch (layer) {
        case kLayerSprite:
        case kLayerMesh:
        case kLayerRBG0:
        case kLayerNBG0_RBG1:
        case kLayerNBG1_EXBG:
        case kLayerNBG2:
        case kLayerNBG3:
            return g_layerIn[uint3(pos.xy, GetBGLayerIndex(layer))];
        case kLayerBack:
            // Brimir: packed RGBA8; BACK at kBackScreenOffset (512) + line
            return UnpackRGBA8(g_lnclBackIn[GetLineIndex(pos.y) + kBackScreenOffset]);
        case kLayerLine:
            return UnpackRGBA8(g_lnclBackIn[GetLineIndex(pos.y)]); // Brimir: packed RGBA8
        default:
            return kTransparentPixel; // should never happpen
    }
}

struct Attributes {
    uint priority;
    bool specColorCalc;
};

Attributes ToAttributes(uint pixelData) {
    Attributes attrs;
    attrs.priority = BitExtract(pixelData, 0, 3);
    attrs.specColorCalc = BitTest(pixelData, kPixelAttrBitSpecColorCalc);
    return attrs;
}

// ---------------------------------------------------------------------------------------------------------------------
// Compositor

uint3 Compose(uint2 basePos) {
    const uint2 pos = uint2(basePos.x, GetOutputY(basePos.y));

    // Clear screen if display is disabled
    const bool displayEnabled = BitTest(g_commonParams.displayParams, 0);
    if (!displayEnabled) {
        // Brimir: matches software (sw:3911-3917): the latched border colour mode, and the back colour as last computed
        // (the front half repeats it on lines prepared with the display latch off).
        if (g_commonParams.borderColorModeLatch != 0) {
            // Use back screen color
            // Brimir: packed RGBA8; BACK at kBackScreenOffset (512) + line
            return UnpackRGBA8(g_lnclBackIn[GetLineIndex(pos.y) + kBackScreenOffset]).rgb;
        }
        return uint3(0, 0, 0);
    }

    // Determine layer order
    uint layerStack[3] = { kLayerBack, kLayerBack, kLayerBack };
    uint layerPrios[3] = { 0, 0, 0 };

    for (uint layer = 0; layer < 6; layer++) {
        // Skip disabled layers
        if (!IsLayerEnabled(layer)) {
            continue;
        }

        const uint4 layerOutput = GetLayerOutput(layer, pos);
        const Attributes attrs = ToAttributes(layerOutput.a);

        // Priority zero means transparent pixel
        if (attrs.priority == 0) {
            continue;
        }

        // Skip normal shadow sprite layer pixels
        if (layer == kLayerSprite) {
            const uint spriteAttrs = g_spriteAttrsIn[uint3(pos, 0)];
            if (BitExtract(spriteAttrs, kSpriteAttrBitSpecial, 2) != kSpriteDataNormal) {
                continue;
            }
        }

        // Insert the layer into the appropriate position in the stack
        // - Higher priority beats lower priority
        // - If same priority, lower Layer index beats higher Layer index
        // - layerStack[0] is topmost (first) layer
        for (int i = 0; i < 3; i++) {
            if (attrs.priority > layerPrios[i] || (attrs.priority == layerPrios[i] && layer < layerStack[i])) {
                // Push layers back
                for (int j = 2; j > i; j--) {
                    layerStack[j] = layerStack[j - 1];
                    layerPrios[j] = layerPrios[j - 1];
                }
                layerStack[i] = layer;
                layerPrios[i] = attrs.priority;
                break;
            }
        }
    }

    // Find sprite mesh layer stack position
    uint meshLayer = 0xFF;
    uint3 meshPixel;
    uint meshPriority = 0;
    if (transparentMeshes && IsLayerEnabled(kLayerSprite)) {
        const uint4 meshOutput = GetLayerOutput(kLayerMesh, pos);
        meshPixel = meshOutput.rgb;
        const Attributes meshAttrs = ToAttributes(meshOutput.a);
        meshPriority = meshAttrs.priority;
        const uint meshSpriteAttrs = g_spriteAttrsIn[uint3(pos, 1)];
        // Brimir: matches software (sw:4009): only normal mesh pixels are inserted (Ymir skips only shadow pixels).
        if (meshAttrs.priority > 0 && BitExtract(meshSpriteAttrs, kSpriteAttrBitSpecial, 2) == kSpriteDataNormal) {
            for (uint i = 0; i < 3; i++) {
                // The sprite layer has the highest priority on ties, so the priority check can be simplified.
                // Sprite pixels drawn of top of mesh pixels erase the corresponding pixels from the mesh layer,
                // therefore the mesh layer can be considered always on top of the sprite layer.
                if (meshAttrs.priority >= layerPrios[i]) {
                    meshLayer = i;
                    break;
                }
            }
        }
    }

    uint3 output = { 0, 0, 0 };

    const bool useColorGrad = normalTVMode && cramMode == 0 && colorGradEnable;
    const bool layer0LineColorEnabled = !useColorGrad && IsLineColorEnabled(layerStack[0], pos);
    const bool extendedColorCalc = BitTest(g_commonParams.layerParams, 25);

    uint3 layer0Pixel = GetLayerOutput(layerStack[0], pos).rgb;

    if (IsColorCalcEnabled(layerStack[0], pos)) {
        uint3 layer1Pixel = GetLayerOutput(layerStack[1], pos).rgb;

        // Compute layer 1 output
        if (useColorGrad) {
            // Compute color gradation
            const uint colorGradScreen = BitExtract(g_commonParams.layerParams, 29, 3);
            const uint colorGradLayer = GetColorGradScreenLayerIndex(colorGradScreen);

            // Set layer 1 output to the color gradation screen where the designated screen is the topmost two layers
            if (layerStack[0] == colorGradLayer || layerStack[1] == colorGradLayer) {
                const uint3 input2 = GetLayerOutput(colorGradLayer, uint2(max(int(pos.x) - 2, 0), pos.y)).rgb;
                const uint3 input1 = GetLayerOutput(colorGradLayer, uint2(max(int(pos.x) - 1, 0), pos.y)).rgb;
                const uint3 input0 = GetLayerOutput(colorGradLayer, pos).rgb;
                layerStack[1] = colorGradLayer;
                layer1Pixel = (((input2 + input1) >> 1u) + input0) >> 1u;
            }
        } else if (normalTVMode && extendedColorCalc) {
            // Apply color gradation or extended color calculations to layer 1
            // Brimir: matches software (sw:4227): IsLayer1ColorCalcEnabled
            if (IsLayer1ColorCalcEnabled(layerStack[1], pos)) {
                uint3 layer2Pixel = GetLayerOutput(layerStack[2], pos).rgb;

                // Blend layer 2 with sprite mesh layer colors
                // TODO: apply color calculation effects
                if (transparentMeshes && meshLayer == 2) {
                    layer2Pixel = (layer2Pixel + meshPixel) >> 1;
                }

                layer1Pixel = (layer1Pixel + layer2Pixel) >> 1;
            }

            if (layer0LineColorEnabled) {
                const uint3 lineColor = GetLineColor(layerStack[0], pos); // Brimir: the output position
                if (IsColorCalcEnabled(kLayerLine, pos)) {
                    // Blend line color if top layer uses it
                    layer1Pixel = (layer1Pixel + lineColor) >> 1;
                } else {
                    // Replace with line color if top layer uses it
                    layer1Pixel = lineColor;
                }
            }
        } else if (layer0LineColorEnabled) {
            // Replace layer 1 pixels with line color screen
            layer1Pixel = GetLineColor(layerStack[0], pos); // Brimir: the output position
        }

        // Blend layer 1 with sprite mesh layer colors
        // TODO: apply color calculation effects
        if (transparentMeshes && meshLayer == 1) {
            layer1Pixel = (layer1Pixel + meshPixel) >> 1;
        }

        // Blend layer 0 and layer 1
        const bool useAdditiveBlend = BitTest(g_commonParams.layerParams, 26);
        if (useAdditiveBlend) {
            output = min(layer0Pixel + layer1Pixel, 255);
        } else {
            const bool useSecondScreenRatio = BitTest(g_commonParams.layerParams, 27);
            int ratio;
            if (useSecondScreenRatio && layer0LineColorEnabled) {
                ratio = g_composeParams[0].backLineColorCalcRatios[1];
            } else {
                const uint ratioLayer = useSecondScreenRatio ? layerStack[1] : layerStack[0];
                ratio = GetColorCalcRatio(ratioLayer, pos);
            }
            output = int3(layer1Pixel) + (((int3(layer0Pixel) - int3(layer1Pixel)) * ratio) >> 5);
        }
    } else {
        output = layer0Pixel;
    }

    // Apply sprite shadow if sprite layer has a shadow pixel and is on top of the topmost layer
    const uint4 spriteOutput = GetLayerOutput(kLayerSprite, pos);
    const uint spritePriority = BitExtract(spriteOutput.a, 0, 3);
    if (spritePriority >= layerPrios[0]) {
        const uint spriteAttrs = g_spriteAttrsIn[uint3(pos, 0)];
        const bool useSpriteWindow = BitTest(g_commonParams.spriteParams, 20);
        const bool isNormalShadow = BitExtract(spriteAttrs, kSpriteAttrBitSpecial, 2) == kSpriteDataShadow;
        const bool isMSBShadow = !useSpriteWindow && BitTest(spriteAttrs, kSpriteAttrBitShadowWindow);
        if (isNormalShadow || isMSBShadow) {
            // Brimir: matches software (sw:4113-4117): a sprite on top uses its own shadow bit, other layers and the
            // back screen their SDCTL shadow enable (Ymir shadows any top layer).
            const bool shadowEnabled = layerStack[0] == kLayerSprite
                                           ? BitTest(spriteAttrs, kSpriteAttrBitShadowWindow)
                                           : BitTest(g_composeParams[0].shadowEnable, layerStack[0]);
            if (shadowEnabled) {
                output >>= 1;
            }
        }
    }

    // Apply color offset if enabled
    if (IsColorOffsetEnabled(layerStack[0])) {
        const int3 offset = GetColorOffset(layerStack[0]);
        output = clamp(int3(output) + offset, 0, 255);
    }

    // Blend layer 0 with sprite mesh layer colors
    if (transparentMeshes && meshLayer == 0) {
        // Brimir: matches software (sw:4322-4359): the mesh pixel's colour calculation (IsMeshColorCalcEnabled) with
        // the mesh pixel's own ratio.
        if (IsMeshColorCalcEnabled(meshPriority, pos)) {
            const bool useAdditiveBlend = BitTest(g_commonParams.layerParams, 26);
            if (useAdditiveBlend) {
                meshPixel = min(meshPixel + output, 255);
            } else {
                const int ratio = BitExtract(g_spriteAttrsIn[uint3(pos, 1)], kSpriteAttrBitColorCalcRatio, 5);
                meshPixel = int3(output) + (((int3(meshPixel) - int3(output)) * ratio) >> 5);
            }
        }

        // Apply color offset if enabled
        // Brimir: matches software (sw:4362-4375): the sprite layer's colour offset enable and select.
        if (IsColorOffsetEnabled(kLayerSprite)) {
            const int3 offset = GetColorOffset(kLayerSprite);
            meshPixel = clamp(int3(meshPixel) + offset, 0, 255);
        }

        // Blend with output
        output = (output + meshPixel) >> 1;
    }

    return output;
}

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint

// TODO: 32 threads might be suboptimal on AMD GPUs
[numthreads(32, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint2 drawCoord = uint2(id.x, id.y + g_commonParams.startY);
    const uint2 outCoord = uint2(drawCoord.x, GetOutputY(drawCoord.y));
    // Brimir: nothing outside the frame (HRes x VRes) is written (no robust access; the pitch is HRes).
    if (any(outCoord >= outputSize)) {
        return;
    }
    const uint3 outColor = Compose(drawCoord);
    // Brimir: packed XBGR (the UNORM store clamped to 1.0; min() keeps that) instead of float4(outColor / 255.0, 1).
    const uint3 c = min(outColor, 255u);
    const uint packed = c.r | (c.g << 8) | (c.b << 16) | 0xFF000000u;
    g_compositeOut[outCoord.y * outputSize.x + outCoord.x] = packed;
    if (firstInterlacedFrame && !deinterlace && IsInterlaced()) {
        // Brimir: the other field's row only when it is inside the frame.
        const uint otherY = outCoord.y ^ 1u;
        if (otherY < outputSize.y) {
            g_compositeOut[otherY * outputSize.x + outCoord.x] = packed;
        }
    }
}
