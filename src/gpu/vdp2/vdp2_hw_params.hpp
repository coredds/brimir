#pragma once

// GPU-visible VDP2 rendering parameters, shared by the Vulkan-free front half (it packs them) and the VDP2 shaders
// (src/gpu/shaders/vdp2/include/*.hlsli declare the same layouts).
//
// Derived from Ymir libs/ymir-core/src/ymir/hw/vdp/renderer/common_hw/common_hw_defs.hpp at 8cdf0a53: the basic HLSL
// types and EnhancementsParams (lines 24-95) and the VDP2 part (lines 242-800). Changes from Ymir, each marked
// "// Brimir:" where it applies:
// - namespace brimir::gpu::vdp2 and <cstdint> types (uint32 -> uint32_t, ...); Windows UINT -> uint32_t;
// - no cmrc (Ymir's embedded shader filesystem, kCSEntrypoint and g_fsShaders are not copied);
// - VDP2ComposeParams re-padded to the std430 layout the Vulkan shaders see (Ymir's host struct is packed for D3D12);
// - VDP2CommonRenderParams fills the std430 hole before spritePriosRatios and its tail (push-constant block);
// - Brimir fields: VDP2CommonRenderParams::borderColorModeLatch and outputSize, VDP2ComposeParams::shadowEnable,
//   VDP2NBGLine, kMaxResH/kMaxResV.
// Every offset and size is checked against the shaders' reflected layouts in tests/unit/test_gpu_vdp2_params.cpp.
// No Vulkan header may be included here.

#include <ymir/hw/vdp/vdp2_defs.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace brimir::gpu::vdp2 {

// Brimir: the largest VDP2 output (hi-res, double-density interlace); sizes the per-line arrays and images.
inline constexpr uint32_t kMaxResH = 704;
inline constexpr uint32_t kMaxResV = 512;
static_assert(kMaxResH == ymir::vdp::kMaxResH && kMaxResV == ymir::vdp::kMaxResV);

// ---------------------------------------------------------------------------------------------------------------------
// Basic types

using HLSLbool = uint32_t; // bools align to 4 bytes
using HLSLint = int32_t;
using HLSLuint = uint32_t;

struct alignas(HLSLuint) ColorR8G8B8A8 {
    uint8_t r, g, b, a;
};
static_assert(sizeof(ColorR8G8B8A8) == sizeof(uint32_t));

// Brimir: MSVC /W4 warns about the anonymous structs (C4201, a common extension Ymir relies on).
#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 4201)
#endif

union HLSLuint2 {
    std::array<HLSLuint, 2> array;
    struct {
        HLSLuint x, y;
    };
    struct {
        HLSLuint r, g;
    };
};
static_assert(sizeof(HLSLuint2) == sizeof(HLSLuint) * 2);

union HLSLuint3 {
    std::array<HLSLuint, 3> array;
    struct {
        HLSLuint x, y, z;
    };
    struct {
        HLSLuint r, g, b;
    };
};
static_assert(sizeof(HLSLuint3) == sizeof(HLSLuint) * 3);

union HLSLuint4 {
    std::array<HLSLuint, 4> array;
    struct {
        HLSLuint x, y, z, w;
    };
    struct {
        HLSLuint r, g, b, a;
    };
};
static_assert(sizeof(HLSLuint4) == sizeof(HLSLuint) * 4);

union HLSLint2 {
    std::array<HLSLint, 2> array;
    struct {
        HLSLint x, y;
    };
    struct {
        HLSLint r, g;
    };
};
static_assert(sizeof(HLSLint2) == sizeof(HLSLint) * 2);

union HLSLint3 {
    std::array<HLSLint, 3> array;
    struct {
        HLSLint x, y, z;
    };
    struct {
        HLSLint r, g, b;
    };
};
static_assert(sizeof(HLSLint3) == sizeof(HLSLint) * 3);

#ifdef _MSC_VER
    #pragma warning(pop)
#endif

// =====================================================================================================================
// Common rendering parameters

struct EnhancementsParams {
    HLSLuint deinterlace : 1;       //     0  Deinterlace
    HLSLuint transparentMeshes : 1; //     1  Render mesh sprites as transparent
};
static_assert(sizeof(EnhancementsParams) == sizeof(HLSLuint));

/// @brief Base Xst, Yst, KA for params A and B for a given line.
struct alignas(16) VDP2RotParamBase {
    uint32_t tableAddress;
    int32_t Xst, Yst;
    uint32_t KA;
};
static_assert(sizeof(VDP2RotParamBase) == sizeof(uint32_t) * 4);

// =====================================================================================================================
// VDP2 rendering
//
// The VDP2 rendering pipeline is invoked at least once per frame. When VRAM, CRAM and/or register writes happen, the
// renderer processes scanlines up to the previous VCNT and commits all changes before proceeding.
//
// Since the VDP2 rendering process has no visible effect on the rest of the Saturn's components, there is no need for
// additional synchronization constraints on memory and register reads or writes. The VDP2 state is handled by the VDP
// controller and the renderer maintains a local independent copy of the VRAM, CRAM and VDP2 registers for fully
// asynchronous rendering.
//
// Root 32-bit constants hold renderer parameters shared across all VDP2 compute shaders such as the starting line for
// continuation of work interrupted by state changes, relevant registers and active enhancements.
// Brimir: the root constants are a Vulkan push-constant block (std430).

/// @brief Common VDP2 rendering parameters shared by all shaders.
struct VDP2CommonRenderParams {
    // Top Y coordinate of target rendering area.
    HLSLuint startY;

    struct DisplayParams {                 //  bits  use
        HLSLuint displayEnable : 1;        //     0  Display enabled
                                           //          0 = display off
                                           //          1 = display on
        HLSLuint borderColorMode : 1;      //     1  Border color mode
                                           //          0 = black
                                           //          1 = back screen color
        HLSLuint interlaceMode : 2;        //   2-3  Interlace mode
                                           //          0 = progressive
                                           //          1 = invalid
                                           //          2 = single-density interlace
                                           //          3 = double-density interlace
        HLSLuint oddField : 1;             //     4  Field
                                           //          0 = even
                                           //          1 = odd
        HLSLuint exclusiveMonitor : 1;     //     5  Exclusive monitor mode
                                           //          0 = normal
                                           //          1 = exclusive
        HLSLuint colorRAMMode : 2;         //   6-7  Color RAM mode
                                           //          0 = RGB 5:5:5, 1024 words
                                           //          1 = RGB 5:5:5, 2048 words
                                           //          2 = RGB 8:8:8, 1024 words
                                           //          3 = RGB 8:8:8, 1024 words  (same as mode 2, undocumented)
        HLSLuint hiResH : 1;               //     8  Horizontal resolution
                                           //          0 = 320/352
                                           //          1 = 640/704
        HLSLuint palMode : 1;              //     9  Display standard (VDP2 TVSTAT.PAL)
                                           //          0 = NTSC
                                           //          1 = PAL
        HLSLuint hresMode : 3;             // 10-12  Horizontal resolution mode (VDP2 TVMD.HRESO2-0)
        HLSLuint vresMode : 2;             // 13-14  Vertical resolution mode   (VDP2 TVMD.VRESO1-0)
        HLSLuint dblInterlaceEnable : 1;   //    15  VDP1 double interlace enable flag (VDP1 FBCR.DIE)
        HLSLuint dblInterlaceDrawLine : 1; //    16  VDP1 double interlace draw line   (VDP1 FBCR.DIL)
        HLSLuint firstInterlacedFrame : 1; //    17  VDP2 resolution changed to interlaced this frame?
    } displayParams;
    static_assert(sizeof(DisplayParams) == sizeof(HLSLuint));

    struct LayerParams {                   //  bits  use
        HLSLuint layerEnabled : 6;         //   0-5  Layer enable state based on BGON and other factors:
                                           //        bit  RBG0+RBG1   RBG0        RBG1        no RBGs
                                           //          0  Sprite      Sprite      Sprite      Sprite
                                           //          1  RBG0        RBG0        -           -
                                           //          2  RBG1        NBG0        RBG1        NBG0
                                           //          3  EXBG        NBG1/EXBG   NBG1/EXBG   NBG1/EXBG
                                           //          4  -           NBG2        NBG2        NBG2
                                           //          5  -           NBG3        NBG3        NBG3
        HLSLuint bgEnabled : 6;            //  6-11  Individual layer enable flags
                                           //        bit  layer
                                           //          8  NBG0
                                           //          9  NBG1
                                           //         10  NBG2
                                           //         11  NBG3
                                           //         12  RBG0
                                           //         13  RBG1
        HLSLuint lineColorEnableRBG0 : 1;  //    12  Line color screen enable for RBG0
        HLSLuint lineColorEnableRBG1 : 1;  //    13  Line color screen enable for RBG1
        HLSLuint mosaicH : 4;              // 14-17  Horizontal mosaic size (minus one)
        HLSLuint mosaicV : 4;              // 18-21  Vertical mosaic size (minus one)
        HLSLuint rotParamMode : 2;         // 22-23  Rotation parameter mode
                                           //          0 = always use A
                                           //          1 = always use B
                                           //          2 = select based on coefficient data
                                           //          3 = select based on window flag
        HLSLuint restrictedColorCalc : 1;  //    24  Restricted color calculations
        HLSLuint extendedColorCalc : 1;    //    25  Use extended color calculation
                                           //        (always disabled in hi-res modes)
        HLSLuint useAdditiveBlend : 1;     //    26  Blend mode
                                           //          0 = alpha
                                           //          1 = additive
        HLSLuint useSecondScreenRatio : 1; //    27  Use second screen ratio
        HLSLuint colorGradEnable : 1;      //    28  Color gradation enabled
        HLSLuint colorGradScreen : 3;      // 29-31  Color gradation screen
                                           //          0  Sprite
                                           //          1  RBG0
                                           //          2  NBG0/RBG1
                                           //          3  (invalid)
                                           //          4  NBG1/EXBG
                                           //          5  NBG2
                                           //          6  NBG3
                                           //          7  (invalid)
    } layerParams;
    static_assert(sizeof(LayerParams) == sizeof(HLSLuint));

    struct RotParams {                       //  bits  use
        HLSLuint coeffTableCRAM : 1;         //     0  Coefficient table location
                                             //          0 = VRAM
                                             //          1 = CRAM
        HLSLuint coeffDataAccess : 4;        //   1-4  Coefficient data access for VRAM banks:
                                             //         bit  bank
                                             //           0  A0/A
                                             //           1  A1
                                             //           2  B0/B
                                             //           3  B1
        HLSLuint coeffDataPerDot : 1;        //     5  Per-dot coefficients
                                             //          false = per line
                                             //          true  = per dot
                                             // ------------------------------------------------
        HLSLuint coeffATableEnable : 1;      //     6  Coefficient table A enabled
        HLSLuint coeffADataSize : 1;         //     7  Coefficient A data size
                                             //          0 = 2 words
                                             //          1 = 1 word
        HLSLuint coeffADataMode : 2;         //   8-9  Coefficient A data mode
                                             //          0 = kx/ky
                                             //          1 = kx
                                             //          2 = ky
                                             //          3 = Px
        HLSLuint coeffAUseLineColorData : 1; //    10  Use coefficient A line color data
                                             // ------------------------------------------------
        HLSLuint coeffBTableEnable : 1;      //    11  Coefficient table B enabled
        HLSLuint coeffBDataSize : 1;         //    12  Coefficient B data size
                                             //          0 = 2 words
                                             //          1 = 1 word
        HLSLuint coeffBDataMode : 2;         // 13-14  Coefficient B data mode
                                             //          0 = kx/ky
                                             //          1 = kx
                                             //          2 = ky
                                             //          3 = Px
        HLSLuint coeffBUseLineColorData : 1; //    15  Use coefficient B line color data
    } rotParams;
    static_assert(sizeof(RotParams) == sizeof(HLSLuint));

    struct SpriteParams {             //  bits  use
        HLSLuint rotate : 1;          //     0  Sprite layer rotation
                                      //          0 = normal
                                      //          1 = use rotation parameter A
        HLSLuint pixel8Bits : 1;      //     1  VDP1 data size
                                      //          0 = 16-bit
                                      //          1 = 8-bit
        HLSLuint type : 4;            //   2-5  Sprite data type
        HLSLuint fbSizeH : 1;         //     6  VDP1 framebuffer horizontal size shift  (512 << x)
        HLSLuint fbSizeV : 1;         //     7  VDP1 framebuffer vertical size shift    (256 << x)
        HLSLuint inHalfResH : 1;      //     8  Sprite input at half resolution
        HLSLuint outHalfResH : 1;     //     9  Sprite output at half resolution
        HLSLuint mixedFormat : 1;     //    10  Sprite layer color format
                                      //          0 = palette only
                                      //          1 = mixed palette/RGB
        HLSLuint colorCalcEnable : 1; //    11  Sprite color calculation enable
        HLSLuint colorCalcValue : 3;  // 12-14  Sprite target color calculation value
        HLSLuint colorCalcCond : 2;   // 15-16  Special color calculation condition
        HLSLuint colorDataOffset : 3; // 17-19  Special color data offset in CRAM
        HLSLuint useSpriteWindow : 1; //    20  Use sprite window
        HLSLuint windowEnabled : 1;   //    21  Sprite window enabled for the sprite layer
        HLSLuint windowInverted : 1;  //    22  Sprite window inverted for the sprite layer
        HLSLuint displayFB : 1;       //    23  Current sprite display framebuffer index
    } spriteParams;
    static_assert(sizeof(SpriteParams) == sizeof(HLSLuint));

    // Brimir: VDP2Regs::borderColorModeLatch (TVMD.BDCLMD as latched with TVMD.DISP), which the software renderer
    // uses for display-off lines (0 = black, 1 = back screen colour). Fills the std430 hole at offset 20 before the
    // 8-byte aligned uint2.
    HLSLuint borderColorModeLatch;

    // Packed 8x 3-bit sprite priorities + 5-bit color calculation ratios
    HLSLuint2 spritePriosRatios;

    struct VCellScroll {            //  bits  use
        HLSLuint tableAddress : 19; //  0-18  Vertical cell scroll table address
        HLSLuint inc : 3;           // 19-21  Vertical cell scroll address increment per cell  (x << 2)
    } vcellScroll;
    static_assert(sizeof(VCellScroll) == sizeof(HLSLuint));

    struct Windows {                       //  bits  use
        HLSLuint spriteWindowLogic : 1;    //     0  Sprite window logic        0=OR; 1=AND
        HLSLuint spriteW0Enable : 1;       //     1  Sprite W0 enable           0=disable; 1=enable
        HLSLuint spriteW0Invert : 1;       //     2  Sprite W0 invert           0=disable; 1=enable
        HLSLuint spriteW1Enable : 1;       //     3  Sprite W1 enable           0=disable; 1=enable
        HLSLuint spriteW1Invert : 1;       //     4  Sprite W1 invert           0=disable; 1=enable
        HLSLuint colorCalcWindowLogic : 1; //     5  Color calc. window logic   0=OR; 1=AND
        HLSLuint colorCalcW0Enable : 1;    //     6  Color calc. W0 enable      0=disable; 1=enable
        HLSLuint colorCalcW0Invert : 1;    //     7  Color calc. W0 invert      0=disable; 1=enable
        HLSLuint colorCalcW1Enable : 1;    //     8  Color calc. W1 enable      0=disable; 1=enable
        HLSLuint colorCalcW1Invert : 1;    //     9  Color calc. W1 invert      0=disable; 1=enable
        HLSLuint colorCalcSWEnable : 1;    //    10  Color calc. SW enable      0=disable; 1=enable
        HLSLuint colorCalcSWInvert : 1;    //    11  Color calc. SW invert      0=disable; 1=enable
    } windows;
    static_assert(sizeof(Windows) == sizeof(HLSLuint));

    EnhancementsParams enhancements;

    // Brimir: the frame's output size as VDP2SetResolution set it (Vdp2FrameRecord::hres/vres): HRes in bits 0-15,
    // VRes in bits 16-31, always the native size. The shaders use it (times renderScale) as the composite pitch and as
    // the bounds they check before touching their outputs (no robustness features). The back half sets it per dispatch
    // with MakeOutputSize, like startY; the front half leaves it 0.
    HLSLuint outputSize;

    // Brimir: the internal resolution N (1-4) of the background and compose dispatches: they run over N x N output
    // pixels per native pixel, write the background layer image and the composite at N x the native size (pitch
    // N * HRes), and read the native images (sprite layers and attributes, colour calculation window, RBG line colours)
    // at the native pixel. outputSize stays native. The sprite and NBG fetch passes are native and ignore it. The back
    // half sets it per dispatch like startY; the front half leaves it 0, which the shaders read as 1.
    HLSLuint renderScale;
    HLSLuint pad; // std430 tail (the block size is a multiple of the uint2 alignment)
};
static_assert(sizeof(VDP2CommonRenderParams) == 56);
static_assert(sizeof(VDP2CommonRenderParams) <= 128, "must fit the minimum maxPushConstantsSize");

/// @brief Brimir: packs VDP2CommonRenderParams::outputSize. The size must fit the output images and the composite
/// buffer (kMaxResH x kMaxResV); the shaders clamp to that too.
inline HLSLuint MakeOutputSize(uint32_t hres, uint32_t vres) {
    assert(hres <= kMaxResH && vres <= kMaxResV);
    return hres | (vres << 16u);
}

/// @brief Global VDP2 window parameters.
struct VDP2GlobalWindowParams {
    // Top-left coordinates
    HLSLuint2 start;

    // Bottom-right coordinates
    HLSLuint2 end;

    // Base address of the line window table
    HLSLuint lineWindowTableAddress;

    // Whether to use the line window table
    HLSLbool lineWindowTableEnable;
};

/// @brief Per-layer VDP2 window parameters.
struct VDP2LayerWindowParams {
    // Window logic
    //   false = OR
    //   true = AND
    HLSLbool windowLogicAnd;

    // Window 0 enable
    HLSLbool window0Enable;

    // Window 0 invert
    HLSLbool window0Invert;

    // Window 1 enable
    HLSLbool window1Enable;

    // Window 1 invert
    HLSLbool window1Invert;
};

/// @brief VDP2 extended window parameters (includes sprite window).
struct VDP2LayerWindowParamsS {
    VDP2LayerWindowParams base;

    // Sprite window enable
    HLSLbool spriteWindowEnable;

    // Sprite window invert
    HLSLbool spriteWindowInvert;
};

/// @brief Base VDP2 layer rendering parameters, common to NBGs and RBGs.
struct VDP2BaseBGParams {
    // Background enabled
    HLSLbool enabled;

    // If true, honor transparency bit in color data.
    // Derived from BGON.xxTPON
    HLSLbool enableTransparency;

    // Whether the background uses cells (false) or a bitmap (true).
    // Derived from CHCTLA/CHCTLB.xxBMEN
    HLSLbool bitmap;

    // Priority number from 0 (transparent) to 7 (highest).
    // Derived from PRINA/PRINB/PRIR.xxPRINn
    HLSLuint priorityNumber;

    // Special priority mode.
    // Derived from SFPRMD.xxSPRMn
    HLSLuint priorityMode;

    // Special function select (0=A, 1=B).
    // Derived from SFSEL.xxSFCS
    HLSLuint specialFunctionSelect;

    // Cell size shift corresponding to the dimensions of a character pattern (0=1x1, 1=2x2).
    // Derived from CHCTLA/CHCTLB.xxCHSZ
    HLSLuint cellSizeShift;

    // Character color format.
    // Derived from CHCTLA/CHCTLB.xxCHCNn
    HLSLuint colorFormat;

    // Color RAM base offset.
    // Derived from CRAOFA/CRAOFB.xxCAOSn
    HLSLuint cramOffset;

    // Supplementary bits 4-0 for scroll screen character number, when using 1-word characters.
    // Derived from PNCNn/PNCR.xxSCNn
    HLSLuint supplScrollCharNum;

    // Supplementary bits 6-4 for palette number.
    // Used with scroll screen when using 1-word characters, or with bitmap screens.
    // The value is already shifted in place to optimize rendering calculations.
    // Derived from PNCNn/PNCR.xxSPLTn (scroll) or BMPNA/BMPNB.xxBMPn (bitmap)
    HLSLuint supplPalNum;

    // Supplementary special color calculation bit.
    // Derived from PNCNn/PNCR.xxSCC (scroll) or BMPNA/BMPNB.xxBMCC (bitmap)
    HLSLuint supplSpecialColorCalc;

    // Supplementary special priority bit.
    // Derived from PNCNn/PNCR.xxSPR (scroll) or BMPNA/BMPNB.xxBMPR (bitmap)
    HLSLuint supplSpecialPriority;

    // Enables the mosaic effect.
    // If vertical cell scroll is also enabled, the mosaic effect is bypassed.
    // Derived from MZCTL.xxMZE
    HLSLbool mosaicEnable;

    // Enables color calculation.
    // Derived from CCCTL.xxCCEN
    HLSLbool colorCalcEnable;

    // Character number width: 10 bits (false) or 12 bits (true).
    // When true, disables the horizontal and vertical flip bits in the character.
    // Derived from PNCNn/PNCR.xxCNSM
    HLSLbool extChar;

    // Whether characters use one (false) or two (true) words.
    // Derived from PNCNn/PNCR.xxPNB
    HLSLbool twoWordChar;

    // Whether pattern name data is accessible for each VRAM bank (bits 0 to 3: A0, A1, B0, B1).
    // Derived from CYCxn, RAMCTL and BGON (for RBG0/1 restrictions to NBGs)
    HLSLuint patNameAccess;

    // Whether character pattern data is accessible for each VRAM bank (bits 0 to 3: A0, A1, B0, B1).
    // Derived from CYCxn, RAMCTL and BGON (for RBG0/1 restrictions to NBGs)
    HLSLuint charPatAccess;

    // Whether accesses to character pattern data for this background is delayed per bank due to illegal VRAM access
    // patterns (bits 0 to 3: A0, A1, B0, B1). Derived from CYCxn, RAMCTL, ZMCTL and CHCTLA/CHCTLB.xxCHSZ
    HLSLuint charPatDelay;

    // Address offset for VRAM data for this background on each VRAM bank caused by illegal VRAM access patterns
    // (bits 0 to 3: A0, A1, B0, B1). Derived from CYCxn, RAMCTL and ZMCTL
    HLSLuint vramDataOffset;

    // Special color calculation mode.
    // Derived from SFCCMD.xxSCCMn
    HLSLuint specialColorCalcMode;

    // Page shifts are either 0 or 1, used when determining which plane a particular (x,y) coordinate belongs to.
    // A shift of 0 corresponds to 1 page per plane dimension.
    // A shift of 1 corresponds to 2 pages per plane dimension.
    // Derived from PLSZ.xxPLSZn
    HLSLuint2 pageShift;

    // Bitmap dimensions, when the screen is in bitmap mode.
    // Derived from CHCTLA/CHCTLB.xxBMSZ
    HLSLuint2 bitmapSize;

    // Base address of bitmap data.
    // Derived from MPOFN (NBG0-3) or MPOFR (RotParam A-B)
    HLSLuint bitmapBaseAddress;

    // Window parameters
    VDP2LayerWindowParamsS windowParams;
};

/// @brief VDP2 NBG layer rendering parameters.
struct NBGParams {
    VDP2BaseBGParams base;

    // Screen scroll amount, in 11.8 fixed-point format.
    // Used in scroll NBGs.
    // Scroll amounts for NBGs 2 and 3 do not have a fractional part, but the values are still stored with 8 fractional
    // bits here for consistency and ease of implementation.
    // Derived from SCXINn and SCXDNn
    HLSLuint2 scrollAmount;

    // Screen scroll increment per pixel, in 11.8 fixed-point format.
    // NBGs 2 and 3 do not have increment registers; they always increment each coordinate by 1.0, which is stored here
    // for consistency and ease of implementation.
    // Derived from ZMXINn and ZMXDNn
    HLSLuint2 scrollInc;

    // Page base addresses for NBG planes A-D.
    // Derived from MPOFN, MPABNn, MPCDNn, CHCTLA/CHCTLB.xxCHSZ, PNCNn.xxPNB and PLSZ.xxPLSZn
    std::array<HLSLuint, 4> pageBaseAddresses;

    // Whether to use the vertical cell scroll table in VRAM.
    // Only valid for NBG0 and NBG1.
    // Derived from SCRCTL.NnVCSC
    HLSLbool vcellScrollEnable;

    // Whether to use the horizontal line scroll table in VRAM.
    // Only valid for NBG0 and NBG1.
    // Derived from SCRCTL.NnLSCX
    HLSLbool lineScrollXEnable;

    // Whether to use the vertical line scroll table in VRAM.
    // Only valid for NBG0 and NBG1.
    // Derived from SCRCTL.NnLSCY
    HLSLbool lineScrollYEnable;

    // Whether to use horizontal line zoom/scaling.
    // Only valid for NBG0 and NBG1.
    // Derived from SCRCTL.NnLZMX
    HLSLbool lineZoomEnable;

    // Line scroll table interval shift. The interval is calculated as (1 << lineScrollInterval).
    // Only valid for NBG0 and NBG1.
    // Derived from SCRCTL.NnLSS1-0
    HLSLuint lineScrollInterval;

    // Line scroll table base address.
    // Only valid for NBG0 and NBG1.
    // Derived from LSTAnU/L
    HLSLuint lineScrollTableAddress;

    // Vertical cell scroll offset.
    // Only valid for NBG0 and NBG1.
    // Based on CYCA0/A1/B0/B1 parameters.
    HLSLuint vcellScrollOffset;

    // Is the vertical cell scroll read delayed by one cycle?
    // Only valid for NBG0 and NBG1.
    // Based on CYCA0/A1/B0/B1 parameters.
    HLSLuint vcellScrollDelay;

    // Is the first vertical cell scroll entry repeated?
    // Only valid for NBG0.
    // Based on CYCA0/A1/B0/B1 parameters.
    HLSLuint vcellScrollRepeat;

    // Base Y coordinate for vertical scroll increment calculations.
    // Only valid for NBG2 and NBG3.
    HLSLuint scrollYIncBase;
};

/// @brief VDP2 RBG layer rendering parameters.
struct RBGParams {
    VDP2BaseBGParams base;

    // Rotation BG screen-over process.
    // Derived from PLSZ.RxOVRn
    HLSLuint screenOverProcess;

    // Screen-over pattern name value.
    // Derived from OVPNRA/B
    HLSLuint screenOverPatternName;

    /// @brief Page base addresses for RBG planes A-P using Rotation Parameters A and B.
    /// Indexing: [RotParam A/B][Plane A-P]
    /// Derived from `mapIndices`, `CHCTLA/CHCTLB.xxCHSZ`, `PNCR.xxPNB` and `PLSZ.xxPLSZn`.
    std::array<std::array<HLSLuint, 16>, 2> pageBaseAddresses;
};

/// @brief LNCL/BACK screen parameters.
struct VDP2LineBackScreenParams {
    // Base address of color data
    HLSLuint baseAddress;

    // Use colors per line
    //   false = per screen
    //   true  = per line
    HLSLbool perLine;
};

/// @brief VDP2 layer rendering parameters.
struct VDP2LayerRenderParams {
    NBGParams nbg[4];
    RBGParams rbg[2];

    VDP2GlobalWindowParams windows[2];

    VDP2LayerWindowParams rotWindows;

    VDP2LineBackScreenParams lineScreenParams;
    VDP2LineBackScreenParams backScreenParams;

    // Bit-packed special function code flags.
    //  bits  use
    //   0-7  Special function code A
    //  8-15  Special function code B
    HLSLuint specialFunctionCodes;
};
static_assert(sizeof(VDP2LayerRenderParams) == 1464);

/// @brief VDP2 compositor parameters.
// Brimir: std430 layout (int3 aligns to 16): shadowEnable fills the hole after colorOffsetA, so colorOffsetB is at 32,
// bgColorCalcRatios at 44, backLineColorCalcRatios at 64, and the explicit tail padding makes the size 80 (the
// StructuredBuffer stride). Ymir's packed host struct (68 bytes) does not match a Vulkan StructuredBuffer.
struct VDP2ComposeParams {
    // Use color calculation per layer (0=disable; 1=enable)
    //   bit  layer
    //     0  Sprite
    //     1  RBG0
    //     2  RBG1/NBG0
    //     3  NBG1/EXBG
    //     4  NBG2
    //     5  NBG3
    //     6  Back screen
    //     7  Line screen
    HLSLuint colorCalcEnable;

    // Color offset enable per layer (0=disable; 1=enable)
    //   bit  layer
    //     0  Sprite
    //     1  RBG0
    //     2  RBG1/NBG0
    //     3  NBG1/EXBG
    //     4  NBG2
    //     5  NBG3
    //     6  Back screen
    HLSLuint colorOffsetEnable;

    // Color offset select per layer (0=A; 1=B)
    //   bit  layer
    //     0  Sprite
    //     1  RBG0
    //     2  RBG1/NBG0
    //     3  NBG1/EXBG
    //     4  NBG2
    //     5  NBG3
    //     6  Back screen
    HLSLuint colorOffsetSelect;

    // Line color enable per layer (0=disable; 1=enable)
    //   bit  layer
    //     0  Sprite
    //     1  RBG0
    //     2  RBG1/NBG0
    //     3  NBG1/EXBG
    //     4  NBG2
    //     5  NBG3
    //     6  Back screen (always false to simplify shader implementation)
    HLSLuint lineColorEnable;

    // Color offset A (RGB999)
    HLSLint3 colorOffsetA;

    // Brimir: shadow enable per layer (0=disable; 1=enable), from SDCTL
    //   bit  layer
    //     0  Sprite
    //     1  RBG0
    //     2  RBG1/NBG0
    //     3  NBG1/EXBG
    //     4  NBG2
    //     5  NBG3
    //     6  Back screen
    HLSLuint shadowEnable;

    // Color offset B (RGB999)
    HLSLint3 colorOffsetB;

    // NBG/RBG color calculation ratios
    // index  layer
    //     0  RBG0
    //     1  NBG0/RBG1
    //     2  NBG1/EXBG
    //     3  NBG2
    //     4  NBG3
    std::array<HLSLuint, 5> bgColorCalcRatios;

    // Back/line screen color calculation ratios
    // index  layer
    //     0  Back screen
    //     1  Line screen
    std::array<HLSLuint, 2> backLineColorCalcRatios;

    // Brimir: explicit tail padding to the std430 stride.
    std::array<HLSLuint, 2> pad;
};
static_assert(offsetof(VDP2ComposeParams, colorOffsetB) == 32);
static_assert(offsetof(VDP2ComposeParams, bgColorCalcRatios) == 44);
static_assert(offsetof(VDP2ComposeParams, backLineColorCalcRatios) == 64);
static_assert(sizeof(VDP2ComposeParams) == 80);

// Brimir: per-output-row NBG scroll state, one per NBG per row, computed on the CPU exactly as the software renderer's
// per-line NBG state machine does (stage1b-port-inventory.md section 4.3).
// - fracScrollX, fracScrollY, scrollIncH: NBGLayerState as the software renderer draws the row with (fracScrollY
//   includes the alternate-line increment of double density deinterlacing, sw:4524, 4628);
// - mosaicSrcLine: the dispatch row whose layer output the row repeats (vertical mosaic), or the row itself;
// - prevLastVCellScroll: the field's VRAMFetcher::lastVCellScroll before the row is drawn;
// - nextChar: the field's VRAMFetcher::nextChar before the row is drawn (ymir::vdp::Character::u32, decoded on the CPU
//   from the character the previous drawn scroll line fetched last, sw:5193-5205), the delayed character at the left
//   edge.
struct VDP2NBGLine {
    int32_t fracScrollX, fracScrollY, scrollIncH;
    uint32_t mosaicSrcLine, prevLastVCellScroll, nextChar, pad0, pad1;
};
static_assert(sizeof(VDP2NBGLine) == 32);

/// @brief Number of entries in the VDP2 CRAM color cache.
/// VDP2 CRAM can have at most 2048 colors (in mode 1 - RGB 5:5:5 with access to full CRAM).
inline constexpr size_t kVDP2CRAMColorCacheEntries = ymir::vdp::kVDP2CRAMSize / sizeof(uint16_t);

/// @brief VDP2 CRAM converted color cache array.
using CRAMColorCache = std::array<ColorR8G8B8A8, kVDP2CRAMColorCacheEntries>;

/// @brief Size of the VDP2 CRAM color buffer, in bytes.
// Brimir: two tables of kVDP2CRAMColorCacheEntries: the mode-dependent colour cache, then (from entry
// kVDP2CRAM555TableBase) every CRAM word as RGB555, the software renderer's colour cache (sw:897-914), which line colours
// read in every CRAM mode.
inline constexpr uint32_t kVDP2CRAM555TableBase = kVDP2CRAMColorCacheEntries;
inline constexpr uint32_t kVDP2CRAMColorBufferSize = 2 * sizeof(CRAMColorCache);

/// @brief Size of the VDP2 CRAM rotation coefficients buffer, in bytes.
/// The second half of CRAM can be used for that purpose.
inline constexpr uint32_t kVDP2CRAMRotCoeffBufferSize = ymir::vdp::kVDP2CRAMSize / 2;

} // namespace brimir::gpu::vdp2
