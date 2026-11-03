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
// - sizes: the buffer structs Vdp1CommandParams (80 bytes), Vdp1Span (32, the size the plan names), Vdp1NxSpan (32)
//   and Vdp1FbWrite (16) are multiples of 16 bytes; Vdp1Fragment, one slot of a buffer of millions, is 8 bytes; the push-constant blocks
//   are Vdp1BatchPushConstants 24, Vdp1ErasePushConstants 32 and Vdp1FbWritePushConstants 8 bytes, and the N x
//   passes' Vdp1EraseNxPushConstants 36, Vdp1FbWriteNxPushConstants 12 and Vdp1RebuildNxPushConstants 8 bytes.
// Every offset and size is checked against the reflected layouts of src/gpu/shaders/vdp1/cs_vdp1_layout_probe.hlsl and
// cs_vdp1_nx_layout_probe.hlsl (the N x structs), which declare them all (tests/unit/test_gpu_vdp1_params.cpp). No
// Vulkan header may be included here.

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
                             // (textured; int16, -1 to 255 as Vdp1FrontEnd::AddSpan asserts: rows 0 to CMDSIZE.V - 1,
                             // -1 when CMDSIZE.V is 0 with a non-zero width)
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
// N x span (GPU stage 3; StructuredBuffer<NxSpan>, 32 bytes): one span of an N x sub-batch, as the N x generator
// (vdp1_nx_front.hpp) emits it from a native batch. Three kinds (tests/unit/vdp1_nx_reference.hpp is the spec):
//   Rect       one N x sub-row of a native rect row: x0..y1 the native span's end points, subRow the sub-row in
//              stepping order, texV the sub-row's texel row and gouraudLR its edge colours; N * native Length() slots
//   Quad       an N x line: x0..y1 the N x end points (quads, thin quads and parallel lines), texV the texel row of the
//              N x quad step; 2 * Length() slots after the N x system clip, key slotBase + 2 * step + isAA
//   Replicate  a native span whose fragments become N x N blocks: x0..y1, texV and gouraudLR the native span's;
//              N^2 * native slots, key slotBase + N^2 * nativeKey + j * N + i
// A sub-batch's spans have contiguous slot ranges in emission order starting at 0: a span's slots end at the next
// span's slotBase (the last one's at the sub-batch's slot count), so the slot order is the drawing order.
//
// flags:
//   bits 1-0    kind (kNxSpanRect, kNxSpanQuad, kNxSpanReplicate)
//   bit 2       antialiased (Quad)
//   bits 4-3    sub-row 0..N-1 (Rect)
//   bits 31-16  texel row (int16)

inline constexpr uint32_t kMaxNxScale = 4;
inline constexpr uint32_t kNxSpanRect = 0;
inline constexpr uint32_t kNxSpanQuad = 1;
inline constexpr uint32_t kNxSpanReplicate = 2;
/// @brief Sub-batch capacities: the slots of the fragment buffer and the spans of the span buffer (equal to
/// kMaxFragmentsPerBatch and kMaxSpansPerBatch, vdp1_bindings.hpp); one span over the slot capacity goes alone.
inline constexpr uint32_t kMaxNxSlotsPerSubBatch = 4u * 1024 * 1024;
inline constexpr uint32_t kMaxNxSpansPerSubBatch = 256u * 1024;

struct Vdp1NxSpan {
    HLSLuint x0, y0, x1, y1; // end points (int32): native (Rect, Replicate) or N x (Quad)
    HLSLuint cmdIndex;       // Vdp1CommandParams index (relative to Vdp1BatchPushConstants::cmdOffset)
    HLSLuint slotBase;       // first slot in the sub-batch
    HLSLuint gouraudLR;      // left Color555 | right << 16
    HLSLuint flags;          // see above
};
static_assert(sizeof(Vdp1NxSpan) == 32);
static_assert(offsetof(Vdp1NxSpan, x0) == 0 && offsetof(Vdp1NxSpan, y0) == 4 && offsetof(Vdp1NxSpan, x1) == 8 &&
              offsetof(Vdp1NxSpan, y1) == 12 && offsetof(Vdp1NxSpan, cmdIndex) == 16 &&
              offsetof(Vdp1NxSpan, slotBase) == 20 && offsetof(Vdp1NxSpan, gouraudLR) == 24 &&
              offsetof(Vdp1NxSpan, flags) == 28);
static_assert(std::is_trivially_copyable_v<Vdp1NxSpan> && std::is_standard_layout_v<Vdp1NxSpan>);

constexpr uint32_t Vdp1MakeNxSpanFlags(uint32_t kind, bool antiAlias, uint32_t subRow, int32_t texV) {
    return (kind & 3u) | (antiAlias ? 4u : 0u) | ((subRow & 3u) << 3) | (static_cast<uint32_t>(texV) << 16);
}
constexpr uint32_t Vdp1NxSpanKind(uint32_t f) {
    return f & 3u;
}
constexpr bool Vdp1NxSpanAntiAlias(uint32_t f) {
    return ((f >> 2) & 1u) != 0;
}
constexpr uint32_t Vdp1NxSpanSubRow(uint32_t f) {
    return (f >> 3) & 3u;
}
/// @brief The texel row, sign-extended from 16 bits (as Vdp1SpanTexV).
constexpr uint32_t Vdp1NxSpanTexV(uint32_t f) {
    return static_cast<uint32_t>(static_cast<int32_t>(f) >> 16);
}
static_assert(Vdp1NxSpanTexV(Vdp1MakeNxSpanFlags(kNxSpanQuad, true, 3, -1)) == 0xFFFFFFFFu &&
              Vdp1NxSpanKind(Vdp1MakeNxSpanFlags(kNxSpanReplicate, false, 0, 255)) == kNxSpanReplicate &&
              Vdp1NxSpanSubRow(Vdp1MakeNxSpanFlags(kNxSpanRect, false, 3, 7)) == 3 &&
              Vdp1NxSpanAntiAlias(Vdp1MakeNxSpanFlags(kNxSpanQuad, true, 0, 0)));

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
// N x surface and N x fragments (GPU stage 3; research section 1.1 and 1.4)
//
// The N x surface is a storage buffer of the same eight slices as FBRAM, each N^2 times as large: native word w of a
// slice is the N x N block of N x words w * N^2 + j * N + i for the sub-texel (i, j), stored as FBRAM stores its words
// (N x word k of a slice is the half k & 1 of buffer uint k >> 1, big-endian bytes). 2 MiB * N^2: 8, 18, 32 MiB.
//
// The N x resolve unit is (native unit) * N^2 + j * N + i, the native unit being word | alt << 17: 22 bits at 4x. An N x
// fragment target keeps the unit in bits 21-0, the native target's flags (bits 24-18: odd byte, colour calculation,
// mesh, MSB on, 8 bpp, transparent meshes) four bits up in bits 28-22, and `live` in bit 31.

inline constexpr uint32_t kMaxNxUnitCount = kMaxNxScale * kMaxNxScale * kUnitCount;
inline constexpr uint32_t kNxFragUnitBits = 22;
inline constexpr uint32_t kNxFragUnitMask = (1u << kNxFragUnitBits) - 1;
inline constexpr uint32_t kFragFlagMask = 0x7Fu << 18; // the native target's flags (bits 24-18)
inline constexpr uint32_t kNxFragFlagShift = 4;
static_assert(kMaxNxUnitCount == (1u << kNxFragUnitBits) && (kFragFlagMask << kNxFragFlagShift) >> 29 == 0);

constexpr uint32_t Vdp1NxUnitCount(uint32_t scale) {
    return scale * scale * kUnitCount;
}
constexpr uint32_t Vdp1NxFbramSliceSize(uint32_t scale) {
    return kFbramSliceSize * scale * scale;
}
constexpr uint32_t Vdp1NxFbramBufferSize(uint32_t scale) {
    return kFbramBufferSize * scale * scale;
}
/// @brief Byte address in the N x surface of N x word `index` (native word * N^2 + sub) of slice `slice`.
constexpr uint32_t Vdp1NxFbramByteAddress(uint32_t slice, uint32_t index, uint32_t scale) {
    return (slice & 7u) * Vdp1NxFbramSliceSize(scale) + index * 2u;
}
/// @brief The N x unit of sub-texel `sub` (j * N + i) of a native unit (word | alt << 17).
constexpr uint32_t Vdp1NxUnit(uint32_t nativeUnit, uint32_t sub, uint32_t scale) {
    return nativeUnit * scale * scale + sub;
}
/// @brief The N x fragment target of N x unit `nxUnit` with the flags of the native target `nativeTarget`.
constexpr uint32_t Vdp1MakeNxFragmentTarget(uint32_t nxUnit, uint32_t nativeTarget) {
    return (nxUnit & kNxFragUnitMask) | ((nativeTarget & kFragFlagMask) << kNxFragFlagShift) | kFragLiveBit;
}
constexpr uint32_t Vdp1NxFragUnit(uint32_t t) {
    return t & kNxFragUnitMask;
}
/// @brief The flags and `live` of an N x target in the native layout (word and field 0): what ApplyFragment reads.
constexpr uint32_t Vdp1NxFragNativeTarget(uint32_t t) {
    return ((t >> kNxFragFlagShift) & kFragFlagMask) | (t & kFragLiveBit);
}
static_assert(Vdp1NxFragNativeTarget(Vdp1MakeNxFragmentTarget(kNxFragUnitMask, 0xFFFFFFFFu)) ==
                  (kFragFlagMask | kFragLiveBit) &&
              Vdp1NxFragUnit(Vdp1MakeNxFragmentTarget(kNxFragUnitMask, 0xFFFFFFFFu)) == kNxFragUnitMask);

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
    HLSLuint scale;          // 1 for the native passes; N for an N x sub-batch (raster_nx, bucket_sums_nx, scatter,
                             // resolve_nx)
};
static_assert(sizeof(Vdp1BatchPushConstants) == 24);

/// @brief Vdp1BatchPushConstants::drawFB of an N x sub-batch (GPU stage 3, the overwrite-class fast path of research
/// section 1.6): bit 0 the framebuffer; kBatchOverwrite: every fragment of the sub-batch replaces what it writes
/// (no MSB-on, 16 bpp replace or half-luminance or 8 bpp, no write to the mesh framebuffer, one pixel depth and
/// transparent meshes setting), so a unit's result is its last fragment's: raster_nx_overwrite keeps the highest
/// slot + 1 per N x unit (InterlockedMax into the unit counts) instead of counting, and resolve_fast_nx applies that one
/// fragment; kBatchOverwriteOddBytes (8 bpp): the odd bytes' winners go to the unit offsets (a word's two bytes are
/// written independently). kBatchHybrid: a sub-batch mixing overwrite-class fragments with ones that read what they
/// write (MSB-on, shadow, half-transparency, mesh framebuffer writes), with one pixel depth and transparent-meshes
/// setting (an overwrite-class fragment then replaces every bit any fragment writes at its word, or byte at 8 bpp, the
/// mesh word too when transparent meshes clear it): raster_nx_hybrid keeps each unit's (byte's) last overwrite-class slot + 1 in the
/// unit offsets, filter_nx counts only that fragment and the later ones (the others are marked dead), then the ordered
/// passes run.
inline constexpr uint32_t kBatchDrawFbMask = 1u;
inline constexpr uint32_t kBatchOverwrite = 2u;
inline constexpr uint32_t kBatchOverwriteOddBytes = 4u;
inline constexpr uint32_t kBatchHybrid = 8u;

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

/// @brief N x erase pass (erase_nx): the native erase's region, value and write limit; each erased native word's N x
/// block gets the value (mesh blocks 0).
struct Vdp1EraseNxPushConstants {
    Vdp1ErasePushConstants erase;
    HLSLuint scale; // N, 2 to kMaxNxScale
};
static_assert(sizeof(Vdp1EraseNxPushConstants) == 36 && offsetof(Vdp1EraseNxPushConstants, scale) == 32);

/// @brief N x CPU framebuffer write pass (fb_write_nx, after the native run): each record's N x blocks take the bits of
/// its mask from the final native word.
struct Vdp1FbWriteNxPushConstants {
    Vdp1FbWritePushConstants run;
    HLSLuint scale;
};
static_assert(sizeof(Vdp1FbWriteNxPushConstants) == 12 && offsetof(Vdp1FbWriteNxPushConstants, scale) == 8);

/// @brief N x rebuild pass (rebuild_nx): every N x word of the slices in sliceMask (bit s: slice s) becomes its native
/// word.
struct Vdp1RebuildNxPushConstants {
    HLSLuint sliceMask;
    HLSLuint scale;
};
static_assert(sizeof(Vdp1RebuildNxPushConstants) == 8);

} // namespace brimir::gpu::vdp1
