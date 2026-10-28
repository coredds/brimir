// Brimir: VDP1 GPU constants and the accessors of the packed fields. Brimir only (not derived from Ymir's
// vdp1_*_params.hlsli, whose bitfield layouts do not match their host structs).
// Host mirror: src/gpu/vdp1/vdp1_hw_params.hpp, where every field, bit position and function is documented; each
// function here has the same name and semantics there (tests/unit/test_gpu_vdp1_params.cpp compares them over random
// words through cs_vdp1_layout_probe.hlsl).
#ifndef BRIMIR_VDP1_DEFS_HLSLI
#define BRIMIR_VDP1_DEFS_HLSLI

#include "util/bit_ops.hlsli"

// Framebuffer memory: eight 256 KiB slices, main 0-1, alternate field 2-3, mesh field 0 4-5, mesh field 1 6-7.
static const uint kFbramSliceSize = 256 * 1024;
static const uint kFbramBufferSize = 8 * kFbramSliceSize;
static const uint kWordsPerFramebuffer = kFbramSliceSize / 2;
static const uint kWordIndexMask = kWordsPerFramebuffer - 1;
static const uint kUnitCount = 2 * kWordsPerFramebuffer;
static const uint kVRAMBufferSize = 512 * 1024;

uint Vdp1MainSlice(uint fb) { return fb & 1; }
uint Vdp1AltSlice(uint fb) { return 2 + (fb & 1); }
uint Vdp1MeshSlice(uint alt, uint fb) { return 4 + 2 * (alt & 1) + (fb & 1); }
uint Vdp1FbramByteAddress(uint slice, uint word) { return (slice & 7) * kFbramSliceSize + (word & kWordIndexMask) * 2; }
uint Vdp1FbWordShift(uint word) { return (word & 1) * 16; }

uint Vdp1Lo16(uint v) { return v & 0xFFFF; }
uint Vdp1Hi16(uint v) { return v >> 16; }
uint Vdp1Pack16(uint lo, uint hi) { return (lo & 0xFFFF) | (hi << 16); }

// CommandParams::drawFlags (CMDPMOD at bits 15-0, CMDCTRL's low six bits at 21-16, textured, antialiased)
static const uint kCmdpmodMask = 0x9FFF;
static const uint kCmdctrlMask = 0x3F;
uint Vdp1MakeDrawFlags(uint cmdpmod, uint cmdctrl, bool textured, bool antiAlias) {
    return (cmdpmod & kCmdpmodMask) | ((cmdctrl & kCmdctrlMask) << 16) | (textured ? (1u << 22) : 0u) |
           (antiAlias ? (1u << 23) : 0u);
}
uint Vdp1CmdColorCalc(uint f) { return f & 3; }
bool Vdp1CmdGouraud(uint f) { return BitTest(f, 2); }
uint Vdp1CmdColorMode(uint f) { return BitExtract(f, 3, 3); }
bool Vdp1CmdTransparentPixelDisable(uint f) { return BitTest(f, 6); }
bool Vdp1CmdEndCodeDisable(uint f) { return BitTest(f, 7); }
bool Vdp1CmdMesh(uint f) { return BitTest(f, 8); }
bool Vdp1CmdClippingMode(uint f) { return BitTest(f, 9); }
bool Vdp1CmdUserClip(uint f) { return BitTest(f, 10); }
bool Vdp1CmdHighSpeedShrink(uint f) { return BitTest(f, 12); }
bool Vdp1CmdMsbOn(uint f) { return BitTest(f, 15); }
uint Vdp1CmdType(uint f) { return BitExtract(f, 16, 4); }
bool Vdp1CmdFlipH(uint f) { return BitTest(f, 20); }
bool Vdp1CmdFlipV(uint f) { return BitTest(f, 21); }
bool Vdp1CmdTextured(uint f) { return BitTest(f, 22); }
bool Vdp1CmdAntiAlias(uint f) { return BitTest(f, 23); }

// CommandParams::rasterFlags
uint Vdp1MakeRasterFlags(bool pixel8Bits, uint fbSizeH, bool dblInterlaceEnable, bool dblInterlaceDrawLine,
                         bool evenOddCoordSelect, bool doubleDensity, bool deinterlace, bool transparentMeshes,
                         uint doubleV) {
    return (pixel8Bits ? 1u : 0u) | (fbSizeH == 1024 ? 2u : 0u) | (dblInterlaceEnable ? 4u : 0u) |
           (dblInterlaceDrawLine ? 8u : 0u) | (evenOddCoordSelect ? 16u : 0u) | (doubleDensity ? 32u : 0u) |
           (deinterlace ? 64u : 0u) | (transparentMeshes ? 128u : 0u) | ((doubleV & 1) << 8);
}
bool Vdp1RasPixel8(uint f) { return BitTest(f, 0); }
uint Vdp1RasFbSizeH(uint f) { return BitTest(f, 1) ? 1024 : 512; }
bool Vdp1RasDblInterlace(uint f) { return BitTest(f, 2); }
bool Vdp1RasDblInterlaceDrawLine(uint f) { return BitTest(f, 3); }
bool Vdp1RasEvenOddCoordSelect(uint f) { return BitTest(f, 4); }
bool Vdp1RasDoubleDensity(uint f) { return BitTest(f, 5); }
bool Vdp1RasDeinterlace(uint f) { return BitTest(f, 6); }
bool Vdp1RasTransparentMeshes(uint f) { return BitTest(f, 7); }
uint Vdp1RasDoubleV(uint f) { return BitExtract(f, 8, 1); }

// Span::stepTexV
uint Vdp1SpanQuadStep(uint stepTexV) { return Vdp1Lo16(stepTexV); }
// The texel row is int16 (-1 when CMDSIZE.V is 0), sign-extended to 32 bits
uint Vdp1SpanTexV(uint stepTexV) { return uint(int(stepTexV) >> 16); }

// Fragment::target (bit 31 live; a slot without a fragment reads as 0)
static const uint kFragAltBit = 1u << 17;
static const uint kFragLiveBit = 1u << 31;
static const uint kFragUnitMask = kWordIndexMask | kFragAltBit;
uint Vdp1MakeFragmentTarget(uint word, bool alt, bool oddByte, uint colorCalc, bool mesh, bool msbOn, bool pixel8,
                            bool transparentMeshes) {
    return (word & kWordIndexMask) | (alt ? kFragAltBit : 0u) | (oddByte ? (1u << 18) : 0u) |
           ((colorCalc & 3) << 19) | (mesh ? (1u << 21) : 0u) | (msbOn ? (1u << 22) : 0u) |
           (pixel8 ? (1u << 23) : 0u) | (transparentMeshes ? (1u << 24) : 0u) | kFragLiveBit;
}
uint Vdp1FragWord(uint t) { return t & kWordIndexMask; }
bool Vdp1FragAlt(uint t) { return BitTest(t, 17); }
bool Vdp1FragOddByte(uint t) { return BitTest(t, 18); }
uint Vdp1FragColorCalc(uint t) { return BitExtract(t, 19, 2); }
bool Vdp1FragMesh(uint t) { return BitTest(t, 21); }
bool Vdp1FragMsbOn(uint t) { return BitTest(t, 22); }
bool Vdp1FragPixel8(uint t) { return BitTest(t, 23); }
bool Vdp1FragTransparentMeshes(uint t) { return BitTest(t, 24); }
bool Vdp1FragLive(uint t) { return BitTest(t, 31); }
uint Vdp1FragUnit(uint t) { return t & kFragUnitMask; }

// FbWrite::address
uint Vdp1MakeFbWriteAddress(uint word, uint fb, bool mirrorAlt) {
    return (word & kWordIndexMask) | ((fb & 1) << 17) | (mirrorAlt ? (1u << 18) : 0u);
}
uint Vdp1FbWriteWord(uint a) { return a & kWordIndexMask; }
uint Vdp1FbWriteFb(uint a) { return BitExtract(a, 17, 1); }
bool Vdp1FbWriteMirrorAlt(uint a) { return BitTest(a, 18); }

// g_batchState words (vdp1_bindings.hpp kBatchState*)
static const uint kBatchStateBucketArgs = 0;
static const uint kBatchStateResolveArgs = 3;
static const uint kBatchStateUnitLo = 6;
static const uint kBatchStateUnitHi = 7;
static const uint kBatchStateAccLoInv = 8;
static const uint kBatchStateAccHi = 9;

// g_fbWinners index (vdp1_bindings.hpp Vdp1FbWinnerIndex)
uint Vdp1FbWinnerIndex(bool alt, uint fb, uint word, bool lowByte) {
    return ((((alt ? 2u : 0u) + (fb & 1)) * kWordsPerFramebuffer + (word & kWordIndexMask)) << 1) | (lowByte ? 1u : 0u);
}

// ErasePushConstants::flags
uint Vdp1MakeEraseFlags(uint fb, bool mirrorAlt, bool clearMesh) {
    return (fb & 1) | (mirrorAlt ? 2u : 0u) | (clearMesh ? 4u : 0u);
}
uint Vdp1EraseFb(uint f) { return f & 1; }
bool Vdp1EraseMirrorAlt(uint f) { return BitTest(f, 1); }
bool Vdp1EraseClearMesh(uint f) { return BitTest(f, 2); }

#endif
