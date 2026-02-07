#version 450

// VDP1 Fragment Shader
// Handles Saturn VDP1 color modes and texture sampling
// Supports inverse bilinear UV mapping for correct DistortedSprite texturing

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord;
// Flat inputs: quad corner data for inverse bilinear mapping
layout(location = 2) flat in vec2 fragQuadP0;
layout(location = 3) flat in vec2 fragQuadP1;
layout(location = 4) flat in vec2 fragQuadP2;
layout(location = 5) flat in vec2 fragQuadP3;
layout(location = 6) flat in vec2 fragQuadUV0;
layout(location = 7) flat in vec2 fragQuadUV1;
layout(location = 8) flat in vec2 fragQuadUV2;
layout(location = 9) flat in vec2 fragQuadUV3;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;  // VDP1 VRAM texture

layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    uint flags;
    uint priority;
} pc;

// Flags
const uint FLAG_TEXTURED = 1u << 0;
const uint FLAG_GOURAUD = 1u << 1;
const uint FLAG_HALF_LUMINANCE = 1u << 2;
const uint FLAG_HALF_TRANSPARENT = 1u << 3;
const uint FLAG_BILERP_UV = 1u << 4;

// 2D cross product
float cross2d(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// Inverse bilinear interpolation (based on Inigo Quilez)
// Given point p inside quad (a,b,c,d), compute (s,t) in [0,1]^2 such that:
//   p = (1-s)(1-t)*a + s*(1-t)*b + s*t*c + (1-s)*t*d
// Returns (-1,-1) if no valid solution.
vec2 invBilinear(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d) {
    vec2 e = b - a;
    vec2 f = d - a;
    vec2 g = a - b + c - d;
    vec2 h = p - a;

    float k2 = cross2d(g, f);
    float k1 = cross2d(e, f) + cross2d(h, g);
    float k0 = cross2d(h, e);

    float u, v;

    // If edges are parallel (rectangle/parallelogram), linear equation
    if (abs(k2) < 0.001) {
        v = -k0 / k1;
        // Use the more numerically stable component for u
        float denomX = e.x + g.x * v;
        float denomY = e.y + g.y * v;
        if (abs(denomX) > abs(denomY)) {
            u = (h.x - f.x * v) / denomX;
        } else {
            u = (h.y - f.y * v) / denomY;
        }
    } else {
        // Quadratic equation
        float w = k1 * k1 - 4.0 * k0 * k2;
        if (w < 0.0) return vec2(-1.0);
        w = sqrt(w);

        float ik2 = 0.5 / k2;
        v = (-k1 - w) * ik2;

        // Compute u using the more stable component
        vec2 denom = e + g * v;
        if (abs(denom.x) > abs(denom.y)) {
            u = (h.x - f.x * v) / denom.x;
        } else {
            u = (h.y - f.y * v) / denom.y;
        }

        // If first root is invalid, try second root
        if (u < -0.01 || u > 1.01 || v < -0.01 || v > 1.01) {
            v = (-k1 + w) * ik2;
            denom = e + g * v;
            if (abs(denom.x) > abs(denom.y)) {
                u = (h.x - f.x * v) / denom.x;
            } else {
                u = (h.y - f.y * v) / denom.y;
            }
        }
    }

    return vec2(u, v);
}

void main() {
    vec4 color = fragColor;

    // Sample texture if textured mode
    if ((pc.flags & FLAG_TEXTURED) != 0u) {
        vec2 uv;

        if ((pc.flags & FLAG_BILERP_UV) != 0u) {
            // Measure how non-rectangular this quad is.
            // g = a - b + c - d: zero for parallelograms, nonzero for trapezoids/arbitrary quads.
            // Compare to edge length to get a relative distortion metric.
            vec2 e = fragQuadP1 - fragQuadP0;
            vec2 f = fragQuadP3 - fragQuadP0;
            vec2 g = fragQuadP0 - fragQuadP1 + fragQuadP2 - fragQuadP3;
            float edgeLen = max(length(e), max(length(f), 1.0));
            float distortion = length(g) / edgeLen;

            if (distortion > 0.02) {
                // Significantly non-rectangular quad: use inverse bilinear for correct UV
                vec2 fragPos = gl_FragCoord.xy;
                vec2 st = invBilinear(fragPos, fragQuadP0, fragQuadP1, fragQuadP2, fragQuadP3);

                // Validate result; fall back to linear UV if invalid
                if (st.x >= -0.01 && st.x <= 1.01 && st.y >= -0.01 && st.y <= 1.01) {
                    st = clamp(st, 0.0, 1.0);
                    uv = (1.0 - st.x) * (1.0 - st.y) * fragQuadUV0
                       + st.x * (1.0 - st.y) * fragQuadUV1
                       + st.x * st.y * fragQuadUV2
                       + (1.0 - st.x) * st.y * fragQuadUV3;
                } else {
                    // Invalid inverse bilinear result: fall back to linear UV
                    uv = fragTexCoord;
                }
            } else {
                // Nearly rectangular quad: linear UV is accurate and stable
                // (avoids sub-texel jitter with nearest-neighbor filtering)
                uv = fragTexCoord;
            }
        } else {
            // Standard linear UV interpolation (for Normal/Scaled sprites)
            uv = fragTexCoord;
        }

        vec4 texColor = texture(texSampler, uv);

        // Saturn MSB transparency check
        if (texColor.a < 0.5) {
            discard;
        }

        // Apply Gouraud shading (modulate texture by vertex color)
        if ((pc.flags & FLAG_GOURAUD) != 0u) {
            color = texColor * fragColor;
        } else {
            color = texColor;
        }
    }

    // Half-luminance effect
    if ((pc.flags & FLAG_HALF_LUMINANCE) != 0u) {
        color.rgb *= 0.5;
    }

    // Half-transparent (50% alpha)
    if ((pc.flags & FLAG_HALF_TRANSPARENT) != 0u) {
        color.a = 0.5;
    }

    outColor = color;
}
