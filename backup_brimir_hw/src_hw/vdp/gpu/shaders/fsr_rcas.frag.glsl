// Brimir - FSR 1.0 RCAS (Robust Contrast Adaptive Sharpening)
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. (MIT License)
// Ported from AMD FidelityFX FSR v1.0.2
#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// Input: the upscaled image (output of EASU or other upscale filter)
layout(set = 0, binding = 0) uniform sampler2D inputTexture;

// Same push constant layout as upscale shader for pipeline layout compatibility
layout(push_constant) uniform UpscaleConstants {
    vec2 inputSize;     // Original native resolution
    vec2 outputSize;    // Upscaled resolution (= texture size for RCAS)
    uint filterMode;    // Reused as RCAS sharpness preset: 0=strong, 1=medium, 2=light
    uint scanlines;     // unused in RCAS
    float brightness;   // unused in RCAS
    float gamma;        // unused in RCAS
} pc;

// Sharpening limit (prevents unnatural results)
#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

// Fast approximate reciprocal (medium precision)
float APrxMedRcpF1(float a) {
    float b = uintBitsToFloat(uint(0x7ef19fff) - floatBitsToUint(a));
    return b * (-b * a + 2.0);
}

float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }

// Compute luminance
float RGBToLuma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    // Sharpness: 0.0 = maximum, higher = less sharp
    // filterMode 0 = strong (0.1), 1 = medium (0.25), 2 = light (0.5)
    float sharpness = 0.25;  // Good default for emulator content

    vec2 texelSize = 1.0 / pc.outputSize;
    vec2 uv = fragTexCoord;

    // 3x3 neighborhood sampling
    //   b
    // d e f
    //   h
    vec3 bC = texture(inputTexture, uv + vec2( 0.0, -texelSize.y)).rgb;
    vec3 dC = texture(inputTexture, uv + vec2(-texelSize.x,  0.0)).rgb;
    vec3 eC = texture(inputTexture, uv).rgb;
    vec3 fC = texture(inputTexture, uv + vec2( texelSize.x,  0.0)).rgb;
    vec3 hC = texture(inputTexture, uv + vec2( 0.0,  texelSize.y)).rgb;

    // Compute luma for sharpening analysis
    float bL = RGBToLuma(bC);
    float dL = RGBToLuma(dC);
    float eL = RGBToLuma(eC);
    float fL = RGBToLuma(fC);
    float hL = RGBToLuma(hC);

    // Min and max of ring
    float mn1L = min(AMin3F1(bL, dL, fL), hL);
    float mx1L = max(AMax3F1(bL, dL, fL), hL);

    // Compute sharpening lobe
    float hitMinL = min(mn1L, eL) / (4.0 * mx1L);
    float hitMaxL = (1.0 - max(mx1L, eL)) / (4.0 * mn1L - 4.0);
    float lobeL = max(-hitMinL, hitMaxL);
    float lobe = max(-FSR_RCAS_LIMIT, min(lobeL, 0.0)) * exp2(-clamp(sharpness, 0.0, 2.0));

    // Denoise: reduce sharpening on noisy areas
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    nz = clamp(abs(nz) * APrxMedRcpF1(
        AMax3F1(AMax3F1(bL, dL, eL), fL, hL) -
        AMin3F1(AMin3F1(bL, dL, eL), fL, hL)
    ), 0.0, 1.0);
    nz = -0.5 * nz + 1.0;
    lobe *= nz;

    // Apply sharpening to RGB using medium-precision reciprocal
    float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
    vec3 result = (lobe * bC + lobe * dC + lobe * hC + lobe * fC + eC) * rcpL;

    outColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}
