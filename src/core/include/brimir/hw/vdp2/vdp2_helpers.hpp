/**
 * @file vdp2_helpers.hpp
 * @brief VDP2 Helper Structures and Functions
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * Contains scroll, rotation, and window calculation helpers.
 * 
 * Copyright (C) 2015-2019 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#pragma once

#include <brimir/core/types.hpp>

namespace brimir::vdp2 {

// =============================================================================
// Scroll Helpers
// =============================================================================

/**
 * @brief Line scroll entry (from VRAM table)
 */
struct LineScrollEntry {
    sint16 xScroll;     // Horizontal scroll for this line
    sint16 yScroll;     // Vertical scroll (line zoom)
    uint16 xZoom;       // Horizontal zoom
    uint16 yZoom;       // Vertical zoom
};

/**
 * @brief Scroll accumulator state
 */
struct ScrollState {
    uint32 xAccum;      // X scroll accumulator (16.16 fixed)
    uint32 yAccum;      // Y scroll accumulator (16.16 fixed)
    uint16 xInc;        // X increment per pixel
    uint16 yInc;        // Y increment (for zoom)
    uint32 lineAddr;    // Current line scroll table address
};

/**
 * @brief Fetch line scroll entry from VRAM
 * @param addr VRAM address
 * @param entry Output entry
 */
void FetchLineScroll(uint32 addr, LineScrollEntry& entry);

/**
 * @brief Calculate scroll for current line
 * @param layerIndex Layer index (0-3 for NBG)
 * @param line Current scanline
 * @param state Output scroll state
 */
void CalculateScroll(uint32 layerIndex, uint32 line, ScrollState& state);

// =============================================================================
// Rotation Helpers
// =============================================================================

/**
 * @brief Calculate rotation coordinates for a screen pixel
 * @param params Rotation parameters
 * @param screenX Screen X coordinate
 * @param screenY Screen Y coordinate (line number)
 * @param vramX Output VRAM X coordinate (16.16 fixed)
 * @param vramY Output VRAM Y coordinate (16.16 fixed)
 */
void CalculateRotationCoord(const struct RotationParams& params,
                            sint32 screenX, sint32 screenY,
                            sint32& vramX, sint32& vramY);

/**
 * @brief Fetch coefficient from table
 * @param addr Table address
 * @param index Coefficient index
 * @param kx Output X coefficient
 * @param ky Output Y coefficient
 */
void FetchCoefficient(uint32 addr, uint32 index, sint32& kx, sint32& ky);

/**
 * @brief Update rotation accumulators for next line
 * @param params Rotation parameters to update
 */
void UpdateRotationAccum(struct RotationParams& params);

// =============================================================================
// Window Helpers
// =============================================================================

/**
 * @brief Window calculation result
 */
struct WindowResult {
    bool w0Inside;      // Pixel inside window 0
    bool w1Inside;      // Pixel inside window 1
    bool logicResult;   // Combined logic result
};

/**
 * @brief Calculate window state for current line
 * @param line Current scanline
 */
void CalculateWindowLine(uint32 line);

/**
 * @brief Evaluate window at pixel position
 * @param x X coordinate
 * @param layerMask Layer window control bits
 * @return True if pixel should be drawn
 */
bool EvaluateWindow(sint32 x, uint8 layerMask);

/**
 * @brief Fetch per-line window coordinates from VRAM
 * @param windowId Window identifier (0 or 1)
 * @param line Current scanline
 * @param xStart Output X start
 * @param xEnd Output X end
 */
void FetchLineWindow(uint8 windowId, uint32 line, uint16& xStart, uint16& xEnd);

// =============================================================================
// VRAM Access Helpers
// =============================================================================

/**
 * @brief Read 8-bit value from VRAM
 * @param addr VRAM address
 * @return Value
 */
uint8 ReadVRAM8(uint32 addr);

/**
 * @brief Read 16-bit value from VRAM
 * @param addr VRAM address (must be aligned)
 * @return Value
 */
uint16 ReadVRAM16(uint32 addr);

/**
 * @brief Read 32-bit value from VRAM (big-endian)
 * @param addr VRAM address (must be aligned)
 * @return Value
 */
uint32 ReadVRAM32(uint32 addr);

// =============================================================================
// Pattern Name / Character Helpers
// =============================================================================

/**
 * @brief Decoded pattern name entry
 */
struct PatternName {
    uint32 charAddr;        // Character address in VRAM
    uint16 paletteNum;      // Palette number
    bool hFlip;             // Horizontal flip
    bool vFlip;             // Vertical flip
    uint8 specialFunc;      // Special function code
    uint8 specialCC;        // Special color calculation
};

/**
 * @brief Decode pattern name table entry
 * @param entry Raw 16-bit entry
 * @param pncMode PNC mode register bits
 * @param charBase Character base address
 * @param result Output decoded entry
 */
void DecodePatternName(uint16 entry, uint8 pncMode, uint32 charBase, PatternName& result);

/**
 * @brief Get tile pixel color index
 * @param charAddr Character address
 * @param x X position within tile (0-7)
 * @param y Y position within tile (0-7)
 * @param colorMode Color mode (bpp)
 * @return Color index
 */
uint32 GetTilePixel(uint32 charAddr, uint32 x, uint32 y, uint8 colorMode);

// =============================================================================
// Mosaic Helper
// =============================================================================

/**
 * @brief Calculate mosaic-adjusted coordinates
 * @param x Input X coordinate
 * @param y Input Y coordinate
 * @param hSize Horizontal mosaic size (1-16)
 * @param vSize Vertical mosaic size (1-16)
 * @param mosaicX Output mosaic X
 * @param mosaicY Output mosaic Y
 */
void ApplyMosaic(uint32 x, uint32 y, uint32 hSize, uint32 vSize,
                 uint32& mosaicX, uint32& mosaicY);

// =============================================================================
// Fixed Point Helpers
// =============================================================================

/**
 * @brief Sign-extend an N-bit value to sint32
 * @param bits Number of bits
 * @param value Value to extend
 * @return Sign-extended value
 */
inline sint32 SignExtend(uint32 bits, uint32 value) {
    const uint32 signBit = 1u << (bits - 1);
    if (value & signBit) {
        return static_cast<sint32>(value | (~0u << bits));
    }
    return static_cast<sint32>(value);
}

/**
 * @brief Convert 16.16 fixed point to integer
 * @param value Fixed point value
 * @return Integer part
 */
inline sint32 FixedToInt(sint32 value) {
    return value >> 16;
}

/**
 * @brief Convert integer to 16.16 fixed point
 * @param value Integer value
 * @return Fixed point value
 */
inline sint32 IntToFixed(sint32 value) {
    return value << 16;
}

} // namespace brimir::vdp2










