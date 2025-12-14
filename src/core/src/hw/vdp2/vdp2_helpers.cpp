/**
 * @file vdp2_helpers.cpp
 * @brief VDP2 Helper Functions
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * 
 * Copyright (C) 2015-2019 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp2/vdp2_helpers.hpp>
#include <brimir/hw/vdp2/vdp2.hpp>

namespace brimir::vdp2 {

// =============================================================================
// VRAM Access
// =============================================================================
// Note: ReadVRAM8/16/32 and WriteVRAM8/16/32 are defined in vdp2_registers.cpp

uint32 ReadVRAM32(uint32 addr) {
    const uint32 offset = (addr & (VRAM_SIZE - 1)) >> 1;
    return (static_cast<uint32>(VRAM[offset]) << 16) | VRAM[offset + 1];
}

// =============================================================================
// Scroll Helpers
// =============================================================================

void FetchLineScroll(uint32 addr, LineScrollEntry& entry) {
    // TODO: Implement line scroll fetch
    entry.xScroll = 0;
    entry.yScroll = 0;
    entry.xZoom = 0x100;  // 1.0
    entry.yZoom = 0x100;
}

void CalculateScroll(uint32 layerIndex, uint32 line, ScrollState& state) {
    // TODO: Implement scroll calculation with line scroll
    state.xAccum = Regs.scrollX[layerIndex] << 16;
    state.yAccum = Regs.scrollY[layerIndex] << 16;
    state.xInc = 0x100;  // 1.0
    state.yInc = 0x100;
    state.lineAddr = 0;
}

// =============================================================================
// Rotation Helpers
// =============================================================================

void CalculateRotationCoord(const RotationParams& params,
                            sint32 screenX, sint32 screenY,
                            sint32& vramX, sint32& vramY) {
    // TODO: Implement rotation coordinate calculation
    // This applies the rotation matrix to transform screen coords to VRAM coords
    
    // Simplified (no rotation):
    vramX = screenX << 16;
    vramY = screenY << 16;
}

void FetchCoefficient(uint32 addr, uint32 index, sint32& kx, sint32& ky) {
    // TODO: Implement coefficient table fetch
    kx = 0x10000;  // 1.0
    ky = 0x10000;
}

void UpdateRotationAccum(RotationParams& params) {
    // TODO: Update rotation accumulators for next line
    params.XstAccum += params.DYst;
    params.YstAccum += params.DY;
    params.KAstAccum += params.DKAst;
}

// =============================================================================
// Window Helpers
// =============================================================================

void CalculateWindowLine(uint32 line) {
    // TODO: Calculate window state for current line
    // Update Windows[0] and Windows[1] for current line
}

bool EvaluateWindow(sint32 x, uint8 layerMask) {
    // TODO: Implement window evaluation
    // Check if pixel at x should be drawn based on window settings
    
    // Default: no window masking
    return true;
}

void FetchLineWindow(uint8 windowId, uint32 line, uint16& xStart, uint16& xEnd) {
    // TODO: Fetch per-line window coordinates from VRAM
    xStart = 0;
    xEnd = static_cast<uint16>(HRes - 1);
}

// =============================================================================
// Pattern Name / Character Helpers
// =============================================================================

void DecodePatternName(uint16 entry, uint8 pncMode, uint32 charBase, PatternName& result) {
    // TODO: Decode pattern name table entry based on mode
    
    // Simplified decode (mode 0, no supplementary data)
    result.charAddr = charBase + ((entry & 0x3FF) << 5);
    result.paletteNum = (entry >> 12) & 0xF;
    result.hFlip = (entry & 0x0400) != 0;
    result.vFlip = (entry & 0x0800) != 0;
    result.specialFunc = 0;
    result.specialCC = 0;
}

uint32 GetTilePixel(uint32 charAddr, uint32 x, uint32 y, uint8 colorMode) {
    // TODO: Get pixel from character data
    // Handle different color modes (4bpp, 8bpp, 16bpp)
    
    const uint32 pixelAddr = charAddr + y * 8 + x;
    
    switch (colorMode) {
        case 0: // 16 colors (4bpp)
        case 1: // 16 colors with palette
            return (ReadVRAM8(pixelAddr >> 1) >> ((pixelAddr & 1) ? 0 : 4)) & 0xF;
            
        case 2: // 64 colors
        case 3: // 128 colors
        case 4: // 256 colors
            return ReadVRAM8(pixelAddr);
            
        case 5: // RGB (16bpp)
            return ReadVRAM16(charAddr + (y * 8 + x) * 2);
            
        default:
            return 0;
    }
}

// =============================================================================
// Mosaic Helper
// =============================================================================

void ApplyMosaic(uint32 x, uint32 y, uint32 hSize, uint32 vSize,
                 uint32& mosaicX, uint32& mosaicY) {
    mosaicX = (x / hSize) * hSize;
    mosaicY = (y / vSize) * vSize;
}

} // namespace brimir::vdp2










