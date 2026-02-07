// Brimir - VDP2 NBG Layer Fragment Shader
// Renders scroll/bitmap background layers with pattern lookups
#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec2 fragScreenPos;
layout(location = 0) out vec4 outColor;

// VRAM texture (raw data, patterns and tiles)
layout(set = 0, binding = 0) uniform usampler2D vramTexture;

// Color RAM texture (palette)
layout(set = 0, binding = 1) uniform sampler2D cramTexture;

layout(push_constant) uniform NBGConstants {
    vec2 screenSize;        // Output resolution
    vec2 scrollOffset;      // X, Y scroll (8 fractional bits)
    vec2 scrollInc;         // Pixel increment for zoom
    uint layerIndex;        // Which NBG layer
    uint flags;             // Layer flags
    
    // Layer parameters
    uint patternNameAddr;   // Pattern name table address
    uint characterAddr;     // Character pattern base address
    uint colorFormat;       // 0=16, 1=256, 2=2048, 3=32768, 4=16M colors
    uint cramOffset;        // CRAM offset for palette
    
    // Map geometry
    uint pageSize;          // Page dimensions (bits)
    uint planeSize;         // Plane dimensions (bits)
    uint cellSize;          // Cell size (0=1x1, 1=2x2)
    uint bitmapMode;        // 0=scroll, 1=bitmap
    
    // Bitmap parameters
    uint bitmapWidth;
    uint bitmapHeight;
    uint bitmapAddr;
    uint reserved;
} pc;

// Bit flags
const uint FLAG_TRANSPARENCY = 0x01u;
const uint FLAG_MOSAIC = 0x02u;
const uint FLAG_LINE_SCROLL = 0x04u;

// Color format constants
const uint COLOR_16 = 0u;
const uint COLOR_256 = 1u;
const uint COLOR_2048 = 2u;
const uint COLOR_32768 = 3u;
const uint COLOR_16M = 4u;

// Read a byte from VRAM texture
uint readVRAMByte(uint addr) {
    // VRAM is stored as R32_UINT (4 bytes per texel)
    // Address to texel: addr / 4, byte offset: addr % 4
    uint texelAddr = addr >> 2;
    uint byteOffset = addr & 3u;
    
    uvec2 texCoord = uvec2(texelAddr & 0x1FFu, texelAddr >> 9);
    uint texel = texelFetch(vramTexture, ivec2(texCoord), 0).r;
    
    return (texel >> (byteOffset * 8u)) & 0xFFu;
}

// Read a 16-bit word from VRAM
uint readVRAMWord(uint addr) {
    return readVRAMByte(addr) | (readVRAMByte(addr + 1u) << 8);
}

// Look up color from CRAM
vec4 lookupCRAM(uint colorIndex) {
    // CRAM is stored as RGBA8 texture (1024 or 2048 entries)
    ivec2 cramCoord = ivec2(colorIndex & 0x3Fu, colorIndex >> 6);
    return texelFetch(cramTexture, cramCoord, 0);
}

// Decode RGB555 color to vec4
vec4 decodeRGB555(uint color) {
    float r = float((color >> 0) & 0x1Fu) / 31.0;
    float g = float((color >> 5) & 0x1Fu) / 31.0;
    float b = float((color >> 10) & 0x1Fu) / 31.0;
    float a = ((color & 0x8000u) != 0u) ? 1.0 : 0.0;  // MSB = opaque
    return vec4(r, g, b, a);
}

void main() {
    // Calculate scrolled coordinates
    vec2 scrolledPos = fragScreenPos * pc.scrollInc + pc.scrollOffset;
    
    // Apply mosaic if enabled
    if ((pc.flags & FLAG_MOSAIC) != 0u) {
        // TODO: Implement mosaic
    }
    
    vec4 color = vec4(0.0);
    
    if (pc.bitmapMode != 0u) {
        // ===== Bitmap Mode =====
        // Direct pixel access from VRAM
        
        int x = int(scrolledPos.x) % int(pc.bitmapWidth);
        int y = int(scrolledPos.y) % int(pc.bitmapHeight);
        if (x < 0) x += int(pc.bitmapWidth);
        if (y < 0) y += int(pc.bitmapHeight);
        
        uint pixelAddr = pc.bitmapAddr;
        
        if (pc.colorFormat == COLOR_16) {
            // 4bpp: 2 pixels per byte
            pixelAddr += uint(y * int(pc.bitmapWidth) + x) >> 1;
            uint byte = readVRAMByte(pixelAddr);
            uint colorIndex = ((x & 1) != 0) ? (byte >> 4) : (byte & 0xFu);
            
            if (colorIndex == 0u && (pc.flags & FLAG_TRANSPARENCY) != 0u) {
                discard;
            }
            
            color = lookupCRAM(pc.cramOffset + colorIndex);
            
        } else if (pc.colorFormat == COLOR_256) {
            // 8bpp: 1 pixel per byte
            pixelAddr += uint(y * int(pc.bitmapWidth) + x);
            uint colorIndex = readVRAMByte(pixelAddr);
            
            if (colorIndex == 0u && (pc.flags & FLAG_TRANSPARENCY) != 0u) {
                discard;
            }
            
            color = lookupCRAM(pc.cramOffset + colorIndex);
            
        } else if (pc.colorFormat == COLOR_32768) {
            // 16bpp RGB555: 2 bytes per pixel
            pixelAddr += uint(y * int(pc.bitmapWidth) + x) * 2u;
            uint rgb555 = readVRAMWord(pixelAddr);
            
            if ((rgb555 & 0x8000u) == 0u && (pc.flags & FLAG_TRANSPARENCY) != 0u) {
                discard;
            }
            
            color = decodeRGB555(rgb555);
        }
        
    } else {
        // ===== Scroll (Cell) Mode =====
        // Pattern name table lookup -> character pattern -> pixel
        
        // TODO: Implement full scroll mode with:
        // - Pattern name table lookup
        // - Character pattern data fetch
        // - Multi-plane support
        // - Vertical cell scroll
        // For now, output placeholder
        
        int x = int(scrolledPos.x);
        int y = int(scrolledPos.y);
        
        // Cell coordinates
        int cellX = x >> 3;  // Divide by 8 (cell width)
        int cellY = y >> 3;  // Divide by 8 (cell height)
        
        // Pixel within cell
        int pixelX = x & 7;
        int pixelY = y & 7;
        
        // Pattern name table entry
        // TODO: Proper plane/page calculation
        uint pnAddr = pc.patternNameAddr + uint((cellY * 64 + cellX) * 2);
        uint patternName = readVRAMWord(pnAddr);
        
        // Extract pattern info (simplified 1-word pattern name)
        uint charIndex = patternName & 0x3FFu;
        uint palette = (patternName >> 12) & 0xFu;
        bool flipH = (patternName & 0x400u) != 0u;
        bool flipV = (patternName & 0x800u) != 0u;
        
        // Apply flip
        if (flipH) pixelX = 7 - pixelX;
        if (flipV) pixelY = 7 - pixelY;
        
        // Character data address
        uint charAddr = pc.characterAddr + charIndex * 32u;  // 8x8x4bpp = 32 bytes
        
        // Read pixel from character
        if (pc.colorFormat == COLOR_16) {
            // 4bpp
            charAddr += uint(pixelY * 4 + pixelX / 2);
            uint byte = readVRAMByte(charAddr);
            uint colorIndex = ((pixelX & 1) != 0) ? (byte >> 4) : (byte & 0xFu);
            
            if (colorIndex == 0u && (pc.flags & FLAG_TRANSPARENCY) != 0u) {
                discard;
            }
            
            color = lookupCRAM(pc.cramOffset + palette * 16u + colorIndex);
            
        } else {
            // TODO: Other color depths
            color = vec4(1.0, 0.0, 1.0, 1.0);  // Magenta placeholder
        }
    }
    
    outColor = color;
}

