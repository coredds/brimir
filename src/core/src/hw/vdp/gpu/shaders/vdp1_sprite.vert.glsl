// Brimir - VDP1 Sprite Vertex Shader
// Renders sprites and polygons to the VDP1 sprite layer
#version 450

// Input vertex attributes (from CPU-side vertex buffer)
layout(location = 0) in vec2 inPosition;   // Screen position
layout(location = 1) in vec2 inTexCoord;   // Texture coordinate (normalized)
layout(location = 2) in vec4 inColor;      // Gouraud shading color or flat color
layout(location = 3) in uint inFlags;      // Packed flags: colorMode, transparent, etc.

// Output to fragment shader
layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) flat out uint fragFlags;

// Push constants for per-frame data
layout(push_constant) uniform PushConstants {
    vec2 screenSize;    // Native Saturn resolution (e.g., 320x224)
    uint frameFlags;    // Global frame flags
    uint reserved;
} pc;

void main() {
    // Convert screen coordinates to normalized device coordinates
    // Saturn screen: (0,0) top-left, (width-1, height-1) bottom-right
    vec2 ndc = (inPosition / pc.screenSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y for Vulkan coordinate system
    
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragTexCoord = inTexCoord;
    fragColor = inColor;
    fragFlags = inFlags;
}

