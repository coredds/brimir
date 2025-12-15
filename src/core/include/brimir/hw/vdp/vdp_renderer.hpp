// Brimir - VDP Renderer Abstraction
// Allows switching between Software and GPU renderers

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include "vdp_defs.hpp"

namespace brimir::vdp {

// Forward declarations
struct VDP1Command;
struct VDP2LayerState;
struct VDP2RotationState;

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
    uint32_t maxInternalScale;  // Max upscale factor (1x, 2x, 4x, 8x)
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
    
    // ===== VDP1 Rendering (Sprites/Polygons) =====
    
    /// Draw VDP1 polygon
    virtual void VDP1DrawPolygon(const VDP1Command& cmd) = 0;
    
    /// Draw VDP1 normal sprite
    virtual void VDP1DrawSprite(const VDP1Command& cmd) = 0;
    
    /// Draw VDP1 scaled sprite
    virtual void VDP1DrawScaledSprite(const VDP1Command& cmd) = 0;
    
    /// Draw VDP1 distorted sprite
    virtual void VDP1DrawDistortedSprite(const VDP1Command& cmd) = 0;
    
    /// Draw VDP1 line
    virtual void VDP1DrawLine(const VDP1Command& cmd) = 0;
    
    /// Draw VDP1 polyline
    virtual void VDP1DrawPolyline(const VDP1Command& cmd) = 0;
    
    // ===== Simplified Drawing API (for direct GPU access) =====
    
    /// Draw a solid-color polygon (Color555 format)
    virtual void DrawSolidPolygon(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                  int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                                  Color555 color) = 0;
    
    /// Draw a Gouraud-shaded polygon (Color555 format for each vertex)
    virtual void DrawGouraudPolygon(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                    int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                                    Color555 colorA, Color555 colorB,
                                    Color555 colorC, Color555 colorD) = 0;
    
    // ===== VDP2 Rendering (Backgrounds) =====
    
    /// Draw VDP2 background layer (NBG0, NBG1, etc.)
    virtual void VDP2DrawBackground(int layer, const VDP2LayerState& state) = 0;
    
    /// Draw VDP2 rotation layer (RBG0, RBG1)
    virtual void VDP2DrawRotation(int layer, const VDP2RotationState& state) = 0;
    
    /// Draw VDP2 sprite layer
    virtual void VDP2DrawSpriteLayer(const VDP2LayerState& state) = 0;
    
    // ===== Compositing =====
    
    /// Composite all layers according to priority
    virtual void CompositeLayers() = 0;
    
    /// Apply color calculation (blending, transparency)
    virtual void ApplyColorCalculation() = 0;
    
    /// Apply deinterlacing
    virtual void ApplyDeinterlacing() = 0;
    
    // ===== Configuration =====
    
    /// Set internal rendering scale (1x, 2x, 4x, 8x)
    virtual void SetInternalScale(uint32_t scale) = 0;
    
    /// Get current internal scale
    virtual uint32_t GetInternalScale() const = 0;
    
    /// Enable/disable texture filtering
    virtual void SetTextureFiltering(bool enable) = 0;
    
    /// Enable/disable MSAA (if supported)
    virtual void SetMSAA(uint32_t samples) = 0;
    
    // ===== Capabilities =====
    
    /// Get renderer type
    virtual RendererType GetType() const = 0;
    
    /// Get renderer capabilities
    virtual RendererCapabilities GetCapabilities() const = 0;
    
    /// Check if feature is supported
    virtual bool SupportsFeature(RendererFeature feature) const = 0;
    
    // ===== Debugging =====
    
    /// Get renderer statistics (for profiling)
    struct Statistics {
        uint64_t frameCount;
        uint64_t drawCallCount;
        uint64_t triangleCount;
        uint64_t textureUploadCount;
        float lastFrameTime;  // milliseconds
        float averageFrameTime;
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

