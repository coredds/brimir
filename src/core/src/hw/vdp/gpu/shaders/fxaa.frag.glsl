// Brimir - FXAA 3.11 Quality Fragment Shader
// Fast Approximate Anti-Aliasing applied to upscaled output
// Reuses upscale.vert.glsl fullscreen triangle vertex shader
#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// Upscaled framebuffer texture (output of upscale pass)
layout(set = 0, binding = 0) uniform sampler2D inputTexture;

// Push constants - reuses UpscalePushConstants layout
layout(push_constant) uniform FXAAConstants {
    vec2 inputSize;     // unused (kept for layout compatibility)
    vec2 outputSize;    // Upscaled resolution for texel size calculation
    uint filterMode;    // unused
    uint scanlines;     // unused
    float brightness;   // unused
    float gamma;        // unused
} pc;

// FXAA quality settings
#define FXAA_EDGE_THRESHOLD     (1.0 / 8.0)
#define FXAA_EDGE_THRESHOLD_MIN (1.0 / 24.0)
#define FXAA_SEARCH_STEPS       12
#define FXAA_SUBPIX_QUALITY     0.75

// Compute perceptual luminance from linear RGB
float FxaaLuma(vec3 rgb) {
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec2 texelSize = 1.0 / pc.outputSize;
    vec2 uv = fragTexCoord;
    
    // Sample center and 4 neighbors
    vec3 rgbM  = texture(inputTexture, uv).rgb;
    vec3 rgbN  = texture(inputTexture, uv + vec2( 0.0, -texelSize.y)).rgb;
    vec3 rgbS  = texture(inputTexture, uv + vec2( 0.0,  texelSize.y)).rgb;
    vec3 rgbE  = texture(inputTexture, uv + vec2( texelSize.x,  0.0)).rgb;
    vec3 rgbW  = texture(inputTexture, uv + vec2(-texelSize.x,  0.0)).rgb;
    
    // Compute luma for center and neighbors
    float lumaM = FxaaLuma(rgbM);
    float lumaN = FxaaLuma(rgbN);
    float lumaS = FxaaLuma(rgbS);
    float lumaE = FxaaLuma(rgbE);
    float lumaW = FxaaLuma(rgbW);
    
    // Determine local contrast range
    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float lumaRange = lumaMax - lumaMin;
    
    // Skip pixel if contrast is too low (not an edge)
    if (lumaRange < max(FXAA_EDGE_THRESHOLD_MIN, lumaMax * FXAA_EDGE_THRESHOLD)) {
        outColor = vec4(rgbM, 1.0);
        return;
    }
    
    // Sample diagonal neighbors for subpixel aliasing test
    vec3 rgbNW = texture(inputTexture, uv + vec2(-texelSize.x, -texelSize.y)).rgb;
    vec3 rgbNE = texture(inputTexture, uv + vec2( texelSize.x, -texelSize.y)).rgb;
    vec3 rgbSW = texture(inputTexture, uv + vec2(-texelSize.x,  texelSize.y)).rgb;
    vec3 rgbSE = texture(inputTexture, uv + vec2( texelSize.x,  texelSize.y)).rgb;
    
    float lumaNW = FxaaLuma(rgbNW);
    float lumaNE = FxaaLuma(rgbNE);
    float lumaSW = FxaaLuma(rgbSW);
    float lumaSE = FxaaLuma(rgbSE);
    
    // Subpixel aliasing test
    float lumaL = (lumaN + lumaS + lumaE + lumaW) * 0.25;
    float rangeL = abs(lumaL - lumaM);
    float blendL = max(0.0, (rangeL / lumaRange) - FXAA_SUBPIX_QUALITY);
    blendL = min(blendL, 0.75);  // Cap subpixel blend
    
    // Determine edge direction (horizontal vs vertical)
    float edgeH = abs(lumaNW + lumaNE - 2.0 * lumaN)
                + abs(lumaW  + lumaE  - 2.0 * lumaM) * 2.0
                + abs(lumaSW + lumaSE - 2.0 * lumaS);
    float edgeV = abs(lumaNW + lumaSW - 2.0 * lumaW)
                + abs(lumaN  + lumaS  - 2.0 * lumaM) * 2.0
                + abs(lumaNE + lumaSE - 2.0 * lumaE);
    bool isHorizontal = (edgeH >= edgeV);
    
    // Select edge endpoints
    float stepLength = isHorizontal ? texelSize.y : texelSize.x;
    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    
    bool is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));
    
    // Step in the direction of the steepest edge
    if (!is1Steepest) {
        stepLength = -stepLength;
    }
    
    // Average luma in the edge direction
    float lumaLocalAvg;
    if (is1Steepest) {
        lumaLocalAvg = 0.5 * (luma1 + lumaM);
    } else {
        lumaLocalAvg = 0.5 * (luma2 + lumaM);
    }
    
    // Shift UV to edge center
    vec2 currentUV = uv;
    if (isHorizontal) {
        currentUV.y += stepLength * 0.5;
    } else {
        currentUV.x += stepLength * 0.5;
    }
    
    // Walk along the edge in both directions to find endpoints
    vec2 offset = isHorizontal ? vec2(texelSize.x, 0.0) : vec2(0.0, texelSize.y);
    
    vec2 uv1 = currentUV - offset;
    vec2 uv2 = currentUV + offset;
    
    float lumaEnd1 = FxaaLuma(texture(inputTexture, uv1).rgb) - lumaLocalAvg;
    float lumaEnd2 = FxaaLuma(texture(inputTexture, uv2).rgb) - lumaLocalAvg;
    
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;
    
    // Continue searching along the edge
    for (int i = 2; i < FXAA_SEARCH_STEPS; i++) {
        if (!reached1) {
            uv1 -= offset;
            lumaEnd1 = FxaaLuma(texture(inputTexture, uv1).rgb) - lumaLocalAvg;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2) {
            uv2 += offset;
            lumaEnd2 = FxaaLuma(texture(inputTexture, uv2).rgb) - lumaLocalAvg;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
        if (reached1 && reached2) break;
    }
    
    // Compute distances to each endpoint
    float distance1, distance2;
    if (isHorizontal) {
        distance1 = uv.x - uv1.x;
        distance2 = uv2.x - uv.x;
    } else {
        distance1 = uv.y - uv1.y;
        distance2 = uv2.y - uv.y;
    }
    
    // Determine which endpoint is closer
    bool isDirection1 = distance1 < distance2;
    float distanceFinal = min(distance1, distance2);
    float edgeThickness = distance1 + distance2;
    
    // Calculate pixel offset along edge
    float pixelOffset = -distanceFinal / edgeThickness + 0.5;
    
    // Check if the luma at the closer endpoint is in the same direction as the local gradient
    bool isLumaMSmaller = lumaM < lumaLocalAvg;
    bool correctVariation = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaMSmaller;
    
    float finalOffset = correctVariation ? pixelOffset : 0.0;
    
    // Use the maximum of edge offset and subpixel blend
    finalOffset = max(finalOffset, blendL);
    
    // Apply the offset
    vec2 finalUV = uv;
    if (isHorizontal) {
        finalUV.y += finalOffset * stepLength;
    } else {
        finalUV.x += finalOffset * stepLength;
    }
    
    vec3 finalColor = texture(inputTexture, finalUV).rgb;
    outColor = vec4(finalColor, 1.0);
}
