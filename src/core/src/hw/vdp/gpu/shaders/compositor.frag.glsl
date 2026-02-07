// Brimir - VDP Layer Compositor Fragment Shader
// Composites all VDP layers according to Saturn's priority system
#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// Layer textures (sampled from per-layer render targets)
layout(set = 0, binding = 0) uniform sampler2D layerBack;     // Back screen
layout(set = 0, binding = 1) uniform sampler2D layerNBG3;     // NBG3
layout(set = 0, binding = 2) uniform sampler2D layerNBG2;     // NBG2
layout(set = 0, binding = 3) uniform sampler2D layerNBG1;     // NBG1/EXBG
layout(set = 0, binding = 4) uniform sampler2D layerNBG0;     // NBG0/RBG1
layout(set = 0, binding = 5) uniform sampler2D layerRBG0;     // RBG0
layout(set = 0, binding = 6) uniform sampler2D layerSprite;   // VDP1 Sprites
layout(set = 0, binding = 7) uniform sampler2D layerLineColor; // Line color screen

// Layer configuration (uniforms)
layout(push_constant) uniform CompositorConstants {
    // Priority for each layer (0-7, higher = on top)
    // Packed as 8x 4-bit values in two uints
    uint priorities0_3;     // [NBG3:4][NBG2:4][NBG1:4][NBG0:4]
    uint priorities4_7;     // [unused:4][LNCL:4][Sprite:4][RBG0:4]
    
    // Layer enable mask (bit per layer)
    uint layerEnableMask;
    
    // Color calculation enable mask
    uint colorCalcEnableMask;
    
    // Color calculation ratios (packed)
    uint colorCalcRatios;   // 5 bits per layer (0-31)
    
    // Flags
    uint flags;
} pc;

// Extract 4-bit priority from packed uint
int getPriority(uint packed, int layer) {
    return int((packed >> (layer * 4)) & 0xFu);
}

// Check if layer is enabled
bool isLayerEnabled(int layer) {
    return (pc.layerEnableMask & (1u << layer)) != 0u;
}

// Get color calculation ratio for layer (0-31)
int getColorCalcRatio(int layer) {
    return int((pc.colorCalcRatios >> (layer * 5)) & 0x1Fu);
}

void main() {
    vec2 uv = fragTexCoord;
    
    // Sample all layers
    vec4 back = texture(layerBack, uv);
    vec4 nbg3 = texture(layerNBG3, uv);
    vec4 nbg2 = texture(layerNBG2, uv);
    vec4 nbg1 = texture(layerNBG1, uv);
    vec4 nbg0 = texture(layerNBG0, uv);
    vec4 rbg0 = texture(layerRBG0, uv);
    vec4 sprite = texture(layerSprite, uv);
    vec4 lineColor = texture(layerLineColor, uv);
    
    // Build layer array with priorities
    // Saturn composites layers based on priority (0-7)
    // Multiple layers can have the same priority - order within priority is:
    // Sprite > RBG0 > NBG0 > NBG1 > NBG2 > NBG3 > Back
    
    // Start with back screen color
    vec3 result = back.rgb;
    
    // Extract priorities for each layer
    int prioNBG3 = getPriority(pc.priorities0_3, 0);
    int prioNBG2 = getPriority(pc.priorities0_3, 1);
    int prioNBG1 = getPriority(pc.priorities0_3, 2);
    int prioNBG0 = getPriority(pc.priorities0_3, 3);
    int prioRBG0 = getPriority(pc.priorities4_7, 0);
    int prioSprite = getPriority(pc.priorities4_7, 1);
    
    // Simple priority-based compositing (lowest priority first)
    // This is a simplified version - full Saturn has more complex rules
    
    // Create sorted layer list by priority (using selection sort inline)
    // For now, use a simple approach: iterate through priorities 0-7
    // and blend enabled, non-transparent layers
    
    for (int prio = 0; prio <= 7; prio++) {
        // Check each layer at this priority level
        // Order within same priority: Back < NBG3 < NBG2 < NBG1 < NBG0 < RBG0 < Sprite
        
        if (prioNBG3 == prio && isLayerEnabled(1) && nbg3.a > 0.0) {
            result = mix(result, nbg3.rgb, nbg3.a);
        }
        if (prioNBG2 == prio && isLayerEnabled(2) && nbg2.a > 0.0) {
            result = mix(result, nbg2.rgb, nbg2.a);
        }
        if (prioNBG1 == prio && isLayerEnabled(3) && nbg1.a > 0.0) {
            result = mix(result, nbg1.rgb, nbg1.a);
        }
        if (prioNBG0 == prio && isLayerEnabled(4) && nbg0.a > 0.0) {
            result = mix(result, nbg0.rgb, nbg0.a);
        }
        if (prioRBG0 == prio && isLayerEnabled(5) && rbg0.a > 0.0) {
            result = mix(result, rbg0.rgb, rbg0.a);
        }
        if (prioSprite == prio && isLayerEnabled(6) && sprite.a > 0.0) {
            result = mix(result, sprite.rgb, sprite.a);
        }
    }
    
    // Apply line color screen if enabled
    if (isLayerEnabled(7) && lineColor.a > 0.0) {
        result = mix(result, lineColor.rgb, lineColor.a);
    }
    
    outColor = vec4(result, 1.0);
}

