// Brimir - VDP Renderer Abstraction
// Provides GPU upscaling and post-processing of software-rendered output

#pragma once

#include <cstdint>
#include <memory>

namespace brimir::vdp {

// Renderer types
enum class RendererType {
    Auto,      // Auto-detect best available
    Software,  // CPU-based rendering (current implementation)
    GPU,       // GPU-based rendering (Vulkan/OpenGL)
    Vulkan,    // Force Vulkan
    OpenGL,    // Force OpenGL
};

// Renderer features
enum class RendererFeature {
    BasicRendering,        // Can render basic VDP1/VDP2
    InternalUpscaling,     // Can render at higher internal resolution
    AntiAliasing,          // Supports MSAA/SSAA
    TextureFiltering,      // Supports bilinear/trilinear filtering
    ComputeShaders,        // Has compute shader support
    FastDeinterlacing,     // GPU-accelerated deinterlacing
    PostProcessing,        // Can apply post-process effects
};

// Framebuffer format
enum class FramebufferFormat {
    RGB565,    // 16-bit (Libretro default)
    XRGB8888,  // 32-bit (GPU native)
};

// Renderer capabilities
struct RendererCapabilities {
    RendererType type;
    bool supportsUpscaling;
    bool supportsAntiAliasing;
    bool supportsTextureFiltering;
    bool supportsComputeShaders;
    uint32_t maxTextureSize;
    uint32_t maxInternalScale;      // Max upscale factor (1x, 2x, 4x, 8x)
};

// Renderer interface
class IVDPRenderer {
public:
    virtual ~IVDPRenderer() = default;
    
    // ===== Lifecycle =====
    
    /// Initialize renderer
    virtual bool Initialize() = 0;
    
    /// Shutdown renderer and free resources
    virtual void Shutdown() = 0;
    
    /// Reset renderer state
    virtual void Reset() = 0;
    
    // ===== Frame Management =====
    
    /// Begin a new frame
    virtual void BeginFrame() = 0;
    
    /// End frame and prepare for display
    virtual void EndFrame() = 0;
    
    /// Get framebuffer pointer (RGB565 or XRGB8888)
    /// Returns nullptr if not ready
    virtual const void* GetFramebuffer() const = 0;
    
    /// Get framebuffer width
    virtual uint32_t GetFramebufferWidth() const = 0;
    
    /// Get framebuffer height
    virtual uint32_t GetFramebufferHeight() const = 0;
    
    /// Get framebuffer pitch (bytes per line)
    virtual uint32_t GetFramebufferPitch() const = 0;
    
    /// Get framebuffer format
    virtual FramebufferFormat GetFramebufferFormat() const = 0;
    
    // ===== GPU Upscaling (Hybrid Mode) =====
    // These methods enable GPU upscaling of software-rendered output
    
    /// Upload software-rendered framebuffer to GPU for upscaling
    /// @param data Pointer to XRGB8888 framebuffer
    /// @param width Framebuffer width
    /// @param height Framebuffer height
    /// @param pitch Bytes per line
    virtual void UploadSoftwareFramebuffer(const uint32_t* data, uint32_t width, 
                                           uint32_t height, uint32_t pitch) = 0;
    
    /// Render upscaled output to GPU framebuffer
    /// Call after UploadSoftwareFramebuffer to produce upscaled output
    /// @return True if GPU output is ready, false to fall back to software
    virtual bool RenderUpscaled() = 0;
    
    /// Get GPU-upscaled framebuffer (after RenderUpscaled returns true)
    /// @return Pointer to upscaled XRGB8888 framebuffer, or nullptr if not ready
    virtual const void* GetUpscaledFramebuffer() const = 0;
    
    /// Get upscaled framebuffer width
    virtual uint32_t GetUpscaledWidth() const = 0;
    
    /// Get upscaled framebuffer height  
    virtual uint32_t GetUpscaledHeight() const = 0;
    
    /// Get upscaled framebuffer pitch (bytes per row)
    virtual uint32_t GetUpscaledPitch() const = 0;
    
    // ===== Configuration =====
    
    /// Set internal rendering scale (1x, 2x, 4x, 8x)
    virtual void SetInternalScale(uint32_t scale) = 0;
    
    /// Get current internal scale
    virtual uint32_t GetInternalScale() const = 0;
    
    /// Set output resolution (for upscaling)
    virtual void SetOutputResolution(uint32_t width, uint32_t height) = 0;
    
    /// Set upscale filter mode (0=nearest, 1=bilinear, 2=sharp bilinear, 3=FSR)
    virtual void SetUpscaleFilter(uint32_t mode) = 0;
    
    /// Enable/disable color debanding
    virtual void SetDebanding(bool enable) = 0;
    
    /// Set brightness multiplier (1.0 = normal)
    virtual void SetBrightness(float brightness) = 0;
    
    /// Set gamma correction (1.0 = linear)
    virtual void SetGamma(float gamma) = 0;
    
    /// Enable/disable FXAA anti-aliasing
    virtual void SetFXAA(bool enable) = 0;
    
    /// Set sharpening/post-processing mode (0 = off, 1 = FXAA, 2 = RCAS)
    virtual void SetSharpeningMode(uint32_t mode) = 0;
    
    /// Enable/disable wireframe mode (GPU only, for debugging)
    virtual void SetWireframeMode(bool enable) = 0;
    
    /// Set hardware render context (for Vulkan/OpenGL integration with frontend)
    virtual void SetHWContext(void* hw_context) = 0;
    
    // ===== Capabilities =====
    
    /// Get renderer type
    virtual RendererType GetType() const = 0;

    /// Get renderer capabilities
    virtual RendererCapabilities GetCapabilities() const = 0;

    /// Check if feature is supported
    virtual bool SupportsFeature(RendererFeature feature) const = 0;
    
    /// Get last error message (for debugging initialization failures)
    virtual const char* GetLastError() const = 0;
    
    // ===== Debugging =====
    
    /// Get renderer statistics (for profiling)
    struct Statistics {
        uint64_t frameCount;
        uint64_t drawCallCount;
        uint64_t triangleCount;
        uint64_t textureUploadCount;
        float lastFrameTime;      // milliseconds
        float averageFrameTime;
        float gpuTimeMs;          // GPU-side rendering time
    };
    
    virtual Statistics GetStatistics() const = 0;
    
    /// Reset statistics
    virtual void ResetStatistics() = 0;
};

// ===== Renderer Factory =====

/// Create renderer of specified type
/// Returns nullptr if type is unavailable
std::unique_ptr<IVDPRenderer> CreateRenderer(RendererType type);

/// Detect best available renderer
RendererType DetectBestRenderer();

/// Check if renderer type is available
bool IsRendererAvailable(RendererType type);

/// Get renderer type name (for display)
const char* GetRendererTypeName(RendererType type);

} // namespace brimir::vdp
