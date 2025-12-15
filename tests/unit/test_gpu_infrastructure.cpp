// Brimir - GPU Infrastructure Tests
// Tests for GPU renderer initialization, capabilities, and basic functionality

#include "catch_amalgamated.hpp"
#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <cmath>
#include <vector>

using namespace brimir::vdp;

// ===== Helper Functions =====

// Calculate PSNR (Peak Signal-to-Noise Ratio) between two framebuffers
// Higher is better, >40 dB is excellent match
struct FramebufferComparison {
    uint32_t totalPixels;
    uint32_t differentPixels;
    uint32_t maxColorDelta;
    double psnr;  // Peak Signal-to-Noise Ratio (dB)
};

FramebufferComparison CompareFramebuffers(
    const uint16_t* fb1, const uint16_t* fb2, 
    uint32_t width, uint32_t height
) {
    FramebufferComparison result{};
    result.totalPixels = width * height;
    result.differentPixels = 0;
    result.maxColorDelta = 0;
    
    uint64_t mse = 0;  // Mean Squared Error
    
    for (uint32_t i = 0; i < result.totalPixels; i++) {
        uint16_t p1 = fb1[i];
        uint16_t p2 = fb2[i];
        
        if (p1 != p2) {
            result.differentPixels++;
            
            // Extract RGB565 components
            uint8_t r1 = (p1 >> 11) & 0x1F;
            uint8_t g1 = (p1 >> 5) & 0x3F;
            uint8_t b1 = p1 & 0x1F;
            
            uint8_t r2 = (p2 >> 11) & 0x1F;
            uint8_t g2 = (p2 >> 5) & 0x3F;
            uint8_t b2 = p2 & 0x1F;
            
            // Calculate delta (approximate to 8-bit for comparison)
            uint32_t dr = std::abs((r1 << 3) - (r2 << 3));
            uint32_t dg = std::abs((g1 << 2) - (g2 << 2));
            uint32_t db = std::abs((b1 << 3) - (b2 << 3));
            
            uint32_t delta = std::max({dr, dg, db});
            result.maxColorDelta = std::max(result.maxColorDelta, delta);
            
            // MSE in 8-bit space
            mse += dr * dr + dg * dg + db * db;
        }
    }
    
    // Calculate PSNR
    if (mse == 0) {
        result.psnr = 100.0;  // Perfect match
    } else {
        double meanMse = static_cast<double>(mse) / (result.totalPixels * 3);
        result.psnr = 10.0 * std::log10((255.0 * 255.0) / meanMse);
    }
    
    return result;
}

// ===== Renderer Detection Tests =====

TEST_CASE("GPU: Renderer detection", "[gpu][infrastructure]") {
    SECTION("Detect best available renderer") {
        auto bestType = DetectBestRenderer();
        
        // Should return something valid
        REQUIRE((bestType == RendererType::Software ||
                 bestType == RendererType::GPU ||
                 bestType == RendererType::Vulkan ||
                 bestType == RendererType::OpenGL));
        
        INFO("Best renderer: " << GetRendererTypeName(bestType));
    }
    
    SECTION("Check software renderer availability") {
        // Software renderer should always be available
        REQUIRE(IsRendererAvailable(RendererType::Software));
    }
    
    SECTION("Check GPU renderer availability") {
        bool gpuAvailable = IsRendererAvailable(RendererType::GPU);
        INFO("GPU renderer available: " << (gpuAvailable ? "yes" : "no"));
        
        // This is informational - GPU may not be available in CI
        REQUIRE((gpuAvailable == true || gpuAvailable == false));
    }
}

TEST_CASE("GPU: Renderer type names", "[gpu][infrastructure]") {
    REQUIRE(std::string(GetRendererTypeName(RendererType::Auto)) == "Auto");
    REQUIRE(std::string(GetRendererTypeName(RendererType::Software)) == "Software");
    REQUIRE(std::string(GetRendererTypeName(RendererType::GPU)) == "GPU");
    REQUIRE(std::string(GetRendererTypeName(RendererType::Vulkan)) == "Vulkan");
    REQUIRE(std::string(GetRendererTypeName(RendererType::OpenGL)) == "OpenGL");
}

// ===== Renderer Creation Tests =====

TEST_CASE("GPU: Create software renderer", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->GetType() == RendererType::Software);
    
    // Initialize
    bool initialized = renderer->Initialize();
    REQUIRE(initialized);
    
    // Check capabilities
    auto caps = renderer->GetCapabilities();
    REQUIRE(caps.type == RendererType::Software);
    INFO("Max texture size: " << caps.maxTextureSize);
    
    // Cleanup
    renderer->Shutdown();
}

TEST_CASE("GPU: Create GPU renderer (if available)", "[gpu][infrastructure]") {
    if (!IsRendererAvailable(RendererType::GPU)) {
        WARN("GPU renderer not available - skipping test");
        return;
    }
    
    auto renderer = CreateRenderer(RendererType::GPU);
    
    REQUIRE(renderer != nullptr);
    REQUIRE((renderer->GetType() == RendererType::Vulkan ||
             renderer->GetType() == RendererType::OpenGL));
    
    // Initialize
    bool initialized = renderer->Initialize();
    if (!initialized) {
        WARN("GPU renderer failed to initialize - may need graphics context");
        return;
    }
    
    // Check capabilities
    auto caps = renderer->GetCapabilities();
    INFO("GPU type: " << GetRendererTypeName(caps.type));
    INFO("Supports upscaling: " << caps.supportsUpscaling);
    INFO("Max internal scale: " << caps.maxInternalScale << "x");
    INFO("Max texture size: " << caps.maxTextureSize);
    
    // Cleanup
    renderer->Shutdown();
}

// ===== Renderer Lifecycle Tests =====

TEST_CASE("GPU: Renderer initialization and shutdown", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    
    // Initialize
    REQUIRE(renderer->Initialize());
    
    // Should be able to get framebuffer dimensions
    uint32_t width = renderer->GetFramebufferWidth();
    uint32_t height = renderer->GetFramebufferHeight();
    uint32_t pitch = renderer->GetFramebufferPitch();
    
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    REQUIRE(pitch >= width * 2);  // At least RGB565 bytes
    
    // Shutdown
    REQUIRE_NOTHROW(renderer->Shutdown());
}

TEST_CASE("GPU: Renderer reset", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    
    // Reset should not crash
    REQUIRE_NOTHROW(renderer->Reset());
    
    // Should still work after reset
    REQUIRE_NOTHROW(renderer->BeginFrame());
    REQUIRE_NOTHROW(renderer->EndFrame());
    
    renderer->Shutdown();
}

// ===== Feature Detection Tests =====

TEST_CASE("GPU: Feature support detection", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    
    SECTION("Basic rendering") {
        // All renderers must support basic rendering
        REQUIRE(renderer->SupportsFeature(RendererFeature::BasicRendering));
    }
    
    SECTION("Advanced features") {
        // Software renderer typically doesn't support these
        bool supportsUpscaling = renderer->SupportsFeature(RendererFeature::InternalUpscaling);
        bool supportsAA = renderer->SupportsFeature(RendererFeature::AntiAliasing);
        bool supportsFiltering = renderer->SupportsFeature(RendererFeature::TextureFiltering);
        
        INFO("Upscaling: " << supportsUpscaling);
        INFO("Anti-aliasing: " << supportsAA);
        INFO("Texture filtering: " << supportsFiltering);
        
        // These are informational - features may or may not be supported
        REQUIRE((supportsUpscaling == true || supportsUpscaling == false));
    }
    
    renderer->Shutdown();
}

// ===== Frame Management Tests =====

TEST_CASE("GPU: Basic frame rendering", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    
    // Begin frame
    REQUIRE_NOTHROW(renderer->BeginFrame());
    
    // Should be able to get framebuffer
    const void* fb = renderer->GetFramebuffer();
    // May be null until EndFrame, so we don't require it here
    
    // End frame
    REQUIRE_NOTHROW(renderer->EndFrame());
    
    // After EndFrame, framebuffer should be available
    fb = renderer->GetFramebuffer();
    REQUIRE(fb != nullptr);
    
    renderer->Shutdown();
}

TEST_CASE("GPU: Multiple frames", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    
    // Render 10 frames
    for (int i = 0; i < 10; i++) {
        REQUIRE_NOTHROW(renderer->BeginFrame());
        REQUIRE_NOTHROW(renderer->EndFrame());
        
        const void* fb = renderer->GetFramebuffer();
        REQUIRE(fb != nullptr);
    }
    
    renderer->Shutdown();
}

// ===== Configuration Tests =====

TEST_CASE("GPU: Internal scale configuration", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    
    // Default scale should be 1x
    REQUIRE(renderer->GetInternalScale() == 1);
    
    // Try setting scale (may not be supported by software renderer)
    REQUIRE_NOTHROW(renderer->SetInternalScale(2));
    
    // If supported, scale should change
    uint32_t scale = renderer->GetInternalScale();
    INFO("Internal scale: " << scale << "x");
    REQUIRE((scale == 1 || scale == 2));  // Either not supported or changed
    
    renderer->Shutdown();
}

TEST_CASE("GPU: Texture filtering configuration", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    
    // Should not crash (even if not supported)
    REQUIRE_NOTHROW(renderer->SetTextureFiltering(true));
    REQUIRE_NOTHROW(renderer->SetTextureFiltering(false));
    
    renderer->Shutdown();
}

// ===== Statistics Tests =====

TEST_CASE("GPU: Renderer statistics", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    
    // Get initial statistics
    auto stats1 = renderer->GetStatistics();
    REQUIRE(stats1.frameCount == 0);
    
    // Render a frame
    renderer->BeginFrame();
    renderer->EndFrame();
    
    // Statistics should update
    auto stats2 = renderer->GetStatistics();
    REQUIRE(stats2.frameCount > stats1.frameCount);
    
    INFO("Frame count: " << stats2.frameCount);
    INFO("Last frame time: " << stats2.lastFrameTime << "ms");
    
    // Reset statistics
    renderer->ResetStatistics();
    auto stats3 = renderer->GetStatistics();
    REQUIRE(stats3.frameCount == 0);
    
    renderer->Shutdown();
}

// ===== Framebuffer Format Tests =====

TEST_CASE("GPU: Framebuffer format", "[gpu][infrastructure]") {
    auto renderer = CreateRenderer(RendererType::Software);
    REQUIRE(renderer != nullptr);
    REQUIRE(renderer->Initialize());
    
    auto format = renderer->GetFramebufferFormat();
    
    // Should be either RGB565 or XRGB8888
    REQUIRE((format == FramebufferFormat::RGB565 ||
             format == FramebufferFormat::XRGB8888));
    
    INFO("Framebuffer format: " << 
         (format == FramebufferFormat::RGB565 ? "RGB565" : "XRGB8888"));
    
    renderer->Shutdown();
}

// ===== Comparison Tests =====

TEST_CASE("GPU: Framebuffer comparison utility", "[gpu][infrastructure]") {
    // Test the comparison utility itself
    std::vector<uint16_t> fb1(320 * 224, 0x0000);  // Black
    std::vector<uint16_t> fb2(320 * 224, 0x0000);  // Black
    
    SECTION("Identical framebuffers") {
        auto diff = CompareFramebuffers(fb1.data(), fb2.data(), 320, 224);
        
        REQUIRE(diff.totalPixels == 320 * 224);
        REQUIRE(diff.differentPixels == 0);
        REQUIRE(diff.maxColorDelta == 0);
        REQUIRE(diff.psnr > 40.0);  // Perfect match
    }
    
    SECTION("Different framebuffers") {
        fb2[100] = 0xFFFF;  // White pixel
        
        auto diff = CompareFramebuffers(fb1.data(), fb2.data(), 320, 224);
        
        REQUIRE(diff.totalPixels == 320 * 224);
        REQUIRE(diff.differentPixels == 1);
        REQUIRE(diff.maxColorDelta > 0);
        INFO("PSNR with 1 different pixel: " << diff.psnr << " dB");
    }
    
    SECTION("Many different pixels") {
        for (int i = 0; i < 1000; i++) {
            fb2[i] = 0xF800;  // Red pixels
        }
        
        auto diff = CompareFramebuffers(fb1.data(), fb2.data(), 320, 224);
        
        REQUIRE(diff.differentPixels == 1000);
        INFO("PSNR with 1000 different pixels: " << diff.psnr << " dB");
        INFO("Max color delta: " << diff.maxColorDelta);
    }
}

