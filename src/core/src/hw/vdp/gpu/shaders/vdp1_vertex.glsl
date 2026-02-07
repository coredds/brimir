#version 450

// VDP1 Vertex Shader
// Renders Saturn VDP1 polygons and sprites
// Supports inverse bilinear UV correction for DistortedSprite quads

layout(location = 0) in vec2 inPosition;     // Vertex position (screen coords)
layout(location = 1) in vec4 inColor;        // Vertex color (RGBA)
layout(location = 2) in vec2 inTexCoord;     // Texture coordinates (linear interpolation)
layout(location = 3) in vec2 inQuadP0;       // Quad corner 0 screen position
layout(location = 4) in vec2 inQuadP1;       // Quad corner 1 screen position
layout(location = 5) in vec2 inQuadP2;       // Quad corner 2 screen position
layout(location = 6) in vec2 inQuadP3;       // Quad corner 3 screen position
layout(location = 7) in vec2 inQuadUV0;      // Quad corner 0 atlas UV
layout(location = 8) in vec2 inQuadUV1;      // Quad corner 1 atlas UV
layout(location = 9) in vec2 inQuadUV2;      // Quad corner 2 atlas UV
layout(location = 10) in vec2 inQuadUV3;     // Quad corner 3 atlas UV

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord;
// Flat outputs: quad corner data for inverse bilinear in fragment shader
layout(location = 2) flat out vec2 fragQuadP0;
layout(location = 3) flat out vec2 fragQuadP1;
layout(location = 4) flat out vec2 fragQuadP2;
layout(location = 5) flat out vec2 fragQuadP3;
layout(location = 6) flat out vec2 fragQuadUV0;
layout(location = 7) flat out vec2 fragQuadUV1;
layout(location = 8) flat out vec2 fragQuadUV2;
layout(location = 9) flat out vec2 fragQuadUV3;

layout(push_constant) uniform PushConstants {
    vec2 screenSize;     // Screen resolution (320x224 typically)
    uint flags;          // Rendering flags (textured, gouraud, etc.)
    uint priority;       // VDP1 priority level
} pc;

void main() {
    // Convert Saturn screen coordinates to Vulkan NDC [-1, 1]
    // Saturn: (0,0) = top-left, Vulkan NDC: (-1,-1) = top-left
    vec2 ndc = (inPosition / pc.screenSize) * 2.0 - 1.0;
    // No Y flip needed - both Saturn and Vulkan use top-left origin
    
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    
    // Pass quad corner data (flat - same for all vertices in the quad)
    fragQuadP0 = inQuadP0;
    fragQuadP1 = inQuadP1;
    fragQuadP2 = inQuadP2;
    fragQuadP3 = inQuadP3;
    fragQuadUV0 = inQuadUV0;
    fragQuadUV1 = inQuadUV1;
    fragQuadUV2 = inQuadUV2;
    fragQuadUV3 = inQuadUV3;
}
