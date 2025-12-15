#version 450

// VDP1 Fragment Shader
// Handles Saturn VDP1 color modes and texture sampling

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;  // VDP1 VRAM texture

layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    uint flags;
    uint priority;
} pc;

// Flags
const uint FLAG_TEXTURED = 1u << 0;
const uint FLAG_GOURAUD = 1u << 1;
const uint FLAG_HALF_LUMINANCE = 1u << 2;
const uint FLAG_HALF_TRANSPARENT = 1u << 3;

void main() {
    vec4 color = fragColor;
    
    // Sample texture if textured mode
    if ((pc.flags & FLAG_TEXTURED) != 0u) {
        vec4 texColor = texture(texSampler, fragTexCoord);
        
        // Saturn MSB transparency check
        if (texColor.a < 0.5) {
            discard;
        }
        
        // Apply Gouraud shading (modulate texture by vertex color)
        if ((pc.flags & FLAG_GOURAUD) != 0u) {
            color = texColor * fragColor;
        } else {
            color = texColor;
        }
    }
    
    // Half-luminance effect
    if ((pc.flags & FLAG_HALF_LUMINANCE) != 0u) {
        color.rgb *= 0.5;
    }
    
    // Half-transparent (50% alpha)
    if ((pc.flags & FLAG_HALF_TRANSPARENT) != 0u) {
        color.a = 0.5;
    }
    
    outColor = color;
}

