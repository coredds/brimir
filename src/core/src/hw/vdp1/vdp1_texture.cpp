/**
 * @file vdp1_texture.cpp
 * @brief VDP1 Texture Fetch Functions
 * 
 * Based on Mednafen's VDP1 implementation with adaptations for Brimir.
 * 
 * Implements 32 texture fetch function variants for all combinations of:
 * - Color modes (4/6/7/8-bit indexed, RGB)
 * - End code disable (ECD)
 * - Skip pixel disable (SPD)
 * 
 * Copyright (C) 2015-2017 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp1/vdp1_render.hpp>
#include <algorithm>

namespace brimir::vdp1 {

// =============================================================================
// Texture Fetch Template
// =============================================================================

/**
 * @brief Template-based texture fetch function
 * 
 * @tparam ECDSPDMode Encoded mode bits:
 *   - Bit 4 (0x10): ECD - End Code Disable
 *   - Bit 3 (0x08): SPD - Skip Pixel Disable (force opaque)
 *   - Bits 0-2 (0x07): Color Mode (0-7)
 * 
 * @param x X coordinate in texture space
 * @return Texture pixel (RGB555) with MSB=0 for transparent, MSB=1 for opaque
 *         Returns 0x80000000 (-1 as uint32) for end code (transparent)
 */
template<unsigned ECDSPDMode>
static uint32 TexFetch(uint32 x)
{
    const uint32 base = LineSetup.tex_base;
    const bool ECD = ECDSPDMode & 0x10;  // End Code Disable
    const bool SPD = ECDSPDMode & 0x08;  // Skip Pixel Disable
    const unsigned ColorMode = ECDSPDMode & 0x07;
    
    uint32 rtd;       // Raw texture data
    uint32 ret_or = 0;  // Value to OR with result (for MSB control)
    
    switch(ColorMode)
    {
        case 0:  // 4-bit color bank (16 colors)
        {
            // 4 pixels per word, extract nibble
            rtd = (VRAM[(base + (x >> 2)) & 0x3FFFF] >> (((x & 0x3) ^ 0x3) << 2)) & 0xF;
            
            // Check for end code (0xF = transparent)
            if(!ECD && rtd == 0xF)
            {
                LineSetup.ec_count--;
                return 0x80000000;  // Transparent
            }
            
            // Apply color bank
            ret_or = LineSetup.cb_or;
            
            // SPD: if pixel is 0, set MSB to make transparent
            if(!SPD)
                ret_or |= (sint32)(rtd - 1) >> 31;
            
            return rtd | ret_or;
        }
        
        case 1:  // 4-bit LUT (16 colors with lookup table)
        {
            // 4 pixels per word, extract nibble
            rtd = (VRAM[(base + (x >> 2)) & 0x3FFFF] >> (((x & 0x3) ^ 0x3) << 2)) & 0xF;
            
            // Check for end code (0xF = transparent)
            if(!ECD && rtd == 0xF)
            {
                LineSetup.ec_count--;
                return 0x80000000;  // Transparent
            }
            
            // SPD: if pixel is 0, set MSB to make transparent
            if(!SPD)
                ret_or |= (sint32)(rtd - 1) >> 31;
            
            // Lookup in CLUT
            return LineSetup.CLUT[rtd] | ret_or;
        }
        
        case 2:  // 6-bit color bank (64 colors)
        {
            // 2 pixels per word, extract byte
            rtd = (VRAM[(base + (x >> 1)) & 0x3FFFF] >> (((x & 0x1) ^ 0x1) << 3)) & 0xFF;
            
            // Check for end code (0xFF = transparent)
            if(!ECD && rtd == 0xFF)
            {
                LineSetup.ec_count--;
                return 0x80000000;  // Transparent
            }
            
            // Apply color bank
            ret_or = LineSetup.cb_or;
            
            // SPD: if pixel is 0, set MSB to make transparent
            if(!SPD)
                ret_or |= (sint32)(rtd - 1) >> 31;
            
            // Mask to 6 bits
            return (rtd & 0x3F) | ret_or;
        }
        
        case 3:  // 7-bit color bank (128 colors)
        {
            // 2 pixels per word, extract byte
            rtd = (VRAM[(base + (x >> 1)) & 0x3FFFF] >> (((x & 0x1) ^ 0x1) << 3)) & 0xFF;
            
            // Check for end code (0xFF = transparent)
            if(!ECD && rtd == 0xFF)
            {
                LineSetup.ec_count--;
                return 0x80000000;  // Transparent
            }
            
            // Apply color bank
            ret_or = LineSetup.cb_or;
            
            // SPD: if pixel is 0, set MSB to make transparent
            if(!SPD)
                ret_or |= (sint32)(rtd - 1) >> 31;
            
            // Mask to 7 bits
            return (rtd & 0x7F) | ret_or;
        }
        
        case 4:  // 8-bit color bank (256 colors)
        {
            // 2 pixels per word, extract byte
            rtd = (VRAM[(base + (x >> 1)) & 0x3FFFF] >> (((x & 0x1) ^ 0x1) << 3)) & 0xFF;
            
            // Check for end code (0xFF = transparent)
            if(!ECD && rtd == 0xFF)
            {
                LineSetup.ec_count--;
                return 0x80000000;  // Transparent
            }
            
            // Apply color bank
            ret_or = LineSetup.cb_or;
            
            // SPD: if pixel is 0, set MSB to make transparent
            if(!SPD)
                ret_or |= (sint32)(rtd - 1) >> 31;
            
            return rtd | ret_or;
        }
        
        case 5:  // RGB 16-bit (32K colors)
        case 6:  // Reserved
        case 7:  // Reserved
        {
            // Mode 6-7: invalid, return first VRAM word
            if(ColorMode >= 6)
                rtd = VRAM[0];
            else
                rtd = VRAM[(base + x) & 0x3FFFF];
            
            // Check for end code (bits 15-14 = 01 = 0x4000)
            if(!ECD && (rtd & 0xC000) == 0x4000)
            {
                LineSetup.ec_count--;
                return 0x80000000;  // Transparent
            }
            
            // SPD: if pixel has 01 in upper bits, set MSB to make transparent
            if(!SPD)
                ret_or |= (sint32)(rtd - 0x4000) >> 31;
            
            return rtd | ret_or;
        }
    }
    
    return 0x80000000;  // Should never reach here
}

// =============================================================================
// Texture Fetch Function Table
// =============================================================================

/**
 * @brief Function pointer table for all texture fetch variants
 * 
 * Index encoding (5 bits):
 *   Bit 4: ECD (End Code Disable)
 *   Bit 3: SPD (Skip Pixel Disable)
 *   Bits 2-0: Color Mode (0-7)
 * 
 * Total: 32 function variants
 */
uint32 (*const TexFetchTab[0x20])(uint32 x) = {
    #define TF(a) (TexFetch<a>)
    
    // ECD=0, SPD=0 (normal transparency)
    TF(0x00), TF(0x01), TF(0x02), TF(0x03),
    TF(0x04), TF(0x05), TF(0x06), TF(0x07),
    
    // ECD=0, SPD=1 (forced opaque)
    TF(0x08), TF(0x09), TF(0x0A), TF(0x0B),
    TF(0x0C), TF(0x0D), TF(0x0E), TF(0x0F),
    
    // ECD=1, SPD=0 (no end code check)
    TF(0x10), TF(0x11), TF(0x12), TF(0x13),
    TF(0x14), TF(0x15), TF(0x16), TF(0x17),
    
    // ECD=1, SPD=1 (no end code + forced opaque)
    TF(0x18), TF(0x19), TF(0x1A), TF(0x1B),
    TF(0x1C), TF(0x1D), TF(0x1E), TF(0x1F),
    
    #undef TF
};

} // namespace brimir::vdp1











