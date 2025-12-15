#version 450

// VDP1 Vertex Shader
// Renders Saturn VDP1 polygons and sprites

layout(location = 0) in vec2 inPosition;  // Vertex position (screen coords)
layout(location = 1) in vec4 inColor;     // Vertex color (RGBA)
layout(location = 2) in vec2 inTexCoord;  // Texture coordinates

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord;

layout(push_constant) uniform PushConstants {
    vec2 screenSize;     // Screen resolution (320x224 typically)
    uint flags;          // Rendering flags (textured, gouraud, etc.)
    uint priority;       // VDP1 priority level
} pc;

void main() {
    // Convert Saturn screen coordinates to Vulkan NDC [-1, 1]
    vec2 ndc = (inPosition / pc.screenSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y (Vulkan top-left vs Saturn top-left)
    
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}

