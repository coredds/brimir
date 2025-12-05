/**
 * @file vdp2_render.hpp
 * @brief VDP2 Layer Compositing and Final Output
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * Handles priority sorting, color calculation, and final display output.
 * 
 * Copyright (C) 2015-2019 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#pragma once

#include <brimir/core/types.hpp>
#include <brimir/hw/vdp2/vdp2.hpp>

namespace brimir::vdp2 {

// =============================================================================
// Color Calculation Modes
// =============================================================================

/**
 * @brief Color calculation modes
 */
enum class CCMode : uint8 {
    Replace = 0,        // Replace (no blending)
    Add = 1,            // Add with ratio
    AddMSB = 2,         // Add based on MSB
    Shadow = 3          // Shadow mode
};

/**
 * @brief Extended color calculation conditions
 */
enum class ExtCCCond : uint8 {
    Always = 0,         // Always apply CC
    Priority = 1,       // Based on sprite priority
    ColorCalcBit = 2,   // Based on CC bit
    Special = 3         // Special conditions
};

// =============================================================================
// SIMD-Optimized Color Cache
// =============================================================================

/**
 * @brief Invalidate the color cache (call when CRAM is modified)
 */
void InvalidateColorCache();

/**
 * @brief Update the color cache (call at start of frame)
 * Batch converts CRAM from RGB555 to RGB888 using SIMD
 */
void UpdateColorCache();

// =============================================================================
// Compositing Functions
// =============================================================================

/**
 * @brief Composite all layers for a scanline
 * @param line Scanline number
 */
void ComposeLine(uint32 line);

/**
 * @brief Composite with specific parameters
 * @tparam deinterlace True for deinterlace mode
 * @tparam transparentMesh True to handle VDP1 mesh transparency
 */
template<bool deinterlace, bool transparentMesh>
void ComposeLineImpl(uint32 line, bool altField);

/**
 * @brief Output composited line to framebuffer
 * @param line Scanline number
 * @param output Output buffer (RGB888)
 */
void OutputLine(uint32 line, uint32* output);

// =============================================================================
// Color Calculation Functions
// =============================================================================

/**
 * @brief Apply color calculation between two colors
 * @param topColor Top layer color (RGB888)
 * @param bottomColor Bottom layer color (RGB888)
 * @param ratio Blend ratio (0-31, 0=top only, 31=bottom only)
 * @param mode Color calculation mode
 * @return Blended color (RGB888)
 */
uint32 ApplyColorCalc(uint32 topColor, uint32 bottomColor, uint8 ratio, CCMode mode);

/**
 * @brief Apply shadow effect
 * @param color Input color (RGB888)
 * @return Shadowed color (RGB888)
 */
uint32 ApplyShadow(uint32 color);

/**
 * @brief Apply half-brightness shadow
 * @param color Input color (RGB888)
 * @return Half-brightness color
 */
uint32 ApplyHalfShadow(uint32 color);

/**
 * @brief Apply color offset
 * @param color Input color (RGB888)
 * @param r Red offset (-256 to 255)
 * @param g Green offset (-256 to 255)
 * @param b Blue offset (-256 to 255)
 * @return Offset color (clamped)
 */
uint32 ApplyColorOffset(uint32 color, sint16 r, sint16 g, sint16 b);

// =============================================================================
// CRAM Access Functions
// =============================================================================

/**
 * @brief Read color from CRAM (RGB555 mode)
 * @param index Color index (0-2047)
 * @return RGB888 color
 */
uint32 ReadCRAM_RGB555(uint16 index);

/**
 * @brief Read color from CRAM (RGB888 mode)
 * @param index Color index (0-1023)
 * @return RGB888 color
 */
uint32 ReadCRAM_RGB888(uint16 index);

/**
 * @brief Read color based on current CRAM mode
 * @param index Color index
 * @return RGB888 color
 */
uint32 ReadCRAM(uint16 index);

/**
 * @brief Convert RGB555 to RGB888
 * @param color555 RGB555 color (Saturn format: bits 10-14=R, 5-9=G, 0-4=B)
 * @return RGB888 color
 */
inline uint32 RGB555toRGB888(uint16 color555) {
    // Saturn VDP2 uses: bits 10-14=Red, 5-9=Green, 0-4=Blue
    uint32 b = (color555 & 0x001F) << 3;
    uint32 g = (color555 & 0x03E0) >> 2;
    uint32 r = (color555 & 0x7C00) >> 7;
    // Expand 5-bit to 8-bit by replicating top bits
    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return (r << 16) | (g << 8) | b;
}

/**
 * @brief Convert RGB888 to RGB555
 * @param color888 RGB888 color
 * @return RGB555 color (Saturn format: bits 10-14=R, 5-9=G, 0-4=B)
 */
inline uint16 RGB888toRGB555(uint32 color888) {
    // Saturn VDP2 uses: bits 10-14=Red, 5-9=Green, 0-4=Blue
    uint16 r = (color888 >> 19) & 0x1F;
    uint16 g = (color888 >> 11) & 0x1F;
    uint16 b = (color888 >> 3) & 0x1F;
    return b | (g << 5) | (r << 10);
}

// =============================================================================
// Priority Functions
// =============================================================================

/**
 * @brief Priority comparison result
 */
struct PriorityResult {
    Layer topLayer;         // Highest priority opaque layer
    Layer secondLayer;      // Second highest (for CC)
    uint8 topPriority;      // Priority value of top layer
    bool applyCC;           // Should apply color calculation
};

/**
 * @brief Determine layer priority at pixel
 * @param x X coordinate
 * @param result Output priority result
 */
void DeterminePriority(uint32 x, PriorityResult& result);

/**
 * @brief Check if color calculation should be applied
 * @param topLayer Top layer
 * @param secondLayer Second layer
 * @param ccctl Color calculation control register
 * @return True if CC should be applied
 */
bool ShouldApplyCC(Layer topLayer, Layer secondLayer, uint16 ccctl);

// =============================================================================
// Special Effects
// =============================================================================

/**
 * @brief Process sprite pixel MSB effects
 * @param pixel Raw sprite pixel value
 * @param shadow Output: true if shadow bit set
 * @param ccEnable Output: true if CC enable bit set
 * @return Color index/value
 */
uint16 ProcessSpriteMSB(uint16 pixel, bool& shadow, bool& ccEnable);

/**
 * @brief Apply special function to color
 * @param color Input color
 * @param funcCode Special function code
 * @param ccCode Special CC code
 * @return Modified color
 */
uint32 ApplySpecialFunc(uint32 color, uint8 funcCode, uint8 ccCode);

// =============================================================================
// Display Output
// =============================================================================

/**
 * @brief Frame output buffer
 */
extern uint32* OutputFramebuffer;
extern uint32 OutputPitch;

/**
 * @brief Set output framebuffer
 * @param buffer Pointer to output buffer
 * @param pitch Pitch in pixels
 */
void SetOutputBuffer(uint32* buffer, uint32 pitch);

/**
 * @brief Get current output line pointer
 * @param line Scanline number
 * @return Pointer to line start in output buffer
 */
uint32* GetOutputLine(uint32 line);

} // namespace brimir::vdp2


