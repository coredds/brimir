/**
 * @file vdp2_window.cpp
 * @brief VDP2 Window Calculations
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * 
 * Copyright (C) 2015-2019 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp2/vdp2.hpp>
#include <brimir/hw/vdp2/vdp2_helpers.hpp>

namespace brimir::vdp2 {

// =============================================================================
// Window State Management
// =============================================================================

/**
 * @brief Update window boundaries for current scanline
 */
void UpdateWindowLine(uint32 line) {
    // Window 0
    if (Windows[0].lineWinEnable) {
        // Fetch from VRAM
        FetchLineWindow(0, line, Windows[0].xStart, Windows[0].xEnd);
    }
    
    // Window 1
    if (Windows[1].lineWinEnable) {
        FetchLineWindow(1, line, Windows[1].xStart, Windows[1].xEnd);
    }
    
    // Update Y boundary flags
    for (int w = 0; w < 2; w++) {
        if (line == Windows[w].yStart) {
            Windows[w].yIn = true;
        }
        if (line == Windows[w].yEnd) {
            Windows[w].yEndMet = true;
        }
        if (Windows[w].yEndMet && line > Windows[w].yEnd) {
            Windows[w].yIn = false;
        }
    }
}

/**
 * @brief Check if pixel is inside window 0
 */
static bool InsideWindow0(sint32 x) {
    if (!Windows[0].yIn) return false;
    return x >= Windows[0].xStart && x <= Windows[0].xEnd;
}

/**
 * @brief Check if pixel is inside window 1
 */
static bool InsideWindow1(sint32 x) {
    if (!Windows[1].yIn) return false;
    return x >= Windows[1].xStart && x <= Windows[1].xEnd;
}

// =============================================================================
// Window Evaluation
// =============================================================================

/**
 * Window control bits (per layer):
 *   Bit 0: Window 0 enable
 *   Bit 1: Window 0 area (0=inside, 1=outside)
 *   Bit 2: Window 1 enable
 *   Bit 3: Window 1 area (0=inside, 1=outside)
 *   Bit 4: Logic (0=OR, 1=AND)
 *   Bit 5: Sprite window enable
 */

bool EvaluateWindowForLayer(sint32 x, uint8 winControl) {
    const bool w0Enable = (winControl & 0x01) != 0;
    const bool w0Area = (winControl & 0x02) != 0;  // 0=inside, 1=outside
    const bool w1Enable = (winControl & 0x04) != 0;
    const bool w1Area = (winControl & 0x08) != 0;
    const bool useAND = (winControl & 0x10) != 0;
    
    // No windows enabled - always draw
    if (!w0Enable && !w1Enable) {
        return true;
    }
    
    bool w0Result = true;
    bool w1Result = true;
    
    // Evaluate Window 0
    if (w0Enable) {
        bool inside = InsideWindow0(x);
        w0Result = w0Area ? !inside : inside;  // Invert if "outside" mode
    }
    
    // Evaluate Window 1
    if (w1Enable) {
        bool inside = InsideWindow1(x);
        w1Result = w1Area ? !inside : inside;
    }
    
    // Combine results
    if (!w0Enable) return w1Result;
    if (!w1Enable) return w0Result;
    
    return useAND ? (w0Result && w1Result) : (w0Result || w1Result);
}

/**
 * @brief Main window evaluation function
 */
bool EvaluateWindowAtPixel(sint32 x, Layer layer) {
    uint8 winControl = 0;
    
    switch (layer) {
        case Layer::NBG0:   winControl = Regs.winControl[0]; break;
        case Layer::NBG1:   winControl = Regs.winControl[1]; break;
        case Layer::NBG2:   winControl = Regs.winControl[2]; break;
        case Layer::NBG3:   winControl = Regs.winControl[3]; break;
        case Layer::RBG0:   winControl = Regs.winControl[4]; break;
        case Layer::Sprite: winControl = Regs.winControl[5]; break;
        default:            return true;  // No window control
    }
    
    return EvaluateWindowForLayer(x, winControl);
}

// =============================================================================
// Sprite Window
// =============================================================================

/**
 * @brief Evaluate sprite window at pixel
 * The sprite window uses VDP1 framebuffer MSB as window data
 */
bool EvaluateSpriteWindow(sint32 x, sint32 y, const uint16* spriteFB, uint32 fbWidth) {
    // TODO: Implement sprite window
    // Read MSB from VDP1 framebuffer at (x, y)
    // Use as window control
    return true;
}

} // namespace brimir::vdp2










