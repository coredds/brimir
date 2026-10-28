#pragma once

// GPU-visible VDP1 structs, shared by the Vulkan-free front half (it packs them) and the VDP1 shaders
// (src/gpu/shaders/vdp1/include/vdp1_params.hlsli and vdp1_defs.hlsli declare the same layouts and accessors).
//
// Written from the CPU reference of the GPU rasterizer (tests/unit/vdp1_raster_reference.hpp: Span, Fragment,
// CommandParams, RasterState, EraseParams) and the software renderer, not from Ymir's structs (common_hw_defs.hpp
// VDP1*Params), whose std430 layouts do not match their host layouts (VDP1SpanParams: uint3 members) and which pack
// bitfields. Rules, as for the VDP2 structs:
// - every field is a 32-bit uint (signed values are int32 stored as their two's complement); no vec3, no bitfields;
// - flags are packed by explicit shifts, documented below and mirrored by the HLSL accessors of the same names
//   (a Vdp1Foo accessor here is Vdp1Foo in vdp1_defs.hlsli); tests/unit/test_gpu_vdp1_params.cpp runs both over
//   random words;
// - struct sizes are multiples of 16 bytes (the span is the 32 bytes the plan names), except the fragment: one slot of
//   a buffer of millions, 8 bytes.
// Every offset and size is checked against the reflected layouts of src/gpu/shaders/vdp1/cs_vdp1_layout_probe.hlsl,
// which declares them all (tests/unit/test_gpu_vdp1_params.cpp). No Vulkan header may be included here.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace brimir::gpu::vdp1 {

using HLSLuint = uint32_t;

// ---------------------------------------------------------------------------------------------------------------------
// Framebuffer memory (FBRAM)
//
// GPU FBRAM is the 2 MiB buffer the VDP2 sprite shader reads (vdp2::kBindingSpriteFB; Ymir's slice layout), eight
// 256 KiB slices of raw bytes, each a framebuffer exactly as the CPU sees it (16-bit words big-endian in memory):
//   slice 0, 1  main framebuffer 0 and 1          (software's mem1.FBRAM)
//   slice 2, 3  alternate field of framebuffer 0 and 1   (deinterlacing; GetAltFBRAM)
//   slice 4, 5  mesh framebuffer 0 and 1, field 0        (transparent meshes; GetMeshFBRAM [0])
//   slice 6, 7  mesh framebuffer 0 and 1, field 1        (GetMeshFBRAM [1])
// A buffer word (uint) holds two FBRAM words: FBRAM word w is the half w & 1 of uint w >> 1, byte-swapped
// (Vdp1FbWordShift, Vdp1FbramByteAddress).
inline constexpr uint32_t kFbramSliceSize = 256 * 1024;
inline constexpr uint32_t kFbramSliceCount = 8;
inline constexpr uint32_t kFbramBufferSize = kFbramSliceCount * kFbramSliceSize;
inline constexpr uint32_t kWordsPerFramebuffer = kFbramSliceSize / 2; // 0x20000 16-bit words
inline constexpr uint32_t kWordIndexBits = 17;
inline constexpr uint32_t kWordIndexMask = kWordsPerFramebuffer - 1;
static_assert(kWordsPerFramebuffer == (1u << kWordIndexBits));

/// @brief The resolve unit is an FBRAM word of one field of the draw framebuffer: word index | alternate field << 17.
inline constexpr uint32_t kUnitCount = 2 * kWordsPerFramebuffer;

constexpr uint32_t Vdp1MainSlice(uint32_t fb) {
    return fb & 1u;
}
constexpr uint32_t Vdp1AltSlice(uint32_t fb) {
    return 2u + (fb & 1u);
}
constexpr uint32_t Vdp1MeshSlice(uint32_t alt, uint32_t fb) {
    return 4u + 2u * (alt & 1u) + (fb & 1u);
}
/// @brief Byte address in the FBRAM buffer of 16-bit word `word` (masked to the slice) of slice `slice`.
constexpr uint32_t Vdp1FbramByteAddress(uint32_t slice, uint32_t word) {
    return (slice & 7u) * kFbramSliceSize + (word & kWordIndexMask) * 2u;
}
/// @brief Shift of the word in its buffer uint (before byte-swapping the 16 bits).
constexpr uint32_t Vdp1FbWordShift(uint32_t word) {
    return (word & 1u) * 16u;
}

constexpr uint32_t Vdp1Lo16(uint32_t v) {
    return v & 0xFFFFu;
}
constexpr uint32_t Vdp1Hi16(uint32_t v) {
    return v >> 16;
}
constexpr uint32_t Vdp1Pack16(uint32_t lo, uint32_t hi) {
    return (lo & 0xFFFFu) | (hi << 16);
}

// ---------------------------------------------------------------------------------------------------------------------
// Command parameters (StructuredBuffer<CommandParams>, 80 bytes): everything the raster pass reads of a command,
// captured at command time. Spans index them (Vdp1Span::cmdIndex, relative to Vdp1BatchPushConstants::cmdOffset).
//
// Stage 3 reads the same data: the four vertices (post local coordinates and doubleV), the Gouraud corners and the
// texture parameters stay available to a pass that subdivides quads or emits sub-pixel spans.

struct Vdp1CommandParams {
    HLSLuint vertex[8];  // A.x, A.y, B.x, B.y, C.x, C.y, D.x, D.y: int32 pixel coordinates after local coordinates and
                         // doubleV (sprites: the corners A-D; lines and polyline segments: A and B or the polygon's)
    HLSLuint gouraud[2]; // the Gouraud table's Color555 corners: A | B << 16, C | D << 16 (0 without Gouraud)
    HLSLuint drawFlags;   // see CmdDraw* below
    HLSLuint rasterFlags; // the registers and enhancements at command time, see Ras* below
    HLSLuint color;       // CMDCOLR (untextured commands)
    HLSLuint colorBank;   // textured: the colour bank (colour modes 0, 2, 3, 4), the look-up table address (mode 1:
                          // CMDCOLR << 3) or unused (mode 5)
    HLSLuint charAddr;    // CMDSRCA * 8; colour mode 5 aligned down to 16 bytes
    HLSLuint charSize;    // character width in pixels (CMDSIZE.H * 8) | height (CMDSIZE.V) << 16
    HLSLuint sysClip;     // system clip: raw 16-bit X (CMDXC) | Y (CMDYC) << 16
    HLSLuint userClip0;   // user clip: raw 16-bit X0 | Y0 << 16
    HLSLuint userClip1;   // raw 16-bit X1 | Y1 << 16
    HLSLuint cmdAddress;  // VRAM address of the command table (diagnostics, hazard tracking)
};
static_assert(sizeof(Vdp1CommandParams) == 80 && sizeof(Vdp1CommandParams) % 16 == 0);
static_assert(offsetof(Vdp1CommandParams, vertex) == 0 && offsetof(Vdp1CommandParams, gouraud) == 32 &&
              offsetof(Vdp1CommandParams, drawFlags) == 40 && offsetof(Vdp1CommandParams, rasterFlags) == 44 &&
              offsetof(Vdp1CommandParams, color) == 48 && offsetof(Vdp1CommandParams, colorBank) == 52 &&
              offsetof(Vdp1CommandParams, charAddr) == 56 && offsetof(Vdp1CommandParams, charSize) == 60 &&
              offsetof(Vdp1CommandParams, sysClip) == 64 && offsetof(Vdp1CommandParams, userClip0) == 68 &&
              offsetof(Vdp1CommandParams, userClip1) == 72 && offsetof(Vdp1CommandParams, cmdAddress) == 76);

// drawFlags: the command's CMDPMOD at bits 0-15 (hardware order, bits 13-14 zero), the low six bits of CMDCTRL at
// bits 16-21, then what the front half derived:
//   bits 1-0   colour calculation (0 replace, 1 shadow, 2 half-luminance, 3 half-transparency)
//   bit 2      Gouraud shading
//   bits 5-3   colour mode (0-5; textured)
//   bit 6      transparent pixel disable (TPD)
//   bit 7      end code disable (ECD)
//   bit 8      mesh
//   bit 9      clipping mode (1: draw outside the user clip area)
//   bit 10     user clipping enable
//   bit 11     pre-clipping disable (not used)
//   bit 12     high-speed shrink
//   bit 15     MSB on
//   bits 19-16 command (CMDCTRL command)
//   bit 20     flip horizontally (CMDCTRL bit 4)
//   bit 21     flip vertically (CMDCTRL bit 5)
//   bit 22     textured
//   bit 23     antialiased (polygons and sprites; not polylines and lines)
inline constexpr uint32_t kCmdpmodMask = 0x9FFF;
inline constexpr uint32_t kCmdctrlMask = 0x3F;
constexpr uint32_t Vdp1MakeDrawFlags(uint32_t cmdpmod, uint32_t cmdctrl, bool textured, bool antiAlias) {
    return (cmdpmod & kCmdpmodMask) | ((cmdctrl & kCmdctrlMask) << 16) | (textured ? 1u << 22 : 0u) |
           (antiAlias ? 1u << 23 : 0u);
}
constexpr uint32_t Vdp1CmdColorCalc(uint32_t f) {
    return f & 3u;
}
constexpr bool Vdp1CmdGouraud(uint32_t f) {
    return ((f >> 2) & 1u) != 0;
}
constexpr uint32_t Vdp1CmdColorMode(uint32_t f) {
    return (f >> 3) & 7u;
}
constexpr bool Vdp1CmdTransparentPixelDisable(uint32_t f) {
    return ((f >> 6) & 1u) != 0;
}
constexpr bool Vdp1CmdEndCodeDisable(uint32_t f) {
    return ((f >> 7) & 1u) != 0;
}
constexpr bool Vdp1CmdMesh(uint32_t f) {
    return ((f >> 8) & 1u) != 0;
}
constexpr bool Vdp1CmdClippingMode(uint32_t f) {
    return ((f >> 9) & 1u) != 0;
}
constexpr bool Vdp1CmdUserClip(uint32_t f) {
    return ((f >> 10) & 1u) != 0;
}
constexpr bool Vdp1CmdHighSpeedShrink(uint32_t f) {
    return ((f >> 12) & 1u) != 0;
}
constexpr bool Vdp1CmdMsbOn(uint32_t f) {
    return ((f >> 15) & 1u) != 0;
}
constexpr uint32_t Vdp1CmdType(uint32_t f) {
    return (f >> 16) & 0xFu;
}
constexpr bool Vdp1CmdFlipH(uint32_t f) {
    return ((f >> 20) & 1u) != 0;
}
constexpr bool Vdp1CmdFlipV(uint32_t f) {
    return ((f >> 21) & 1u) != 0;
}
constexpr bool Vdp1CmdTextured(uint32_t f) {
    return ((f >> 22) & 1u) != 0;
}
constexpr bool Vdp1CmdAntiAlias(uint32_t f) {
    return ((f >> 23) & 1u) != 0;
}

// rasterFlags: the VDP1 registers and enhancements the command is drawn under (software reads them live at plot
// time), so a mid-frame register write needs no batch split:
//   bit 0   TVMR 8 bits per pixel
//   bit 1   framebuffer width 1024 (TVMR; else 512)
//   bit 2   FBCR double interlace enable (DIE)
//   bit 3   FBCR double interlace draw line (DIL)
//   bit 4   FBCR even/odd coordinate select (EOS)
//   bit 5   TVMD double-density interlace
//   bit 6   deinterlace enhancement
//   bit 7   transparent meshes enhancement
//   bit 8   doubleV (0 or 1, latched at VDP1BeginFrame)
// The framebuffer drawn to (0 or 1) is not here: a batch draws to one framebuffer, Vdp1BatchPushConstants::drawFB.
constexpr uint32_t Vdp1MakeRasterFlags(bool pixel8Bits, uint32_t fbSizeH, bool dblInterlaceEnable,
                                       bool dblInterlaceDrawLine, bool evenOddCoordSelect, bool doubleDensity,
                                       bool deinterlace, bool transparentMeshes, uint32_t doubleV) {
    return (pixel8Bits ? 1u : 0u) | (fbSizeH == 1024 ? 2u : 0u) | (dblInterlaceEnable ? 4u : 0u) |
           (dblInterlaceDrawLine ? 8u : 0u) | (evenOddCoordSelect ? 16u : 0u) | (doubleDensity ? 32u : 0u) |
           (deinterlace ? 64u : 0u) | (transparentMeshes ? 128u : 0u) | ((doubleV & 1u) << 8);
}
constexpr bool Vdp1RasPixel8(uint32_t f) {
    return (f & 1u) != 0;
}
constexpr uint32_t Vdp1RasFbSizeH(uint32_t f) {
    return (f & 2u) != 0 ? 1024u : 512u;
}
constexpr bool Vdp1RasDblInterlace(uint32_t f) {
    return ((f >> 2) & 1u) != 0;
}
constexpr bool Vdp1RasDblInterlaceDrawLine(uint32_t f) {
    return ((f >> 3) & 1u) != 0;
}
constexpr bool Vdp1RasEvenOddCoordSelect(uint32_t f) {
    return ((f >> 4) & 1u) != 0;
}
constexpr bool Vdp1RasDoubleDensity(uint32_t f) {
    return ((f >> 5) & 1u) != 0;
}
constexpr bool Vdp1RasDeinterlace(uint32_t f) {
    return ((f >> 6) & 1u) != 0;
}
constexpr bool Vdp1RasTransparentMeshes(uint32_t f) {
    return ((f >> 7) & 1u) != 0;
}
constexpr uint32_t Vdp1RasDoubleV(uint32_t f) {
    return (f >> 8) & 1u;
}

// vertex, colour, char size and clip words: Vdp1Lo16 / Vdp1Hi16 (charSize: width, height; sysClip: H, V; userClip0:
// X0, Y0; userClip1: X1, Y1; gouraud: corners A, C / B, D). The clip values are the raw 16-bit register values: the
// shader applies doubleV as software does ((V << dV) | dV for the Y limits).

// ---------------------------------------------------------------------------------------------------------------------
// Span (StructuredBuffer<Span>, 32 bytes): one line software plots, the quad loop's left-right line at `quadStep` or a
// polyline or line segment. The raster pass runs software's line loop on it, one thread per span.

struct Vdp1Span {
    HLSLuint x0, y0, x1, y1; // the line's end points (int32), before SystemClip
    HLSLuint cmdIndex;       // Vdp1CommandParams index (relative to Vdp1BatchPushConstants::cmdOffset)
    HLSLuint fragBase;       // first fragment slot of the span (the batch's prefix sum of 2 * Length() after SystemClip)
    HLSLuint gouraudLR;      // the Gouraud colours at the left and right end: left Color555 | right << 16
    HLSLuint stepTexV;       // quadStep (bits 15-0: the quad step 0-4095 or polyline segment 0-3) | texel row V << 16
                             // (textured; int16: -1 to 254, -1 when CMDSIZE.V is 0 with a non-zero width)
};
static_assert(sizeof(Vdp1Span) == 32);
static_assert(offsetof(Vdp1Span, x0) == 0 && offsetof(Vdp1Span, cmdIndex) == 16 &&
              offsetof(Vdp1Span, fragBase) == 20 && offsetof(Vdp1Span, gouraudLR) == 24 &&
              offsetof(Vdp1Span, stepTexV) == 28);

constexpr uint32_t Vdp1SpanQuadStep(uint32_t stepTexV) {
    return Vdp1Lo16(stepTexV);
}
/// @brief The texel row, sign-extended from 16 bits (QuadStepper::SetupTexture steps from 0 to CMDSIZE.V - 1, so a
/// height of 0 steps to row -1: TextureStepper::Value() is then 0xFFFFFFFF).
constexpr uint32_t Vdp1SpanTexV(uint32_t stepTexV) {
    return static_cast<uint32_t>(static_cast<int32_t>(stepTexV) >> 16);
}

// ---------------------------------------------------------------------------------------------------------------------
// Fragment (RWStructuredBuffer<Fragment>, 8 bytes): one write VDP1PlotPixel would make. The fragment buffer is indexed
// by the order key: slot fragBase + 2 * step + isAA of the span, so the buffer holds no key (a bucket stores slot
// indices and the resolve sorts them). A slot a span does not use reads as 0 (bit 31, `live`, clear); the raster pass
// writes every slot of its span or the buffer is cleared before it.
//
// target:
//   bits 16-0   FBRAM word index (framebuffer byte offset >> 1)
//   bit 17      alternate field (the alt framebuffer; mesh field 1)
//   bit 18      8 bpp: the odd byte of the word
//   bits 20-19  colour calculation (16 bpp; as drawFlags 1-0)
//   bit 21      write to the mesh framebuffer (transparent meshes and mesh)
//   bit 22      MSB on (sets bit 15 of the main word only)
//   bit 23      8 bpp (TVMR at the command)
//   bit 24      transparent meshes (a write to the main framebuffer clears the mesh word)
//   bit 31      live
// color: 16 bpp, the source colour after Gouraud (MSB on: 0); 8 bpp, the byte.

struct Vdp1Fragment {
    HLSLuint target;
    HLSLuint color;
};
static_assert(sizeof(Vdp1Fragment) == 8);
static_assert(offsetof(Vdp1Fragment, target) == 0 && offsetof(Vdp1Fragment, color) == 4);

inline constexpr uint32_t kFragAltBit = 1u << 17;
inline constexpr uint32_t kFragLiveBit = 1u << 31;
/// @brief The resolve unit of a fragment: the FBRAM word and the field.
inline constexpr uint32_t kFragUnitMask = kWordIndexMask | kFragAltBit;
constexpr uint32_t Vdp1MakeFragmentTarget(uint32_t word, bool alt, bool oddByte, uint32_t colorCalc, bool mesh,
                                          bool msbOn, bool pixel8, bool transparentMeshes) {
    return (word & kWordIndexMask) | (alt ? kFragAltBit : 0u) | (oddByte ? 1u << 18 : 0u) | ((colorCalc & 3u) << 19) |
           (mesh ? 1u << 21 : 0u) | (msbOn ? 1u << 22 : 0u) | (pixel8 ? 1u << 23 : 0u) |
           (transparentMeshes ? 1u << 24 : 0u) | kFragLiveBit;
}
constexpr uint32_t Vdp1FragWord(uint32_t t) {
    return t & kWordIndexMask;
}
constexpr bool Vdp1FragAlt(uint32_t t) {
    return ((t >> 17) & 1u) != 0;
}
constexpr bool Vdp1FragOddByte(uint32_t t) {
    return ((t >> 18) & 1u) != 0;
}
constexpr uint32_t Vdp1FragColorCalc(uint32_t t) {
    return (t >> 19) & 3u;
}
constexpr bool Vdp1FragMesh(uint32_t t) {
    return ((t >> 21) & 1u) != 0;
}
constexpr bool Vdp1FragMsbOn(uint32_t t) {
    return ((t >> 22) & 1u) != 0;
}
constexpr bool Vdp1FragPixel8(uint32_t t) {
    return ((t >> 23) & 1u) != 0;
}
constexpr bool Vdp1FragTransparentMeshes(uint32_t t) {
    return ((t >> 24) & 1u) != 0;
}
constexpr bool Vdp1FragLive(uint32_t t) {
    return (t >> 31) != 0;
}
constexpr uint32_t Vdp1FragUnit(uint32_t t) {
    return t & kFragUnitMask;
}

// ---------------------------------------------------------------------------------------------------------------------
// CPU framebuffer write (StructuredBuffer<FbWrite>, 16 bytes), captured when the CPU wrote (the shadow can change
// before the GPU replays it): the word is replaced where `mask` is set. Software's write mirrors to the alternate field
// under deinterlacing in interlace modes and leaves the mesh framebuffer alone.
//   address bits 16-0   FBRAM word index
//   address bit 17      framebuffer index (the draw framebuffer at the write)
//   address bit 18      also write the alternate field
//   value               the 16-bit word value (only the bits of `mask` are used)
//   mask                0xFF00 (a byte write to the even address), 0x00FF (the odd address) or 0xFFFF
struct Vdp1FbWrite {
    HLSLuint address;
    HLSLuint value;
    HLSLuint mask;
    HLSLuint pad; // 16-byte stride
};
static_assert(sizeof(Vdp1FbWrite) == 16);
static_assert(offsetof(Vdp1FbWrite, address) == 0 && offsetof(Vdp1FbWrite, value) == 4 &&
              offsetof(Vdp1FbWrite, mask) == 8 && offsetof(Vdp1FbWrite, pad) == 12);

constexpr uint32_t Vdp1MakeFbWriteAddress(uint32_t word, uint32_t fb, bool mirrorAlt) {
    return (word & kWordIndexMask) | ((fb & 1u) << 17) | (mirrorAlt ? 1u << 18 : 0u);
}
constexpr uint32_t Vdp1FbWriteWord(uint32_t a) {
    return a & kWordIndexMask;
}
constexpr uint32_t Vdp1FbWriteFb(uint32_t a) {
    return (a >> 17) & 1u;
}
constexpr bool Vdp1FbWriteMirrorAlt(uint32_t a) {
    return ((a >> 18) & 1u) != 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Push constants. Every pass has one block (set 0 holds the buffers, vdp1_bindings.hpp).

/// @brief Raster, bucket, scatter and resolve passes: one batch (a run of spans resolved together; a batch boundary is
/// a barrier).
struct Vdp1BatchPushConstants {
    HLSLuint spanOffset;     // first span of the batch in the span buffer
    HLSLuint spanCount;
    HLSLuint cmdOffset;      // first command of the batch in the command buffer
    HLSLuint fragmentCount;  // fragment slots of the batch (the last span's fragBase + its slot count)
    HLSLuint drawFB;         // the framebuffer the batch draws to (0 or 1)
    HLSLuint scale;          // internal resolution of the raster pass: 1 (stage 3 raises it; stage 2 is native only)
};
static_assert(sizeof(Vdp1BatchPushConstants) == 24);

/// @brief Erase pass (VDP1DoEraseFramebuffer): the region is `x1 <= x < x3`, `y1 <= y <= y3` (software's inclusive y3),
/// as software computes it from the latched erase registers, clamped and with y already shifted for double density.
struct Vdp1ErasePushConstants {
    HLSLuint x1, x3;        // clamped to 400 (428 in hi-res) as software
    HLSLuint y1, y3;        // clamped to the vertical resolution (halved in double density) and shifted left by it
    HLSLuint value;         // the 16-bit erase write value
    HLSLuint offsetShift;   // TVMR erase offset shift (8 or 9): word address (y << shift) + x
    HLSLuint writeLimit;    // writes that happen in row-major order (write i happens when i < writeLimit): the VBlank
                            // erase's cycle budget + 1; 0xFFFFFFFF without a budget
    HLSLuint flags;         // see Erase* below
};
static_assert(sizeof(Vdp1ErasePushConstants) == 32);
// flags: bit 0 framebuffer index to erase (the display framebuffer), bit 1 also erase the alternate field (deinterlace
// in double density), bit 2 clear the mesh framebuffer(s) (transparent meshes).
constexpr uint32_t Vdp1MakeEraseFlags(uint32_t fb, bool mirrorAlt, bool clearMesh) {
    return (fb & 1u) | (mirrorAlt ? 2u : 0u) | (clearMesh ? 4u : 0u);
}
constexpr uint32_t Vdp1EraseFb(uint32_t f) {
    return f & 1u;
}
constexpr bool Vdp1EraseMirrorAlt(uint32_t f) {
    return ((f >> 1) & 1u) != 0;
}
constexpr bool Vdp1EraseClearMesh(uint32_t f) {
    return ((f >> 2) & 1u) != 0;
}

/// @brief CPU framebuffer write pass: records [offset, offset + count) of the FbWrite buffer, applied in order.
struct Vdp1FbWritePushConstants {
    HLSLuint offset;
    HLSLuint count;
};
static_assert(sizeof(Vdp1FbWritePushConstants) == 8);

} // namespace brimir::gpu::vdp1
