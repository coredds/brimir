// Brimir - Software Renderer Implementation
// Wraps existing VDP software rendering (no GPU upscaling support)

#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <array>
#include <chrono>
#include <cstring>

namespace brimir::vdp {

// Software renderer - wraps existing VDP implementation
class SoftwareRenderer final : public IVDPRenderer {
public:
    SoftwareRenderer() = default;
    ~SoftwareRenderer() override = default;
    
    // ===== Lifecycle =====
    
    bool Initialize() override {
        m_framebuffer.fill(0xFF000000);  // Black with full alpha
        m_width = 320;
        m_height = 224;
        m_pitch = m_width * 4;
        m_initialized = true;
        return true;
    }
    
    void Shutdown() override {
        m_initialized = false;
    }
    
    void Reset() override {
        m_framebuffer.fill(0xFF000000);
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
        
        if (m_statistics.averageFrameTime == 0.0f) {
            m_statistics.averageFrameTime = m_statistics.lastFrameTime;
        } else {
            m_statistics.averageFrameTime = 
                m_statistics.averageFrameTime * 0.95f + 
                m_statistics.lastFrameTime * 0.05f;
        }
    }
    
    const void* GetFramebuffer() const override { return m_framebuffer.data(); }
    uint32_t GetFramebufferWidth() const override { return m_width; }
    uint32_t GetFramebufferHeight() const override { return m_height; }
    uint32_t GetFramebufferPitch() const override { return m_pitch; }
    FramebufferFormat GetFramebufferFormat() const override { return FramebufferFormat::XRGB8888; }
    
    // ===== GPU Upscaling (not supported by software renderer) =====
    
    void UploadSoftwareFramebuffer(const uint32_t*, uint32_t, uint32_t, uint32_t) override {}
    bool RenderUpscaled() override { return false; }
    const void* GetUpscaledFramebuffer() const override { return nullptr; }
    uint32_t GetUpscaledWidth() const override { return m_width; }
    uint32_t GetUpscaledHeight() const override { return m_height; }
    uint32_t GetUpscaledPitch() const override { return m_width * sizeof(uint32_t); }
    
    // ===== Configuration =====
    
    void SetInternalScale(uint32_t) override {}
    uint32_t GetInternalScale() const override { return 1; }
    void SetOutputResolution(uint32_t, uint32_t) override {}
    void SetUpscaleFilter(uint32_t) override {}
    void SetDebanding(bool) override {}
    void SetBrightness(float) override {}
    void SetGamma(float) override {}
    void SetFXAA(bool) override {}
    void SetSharpeningMode(uint32_t) override {}
    void SetWireframeMode(bool) override {}
    void SetHWContext(void*) override {}
    
    const char* GetLastError() const override { return "No error (software renderer)"; }
    
    // ===== Capabilities =====
    
    RendererType GetType() const override { return RendererType::Software; }
    
    RendererCapabilities GetCapabilities() const override {
        RendererCapabilities caps{};
        caps.type = RendererType::Software;
        caps.supportsUpscaling = false;
        caps.supportsAntiAliasing = false;
        caps.supportsTextureFiltering = false;
        caps.supportsComputeShaders = false;
        caps.maxTextureSize = 1024;
        caps.maxInternalScale = 1;
        return caps;
    }
    
    bool SupportsFeature(RendererFeature feature) const override {
        return feature == RendererFeature::BasicRendering;
    }
    
    Statistics GetStatistics() const override { return m_statistics; }
    void ResetStatistics() override { m_statistics = {}; }
    
private:
    static constexpr uint32_t kMaxWidth = 704;
    static constexpr uint32_t kMaxHeight = 512;
    static constexpr uint32_t kMaxPixels = kMaxWidth * kMaxHeight;
    
    bool m_initialized = false;
    uint32_t m_width = 320;
    uint32_t m_height = 224;
    uint32_t m_pitch = 640;
    
    std::array<uint32_t, kMaxPixels> m_framebuffer{};
    Statistics m_statistics{};
    std::chrono::high_resolution_clock::time_point m_frameStartTime;
};

// Factory function
std::unique_ptr<IVDPRenderer> CreateSoftwareRenderer() {
    return std::make_unique<SoftwareRenderer>();
}

} // namespace brimir::vdp
