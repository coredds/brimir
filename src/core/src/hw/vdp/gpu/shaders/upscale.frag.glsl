// Brimir - Software Framebuffer Upscaling Fragment Shader
// Samples software-rendered framebuffer with optional filtering
#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// Software framebuffer texture
layout(set = 0, binding = 0) uniform sampler2D softwareFramebuffer;

layout(push_constant) uniform UpscaleConstants {
    vec2 inputSize;     // Native resolution (e.g., 320x224)
    vec2 outputSize;    // Upscaled resolution (e.g., 1280x896)
    uint filterMode;    // 0 = nearest, 1 = bilinear, 2 = sharp bilinear
    uint scanlines;     // 0 = off, 1 = on
    float brightness;   // 1.0 = normal
    float gamma;        // 1.0 = linear
} pc;

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

void main() {
    vec2 uv = fragTexCoord;
    
    // The input texture is max size (704x512), but actual content is smaller
    // We need to sample only the portion that contains content
    // inputSize contains the actual content dimensions
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
    } else {
        // Sharp bilinear (best of both)
        color = sharpBilinear(softwareFramebuffer, scaledUV, pc.inputSize);
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

