# Internal Resolution Upscaling & Texture Filtering Assessment

**Date:** 2025-11-29  
**Status:** Technical Feasibility Study

---

## Executive Summary

### TL;DR

| Feature | Feasibility | Performance Impact | Implementation Effort | Recommended? |
|---------|-------------|-------------------|----------------------|--------------|
| **2x Upscaling** | ✅ Possible | ⚠️ **4× slower** (60 FPS → 15 FPS) | 🟡 Moderate (2-3 weeks) | ⚠️ Only with future GPU acceleration |
| **4x Upscaling** | ✅ Possible | 🔴 **16× slower** (60 FPS → 4 FPS) | 🟡 Moderate (2-3 weeks) | ❌ Not viable in software |
| **Texture Filtering** | ✅ Possible | 🟡 **~30% slower** (60 FPS → 45 FPS) | 🟢 Easy (3-5 days) | ✅ Could work as optional feature |

**Bottom Line**: Both features are technically feasible but **performance-prohibitive with current software rendering**. They should be considered **only after implementing a hardware-accelerated (OpenGL/Vulkan) rendering backend**.

---

## Current Architecture Overview

### Rendering Pipeline

Brimir uses Ymir's software-based rendering pipeline:

```
Saturn Hardware Layer:
┌─────────────────────────────────────────────────────┐
│  VDP1 (Sprites/Polygons)   VDP2 (Backgrounds)       │
│         ↓                         ↓                  │
│   512×512 Framebuffer      Tile-based layers        │
│         ↓                         ↓                  │
│         └─────────┬───────────────┘                  │
│                   ↓                                  │
│          Per-pixel composition                       │
│                   ↓                                  │
│    Final framebuffer (e.g., 704×448)                │
└─────────────────────────────────────────────────────┘
                    ↓
            libretro output
         (RGB565, native resolution)
```

### Key Components

1. **VDP1 (Sprite Layer)**
   - Rasterizes sprites, polygons, and lines pixel-by-pixel
   - Texture-mapped sprites for 3D games
   - Outputs to 512×512 intermediate framebuffer
   - **Line-by-line rendering** via `VDP1PlotPixel()`

2. **VDP2 (Background Layers)**
   - Tile-based rendering (8×8 pixel tiles)
   - Recent optimization: **Tile-row caching** (renders 8 pixels at once)
   - Rotation/scaling layers (RBG0/RBG1)
   - **Per-scanline rendering** via `VDP2DrawLine()`

3. **Composition**
   - Per-pixel priority sorting (6 layers + sprites)
   - Color calculation (alpha blending, gouraud shading)
   - Window masking (partially SIMD-accelerated)

### Performance Characteristics

**Current Performance (v0.1.3):**
- 320×224 mode: 120+ FPS (low-res, 71,680 pixels/frame)
- 704×448 mode: 60+ FPS (hi-res, 315,392 pixels/frame)
- **Performance is linear with pixel count** (software rendering bottleneck)

---

## Feature 1: Internal Resolution Upscaling

### What It Is

Instead of rendering at native Saturn resolution, render at 2× or 4× scale:

| Mode | Native | 2× Upscale | 4× Upscale |
|------|--------|------------|------------|
| Low-res | 320×224 (71,680px) | 640×448 (286,720px) | 1280×896 (1,146,880px) |
| Hi-res | 704×448 (315,392px) | 1408×896 (1,261,568px) | 2816×1792 (5,046,272px) |

### Technical Feasibility: ✅ **POSSIBLE**

#### Implementation Approach

**VDP2 (Backgrounds) - Straightforward:**

```cpp
// Current: Render 1 pixel
pixel = VDP2FetchCharacterPixel(bgParams, ch, {dotX, dotY});
framebuffer[y * width + x] = pixel.color;

// With 2× upscaling: Render 2×2 block
pixel = VDP2FetchCharacterPixel(bgParams, ch, {dotX, dotY});
for (int dy = 0; dy < 2; dy++) {
    for (int dx = 0; dx < 2; dx++) {
        framebuffer[(y*2 + dy) * (width*2) + (x*2 + dx)] = pixel.color;
    }
}
```

**VDP1 (Sprites) - More Complex:**

```cpp
// Current: Plot pixel at (x, y)
VDP1PlotPixel({x, y}, pixelParams);

// With 2× upscaling: Plot 2×2 block
for (int dy = 0; dy < 2; dy++) {
    for (int dx = 0; dx < 2; dx++) {
        VDP1PlotPixel({x*2 + dx, y*2 + dy}, pixelParams);
    }
}
```

**Challenges:**

1. **VDP1 Framebuffer Size**
   - Current: Fixed 512×512 or 512×256 buffer
   - Upscaled: Would need 1024×1024 or 2048×2048
   - **Memory increase**: 4× for 2× upscale, 16× for 4× upscale

2. **Line Rendering**
   - Bresenham line algorithm needs modification
   - Anti-aliasing becomes more complex

3. **Special Effects**
   - Mesh patterns (dithering)
   - Color calculation boundaries
   - May not scale correctly

#### Files to Modify

| File | Changes Required |
|------|------------------|
| `vdp.cpp` | Update `UpdateResolution()` to apply upscale multiplier |
| `vdp.cpp` | Modify `VDP1PlotPixel()` to render pixel blocks |
| `vdp.cpp` | Modify `VDP2DrawNormalScrollBG()` to scale tile output |
| `vdp.cpp` | Update framebuffer allocation sizes |
| `core_wrapper.cpp` | Add upscale option, adjust output resolution |

**Estimated Effort:** 2-3 weeks (VDP1 line rendering is tricky)

### Performance Impact: 🔴 **CATASTROPHIC**

#### Pixel Count Analysis

| Resolution | Pixels/Frame | FPS @ Current Performance | 2× Upscale Pixels | 2× FPS | 4× Upscale Pixels | 4× FPS |
|------------|--------------|--------------------------|-------------------|--------|-------------------|--------|
| 320×224 | 71,680 | 120+ | 286,720 | **~30 FPS** | 1,146,880 | **~7.5 FPS** |
| 704×448 | 315,392 | 60 | 1,261,568 | **~15 FPS** | 5,046,272 | **~4 FPS** |

**Why So Slow?**

1. **4× more pixels** for 2× upscale, **16× more pixels** for 4× upscale
2. **Software rasterization** doesn't benefit from modern GPU parallel processing
3. **Tile cache becomes less effective** (cache misses increase with scale)
4. **Composition overhead** scales linearly with pixel count

#### Real-World Example

Running Panzer Dragoon Zwei menu (704×448 mode):
- **Current:** 60 FPS ✅
- **2× upscale (1408×896):** ~15 FPS ❌ (unplayable)
- **4× upscale (2816×1792):** ~4 FPS ❌ (slideshow)

### Recommendation: ⚠️ **Wait for Hardware Acceleration**

Internal upscaling **should not be implemented** until a hardware-accelerated rendering backend is available. The performance penalty is too severe for software rendering.

---

## Feature 2: Texture Filtering

### What It Is

Apply bilinear or nearest-neighbor filtering when scaling textures/tiles:

**Without Filtering (Nearest-Neighbor):**
```
Original:  [A][B]
Scaled:    [A][A][B][B]
           [A][A][B][B]
Result: Sharp, pixelated (Saturn's native look)
```

**With Bilinear Filtering:**
```
Original:  [A][B]
Scaled:    [A  ][AB ][B  ]
           [A  ][AB ][B  ]
Result: Smooth, blurred (modern look)
```

### Technical Feasibility: ✅ **POSSIBLE**

#### Where Filtering Applies

**1. VDP1 Texture-Mapped Sprites (3D Games)**

Saturn's VDP1 can render texture-mapped quads (used in games like Panzer Dragoon, Virtua Fighter):

```cpp
// Current: Fetch texture pixel
uint16 texelColor = FetchTexture(textureAddr, u, v);

// With bilinear filtering:
Color texelColor = BilinearSample(textureAddr, u_float, v_float);

Color BilinearSample(uint32 texAddr, float u, float v) {
    // Get 4 surrounding texels
    int u0 = floor(u), u1 = u0 + 1;
    int v0 = floor(v), v1 = v0 + 1;
    float fu = u - u0, fv = v - v0;
    
    Color c00 = FetchTexture(texAddr, u0, v0);
    Color c10 = FetchTexture(texAddr, u1, v0);
    Color c01 = FetchTexture(texAddr, u0, v1);
    Color c11 = FetchTexture(texAddr, u1, v1);
    
    // Bilinear interpolation
    Color c0 = Lerp(c00, c10, fu);
    Color c1 = Lerp(c01, c11, fu);
    return Lerp(c0, c1, fv);
}
```

**2. VDP2 Rotation/Scaling Layers**

When RBG0/RBG1 layers are rotated/scaled, texels need filtering:

```cpp
// Current: Nearest-neighbor
Pixel pixel = VDP2FetchScrollPixel(layer, x_int, y_int);

// With filtering:
Pixel pixel = VDP2BilinearFetchScrollPixel(layer, x_float, y_float);
```

**3. VDP2 Normal Tiles (2D Games)**

Filtering here is **controversial** - would make pixel art look blurry:

```cpp
// Probably should NOT filter regular tiles (looks bad)
// But could be an option for users who prefer smooth look
```

#### Files to Modify

| File | Changes Required |
|------|------------------|
| `vdp.cpp` | Add `BilinearSample()` helper function |
| `vdp.cpp` | Modify VDP1 sprite rasterization to use bilinear sampling |
| `vdp.cpp` | Modify VDP2 rotation layer rendering for filtering |
| `core_wrapper.cpp` | Add filtering option (Off/Bilinear/Trilinear) |

**Estimated Effort:** 3-5 days (straightforward implementation)

### Performance Impact: 🟡 **MODERATE**

#### Overhead Analysis

**Bilinear Filtering Cost:**
- **4× texture reads** instead of 1 (fetch 2×2 texel block)
- **3 linear interpolations** (2 horizontal, 1 vertical)
- **Per-pixel overhead**: ~4 reads + 6 multiplies + 3 adds

**Estimated Performance:**

| Scenario | Current FPS | With Bilinear | % Loss |
|----------|-------------|---------------|--------|
| 3D-heavy (Panzer Dragoon) | 60 | ~42-45 | 25-30% |
| 2D-only (Saturn Bomberman) | 60+ | 60+ | <5% (minimal filtering needed) |
| Hi-res menu (704×448) | 60 | ~45-50 | 17-25% |

**Why Not Worse?**

- Only affects texture-mapped sprites and rotation layers
- Normal tiles typically don't need filtering
- SIMD can accelerate interpolation (4 pixels at once)

### Visual Impact

**Pros:**
- Smoother appearance in 3D games
- Reduces pixelation on rotated/scaled sprites
- More "modern" look (similar to PlayStation filtering)

**Cons:**
- **Loses Saturn's characteristic sharp pixel art aesthetic**
- Can make text blurry
- Some users strongly prefer crisp pixels

### Recommendation: ✅ **Could Work as Optional Feature**

Texture filtering is **worth implementing** if:
1. Made **optional** (core option: Off / Bilinear)
2. Applied **only** to:
   - VDP1 texture-mapped sprites
   - VDP2 rotation/scaling layers
3. **NOT** applied to normal VDP2 tiles (keeps 2D games crisp)

**Priority:** Low (post-v1.0 feature, after JIT and compatibility work)

---

## Alternative: RetroArch-Side Filtering

### Why It's Better (For Now)

RetroArch already supports GPU-based filtering shaders:

```
Brimir Core (software render)
    ↓ (Native 704×448 RGB565)
RetroArch Frontend
    ↓ (Apply shader)
GPU (xBR, CRT, SABR, etc.)
    ↓ (Filtered 1080p/1440p/4K)
Display
```

**Advantages:**
- **Zero performance cost** to the core
- **User choice** of filtering algorithm (bilinear, xBR, CRT scanlines, etc.)
- **Works today** without code changes
- **GPU-accelerated** (fast)

**How to Enable:**
1. RetroArch → Quick Menu → Shaders
2. Load Shader Preset → `bilinear.glslp` (smooth) or `sharp-bilinear.glslp` (balanced)
3. For advanced: `xbr` or `sabr` presets (edge-detection upscaling)

---

## Future Path: Hardware-Accelerated Rendering

### The Long-Term Solution

Both upscaling and filtering become **trivial** with a GPU-based renderer:

```
VDP1 → OpenGL/Vulkan framebuffer (1024×1024) → Render at any resolution
VDP2 → OpenGL/Vulkan tile textures → Bilinear sampling for free
Composite → GPU fragment shader → 4K output at 60 FPS
```

### Precedents

| Emulator | Approach |
|----------|----------|
| **Mednafen** | Software-only (like Brimir) |
| **Beetle Saturn (libretro)** | Software + optional Vulkan HW renderer |
| **Kronos** | OpenGL-based, supports upscaling |
| **SSF** | Software-only |
| **Yaba Sanshiro** | OpenGL/Vulkan, 4K upscaling |

### Implementation Roadmap

**Phase 1: Software Optimization (Current)**
- ✅ Tile caching (v0.1.3)
- ✅ SIMD acceleration (v0.1.3)
- 🔜 SH-2 JIT (2026 goal)

**Phase 2: Hardware Renderer (2026-2027?)**
- OpenGL/Vulkan backend
- Render VDP1/VDP2 to GPU textures
- Internal upscaling (2×/4×/8×)
- Free bilinear/trilinear filtering
- **Maintain software path for accuracy testing**

**Phase 3: Advanced Features**
- Widescreen hacks
- Texture replacement
- Higher polygon precision

---

## Recommendations Summary

### Immediate (v0.1.x - v0.2.x)

❌ **Do NOT implement upscaling** - performance penalty is unacceptable  
❌ **Do NOT implement filtering** - low priority, RetroArch can handle it  
✅ **Focus on:** SH-2 JIT, compatibility, handheld optimization

### Medium-Term (v0.3.x - v0.5.x)

✅ **Consider bilinear filtering** as optional post-JIT feature  
🟡 **Research hardware renderer** architecture  
📝 **Document current rendering** for future HW backend reference

### Long-Term (v1.0+)

✅ **Hardware-accelerated rendering backend** (OpenGL/Vulkan)  
✅ **Internal upscaling** (2×/4×/8×) via HW renderer  
✅ **Advanced filtering** (bilinear/trilinear) via GPU  
✅ **Optional accuracy mode** (keep software path)

---

## Technical References

### Relevant Code Paths

**VDP1 Pixel Plotting:**
- `src/ymir/libs/ymir-core/src/ymir/hw/vdp/vdp.cpp:2118` - `VDP1PlotPixel()`
- Would need 2×2 or 4×4 block writes for upscaling

**VDP2 Tile Rendering:**
- `src/ymir/libs/ymir-core/src/ymir/hw/vdp/vdp.cpp:6709` - `VDP2FetchCharacterPixel()`
- `src/ymir/libs/ymir-core/src/ymir/hw/vdp/vdp.cpp:6843` - `VDP2RenderTile8Pixels()` (batch renderer)
- Tile cache would need redesign for upscaled tiles

**Resolution Management:**
- `src/ymir/libs/ymir-core/src/ymir/hw/vdp/vdp.cpp:906` - `UpdateResolution()`
- `src/ymir/libs/ymir-core/include/ymir/hw/vdp/vdp_defs.hpp:25` - `kMaxResH/kMaxResV`

**Framebuffer Output:**
- `src/bridge/core_wrapper.cpp:725` - libretro output callback
- Would need to pass upscaled dimensions to frontend

### Performance Profiling Commands

To verify these estimates, profile with:

```bash
# Enable internal profiling (if re-added)
# Test native vs 2× vs 4× pixel counts

# RetroArch FPS display
retroarch --verbose --log-file=perf.log <rom>
```

---

## Conclusion

| Question | Answer |
|----------|--------|
| **Can we upscale?** | Yes, technically possible |
| **Should we upscale?** | Not with software rendering - wait for HW backend |
| **Can we filter?** | Yes, with ~30% perf cost |
| **Should we filter?** | Maybe as optional feature, but RetroArch shaders work better |
| **What's the priority?** | Low - focus on JIT and compatibility first |

**Final Verdict:** Both features are **technically feasible but strategically premature**. The current software renderer is already optimized to its limit. These features should be revisited **after implementing a hardware-accelerated rendering backend** (post-v1.0 timeframe).

For users wanting upscaling/filtering **today**, recommend using RetroArch's built-in shader system (GPU-based, zero perf cost).

---

**Author:** Brimir Development Team  
**Related:** [ROADMAP.md](ROADMAP.md), [PERFORMANCE_ROADMAP.md](PERFORMANCE_ROADMAP.md)

