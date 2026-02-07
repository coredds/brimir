// Brimir - VDP2 NBG Layer Vertex Shader
// Full-screen quad for rendering background layers
#version 450

// Full-screen triangle vertices
const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

const vec2 texCoords[3] = vec2[](
    vec2(0.0, 0.0),
    vec2(2.0, 0.0),
    vec2(0.0, 2.0)
);

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec2 fragScreenPos;

layout(push_constant) uniform NBGConstants {
    vec2 screenSize;        // Output resolution
    vec2 scrollOffset;      // X, Y scroll (fixed point 11.8)
    vec2 scrollInc;         // Pixel increment (fixed point, for zoom)
    uint layerIndex;        // Which NBG layer (0-3)
    uint flags;             // Layer flags
} pc;

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragTexCoord = texCoords[gl_VertexIndex];
    
    // Screen position in pixels
    fragScreenPos = fragTexCoord * pc.screenSize;
}

