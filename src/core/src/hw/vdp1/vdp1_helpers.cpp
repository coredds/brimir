/**
 * @file vdp1_helpers.cpp
 * @brief VDP1 Helper Structures Implementation
 * 
 * Based on Mednafen's VDP1 implementation with adaptations for Brimir.
 * 
 * Copyright (C) 2015-2017 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp1/vdp1_helpers.hpp>

namespace brimir::vdp1 {

// =============================================================================
// Gouraud Shading Lookup Table
// =============================================================================

/**
 * @brief Lookup table for Gouraud shading color saturation
 * 
 * Maps the sum of two 5-bit color components to a saturated 5-bit result.
 * Input range: 0-63 (sum of two 0-31 values)
 * Output range: 0-31 (saturated to 5-bit)
 */
uint8 gouraud_lut[0x40];

/**
 * @brief Initialize Gouraud shading lookup table
 * 
 * Called during VDP1 initialization to populate the saturation LUT.
 */
void InitGouraudLUT()
{
    for(unsigned i = 0; i < 0x40; i++)
    {
        gouraud_lut[i] = (i < 0x20) ? i : 0x1F;  // Saturate at 0x1F (max 5-bit value)
    }
}

// =============================================================================
// Sprite Width Shift Table
// =============================================================================

/**
 * @brief Sprite width to shift amount lookup table
 * 
 * Maps sprite width code (0-7) to log2 shift amount for address calculation.
 * 
 * Width codes:
 *   0 = 8 pixels   (shift 3)
 *   1 = 16 pixels  (shift 4)
 *   2 = 32 pixels  (shift 5)
 *   3 = 64 pixels  (shift 6)
 *   4 = 128 pixels (shift 7)
 *   5 = 256 pixels (shift 8)
 *   6 = 512 pixels (shift 9)
 *   7 = 512 pixels (shift 9)
 */
uint8 spr_w_shift_tab[8] = {
    3,  // 8 pixels
    4,  // 16 pixels
    5,  // 32 pixels
    6,  // 64 pixels
    7,  // 128 pixels
    8,  // 256 pixels
    9,  // 512 pixels
    9   // 512 pixels (duplicate)
};

} // namespace brimir::vdp1













