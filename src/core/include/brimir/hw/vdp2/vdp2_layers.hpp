/**
 * @file vdp2_layers.hpp
 * @brief VDP2 Background Layer Rendering
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * Handles rendering of NBG0-3 and RBG0-1 background layers.
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
// Layer Parameters
// =============================================================================

/**
 * @brief Normal background layer parameters
 */
struct NBGParams {
    // Map/character addresses
    uint32 mapAddress;
    uint32 charAddress;
    
    // Scroll (16.16 fixed point for sub-pixel precision)
    uint32 scrollX;
    uint32 scrollY;
    
    // Zoom (16.16 fixed point, NBG0/1 only)
    uint32 zoomX;
    uint32 zoomY;
    
    // Character attributes
    uint8 colorMode;        // 0-5: palette modes, 6: RGB
    uint8 charSize;         // 0: 1x1, 1: 2x2
    uint8 planeSize;        // 0: 1x1, 1: 2x1, 2: 2x2
    bool bitmapMode;
    uint8 bitmapSize;       // Bitmap dimensions
    
    // Palette
    uint16 paletteBase;
    uint8 paletteOffset;
    
    // Priority and effects
    uint8 priority;
    bool ccEnable;
    uint8 ccRatio;
    bool colorOffsetEnable;
    uint8 colorOffsetSelect;
    
    // Special function
    uint8 specialFunc;
    uint8 specialColorCalc;
};

/**
 * @brief Rotation background layer parameters
 */
struct RBGParams {
    // Map/character addresses
    uint32 mapAddress;
    uint32 charAddress;
    uint32 coeffTableAddr;
    
    // Character attributes
    uint8 colorMode;
    uint8 charSize;
    uint8 planeSize;
    bool bitmapMode;
    
    // Rotation parameters (from RotationParams struct)
    const RotationParams* rotParams;
    
    // Coefficient table control
    bool useCoeffTable;
    uint8 coeffMode;
    
    // Over pattern (for out-of-bounds)
    uint16 overPattern;
    
    // Priority and effects
    uint8 priority;
    bool ccEnable;
    uint8 ccRatio;
    bool colorOffsetEnable;
    uint8 colorOffsetSelect;
};

/**
 * @brief Sprite layer parameters
 */
struct SpriteParams {
    // VDP1 framebuffer info
    const uint16* framebuffer;
    uint32 fbWidth;
    uint32 fbHeight;
    bool fb8bpp;
    bool fbRotate;
    
    // Type select (sprite format)
    uint8 typeSelect;
    
    // Priority levels
    uint8 priority[8];
    
    // Color calculation
    bool ccEnable[8];
    uint8 ccRatio[8];
    uint8 ccCondition;
    
    // Color offset
    bool colorOffsetEnable;
    uint8 colorOffsetSelect;
    
    // Window
    uint8 windowMode;
};

// =============================================================================
// Layer Line Buffer
// =============================================================================

/**
 * @brief Pixel data for compositing
 */
struct LayerPixel {
    uint32 color;           // RGB888 color
    uint8 priority;         // Layer priority (0-7)
    bool transparent;       // Pixel is transparent
    bool shadow;            // Shadow bit set
    bool ccEnable;          // Color calculation enabled
    uint8 ccRatio;          // Color calculation ratio
};

// Line buffers for each layer (indexed by Layer enum)
extern LayerPixel LineBuffer[8][MAX_WIDTH];

// =============================================================================
// Layer Rendering Functions
// =============================================================================

/**
 * @brief Draw normal background 0
 * @param line Scanline number
 */
void DrawNBG0(uint32 line);

/**
 * @brief Draw normal background 1
 * @param line Scanline number
 */
void DrawNBG1(uint32 line);

/**
 * @brief Draw normal background 2
 * @param line Scanline number
 */
void DrawNBG2(uint32 line);

/**
 * @brief Draw normal background 3
 * @param line Scanline number
 */
void DrawNBG3(uint32 line);

/**
 * @brief Draw rotation background 0
 * @param line Scanline number
 */
void DrawRBG0(uint32 line);

/**
 * @brief Draw rotation background 1
 * @param line Scanline number
 */
void DrawRBG1(uint32 line);

/**
 * @brief Draw sprite layer (VDP1 framebuffer)
 * @param line Scanline number
 */
void DrawSprite(uint32 line);

/**
 * @brief Draw back screen
 * @param line Scanline number
 */
void DrawBack(uint32 line);

/**
 * @brief Draw line color screen
 * @param line Scanline number
 */
void DrawLineColor(uint32 line);

// =============================================================================
// Template-based Layer Rendering
// =============================================================================

/**
 * @brief Render NBG line with specific parameters
 * @tparam colorMode Color mode (0-6)
 * @tparam bitmap True for bitmap mode
 * @tparam charSize Character size (0=1x1, 1=2x2)
 */
template<uint32 colorMode, bool bitmap, uint32 charSize>
void RenderNBGLine(uint32 line, uint32 layerIndex, const NBGParams& params);

/**
 * @brief Render RBG line with specific parameters
 * @tparam useCoeff True to use coefficient table
 * @tparam colorMode Color mode
 */
template<bool useCoeff, uint32 colorMode>
void RenderRBGLine(uint32 line, uint32 layerIndex, const RBGParams& params);

/**
 * @brief Render sprite layer pixel
 * @tparam cramMode CRAM mode
 * @tparam rotate True if framebuffer rotation enabled
 */
template<uint32 cramMode, bool rotate>
void RenderSpriteLine(uint32 line, const SpriteParams& params);

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * @brief Setup NBG parameters from registers
 * @param layerIndex Layer index (0-3)
 * @param params Output parameters
 */
void SetupNBGParams(uint32 layerIndex, NBGParams& params);

/**
 * @brief Setup RBG parameters from registers
 * @param layerIndex Layer index (0-1)
 * @param params Output parameters
 */
void SetupRBGParams(uint32 layerIndex, RBGParams& params);

/**
 * @brief Setup sprite parameters from registers
 * @param params Output parameters
 */
void SetupSpriteParams(SpriteParams& params);

/**
 * @brief Fetch rotation parameters from VRAM
 * @param tableAddr Table address in VRAM
 * @param params Output rotation parameters
 */
void FetchRotationParams(uint32 tableAddr, RotationParams& params);

/**
 * @brief Check if layer is enabled
 * @param layer Layer identifier
 * @return True if layer is enabled
 */
bool IsLayerEnabled(Layer layer);

} // namespace brimir::vdp2










