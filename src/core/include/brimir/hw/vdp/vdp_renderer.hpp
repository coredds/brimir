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
    FullPipeline,          // GPU handles VDP1+VDP2+compositing (Approach 3)
};

// Framebuffer format
enum class FramebufferFormat {
    RGB565,    // 16-bit (Libretro default)
    XRGB8888,  // 32-bit (GPU native)
};

// GPU layer indices (matching Saturn VDP2 layer order)
enum class GPULayer : uint8_t {
    Back = 0,       // Back screen (solid color or per-line)
    NBG3 = 1,       // Normal Background 3 (lowest priority BG)
    NBG2 = 2,       // Normal Background 2
    NBG1 = 3,       // Normal Background 1 / EXBG
    NBG0 = 4,       // Normal Background 0 / RBG1
    RBG0 = 5,       // Rotation Background 0
    Sprite = 6,     // VDP1 Sprite layer
    LineColor = 7,  // Line color screen
    Count = 8
};

// GPU layer configuration
struct GPULayerConfig {
    bool enabled = false;
    uint8_t priority = 0;           // 0-7, higher = on top
    bool transparencyEnabled = true;
    bool colorCalcEnabled = false;
    uint8_t colorCalcRatio = 31;    // 0-31 blend ratio
    
    // For scroll layers
    int32_t scrollX = 0;            // Fixed point 11.8
    int32_t scrollY = 0;
    int32_t scrollIncX = 0x100;     // 1.0 in fixed point
    int32_t scrollIncY = 0x100;
    
    // For bitmap layers
    bool isBitmap = false;
    uint32_t bitmapWidth = 512;
    uint32_t bitmapHeight = 256;
    uint32_t bitmapAddress = 0;
    
    // For pattern layers
    uint32_t patternNameAddress = 0;
    uint32_t characterAddress = 0;
    uint8_t characterSize = 0;      // 0 = 1x1 cell, 1 = 2x2 cells
    uint8_t colorFormat = 0;        // ColorFormat enum value
    uint32_t cramOffset = 0;
};

// Renderer capabilities
struct RendererCapabilities {
    RendererType type;
    bool supportsUpscaling;
    bool supportsAntiAliasing;
    bool supportsTextureFiltering;
    bool supportsComputeShaders;
    bool supportsFullPipeline;      // Can do VDP1+VDP2+compositor on GPU
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
    
    // ===== Memory Synchronization (GPU Full Pipeline) =====
    
    /// Upload VDP1 VRAM to GPU texture
    /// @param data Pointer to VDP1 VRAM (512KB)
    /// @param size Size in bytes
    virtual void SyncVDP1VRAM(const uint8_t* data, size_t size) = 0;
    
    /// Upload VDP2 VRAM to GPU texture
    /// @param data Pointer to VDP2 VRAM (512KB, 4 banks of 128KB)
    /// @param size Size in bytes
    virtual void SyncVDP2VRAM(const uint8_t* data, size_t size) = 0;
    
    /// Upload Color RAM to GPU texture
    /// @param data Pointer to CRAM (4KB)
    /// @param size Size in bytes
    /// @param mode CRAM color mode (0=RGB555x1024, 1=RGB555x2048, 2=RGB888x1024)
    virtual void SyncCRAM(const uint8_t* data, size_t size, uint8_t mode) = 0;
    
    /// Upload VDP1 framebuffer to GPU (for sprite layer)
    /// @param data Pointer to sprite framebuffer (256KB)
    /// @param size Size in bytes
    /// @param is8bit True if 8-bit pixel mode, false if 16-bit
    virtual void SyncVDP1Framebuffer(const uint8_t* data, size_t size, bool is8bit) = 0;
    
    // ===== GPU Upscaling (Hybrid Mode) =====
    // These methods enable GPU upscaling of software-rendered output
    // without requiring full GPU VDP1/VDP2 rendering
    
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
    
    // ===== VDP1 Rendering (Sprites/Polygons to Sprite Layer) =====
    
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
    
    // ===== Simplified Drawing API (direct GPU primitives) =====
    
    /// Draw a solid-color polygon (Color555 format)
    virtual void DrawSolidPolygon(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                  int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                                  Color555 color) = 0;
    
    /// Draw a Gouraud-shaded polygon (Color555 format for each vertex)
    virtual void DrawGouraudPolygon(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                    int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                                    Color555 colorA, Color555 colorB,
                                    Color555 colorC, Color555 colorD) = 0;
    
    /// Draw a textured quad (pre-decoded RGBA texture)
    virtual void DrawTexturedQuad(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                  int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                                  const uint32_t* textureData, uint32_t texWidth, uint32_t texHeight,
                                  bool flipH, bool flipV) = 0;
    
    /// Draw a line (rendered as thin quad at high resolution)
    virtual void DrawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Color555 color) = 0;
    
    /// Draw a line with Gouraud shading
    virtual void DrawGouraudLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                  Color555 colorA, Color555 colorB) = 0;
    
    // ===== VDP2 Layer Rendering (GPU Full Pipeline) =====
    
    /// Configure a VDP2 layer
    virtual void SetLayerConfig(GPULayer layer, const GPULayerConfig& config) = 0;
    
    /// Render VDP2 normal background layer to its render target
    /// @param layer Which NBG layer (NBG0-NBG3)
    /// @param y Scanline Y coordinate (for line-based effects)
    virtual void RenderNBGLayer(GPULayer layer, uint32_t y) = 0;
    
    /// Render VDP2 rotation background layer to its render target
    /// @param layer Which RBG layer (RBG0)
    /// @param y Scanline Y coordinate
    virtual void RenderRBGLayer(GPULayer layer, uint32_t y) = 0;
    
    /// Render back screen
    /// @param color Back color (RGB888)
    virtual void RenderBackLayer(uint32_t color) = 0;
    
    // ===== Compositing (GPU Full Pipeline) =====
    
    /// Composite all layers according to priority
    /// This is the final pass that blends all layer render targets
    virtual void CompositeLayers() = 0;
    
    /// Apply color calculation (blending, transparency)
    virtual void ApplyColorCalculation() = 0;
    
    /// Apply deinterlacing
    virtual void ApplyDeinterlacing() = 0;
    
    // ===== Legacy VDP2 interface (for software renderer compatibility) =====
    
    /// Draw VDP2 background layer (NBG0, NBG1, etc.)
    virtual void VDP2DrawBackground(int layer, const VDP2LayerState& state) = 0;
    
    /// Draw VDP2 rotation layer (RBG0, RBG1)
    virtual void VDP2DrawRotation(int layer, const VDP2RotationState& state) = 0;
    
    /// Draw VDP2 sprite layer
    virtual void VDP2DrawSpriteLayer(const VDP2LayerState& state) = 0;
    
    // ===== Configuration =====
    
    /// Set internal rendering scale (1x, 2x, 4x, 8x)
    virtual void SetInternalScale(uint32_t scale) = 0;
    
    /// Get current internal scale
    virtual uint32_t GetInternalScale() const = 0;
    
    /// Set output resolution (for upscaling)
    virtual void SetOutputResolution(uint32_t width, uint32_t height) = 0;
    
    /// Enable/disable texture filtering
    virtual void SetTextureFiltering(bool enable) = 0;
    
    /// Set upscale filter mode (0=nearest, 1=bilinear, 2=sharp bilinear)
    virtual void SetUpscaleFilter(uint32_t mode) = 0;
    
    /// Enable/disable scanline effect
    virtual void SetScanlines(bool enable) = 0;
    
    /// Set brightness multiplier (1.0 = normal)
    virtual void SetBrightness(float brightness) = 0;
    
    /// Set gamma correction (1.0 = linear)
    virtual void SetGamma(float gamma) = 0;
    
    /// Enable/disable FXAA anti-aliasing
    virtual void SetFXAA(bool enable) = 0;
    
    /// Enable/disable MSAA (if supported)
    virtual void SetMSAA(uint32_t samples) = 0;
    
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
        uint64_t vramSyncCount;
        uint64_t cramSyncCount;
        float lastFrameTime;      // milliseconds
        float averageFrameTime;
        float gpuTimeMs;          // GPU-side rendering time
        float compositeTimeMs;    // Compositor pass time
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
