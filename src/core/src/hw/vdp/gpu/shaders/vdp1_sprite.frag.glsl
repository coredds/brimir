// Brimir - VDP1 Sprite Fragment Shader
// Samples from VRAM/CRAM textures for GPU-based sprite rendering
#version 450

// Input from vertex shader
layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) flat in uint fragFlags;

// Output color (includes priority in alpha)
layout(location = 0) out vec4 outColor;

// Texture samplers
layout(set = 0, binding = 0) uniform usampler2D uVDP1VRAM;  // VDP1 VRAM (R32_UINT)
layout(set = 0, binding = 1) uniform sampler2D uCRAM;       // Color RAM (RGBA8)

// Push constants
layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    uint frameFlags;
    uint reserved;
} pc;

// Flag bit definitions (matching CPU-side encoding)
const uint FLAG_TEXTURED = 0x0001u;          // Sprite has texture
const uint FLAG_TRANSPARENT = 0x0002u;       // Enable transparency
const uint FLAG_GOURAUD = 0x0004u;            // Gouraud shading enabled
const uint FLAG_MESH = 0x0008u;               // Mesh enable
const uint FLAG_MSB_ON = 0x0010u;             // MSB shadow bit
const uint FLAG_COLOR_MODE_MASK = 0x0700u;   // Color mode (3 bits, shifted by 8)
const uint FLAG_PRIORITY_MASK = 0x7000u;     // Priority bits (3 bits, shifted by 12)

// Color mode values
const uint COLOR_MODE_4BPP_BANK = 0u;
const uint COLOR_MODE_4BPP_LUT = 1u;
const uint COLOR_MODE_8BPP_64 = 2u;
const uint COLOR_MODE_8BPP_128 = 3u;
const uint COLOR_MODE_8BPP_256 = 4u;
const uint COLOR_MODE_RGB555 = 5u;

// Sample from CRAM palette
vec4 sampleCRAM(uint colorIndex) {
    // CRAM is stored as 64x32 texture (2048 colors)
    float u = float(colorIndex & 63u) / 64.0;
    float v = float(colorIndex >> 6) / 32.0;
    return texture(uCRAM, vec2(u, v));
}

// Convert RGB555 to normalized color
vec4 rgb555ToVec4(uint color) {
    float r = float((color >> 0u) & 31u) / 31.0;
    float g = float((color >> 5u) & 31u) / 31.0;
    float b = float((color >> 10u) & 31u) / 31.0;
    return vec4(r, g, b, 1.0);
}

void main() {
    bool isTextured = (fragFlags & FLAG_TEXTURED) != 0u;
    bool isTransparent = (fragFlags & FLAG_TRANSPARENT) != 0u;
    bool isGouraud = (fragFlags & FLAG_GOURAUD) != 0u;
    bool isMesh = (fragFlags & FLAG_MESH) != 0u;
    uint colorMode = (fragFlags & FLAG_COLOR_MODE_MASK) >> 8u;
    uint priority = (fragFlags & FLAG_PRIORITY_MASK) >> 12u;
    
    vec4 color;
    
    if (isTextured) {
        // For textured sprites, texture data is pre-decoded and passed via fragColor
        // In full GPU mode, we would sample VRAM here
        // For now, use the color passed from vertex shader (CPU-decoded)
        color = fragColor;
        
        // Check for transparent pixel
        if (isTransparent && color.a < 0.5) {
            discard;
        }
    } else {
        // Untextured (polygon/line) - use flat or Gouraud color
        color = fragColor;
    }
    
    // Apply Gouraud shading modifier
    if (isGouraud) {
        // Gouraud color is already interpolated - blend with base color
        // In full implementation, this would modulate the base color
    }
    
    // Mesh effect - checkerboard pattern
    if (isMesh) {
        ivec2 fragCoord = ivec2(gl_FragCoord.xy);
        if (((fragCoord.x + fragCoord.y) & 1) == 1) {
            discard;
        }
    }
    
    // Encode priority in alpha channel for compositor
    // Priority 0-7 maps to specific alpha values
    float priorityAlpha = float(priority + 1u) / 8.0;
    
    outColor = vec4(color.rgb, priorityAlpha);
}

