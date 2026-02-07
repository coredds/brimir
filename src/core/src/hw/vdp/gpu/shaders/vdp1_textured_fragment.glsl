#version 450

// VDP1 Textured Fragment Shader
// Samples from VDP1 VRAM texture atlas

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D vramTexture;  // VDP1 VRAM as texture (1024x512)

layout(push_constant) uniform PushConstants {
    vec2 screenSize;     // Screen resolution (320x224 typically)
    uint flags;          // Rendering flags (textured, gouraud, etc.)
    uint priority;       // VDP1 priority level
} pc;

// Rendering flags (must match vulkan_renderer.cpp)
const uint FLAG_TEXTURED    = 0x1;      // Textured mode
const uint FLAG_MESH_ENABLE = 0x2;      // Mesh transparency (checkerboard)
const uint FLAG_MSB_SHADOW  = 0x4;      // MSB=0 is transparent
const uint FLAG_GOURAUD     = 0x8;      // Gouraud shading

void main() {
    // Sample texture from VDP1 VRAM
    vec4 texColor = texture(vramTexture, fragTexCoord);
    
    // Handle MSB transparency (Saturn RGB555 format)
    // If MSB (bit 15) is 0, pixel is transparent
    if ((pc.flags & FLAG_MSB_SHADOW) != 0 && texColor.a < 0.5) {
        discard;
    }
    
    // Handle mesh transparency (checkerboard pattern)
    // Saturn uses a dithered pattern for semi-transparency
    if ((pc.flags & FLAG_MESH_ENABLE) != 0) {
        // Create checkerboard pattern
        ivec2 pixelPos = ivec2(gl_FragCoord.xy);
        bool evenX = (pixelPos.x & 1) == 0;
        bool evenY = (pixelPos.y & 1) == 0;
        
        // Discard every other pixel in checkerboard pattern
        if (evenX == evenY) {
            discard;
        }
    }
    
    // Modulate texture color with vertex color (for Gouraud shading)
    outColor = texColor * fragColor;
    
    // Ensure alpha is 1.0 for opaque output (Saturn uses RGB565 output)
    outColor.a = 1.0;
}

