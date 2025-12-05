/**
 * @file vdp2_render.cpp
 * @brief VDP2 Layer Compositing and Output
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * 
 * Copyright (C) 2015-2019 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp2/vdp2_render.hpp>
#include <brimir/hw/vdp2/vdp2_layers.hpp>
#include <brimir/hw/simd_utils.hpp>
#include <brimir/util/dev_log.hpp>

#include <algorithm>
#include <cstring>

namespace brimir::vdp2 {

namespace grp {
    struct vdp2_render {
        static constexpr bool enabled = true;
        static constexpr devlog::Level level = devlog::level::debug;
        static constexpr std::string_view name = "VDP2-Render";
    };
}

// =============================================================================
// Output Buffer
// =============================================================================

uint32* OutputFramebuffer = nullptr;
uint32 OutputPitch = 0;

// =============================================================================
// SIMD-Optimized Color Cache
// =============================================================================

// Pre-converted CRAM colors (RGB555 -> RGB888)
// Filled at start of frame to avoid per-pixel conversion
static uint32 ColorCache[2048] alignas(16);
static bool ColorCacheValid = false;

void InvalidateColorCache() {
    ColorCacheValid = false;
}

void UpdateColorCache() {
    if (ColorCacheValid) return;
    
    // Batch convert all CRAM entries to RGB888
#if BRIMIR_HAS_SSE2
    // Process 4 colors at a time using SSE2
    for (uint32 i = 0; i < 2048; i += 4) {
        simd::rgb555_to_rgb888_sse2_4px(&CRAM[i], &ColorCache[i]);
    }
#else
    // Scalar fallback
    for (uint32 i = 0; i < 2048; i++) {
        ColorCache[i] = RGB555toRGB888(CRAM[i]);
    }
#endif
    
    ColorCacheValid = true;
}

// Fast CRAM lookup using precomputed cache
inline uint32 ReadCRAMCached(uint16 index) {
    return ColorCache[index & 0x7FF];
}

void SetOutputBuffer(uint32* buffer, uint32 pitch) {
    OutputFramebuffer = buffer;
    OutputPitch = pitch;
}

uint32* GetOutputLine(uint32 line) {
    if (!OutputFramebuffer) return nullptr;
    
    // For interlaced modes, interleave output lines based on current field
    // Field 0 (even): lines 0, 2, 4, 6, ...
    // Field 1 (odd): lines 1, 3, 5, 7, ...
    uint32 outLine = line;
    if (InterlaceDouble) {
        outLine = (line << 1) | (CurrentField ? 1 : 0);
    }
    
    return OutputFramebuffer + outLine * OutputPitch;
}

// =============================================================================
// CRAM Access
// =============================================================================

uint32 ReadCRAM_RGB555(uint16 index) {
    // Note: CRAM stores colors in Saturn VDP2 format (bits 10-14=R, 5-9=G, 0-4=B)
    // RGB555toRGB888 handles the conversion correctly
    const uint16 color555 = CRAM[index & 0x7FF];
    return RGB555toRGB888(color555);
}

uint32 ReadCRAM_RGB888(uint16 index) {
    // RGB888 mode uses two CRAM entries per color
    const uint32 cramIndex = (index & 0x3FF) << 1;
    const uint16 hi = CRAM[cramIndex];
    const uint16 lo = CRAM[cramIndex + 1];
    
    // Format: 00BBBBBB BBRRRRRR RRGGGGGG GG------
    const uint32 r = (lo >> 2) & 0xFF;
    const uint32 g = ((lo << 6) | (hi >> 10)) & 0xFF;
    const uint32 b = (hi >> 2) & 0xFF;
    
    return (r << 16) | (g << 8) | b;
}

uint32 ReadCRAM(uint16 index) {
    switch (Regs.cramMode) {
        case CRAMMode::RGB555_1024:
            // Use SIMD-optimized cache for RGB555 modes
            return ReadCRAMCached(index & 0x3FF);
        case CRAMMode::RGB555_2048:
            return ReadCRAMCached(index & 0x7FF);
        case CRAMMode::RGB888_1024:
            return ReadCRAM_RGB888(index & 0x3FF);
        default:
            return 0;
    }
}

// =============================================================================
// Color Calculation
// =============================================================================

uint32 ApplyColorCalc(uint32 topColor, uint32 bottomColor, uint8 ratio, CCMode mode) {
    if (mode == CCMode::Replace || ratio == 0) {
        return topColor;
    }
    
    // Extract RGB components
    const uint32 topR = (topColor >> 16) & 0xFF;
    const uint32 topG = (topColor >> 8) & 0xFF;
    const uint32 topB = topColor & 0xFF;
    
    const uint32 botR = (bottomColor >> 16) & 0xFF;
    const uint32 botG = (bottomColor >> 8) & 0xFF;
    const uint32 botB = bottomColor & 0xFF;
    
    uint32 r, g, b;
    
    switch (mode) {
        case CCMode::Add:
            // Blend based on ratio (0-31)
            r = (topR * (32 - ratio) + botR * ratio) >> 5;
            g = (topG * (32 - ratio) + botG * ratio) >> 5;
            b = (topB * (32 - ratio) + botB * ratio) >> 5;
            break;
            
        case CCMode::Shadow:
            // Darken bottom color
            r = botR >> 1;
            g = botG >> 1;
            b = botB >> 1;
            break;
            
        default:
            return topColor;
    }
    
    // Clamp to 0-255
    r = std::min(r, 255u);
    g = std::min(g, 255u);
    b = std::min(b, 255u);
    
    return (r << 16) | (g << 8) | b;
}

uint32 ApplyShadow(uint32 color) {
    const uint32 r = ((color >> 16) & 0xFF) >> 1;
    const uint32 g = ((color >> 8) & 0xFF) >> 1;
    const uint32 b = (color & 0xFF) >> 1;
    return (r << 16) | (g << 8) | b;
}

uint32 ApplyHalfShadow(uint32 color) {
    // Quarter brightness
    const uint32 r = ((color >> 16) & 0xFF) >> 2;
    const uint32 g = ((color >> 8) & 0xFF) >> 2;
    const uint32 b = (color & 0xFF) >> 2;
    return (r << 16) | (g << 8) | b;
}

uint32 ApplyColorOffset(uint32 color, sint16 r_off, sint16 g_off, sint16 b_off) {
    sint32 r = ((color >> 16) & 0xFF) + r_off;
    sint32 g = ((color >> 8) & 0xFF) + g_off;
    sint32 b = (color & 0xFF) + b_off;
    
    // Clamp
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    
    return (r << 16) | (g << 8) | b;
}

// =============================================================================
// Priority Determination - Kept for external use
// =============================================================================

void DeterminePriority(uint32 x, PriorityResult& result) {
    result.topLayer = Layer::Back;
    result.secondLayer = Layer::Back;
    result.topPriority = 0;
    result.applyCC = false;
    
    uint8 highestPrio = 0;
    Layer highestLayer = Layer::Back;
    
    // Layer order: RBG1(5), RBG0(4), NBG3(3), NBG2(2), NBG1(1), NBG0(0), Sprite(6)
    static constexpr uint8 layerIndices[] = {5, 4, 3, 2, 1, 0, 6};
    
    for (uint8 layerIdx : layerIndices) {
        const LayerPixel& pixel = LineBuffer[layerIdx][x];
        if (!pixel.transparent && pixel.priority >= highestPrio) {
            result.secondLayer = highestLayer;
            highestPrio = pixel.priority;
            highestLayer = static_cast<Layer>(layerIdx);
        }
    }
    
    result.topLayer = highestLayer;
    result.topPriority = highestPrio;
}

bool ShouldApplyCC(Layer topLayer, Layer secondLayer, uint16 ccctl) {
    return false;  // TODO: Implement CC enable logic
}

// =============================================================================
// Compositing - Optimized with inlined priority determination
// =============================================================================

void ComposeLine(uint32 line) {
    devlog::trace<grp::vdp2_render>("Compositing line {}", line);
    
    uint32* output = GetOutputLine(line);
    if (!output) return;
    
    // Layer indices for priority check (hoisted outside loop)
    static constexpr uint8 layerIndices[] = {5, 4, 3, 2, 1, 0, 6};
    
    for (uint32 x = 0; x < HRes; x++) {
        // Inlined priority determination for speed
        uint8 highestPrio = 0;
        uint8 topIdx = 7;  // Default to back layer
        
        // Unrolled loop for common case (7 layers)
        // Check each layer's pixel at position x
        for (uint8 layerIdx : layerIndices) {
            const LayerPixel& pixel = LineBuffer[layerIdx][x];
            if (!pixel.transparent && pixel.priority >= highestPrio) {
                highestPrio = pixel.priority;
                topIdx = layerIdx;
            }
        }
        
        // Get color from winning layer
        const LayerPixel& topPixel = LineBuffer[topIdx][x];
        uint32 color = topPixel.color;
        
        // Apply shadow if needed (rare case)
        if (topPixel.shadow) {
            color = ApplyShadow(color);
        }
        
        output[x] = color;
    }
}

void OutputLine(uint32 line, uint32* output) {
    // Direct output without compositing (for testing)
    for (uint32 x = 0; x < HRes; x++) {
        // Just copy back screen color
        output[x] = LineBuffer[7][x].color;
    }
}

// =============================================================================
// Special Effects
// =============================================================================

uint16 ProcessSpriteMSB(uint16 pixel, bool& shadow, bool& ccEnable) {
    // MSB bit 15 controls special functions
    shadow = (pixel & 0x8000) != 0;
    ccEnable = false;  // TODO: Depends on SPCTL register
    
    return pixel & 0x7FFF;
}

uint32 ApplySpecialFunc(uint32 color, uint8 funcCode, uint8 ccCode) {
    // TODO: Implement special function processing
    return color;
}

} // namespace brimir::vdp2


