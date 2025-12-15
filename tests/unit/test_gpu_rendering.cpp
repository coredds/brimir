#include <catch_amalgamated.hpp>
#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <cmath>
#include <cstring>

using namespace brimir::vdp;

// Helper to access VulkanRenderer draw methods
// Since VulkanRenderer is in the .cpp file, we use the base interface
class TestableGPURenderer {
public:
    static void DrawTriangle(IVDPRenderer* renderer, 
                            float x0, float y0, float x1, float y1, float x2, float y2,
                            float r, float g, float b, float a) {
        // For now, we'll test through the base interface
        // In full implementation, VDP would call these methods
    }
    
    static void DrawQuad(IVDPRenderer* renderer,
                        float x0, float y0, float x1, float y1, 
                        float x2, float y2, float x3, float y3,
                        float r, float g, float b, float a) {
    }
};

TEST_CASE("GPU: Basic frame rendering and readback", "[gpu][rendering]") {
    auto renderer = CreateRenderer(RendererType::Vulkan);
    
    if (!renderer || !renderer->Initialize()) {
        WARN("GPU renderer not available - skipping test");
        return;
    }
    
    // Begin frame
    REQUIRE_NOTHROW(renderer->BeginFrame());
    
    // TODO: Draw commands will be added when VDP integration is complete
    // For now, just test that we can render a blank frame
    
    // End frame and render
    REQUIRE_NOTHROW(renderer->EndFrame());
    
    // Get framebuffer
    const uint32_t* framebuffer = static_cast<const uint32_t*>(renderer->GetFramebuffer());
    REQUIRE(framebuffer != nullptr);
    
    uint32_t width = renderer->GetFramebufferWidth();
    uint32_t height = renderer->GetFramebufferHeight();
    
    REQUIRE(width == 320);
    REQUIRE(height == 224);
    
    // Check that framebuffer is cleared to black
    uint32_t firstPixel = framebuffer[0];
    uint8_t r = (firstPixel >> 16) & 0xFF;
    uint8_t g = (firstPixel >> 8) & 0xFF;
    uint8_t b = firstPixel & 0xFF;
    
    INFO("First pixel: R=" << (int)r << " G=" << (int)g << " B=" << (int)b);
    
    // Should be black (background clear color)
    REQUIRE(r == 0);
    REQUIRE(g == 0);
    REQUIRE(b == 0);
}

TEST_CASE("GPU: Multiple frames", "[gpu][rendering]") {
    auto renderer = CreateRenderer(RendererType::Vulkan);
    
    if (!renderer || !renderer->Initialize()) {
        WARN("GPU renderer not available - skipping test");
        return;
    }
    
    // Render 10 frames
    for (int i = 0; i < 10; ++i) {
        REQUIRE_NOTHROW(renderer->BeginFrame());
        REQUIRE_NOTHROW(renderer->EndFrame());
    }
    
    // Verify last frame
    const uint32_t* framebuffer = static_cast<const uint32_t*>(renderer->GetFramebuffer());
    REQUIRE(framebuffer != nullptr);
}

TEST_CASE("GPU: Resolution configuration", "[gpu][rendering]") {
    auto renderer = CreateRenderer(RendererType::Vulkan);
    
    if (!renderer || !renderer->Initialize()) {
        WARN("GPU renderer not available - skipping test");
        return;
    }
    
    // Check default resolution
    REQUIRE(renderer->GetFramebufferWidth() == 320);
    REQUIRE(renderer->GetFramebufferHeight() == 224);
    
    // TODO: Test resolution changes when implemented
}

TEST_CASE("GPU: Performance - empty frames", "[gpu][rendering][perf]") {
    auto renderer = CreateRenderer(RendererType::Vulkan);
    
    if (!renderer || !renderer->Initialize()) {
        WARN("GPU renderer not available - skipping test");
        return;
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Render 60 empty frames
    for (int frame = 0; frame < 60; ++frame) {
        renderer->BeginFrame();
        renderer->EndFrame();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    double avgFrameTime = elapsed.count() / 60.0;
    
    INFO("Average frame time (empty): " << avgFrameTime << "ms");
    INFO("Theoretical FPS: " << (1000.0 / avgFrameTime));
    
    // Should be faster than 33ms (30 FPS) even with GPU→CPU readback
    // Note: Readback is expensive but necessary for libretro
    REQUIRE(avgFrameTime < 33.33);
}

