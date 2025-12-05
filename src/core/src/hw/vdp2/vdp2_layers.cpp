/**
 * @file vdp2_layers.cpp
 * @brief VDP2 Background Layer Rendering
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * 
 * Copyright (C) 2015-2019 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp2/vdp2_layers.hpp>
#include <brimir/hw/vdp2/vdp2_helpers.hpp>
#include <brimir/hw/vdp2/vdp2_render.hpp>
#include <brimir/util/dev_log.hpp>

namespace brimir::vdp2 {

namespace grp {
    struct vdp2_layer {
        static constexpr bool enabled = true;
        static constexpr devlog::Level level = devlog::level::debug;
        static constexpr std::string_view name = "VDP2-Layer";
    };
}

// =============================================================================
// Line Buffer Definition
// =============================================================================

LayerPixel LineBuffer[8][MAX_WIDTH];

// =============================================================================
// Interlace Helper - Calculate effective Y coordinate for current field
// =============================================================================

// For double-density interlace mode (Mednafen-compatible):
// - Y coordinates increment by 2 per scanline (showing every other line)
// - Odd field starts at Y+1 (showing the alternate lines)
// This means field 0 renders lines 0, 2, 4, 6... and field 1 renders 1, 3, 5, 7...
static inline uint32 GetEffectiveYLine(uint32 line) {
    if (InterlaceDouble) {
        return (line << 1) + (CurrentField ? 1 : 0);
    }
    return line;
}

// =============================================================================
// Layer Enable Check
// =============================================================================

bool IsLayerEnabled(Layer layer) {
    switch (layer) {
        case Layer::NBG0:   return (Regs.bgon & 0x0001) != 0;
        case Layer::NBG1:   return (Regs.bgon & 0x0002) != 0;
        case Layer::NBG2:   return (Regs.bgon & 0x0004) != 0;
        case Layer::NBG3:   return (Regs.bgon & 0x0008) != 0;
        case Layer::RBG0:   return (Regs.bgon & 0x0010) != 0;
        case Layer::RBG1:   return (Regs.bgon & 0x0020) != 0;
        case Layer::Sprite: return true;  // Always enabled if VDP1 running
        case Layer::Back:   return true;  // Always enabled
        default:            return false;
    }
}

// =============================================================================
// Internal helpers for raw register access
// =============================================================================

// Raw register storage for direct access (mirrors test format)
// These are set via the adapter - for now we read from Regs struct
static uint16 GetRawReg(uint32 offset) {
    // This would ideally access raw registers, but for now use parsed Regs
    return 0;
}

// CRAM access (Saturn format: bits 10-14=R, 5-9=G, 0-4=B)
static uint32 ReadCRAM_RGB555(uint16 index) {
    uint16 c = CRAM[index & 0x7FF];
    // Saturn VDP2 RGB555: bits 10-14=Red, 5-9=Green, 0-4=Blue
    uint32 b = ((c & 0x001F) << 3) | ((c & 0x001F) >> 2);
    uint32 g = ((c & 0x03E0) >> 2) | ((c & 0x03E0) >> 7);
    uint32 r = ((c & 0x7C00) >> 7) | ((c & 0x7C00) >> 12);
    return (r << 16) | (g << 8) | b;
}

static uint32 ReadCRAMColor(uint16 index) {
    // Use proper CRAM mode handling
    return ReadCRAM(index);
}

// Get 4bpp tile pixel
static uint8 GetTilePixel4BPP(uint32 charAddr, uint32 pixelOffset) {
    uint32 byteAddr = charAddr + pixelOffset / 2;
    uint8 byte = ReadVRAM8(byteAddr);
    return (pixelOffset & 1) ? (byte & 0x0F) : (byte >> 4);
}

// =============================================================================
// Normal Background Rendering - Optimized with Tile Batching
// =============================================================================

static void DrawNBGInternal(uint32 line, uint32 layer) {
    // Initialize layer to transparent
    for (uint32 x = 0; x < HRes; x++) {
        LineBuffer[layer][x].transparent = true;
    }
    
    if (!IsLayerEnabled(static_cast<Layer>(layer))) {
        return;
    }
    
    // =========================================================================
    // HOISTED INVARIANTS - Computed once per line, not per pixel
    // =========================================================================
    
    const uint16 chctl = (layer < 2) ? Regs.chctla : Regs.chctlb;
    
    // Check if bitmap mode is enabled (CHCTL bit 1 for NBG0, bit 5 for NBG1)
    const uint8 bitmapBit = (layer < 2) ? (1 + layer * 4) : 0;
    const bool isBitmapMode = (layer < 2) && ((chctl >> bitmapBit) & 0x01);
    
    // Scroll values with proper precision
    sint32 scrollX, scrollY;
    if (layer < 2) {
        scrollX = ((Regs.scrollX[layer] & 0x7FF) << 8) | (Regs.scrollXD[layer] >> 8);
        scrollY = ((Regs.scrollY[layer] & 0x7FF) << 8) | (Regs.scrollYD[layer] >> 8);
    } else {
        scrollX = (Regs.scrollX[layer] & 0x7FF) << 8;
        scrollY = (Regs.scrollY[layer] & 0x7FF) << 8;
    }
    
    const sint32 scrollXInt = scrollX >> 8;
    const sint32 scrollYInt = scrollY >> 8;
    const uint8 priority = Regs.nbgPrio[layer];
    
    // Map offset calculation
    uint32 mapOffset;
    switch (layer) {
        case 0:  mapOffset = (Regs.mpofn & 0x07) << 16; break;
        case 1:  mapOffset = ((Regs.mpofn >> 4) & 0x07) << 16; break;
        case 2:  mapOffset = ((Regs.mpofn >> 8) & 0x07) << 16; break;
        default: mapOffset = ((Regs.mpofn >> 12) & 0x07) << 16; break;
    }
    
    // ===== BITMAP MODE =====
    if (isBitmapMode) {
        const uint32 effectiveLine = GetEffectiveYLine(line);
        const sint32 pixelY = (scrollYInt + static_cast<sint32>(effectiveLine)) & 0x1FF;
        const uint16 bmpPalette = (layer == 0) ? (Regs.bmpna & 0x07) : ((Regs.bmpna >> 8) & 0x07);
        // Character base from CHCTL bit 10, not MPOFN
        const uint32 charBase = (chctl & 0x0400) ? 0x20000 : 0x00000;
        
        for (uint32 x = 0; x < HRes; x++) {
            const sint32 pixelX = (scrollXInt + static_cast<sint32>(x)) & 0x1FF;
            
            // Bitmap is linear: charBase + y * width + x (8bpp = 1 byte per pixel)
            const uint32 bitmapAddr = charBase + (pixelY * 512 + pixelX);
            const uint8 colorIdx = ReadVRAM8(bitmapAddr);
            
            if (colorIdx == 0) continue;
            
            LineBuffer[layer][x].color = ReadCRAM(bmpPalette * 256 + colorIdx);
            LineBuffer[layer][x].transparent = false;
            LineBuffer[layer][x].priority = priority;
            LineBuffer[layer][x].shadow = false;
            LineBuffer[layer][x].ccEnable = false;
        }
        return;  // Done with bitmap mode
    }
    
    // ===== TILE MODE =====
    // Character size (constant for entire layer)
    const uint8 charSizeBit = (layer < 2) 
        ? (chctl >> (2 + layer * 4)) & 0x01
        : (chctl >> (2 + (layer - 2) * 4)) & 0x01;
    const uint32 tileSize = charSizeBit ? 16 : 8;
    const uint32 tileMask = tileSize - 1;
    const uint32 charBytes = tileSize * tileSize / 2;
    
    // Map plane address
    const uint32 mapA = Regs.mpabn[layer] & 0x3F;
    const uint32 mapAddrBase = mapOffset + (mapA << 11);
    
    // Y position calculations (constant for entire line)
    // Use effective line for interlaced modes
    const uint32 effectiveLine = GetEffectiveYLine(line);
    const sint32 pixelY = (scrollYInt + static_cast<sint32>(effectiveLine)) & 0x1FF;
    const uint32 tileY = static_cast<uint32>(pixelY) / tileSize;
    uint32 subY = static_cast<uint32>(pixelY) & tileMask;  // Mutable! Matches harness bug
    const uint32 mapRowOffset = mapAddrBase + tileY * 64 * 2;
    
    // =========================================================================
    // TILE-BY-TILE RENDERING - Mednafen NBG2/3 approach
    // Loop by tiles, fetch once, decode all pixels. Handles 8x8 and 16x16.
    // =========================================================================
    
    const uint32 xscr = scrollXInt & 0x1FF;
    const uint32 scrollOffset = xscr & (tileSize - 1);
    uint32 tx = xscr / tileSize;
    
    sint32 outX = -static_cast<sint32>(scrollOffset);
    const sint32 endX = static_cast<sint32>(HRes);
    
    while (outX < endX) {
        // Fetch pattern name once per tile
        const uint32 mapAddr = mapRowOffset + tx * 2;
        const uint16 patternName = ReadVRAM16(mapAddr);
        
        const bool hFlip = (patternName >> 10) & 1;
        const bool vFlip = (patternName >> 11) & 1;
        const uint16 charNum = patternName & 0x3FF;
        const uint16 palette = (patternName >> 12) & 0x0F;
        const uint32 charAddr = charNum * charBytes;
        const uint16 paletteBase = palette << 4;
        
        // Fetch ENTIRE tile row at once
        const uint32 effectiveSubY = vFlip ? (tileMask - subY) : subY;
        const uint32 rowAddr = charAddr + effectiveSubY * tileSize / 2;  // tileSize pixels * 4bpp / 8 bits
        
        // Read tile row data (8x8 = 4 bytes, 16x16 = 8 bytes)
        uint16 tileData[4];  // Max 4 words for 16x16 row
        const uint32 wordsPerRow = tileSize / 4;  // 8x8=2 words, 16x16=4 words
        for (uint32 w = 0; w < wordsPerRow; w++) {
            tileData[w] = ReadVRAM16(rowAddr + w * 2);
        }
        
        // Decode all pixels in this tile row
        for (uint32 i = 0; i < tileSize; i++) {
            const sint32 screenX = outX + static_cast<sint32>(i);
            if (screenX < 0 || screenX >= endX) continue;
            
            // Extract color index (4bpp)
            const uint32 pixelIndex = hFlip ? ((tileSize - 1) - i) : i;
            const uint32 wordIndex = pixelIndex / 4;  // Which 16-bit word
            const uint32 nibbleIndex = 3 - (pixelIndex % 4);  // Which nibble in word (MSB first)
            const uint8 colorIdx = (tileData[wordIndex] >> (nibbleIndex * 4)) & 0xF;
            
            // Write pixel
            LineBuffer[layer][screenX].color = ReadCRAM(paletteBase + colorIdx);
            LineBuffer[layer][screenX].transparent = (colorIdx == 0);
            LineBuffer[layer][screenX].priority = priority;
            LineBuffer[layer][screenX].shadow = false;
            LineBuffer[layer][screenX].ccEnable = false;
        }
        
        tx++;
        outX += tileSize;
    }
}

void DrawNBG0(uint32 line) {
    DrawNBGInternal(line, 0);
    devlog::trace<grp::vdp2_layer>("Drawing NBG0 line {}", line);
}

void DrawNBG1(uint32 line) {
    DrawNBGInternal(line, 1);
    devlog::trace<grp::vdp2_layer>("Drawing NBG1 line {}", line);
}

void DrawNBG2(uint32 line) {
    DrawNBGInternal(line, 2);
    devlog::trace<grp::vdp2_layer>("Drawing NBG2 line {}", line);
}

void DrawNBG3(uint32 line) {
    DrawNBGInternal(line, 3);
    devlog::trace<grp::vdp2_layer>("Drawing NBG3 line {}", line);
}

// =============================================================================
// Rotation Background Rendering - Optimized with Tile Batching
// =============================================================================

// Tile fetch cache structure - points directly into VRAM
struct TileFetch {
    const uint16* tileData;  // Pointer to tile data in VRAM
    uint16 paletteBase;
    bool hFlip;
    bool vFlip;
    
    void Fetch4bpp(uint32 mapOffset, sint32 tileX, sint32 tileY) {
        // Map address for cell pattern name - direct VRAM access (no function call overhead)
        const uint32 mapAddr = mapOffset + 0x10000 + ((tileY & 0x3F) * 64 + (tileX & 0x3F)) * 2;
        const uint16 patternName = VRAM[(mapAddr / 2) & (VRAM_SIZE / 2 - 1)];
        
        // Extract pattern attributes
        hFlip = (patternName >> 10) & 1;
        vFlip = (patternName >> 11) & 1;
        const uint16 charNum = patternName & 0x3FF;
        const uint16 palette = (patternName >> 12) & 0x0F;
        paletteBase = palette << 4;
        
        // Point directly to character data in VRAM
        const uint32 charAddr = mapOffset + charNum * 32;  // 8x8 4bpp = 32 bytes
        tileData = &VRAM[(charAddr / 2) & (VRAM_SIZE / 2 - 1)];
    }
    
    inline uint8 GetPixel4bpp(uint32 cellX, uint32 cellY) const {
        // Apply flipping
        const uint32 finalX = hFlip ? (7 - cellX) : cellX;
        const uint32 finalY = vFlip ? (7 - cellY) : cellY;
        
        // Each row is 4 bytes (8 pixels × 4 bits)
        const uint32 rowOffset = finalY * 2;  // 2 words per row
        const uint32 word = tileData[rowOffset + (finalX >> 2)];
        const uint32 shift = ((3 - (finalX & 3)) * 4);
        
        return (word >> shift) & 0xF;
    }
};

static void DrawRBGInternal(uint32 line, uint32 layer) {
    const uint8 layerIdx = 4 + layer;
    
    // Initialize to transparent
    for (uint32 x = 0; x < HRes; x++) {
        LineBuffer[layerIdx][x].transparent = true;
    }
    
    if (!IsLayerEnabled(layer == 0 ? Layer::RBG0 : Layer::RBG1)) {
        return;
    }
    
    // =========================================================================
    // SETUP - Extract rotation parameters
    // =========================================================================
    
    const uint8 priority = Regs.rbgPrio;
    const uint32 mapOffset = (layer == 0) ? 0x40000 : 0x60000;
    
    // Get rotation parameters
    const auto& rot = RotParams[layer];
    
    // Convert fixed-point coordinates (1.12.10 format) to 16.16 for calculations
    // Screen start position
    sint32 Xsp = rot.Xst;  // Already in correct format
    sint32 Ysp = rot.Yst;
    
    // Delta per line
    sint32 dXst = rot.DXst;
    sint32 dYst = rot.DYst;
    
    // Delta per pixel
    sint32 dX = rot.DX;
    sint32 dY = rot.DY;
    
    // Rotation matrix (simplified - using kx/ky as scale factors)
    // For proper rotation, we'd use the full matrix, but for now use delta X/Y
    sint32 kx = rot.matrix[0];  // A coefficient
    sint32 ky = rot.matrix[4];  // E coefficient
    
    // If no rotation matrix set, use identity (1.0 in 1.3.10 format = 0x0400)
    if (kx == 0) kx = 0x0400;
    if (ky == 0) ky = 0x0400;
    
    // Calculate starting position for this line
    const uint32 effectiveLine = GetEffectiveYLine(line);
    sint32 lineXp = Xsp + (dXst * static_cast<sint32>(effectiveLine));
    sint32 lineYp = Ysp + (dYst * static_cast<sint32>(effectiveLine));
    
    // =========================================================================
    // PER-PIXEL ROTATION RENDERING
    // =========================================================================
    
    TileFetch tileFetch{};
    sint32 lastTileX = -1;
    sint32 lastTileY = -1;
    
    for (uint32 screenX = 0; screenX < HRes; screenX++) {
        // Calculate rotated/scaled VRAM coordinates for this pixel
        // Formula: vramX = lineXp + (kx * screenX) / 1024
        //          vramY = lineYp + (ky * screenX) / 1024
        const sint32 vramX = lineXp + ((kx * static_cast<sint32>(screenX)) >> 10);
        const sint32 vramY = lineYp + ((ky * static_cast<sint32>(screenX)) >> 10);
        
        // Convert to tile coordinates (divide by 8, wrapping to 512 pixel space)
        const sint32 pixelX = (vramX >> 16) & 0x1FF;
        const sint32 pixelY = (vramY >> 16) & 0x1FF;
        const sint32 tileX = pixelX >> 3;
        const sint32 tileY = pixelY >> 3;
        
        // Fetch tile if we've moved to a new tile
        if (tileX != lastTileX || tileY != lastTileY) {
            tileFetch.Fetch4bpp(mapOffset, tileX, tileY);
            lastTileX = tileX;
            lastTileY = tileY;
        }
        
        // Get pixel within tile
        const uint32 cellX = pixelX & 7;
        const uint32 cellY = pixelY & 7;
        const uint8 colorIdx = tileFetch.GetPixel4bpp(cellX, cellY);
        
        // Write to line buffer
        if (colorIdx != 0) {
            LineBuffer[layerIdx][screenX].color = ReadCRAM(tileFetch.paletteBase + colorIdx);
            LineBuffer[layerIdx][screenX].transparent = false;
            LineBuffer[layerIdx][screenX].priority = priority;
            LineBuffer[layerIdx][screenX].shadow = false;
            LineBuffer[layerIdx][screenX].ccEnable = false;
        }
    }
}

void DrawRBG0(uint32 line) {
    DrawRBGInternal(line, 0);
    devlog::trace<grp::vdp2_layer>("Drawing RBG0 line {}", line);
}

void DrawRBG1(uint32 line) {
    DrawRBGInternal(line, 1);
    devlog::trace<grp::vdp2_layer>("Drawing RBG1 line {}", line);
}

// =============================================================================
// Sprite Layer Rendering
// =============================================================================

void DrawSprite(uint32 line) {
    // Initialize sprite layer to transparent
    for (uint32 x = 0; x < HRes; x++) {
        LineBuffer[6][x].transparent = true;
    }
    
    // Check if VDP1 framebuffer is available
    if (!VDP1Framebuffer || line >= VDP1FBHeight) {
        return;
    }
    
    // Get sprite priority from register
    uint8 priority = Regs.spritePrio[0];
    
    // Simple VDP1 framebuffer copy
    for (uint32 x = 0; x < HRes && x < VDP1FBWidth; x++) {
        uint16 pix = VDP1Framebuffer[line * VDP1FBWidth + x];
        
        // Check for transparency (pixel value 0)
        if (pix == 0) continue;
        
        // Convert RGB555 (Saturn format is BGR555) to RGB888
        // Saturn: BBBBBGGGGGRRRRR
        uint32 b = ((pix & 0x7C00) >> 7) | ((pix & 0x7C00) >> 12);
        uint32 g = ((pix & 0x03E0) >> 2) | ((pix & 0x03E0) >> 7);
        uint32 r = ((pix & 0x001F) << 3) | ((pix & 0x001F) >> 2);
        
        uint32 color = (r << 16) | (g << 8) | b;
        
        LineBuffer[6][x].color = color;
        LineBuffer[6][x].transparent = false;
        LineBuffer[6][x].priority = priority;
        LineBuffer[6][x].shadow = false;
        LineBuffer[6][x].ccEnable = false;
    }
    
    devlog::trace<grp::vdp2_layer>("Drawing Sprite line {}", line);
}

// =============================================================================
// Back Screen Rendering
// =============================================================================

void DrawBack(uint32 line) {
    // TODO: Implement back screen
    // Read back color from table or single color
    
    uint32 backColor = 0x000000;  // Default black
    
    for (uint32 x = 0; x < HRes; x++) {
        LineBuffer[7][x].color = backColor;
        LineBuffer[7][x].priority = 0;
        LineBuffer[7][x].transparent = false;
        LineBuffer[7][x].shadow = false;
        LineBuffer[7][x].ccEnable = false;
    }
}

void DrawLineColor(uint32 line) {
    // TODO: Implement line color screen
}

// =============================================================================
// Parameter Setup
// =============================================================================

void SetupNBGParams(uint32 layerIndex, NBGParams& params) {
    // TODO: Extract parameters from registers
    params.mapAddress = 0;
    params.charAddress = 0;
    params.scrollX = Regs.scrollX[layerIndex] << 16;
    params.scrollY = Regs.scrollY[layerIndex] << 16;
    params.zoomX = 0x10000;  // 1.0
    params.zoomY = 0x10000;
    params.colorMode = 0;
    params.charSize = 0;
    params.planeSize = 0;
    params.bitmapMode = false;
    params.priority = Regs.nbgPrio[layerIndex];
    params.ccEnable = false;
    params.ccRatio = 0;
}

void SetupRBGParams(uint32 layerIndex, RBGParams& params) {
    // TODO: Extract parameters from registers
    params.mapAddress = 0;
    params.charAddress = 0;
    params.coeffTableAddr = 0;
    params.colorMode = 0;
    params.charSize = 0;
    params.planeSize = 0;
    params.bitmapMode = false;
    params.rotParams = &RotParams[layerIndex];
    params.useCoeffTable = false;
    params.coeffMode = 0;
    params.overPattern = 0;
    params.priority = Regs.rbgPrio;
    params.ccEnable = false;
    params.ccRatio = 0;
}

void SetupSpriteParams(SpriteParams& params) {
    // TODO: Extract parameters from VDP1 state and VDP2 registers
    params.framebuffer = nullptr;
    params.fbWidth = 512;
    params.fbHeight = 256;
    params.fb8bpp = false;
    params.fbRotate = false;
    params.typeSelect = 0;
    
    for (int i = 0; i < 8; i++) {
        params.priority[i] = Regs.spritePrio[i];
        params.ccEnable[i] = false;
        params.ccRatio[i] = Regs.spriteCCRatio[i];
    }
}

void FetchRotationParams(uint32 tableAddr, RotationParams& params) {
    // TODO: Implement rotation parameter fetch from VRAM
    // This reads the rotation matrix, translation, and scaling from VRAM
}

} // namespace brimir::vdp2


