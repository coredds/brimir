// Brimir - Software Renderer Implementation
// Wraps existing VDP software rendering

#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <array>
#include <cstring>

namespace brimir::vdp {

// Software renderer - wraps existing VDP implementation
class SoftwareRenderer final : public IVDPRenderer {
public:
    SoftwareRenderer() = default;
    ~SoftwareRenderer() override = default;
    
    // ===== Lifecycle =====
    
    bool Initialize() override {
        // Initialize framebuffer
        m_framebuffer.fill(0xFF000000);  // Black with full alpha
        m_width = 320;   // Default Saturn resolution
        m_height = 224;
        m_pitch = m_width * 4;  // XRGB8888 = 4 bytes per pixel
        
        m_initialized = true;
        return true;
    }
    
    void Shutdown() override {
        m_initialized = false;
    }
    
    void Reset() override {
        m_framebuffer.fill(0xFF000000);  // Black with full alpha
        m_statistics = {};
    }
    
    // ===== Frame Management =====
    
    void BeginFrame() override {
        if (!m_initialized) return;
        
        m_frameStartTime = std::chrono::high_resolution_clock::now();
    }
    
    void EndFrame() override {
        if (!m_initialized) return;
        
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            now - m_frameStartTime);
        
        m_statistics.frameCount++;
        m_statistics.lastFrameTime = duration.count() / 1000.0f;
        
        // Update average (simple moving average)
        if (m_statistics.averageFrameTime == 0.0f) {
            m_statistics.averageFrameTime = m_statistics.lastFrameTime;
        } else {
            m_statistics.averageFrameTime = 
                m_statistics.averageFrameTime * 0.95f + 
                m_statistics.lastFrameTime * 0.05f;
        }
    }
    
    const void* GetFramebuffer() const override {
        return m_framebuffer.data();
    }
    
    uint32_t GetFramebufferWidth() const override {
        return m_width;
    }
    
    uint32_t GetFramebufferHeight() const override {
        return m_height;
    }
    
    uint32_t GetFramebufferPitch() const override {
        return m_pitch;
    }
    
    FramebufferFormat GetFramebufferFormat() const override {
        return FramebufferFormat::XRGB8888;
    }
    
    // ===== Memory Synchronization (not used by software renderer) =====
    
    void SyncVDP1VRAM(const uint8_t* data, size_t size) override {
        // Software renderer doesn't need VRAM sync - VDP handles it directly
        (void)data; (void)size;
    }
    
    void SyncVDP2VRAM(const uint8_t* data, size_t size) override {
        // Software renderer doesn't need VRAM sync - VDP handles it directly
        (void)data; (void)size;
    }
    
    void SyncCRAM(const uint8_t* data, size_t size, uint8_t mode) override {
        // Software renderer doesn't need CRAM sync - VDP handles it directly
        (void)data; (void)size; (void)mode;
    }
    
    void SyncVDP1Framebuffer(const uint8_t* data, size_t size, bool is8bit) override {
        // Software renderer doesn't need FB sync - VDP handles it directly
        (void)data; (void)size; (void)is8bit;
    }
    
    // ===== GPU Upscaling (not supported by software renderer) =====
    
    void UploadSoftwareFramebuffer(const uint32_t* data, uint32_t width, 
                                   uint32_t height, uint32_t pitch) override {
        // Software renderer doesn't support GPU upscaling
        (void)data; (void)width; (void)height; (void)pitch;
    }
    
    bool RenderUpscaled() override {
        // Software renderer doesn't support GPU upscaling - always return false
        return false;
    }
    
    const void* GetUpscaledFramebuffer() const override {
        // Not supported - return nullptr
        return nullptr;
    }
    
    uint32_t GetUpscaledWidth() const override {
        return m_width;  // Return native size
    }
    
    uint32_t GetUpscaledHeight() const override {
        return m_height;  // Return native size
    }
    
    uint32_t GetUpscaledPitch() const override {
        return m_width * sizeof(uint32_t);  // Return native pitch
    }
    
    // ===== VDP1 Rendering =====
    
    void VDP1DrawPolygon(const VDP1Command& cmd) override {
        // TODO: Forward to actual VDP implementation
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawSprite(const VDP1Command& cmd) override {
        // TODO: Forward to actual VDP implementation
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawScaledSprite(const VDP1Command& cmd) override {
        // TODO: Forward to actual VDP implementation
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawDistortedSprite(const VDP1Command& cmd) override {
        // TODO: Forward to actual VDP implementation
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawLine(const VDP1Command& cmd) override {
        // TODO: Forward to actual VDP implementation
        m_statistics.drawCallCount++;
    }
    
    void VDP1DrawPolyline(const VDP1Command& cmd) override {
        // TODO: Forward to actual VDP implementation
        m_statistics.drawCallCount++;
    }
    
    // ===== Simplified Drawing API =====
    
    void DrawSolidPolygon(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                          Color555 color) override {
        // Convert Color555 to XRGB8888
        uint32_t rgb8 = Color555ToRGB8(color);
        
        // Draw quad as two triangles
        DrawTriangle(x0, y0, x1, y1, x2, y2, rgb8, rgb8, rgb8);
        DrawTriangle(x0, y0, x2, y2, x3, y3, rgb8, rgb8, rgb8);
        
        m_statistics.drawCallCount++;
        m_statistics.triangleCount += 2;
    }
    
    void DrawGouraudPolygon(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                            Color555 colorA, Color555 colorB,
                            Color555 colorC, Color555 colorD) override {
        // Convert Color555 to XRGB8888
        uint32_t colorA8 = Color555ToRGB8(colorA);
        uint32_t colorB8 = Color555ToRGB8(colorB);
        uint32_t colorC8 = Color555ToRGB8(colorC);
        uint32_t colorD8 = Color555ToRGB8(colorD);
        
        // Draw quad as two triangles with Gouraud shading
        DrawTriangle(x0, y0, x1, y1, x2, y2, colorA8, colorB8, colorC8);
        DrawTriangle(x0, y0, x2, y2, x3, y3, colorA8, colorC8, colorD8);
        
        m_statistics.drawCallCount++;
        m_statistics.triangleCount += 2;
    }
    
    void DrawTexturedQuad(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                          const uint32_t* textureData, uint32_t texWidth, uint32_t texHeight,
                          bool flipH, bool flipV) override {
        // Software renderer: not used for textured rendering
        // VDP handles textured rendering directly
        // This is just a fallback/stub implementation
        if (!textureData || texWidth == 0 || texHeight == 0) {
            return;
        }
        
        // Sample center pixel as representative color
        uint32_t centerPixel = textureData[(texHeight / 2) * texWidth + (texWidth / 2)];
        
        // Draw as solid color quad
        DrawTriangle(x0, y0, x1, y1, x2, y2, centerPixel, centerPixel, centerPixel);
        DrawTriangle(x0, y0, x2, y2, x3, y3, centerPixel, centerPixel, centerPixel);
        
        m_statistics.drawCallCount++;
        m_statistics.triangleCount += 2;
    }
    
    void DrawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Color555 color) override {
        // Software renderer: lines are handled by VDP directly
        (void)x0; (void)y0; (void)x1; (void)y1; (void)color;
    }
    
    void DrawGouraudLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          Color555 colorA, Color555 colorB) override {
        // Software renderer: lines are handled by VDP directly
        (void)x0; (void)y0; (void)x1; (void)y1; (void)colorA; (void)colorB;
    }
    
    // ===== VDP2 Layer Rendering (GPU Full Pipeline - not used by software) =====
    
    void SetLayerConfig(GPULayer layer, const GPULayerConfig& config) override {
        // Software renderer doesn't use layer configs
        (void)layer; (void)config;
    }
    
    void RenderNBGLayer(GPULayer layer, uint32_t y) override {
        // Software renderer doesn't use per-layer rendering
        (void)layer; (void)y;
    }
    
    void RenderRBGLayer(GPULayer layer, uint32_t y) override {
        // Software renderer doesn't use per-layer rendering
        (void)layer; (void)y;
    }
    
    void RenderBackLayer(uint32_t color) override {
        // Software renderer doesn't use per-layer rendering
        (void)color;
    }
    
    // ===== Legacy VDP2 interface =====
    
    void VDP2DrawBackground(int layer, const VDP2LayerState& state) override {
        // TODO: Forward to actual VDP implementation
        m_statistics.drawCallCount++;
    }
    
    void VDP2DrawRotation(int layer, const VDP2RotationState& state) override {
        // TODO: Forward to actual VDP implementation
        m_statistics.drawCallCount++;
    }
    
    void VDP2DrawSpriteLayer(const VDP2LayerState& state) override {
        // TODO: Forward to actual VDP implementation
        m_statistics.drawCallCount++;
    }
    
    // ===== Compositing =====
    
    void CompositeLayers() override {
        // TODO: Forward to actual VDP implementation
    }
    
    void ApplyColorCalculation() override {
        // TODO: Forward to actual VDP implementation
    }
    
    void ApplyDeinterlacing() override {
        // TODO: Forward to actual VDP implementation
    }
    
    // ===== Configuration =====
    
    void SetInternalScale(uint32_t scale) override {
        // Software renderer doesn't support upscaling
        m_internalScale = 1;
        (void)scale;
    }
    
    uint32_t GetInternalScale() const override {
        return m_internalScale;
    }
    
    void SetOutputResolution(uint32_t width, uint32_t height) override {
        // Software renderer uses native resolution only
        (void)width; (void)height;
    }
    
    void SetTextureFiltering(bool enable) override {
        // Not supported by software renderer
    }
    
    void SetUpscaleFilter(uint32_t mode) override {
        // Not supported by software renderer
        (void)mode;
    }
    
    void SetScanlines(bool enable) override {
        // Not supported by software renderer
        (void)enable;
    }
    
    void SetBrightness(float brightness) override {
        // Not supported by software renderer
        (void)brightness;
    }
    
    void SetGamma(float gamma) override {
        // Not supported by software renderer
        (void)gamma;
    }
    
    void SetFXAA(bool enable) override {
        // Not supported by software renderer
        (void)enable;
    }
    
    void SetMSAA(uint32_t samples) override {
        // Not supported by software renderer
    }
    
    void SetWireframeMode(bool enable) override {
        // Not supported by software renderer
    }
    
    void SetHWContext(void* hw_context) override {
        // Not needed by software renderer
        (void)hw_context;
    }
    
    const char* GetLastError() const override {
        return "No error (software renderer)";
    }
    
    // ===== Capabilities =====
    
    RendererType GetType() const override {
        return RendererType::Software;
    }
    
    RendererCapabilities GetCapabilities() const override {
        RendererCapabilities caps{};
        caps.type = RendererType::Software;
        caps.supportsUpscaling = false;
        caps.supportsAntiAliasing = false;
        caps.supportsTextureFiltering = false;
        caps.supportsComputeShaders = false;
        caps.supportsFullPipeline = false;  // Software uses VDP's native rendering
        caps.maxTextureSize = 1024;  // Arbitrary for software
        caps.maxInternalScale = 1;
        return caps;
    }
    
    bool SupportsFeature(RendererFeature feature) const override {
        switch (feature) {
        case RendererFeature::BasicRendering:
            return true;
        default:
            return false;
        }
    }
    
    // ===== Debugging =====
    
    Statistics GetStatistics() const override {
        return m_statistics;
    }
    
    void ResetStatistics() override {
        m_statistics = {};
    }
    
private:
    static constexpr uint32_t kMaxWidth = 704;
    static constexpr uint32_t kMaxHeight = 512;
    static constexpr uint32_t kMaxPixels = kMaxWidth * kMaxHeight;
    
    bool m_initialized = false;
    uint32_t m_width = 320;
    uint32_t m_height = 224;
    uint32_t m_pitch = 640;
    uint32_t m_internalScale = 1;
    
    std::array<uint32_t, kMaxPixels> m_framebuffer{};  // XRGB8888
    
    Statistics m_statistics{};
    std::chrono::high_resolution_clock::time_point m_frameStartTime;
    
    // ===== Helper Functions =====
    
    // Convert Saturn Color555 to XRGB8888
    // Matches VDP's ConvertRGB555to888() from vdp_defs.hpp line 74
    uint32_t Color555ToRGB8(Color555 color) {
        // Saturn uses 5-bit RGB (0-31 range)
        // VDP expands to 8-bit by left-shifting by 3 (0-248 range, not 0-255)
        // This matches the real VDP behavior exactly
        uint8_t r = color.r << 3;
        uint8_t g = color.g << 3;
        uint8_t b = color.b << 3;
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    
    // Simple triangle rasterizer with Gouraud shading
    void DrawTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      uint32_t color0, uint32_t color1, uint32_t color2) {
        // Sort vertices by Y coordinate (y0 <= y1 <= y2)
        if (y1 < y0) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            std::swap(color0, color1);
        }
        if (y2 < y0) {
            std::swap(x0, x2);
            std::swap(y0, y2);
            std::swap(color0, color2);
        }
        if (y2 < y1) {
            std::swap(x1, x2);
            std::swap(y1, y2);
            std::swap(color1, color2);
        }
        
        // Skip degenerate triangles
        if (y0 == y2) return;
        
        // Scanline rasterization
        // Use < y2 (not <=) to match GPU's top-left fill rule
        for (int32_t y = y0; y < y2; ++y) {
            if (y < 0 || y >= static_cast<int32_t>(m_height)) continue;
            
            // Calculate X bounds for this scanline
            int32_t xStart, xEnd;
            uint32_t colorStart, colorEnd;
            
            if (y < y1) {
                // Top half (y0 -> y1 and y0 -> y2)
                float t1 = (y1 > y0) ? static_cast<float>(y - y0) / (y1 - y0) : 0.0f;
                float t2 = (y2 > y0) ? static_cast<float>(y - y0) / (y2 - y0) : 0.0f;
                // Round to nearest integer (+ 0.5f) for better GPU match
                xStart = static_cast<int32_t>(x0 + t1 * (x1 - x0) + 0.5f);
                xEnd = static_cast<int32_t>(x0 + t2 * (x2 - x0) + 0.5f);
                colorStart = LerpColor(color0, color1, t1);
                colorEnd = LerpColor(color0, color2, t2);
            } else {
                // Bottom half (y1 -> y2 and y0 -> y2)
                float t1 = (y2 > y1) ? static_cast<float>(y - y1) / (y2 - y1) : 0.0f;
                float t2 = (y2 > y0) ? static_cast<float>(y - y0) / (y2 - y0) : 0.0f;
                // Round to nearest integer (+ 0.5f) for better GPU match
                xStart = static_cast<int32_t>(x1 + t1 * (x2 - x1) + 0.5f);
                xEnd = static_cast<int32_t>(x0 + t2 * (x2 - x0) + 0.5f);
                colorStart = LerpColor(color1, color2, t1);
                colorEnd = LerpColor(color0, color2, t2);
            }
            
            if (xStart > xEnd) {
                std::swap(xStart, xEnd);
                std::swap(colorStart, colorEnd);
            }
            
            // Draw horizontal span
            // Use < xEnd (not <=) to match GPU's top-left fill rule
            for (int32_t x = xStart; x < xEnd; ++x) {
                if (x >= 0 && x < static_cast<int32_t>(m_width)) {
                    float t = (xEnd > xStart) ? static_cast<float>(x - xStart) / (xEnd - xStart) : 0.0f;
                    uint32_t color = LerpColor(colorStart, colorEnd, t);
                    m_framebuffer[y * m_width + x] = color;
                }
            }
        }
    }
    
    // Linear interpolate between two XRGB8888 colors
    // Uses proper rounding to match GPU interpolation
    uint32_t LerpColor(uint32_t color0, uint32_t color1, float t) {
        if (color0 == color1) return color0;  // Optimization for solid colors
        
        uint8_t r0 = (color0 >> 16) & 0xFF;
        uint8_t g0 = (color0 >> 8) & 0xFF;
        uint8_t b0 = color0 & 0xFF;
        
        uint8_t r1 = (color1 >> 16) & 0xFF;
        uint8_t g1 = (color1 >> 8) & 0xFF;
        uint8_t b1 = color1 & 0xFF;
        
        // Use proper rounding (+ 0.5f before truncation) to match GPU
        uint8_t r = static_cast<uint8_t>(r0 + t * (r1 - r0) + 0.5f);
        uint8_t g = static_cast<uint8_t>(g0 + t * (g1 - g0) + 0.5f);
        uint8_t b = static_cast<uint8_t>(b0 + t * (b1 - b0) + 0.5f);
        
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }
};

// Factory function
std::unique_ptr<IVDPRenderer> CreateSoftwareRenderer() {
    return std::make_unique<SoftwareRenderer>();
}

} // namespace brimir::vdp

