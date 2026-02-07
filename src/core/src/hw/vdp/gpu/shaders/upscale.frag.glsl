// Brimir - Software Framebuffer Upscaling Fragment Shader
// Supports: Nearest, Bilinear, Sharp Bilinear, and FSR 1.0 EASU
// Based on AMD FidelityFX Super Resolution 1.0 (MIT license)
#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// Software framebuffer texture
layout(set = 0, binding = 0) uniform sampler2D softwareFramebuffer;

layout(push_constant) uniform UpscaleConstants {
    vec2 inputSize;     // Native resolution (e.g., 320x224)
    vec2 outputSize;    // Upscaled resolution (e.g., 1280x896)
    uint filterMode;    // 0 = nearest, 1 = bilinear, 2 = sharp bilinear, 3 = FSR EASU
    uint scanlines;     // 0 = off, 1 = on
    float brightness;   // 1.0 = normal
    float gamma;        // 1.0 = linear
} pc;

// ===== Utility functions =====

// Sharp bilinear filter (reduces blurriness of pure bilinear)
vec4 sharpBilinear(sampler2D tex, vec2 uv, vec2 texSize) {
    vec2 texelSize = 1.0 / texSize;
    vec2 texPos = uv * texSize;
    vec2 texFloor = floor(texPos - 0.5) + 0.5;
    vec2 texFrac = texPos - texFloor;
    
    // Sharpen the interpolation curve
    vec2 sharpFrac = smoothstep(0.0, 1.0, texFrac);
    
    vec2 sharpPos = (texFloor + sharpFrac) * texelSize;
    return texture(tex, sharpPos);
}

// Simple scanline effect
float scanlineIntensity(float y, float outputHeight) {
    float line = mod(y * outputHeight, 2.0);
    return mix(1.0, 0.7, smoothstep(0.0, 1.0, line));
}

// ===== FSR 1.0 EASU (Edge Adaptive Spatial Upsampling) =====
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. (MIT License)
// Ported from AMD FidelityFX FSR v1.0.2

// Fast approximate reciprocal
float APrxLoRcpF1(float a) {
    return uintBitsToFloat(uint(0x7ef07ebb) - floatBitsToUint(a));
}

// Fast approximate inverse square root
float APrxLoRsqF1(float a) {
    return uintBitsToFloat(uint(0x5f347d74) - (floatBitsToUint(a) >> uint(1)));
}

float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }

// Compute luminance from linear RGB
float RGBToLuma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// EASU tap filter: anisotropic Lanczos-like kernel
void FsrEasuTap(
    inout vec3 aC,  // Accumulated color
    inout float aW, // Accumulated weight
    vec2 off,       // Pixel offset from resolve position to tap
    vec2 dir,       // Gradient direction
    vec2 len,       // Length
    float lob,      // Negative lobe strength
    float clp,      // Clipping point
    vec3 c          // Tap color
) {
    // Rotate offset by direction
    vec2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    // Anisotropy
    v *= len;
    // Distance squared
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    // Approximation of lanczos2
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;  // 25/16 * wB - (25/16 - 1)
    float w = wB * wA;
    // Accumulate
    aC += c * w;
    aW += w;
}

// EASU direction and length analysis for one bilinear quadrant
void FsrEasuSet(
    inout vec2 dir, inout float len, vec2 pp,
    bool biS, bool biT, bool biU, bool biV,
    float lA, float lB, float lC, float lD, float lE
) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;

    // Direction is the '+' diff
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = APrxLoRcpF1(lenX);
    float dirX = lD - lB;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = APrxLoRcpF1(lenY);
    float dirY = lE - lA;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;

    dir += vec2(dirX, dirY) * w;
    len += dot(vec2(w), vec2(lenX, lenY));
}

// Main FSR EASU function
vec3 FsrEasu(sampler2D tex, vec2 uv, vec2 inputSize, vec2 outputSize) {
    // Texture max size (our framebuffer is uploaded into a fixed-size texture)
    vec2 textureMaxSize = vec2(704.0, 512.0);
    vec2 texelSize = 1.0 / textureMaxSize;
    
    // Map output pixel to input position
    vec2 pp = uv * outputSize / textureMaxSize;  // Position in texture space [0,1]
    pp = pp * textureMaxSize;  // Position in texel space
    
    // Actually, map the output pixel to the input image coordinates
    // The input image occupies [0, inputSize/textureMaxSize] in UV space
    vec2 inputTexelPos = uv * inputSize;  // Position in input texels
    
    // Get the integer position and fractional part
    vec2 fp = floor(inputTexelPos - 0.5);
    vec2 frac = inputTexelPos - 0.5 - fp;
    
    // 12-tap kernel sampling pattern (sample from input texture at texel centers)
    //   b c
    // e f g h
    // i j k l
    //   n o
    
    // Sample colors (convert texel positions to UV)
    #define SAMPLE(x, y) texture(tex, (fp + vec2(x, y) + 0.5) / textureMaxSize).rgb
    
    vec3 bC = SAMPLE( 0.0, -1.0);
    vec3 cC = SAMPLE( 1.0, -1.0);
    vec3 eC = SAMPLE(-1.0,  0.0);
    vec3 fC = SAMPLE( 0.0,  0.0);
    vec3 gC = SAMPLE( 1.0,  0.0);
    vec3 hC = SAMPLE( 2.0,  0.0);
    vec3 iC = SAMPLE(-1.0,  1.0);
    vec3 jC = SAMPLE( 0.0,  1.0);
    vec3 kC = SAMPLE( 1.0,  1.0);
    vec3 lC = SAMPLE( 2.0,  1.0);
    vec3 nC = SAMPLE( 0.0,  2.0);
    vec3 oC = SAMPLE( 1.0,  2.0);
    
    #undef SAMPLE
    
    // Compute luma for direction analysis
    float bL = RGBToLuma(bC);
    float cL = RGBToLuma(cC);
    float eL = RGBToLuma(eC);
    float fL = RGBToLuma(fC);
    float gL = RGBToLuma(gC);
    float hL = RGBToLuma(hC);
    float iL = RGBToLuma(iC);
    float jL = RGBToLuma(jC);
    float kL = RGBToLuma(kC);
    float lL = RGBToLuma(lC);
    float nL = RGBToLuma(nC);
    float oL = RGBToLuma(oC);
    
    // Direction and length analysis (4-quadrant bilinear)
    vec2 dir = vec2(0.0);
    float len = 0.0;
    FsrEasuSet(dir, len, frac, true,  false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSet(dir, len, frac, false, true,  false, false, cL, fL, gL, hL, kL);
    FsrEasuSet(dir, len, frac, false, false, true,  false, fL, iL, jL, kL, nL);
    FsrEasuSet(dir, len, frac, false, false, false, true,  gL, jL, kL, lL, oL);
    
    // Normalize direction
    float dirR = dir.x * dir.x + dir.y * dir.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = APrxLoRsqF1(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);
    
    // Transform length
    len = len * 0.5;
    len *= len;
    
    // Stretch kernel
    float stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    
    // Negative lobe strength and clipping
    float lob = 0.5 + (0.21 - 0.5) * len;  // 1/4 - 0.04 = 0.21
    float clp = APrxLoRcpF1(lob);
    
    // Accumulate 12 taps (operating on RGB)
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    FsrEasuTap(aC, aW, vec2( 0.0,-1.0) - frac, dir, len2, lob, clp, bC);
    FsrEasuTap(aC, aW, vec2( 1.0,-1.0) - frac, dir, len2, lob, clp, cC);
    FsrEasuTap(aC, aW, vec2(-1.0, 0.0) - frac, dir, len2, lob, clp, eC);
    FsrEasuTap(aC, aW, vec2( 0.0, 0.0) - frac, dir, len2, lob, clp, fC);
    FsrEasuTap(aC, aW, vec2( 1.0, 0.0) - frac, dir, len2, lob, clp, gC);
    FsrEasuTap(aC, aW, vec2( 2.0, 0.0) - frac, dir, len2, lob, clp, hC);
    FsrEasuTap(aC, aW, vec2(-1.0, 1.0) - frac, dir, len2, lob, clp, iC);
    FsrEasuTap(aC, aW, vec2( 0.0, 1.0) - frac, dir, len2, lob, clp, jC);
    FsrEasuTap(aC, aW, vec2( 1.0, 1.0) - frac, dir, len2, lob, clp, kC);
    FsrEasuTap(aC, aW, vec2( 2.0, 1.0) - frac, dir, len2, lob, clp, lC);
    FsrEasuTap(aC, aW, vec2( 0.0, 2.0) - frac, dir, len2, lob, clp, nC);
    FsrEasuTap(aC, aW, vec2( 1.0, 2.0) - frac, dir, len2, lob, clp, oC);
    
    // Normalize
    vec3 result = aC / aW;
    
    // Dering: clamp to min/max of 4 nearest texels
    vec3 min4 = min(min(fC, gC), min(jC, kC));
    vec3 max4 = max(max(fC, gC), max(jC, kC));
    result = clamp(result, min4, max4);
    
    return clamp(result, 0.0, 1.0);
}

// ===== Main =====

void main() {
    vec2 uv = fragTexCoord;
    
    // The input texture is max size (704x512), but actual content is smaller
    vec2 textureMaxSize = vec2(704.0, 512.0);
    vec2 uvScale = pc.inputSize / textureMaxSize;
    vec2 scaledUV = uv * uvScale;
    
    // Clamp UVs to valid content range
    scaledUV = clamp(scaledUV, vec2(0.0), uvScale);
    
    vec4 color;
    
    if (pc.filterMode == 0u) {
        // Nearest neighbor (sharp pixels)
        color = texture(softwareFramebuffer, scaledUV);
    } else if (pc.filterMode == 1u) {
        // Bilinear (smooth)
        color = texture(softwareFramebuffer, scaledUV);
    } else if (pc.filterMode == 2u) {
        // Sharp bilinear (best of both)
        color = sharpBilinear(softwareFramebuffer, scaledUV, pc.inputSize);
    } else {
        // FSR 1.0 EASU (edge-adaptive upscaling)
        color = vec4(FsrEasu(softwareFramebuffer, uv, pc.inputSize, pc.outputSize), 1.0);
    }
    
    // Apply scanlines if enabled
    if (pc.scanlines != 0u) {
        float scanline = scanlineIntensity(fragTexCoord.y, pc.outputSize.y);
        color.rgb *= scanline;
    }
    
    // Apply brightness and gamma
    color.rgb *= pc.brightness;
    color.rgb = pow(color.rgb, vec3(1.0 / pc.gamma));
    
    outColor = vec4(color.rgb, 1.0);
}
