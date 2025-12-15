#include <catch_amalgamated.hpp>
#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <brimir/hw/vdp/vdp_defs.hpp>

using namespace brimir::vdp;

TEST_CASE("GPU Debug: What is being rendered?", "[gpu][debug]") {
    auto gpuRenderer = CreateRenderer(RendererType::Vulkan);
    
    if (!gpuRenderer || !gpuRenderer->Initialize()) {
        WARN("GPU renderer not available - skipping debug test");
        return;
    }
    
    // Draw a simple red square
    Color555 red{.r = 31, .g = 0, .b = 0, .msb = 0};
    
    gpuRenderer->BeginFrame();
    gpuRenderer->DrawSolidPolygon(100, 100, 200, 100, 200, 200, 100, 200, red);
    gpuRenderer->EndFrame();
    
    auto stats = gpuRenderer->GetStatistics();
    INFO("Draw calls: " << stats.drawCallCount);
    INFO("Triangle count: " << stats.triangleCount);
    
    // Get framebuffer
    const uint32_t* framebuffer = static_cast<const uint32_t*>(gpuRenderer->GetFramebuffer());
    REQUIRE(framebuffer != nullptr);
    
    // Check various pixels
    auto getPixel = [&](int x, int y) {
        uint32_t pixel = framebuffer[y * 320 + x];
        int r = (pixel >> 16) & 0xFF;
        int g = (pixel >> 8) & 0xFF;
        int b = pixel & 0xFF;
        return std::make_tuple(r, g, b);
    };
    
    // Top-left corner (should be black - outside polygon)
    auto [r0, g0, b0] = getPixel(50, 50);
    INFO("Pixel (50,50) - outside polygon: R=" << r0 << " G=" << g0 << " B=" << b0);
    
    // Center of polygon (should be red)
    auto [r1, g1, b1] = getPixel(150, 150);
    INFO("Pixel (150,150) - center of polygon: R=" << r1 << " G=" << g1 << " B=" << b1);
    
    // Edge of polygon
    auto [r2, g2, b2] = getPixel(100, 100);
    INFO("Pixel (100,100) - corner of polygon: R=" << r2 << " G=" << g2 << " B=" << b2);
    
    // Bottom-right (outside)
    auto [r3, g3, b3] = getPixel(250, 250);
    INFO("Pixel (250,250) - outside: R=" << r3 << " G=" << g3 << " B=" << b3);
    
    // Check if center is red
    INFO("Expected center: R=255 G=0 B=0 (red)");
    INFO("Actual center: R=" << r1 << " G=" << g1 << " B=" << b1);
    
    // Count non-black pixels and find bounding box
    int redPixels = 0;
    int minX = 320, maxX = 0, minY = 224, maxY = 0;
    int totalPixels = 320 * 224;
    for (int y = 0; y < 224; ++y) {
        for (int x = 0; x < 320; ++x) {
            int i = y * 320 + x;
            uint32_t pixel = framebuffer[i];
            int r = (pixel >> 16) & 0xFF;
            if (r > 100) {
                redPixels++;
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    INFO("Red pixels: " << redPixels << " out of " << totalPixels);
    INFO("Expected filled square: ~10000 pixels (100x100)");
    INFO("Actual bounding box: (" << minX << "," << minY << ") to (" << maxX << "," << maxY << ")");
    INFO("Expected bounding box: (100,100) to (200,200)");
    INFO("Actual size: " << (maxX - minX + 1) << "x" << (maxY - minY + 1));
    
    // The center should be red (allowing for some conversion error)
    bool isRed = (r1 > 200) && (g1 < 50) && (b1 < 50);
    INFO("Is center red? " << (isRed ? "YES" : "NO"));
    
    // At minimum, we should have SOME red pixels
    REQUIRE(redPixels > 0);
}

