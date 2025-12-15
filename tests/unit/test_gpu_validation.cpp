#include <catch_amalgamated.hpp>
#include <brimir/hw/vdp/vdp_renderer.hpp>
#include <brimir/hw/vdp/vdp_defs.hpp>
#include <cmath>
#include <cstring>
#include <vector>

using namespace brimir::vdp;

// ===== PSNR Calculation (Peak Signal-to-Noise Ratio) =====
// Used to measure image quality - higher is better
// >40 dB = excellent quality (visually identical)
// 30-40 dB = good quality (minor differences)
// <30 dB = poor quality (visible artifacts)

double CalculatePSNR(const uint32_t* image1, const uint32_t* image2, uint32_t width, uint32_t height) {
    double mse = 0.0;
    uint32_t pixelCount = width * height;
    
    for (uint32_t i = 0; i < pixelCount; ++i) {
        uint32_t p1 = image1[i];
        uint32_t p2 = image2[i];
        
        // Extract RGB components (XRGB8888 format)
        int r1 = (p1 >> 16) & 0xFF;
        int g1 = (p1 >> 8) & 0xFF;
        int b1 = p1 & 0xFF;
        
        int r2 = (p2 >> 16) & 0xFF;
        int g2 = (p2 >> 8) & 0xFF;
        int b2 = p2 & 0xFF;
        
        // Accumulate squared differences
        int dr = r1 - r2;
        int dg = g1 - g2;
        int db = b1 - b2;
        
        mse += (dr * dr + dg * dg + db * db) / 3.0;
    }
    
    mse /= pixelCount;
    
    if (mse == 0.0) {
        return 100.0; // Perfect match
    }
    
    double maxPixelValue = 255.0;
    return 20.0 * std::log10(maxPixelValue / std::sqrt(mse));
}

// ===== Test Helpers =====

struct TestPolygon {
    int32_t xa, ya, xb, yb, xc, yc, xd, yd;
    Color555 color;
    bool gouraud;
    Color555 colorA, colorB, colorC, colorD;
    
    const char* description;
};

// Create test polygons covering various cases
std::vector<TestPolygon> CreateTestPolygons() {
    std::vector<TestPolygon> polygons;
    
    // Simple centered square (solid red)
    polygons.push_back({
        100, 100, 200, 100, 200, 200, 100, 200,
        Color555{.r = 31, .g = 0, .b = 0, .msb = 0}, // Red
        false, {}, {}, {}, {},
        "Centered square - solid red"
    });
    
    // Top-left corner triangle (solid green)
    polygons.push_back({
        0, 0, 100, 0, 0, 100, 0, 100,
        Color555{.r = 0, .g = 31, .b = 0, .msb = 0}, // Green
        false, {}, {}, {}, {},
        "Top-left triangle - solid green"
    });
    
    // Bottom-right corner (solid blue)
    polygons.push_back({
        220, 124, 320, 124, 320, 224, 220, 224,
        Color555{.r = 0, .g = 0, .b = 31, .msb = 0}, // Blue
        false, {}, {}, {}, {},
        "Bottom-right corner - solid blue"
    });
    
    // Full-screen quad (solid white)
    polygons.push_back({
        0, 0, 320, 0, 320, 224, 0, 224,
        Color555{.r = 31, .g = 31, .b = 31, .msb = 0}, // White
        false, {}, {}, {}, {},
        "Full-screen quad - solid white"
    });
    
    // Gouraud-shaded quad (red, green, blue, yellow corners)
    polygons.push_back({
        80, 50, 240, 50, 240, 174, 80, 174,
        Color555{}, // Not used for Gouraud
        true,
        Color555{.r = 31, .g = 0, .b = 0, .msb = 0}, // Red (top-left)
        Color555{.r = 0, .g = 31, .b = 0, .msb = 0}, // Green (top-right)
        Color555{.r = 0, .g = 0, .b = 31, .msb = 0}, // Blue (bottom-right)
        Color555{.r = 31, .g = 31, .b = 0, .msb = 0}, // Yellow (bottom-left)
        "Gouraud quad - RGBY corners"
    });
    
    // Small polygon (1 pixel)
    polygons.push_back({
        160, 112, 161, 112, 161, 113, 160, 113,
        Color555{.r = 31, .g = 15, .b = 0, .msb = 0}, // Orange
        false, {}, {}, {}, {},
        "Tiny polygon - 1 pixel"
    });
    
    // Rotated square (diamond shape)
    polygons.push_back({
        160, 50, 240, 112, 160, 174, 80, 112,
        Color555{.r = 15, .g = 0, .b = 31, .msb = 0}, // Purple
        false, {}, {}, {}, {},
        "Rotated square - diamond"
    });
    
    return polygons;
}

// ===== GPU vs Software Validation Tests =====

TEST_CASE("GPU Validation: Single solid polygon", "[gpu][validation][software_compare]") {
    // Create both renderers
    auto gpuRenderer = CreateRenderer(RendererType::Vulkan);
    auto softwareRenderer = CreateRenderer(RendererType::Software);
    
    if (!gpuRenderer || !gpuRenderer->Initialize()) {
        WARN("GPU renderer not available - skipping validation test");
        return;
    }
    
    REQUIRE(softwareRenderer);
    REQUIRE(softwareRenderer->Initialize());
    
    // Test polygon: centered red square
    TestPolygon poly = {
        100, 100, 200, 100, 200, 200, 100, 200,
        Color555{.r = 31, .g = 0, .b = 0, .msb = 0}, // Red
        false, {}, {}, {}, {},
        "Centered square - solid red"
    };
    
    // Render with GPU
    gpuRenderer->BeginFrame();
    gpuRenderer->DrawSolidPolygon(poly.xa, poly.ya, poly.xb, poly.yb,
                                  poly.xc, poly.yc, poly.xd, poly.yd,
                                  poly.color);
    gpuRenderer->EndFrame();
    
    // Render with Software
    softwareRenderer->BeginFrame();
    softwareRenderer->DrawSolidPolygon(poly.xa, poly.ya, poly.xb, poly.yb,
                                       poly.xc, poly.yc, poly.xd, poly.yd,
                                       poly.color);
    softwareRenderer->EndFrame();
    
    // Get framebuffers
    const uint32_t* gpuFB = static_cast<const uint32_t*>(gpuRenderer->GetFramebuffer());
    const uint32_t* softwareFB = static_cast<const uint32_t*>(softwareRenderer->GetFramebuffer());
    
    REQUIRE(gpuFB != nullptr);
    REQUIRE(softwareFB != nullptr);
    
    // Calculate PSNR
    double psnr = CalculatePSNR(gpuFB, softwareFB, 320, 224);
    
    INFO("PSNR: " << psnr << " dB");
    INFO("Test: " << poly.description);
    
    // PSNR should be very high (>40 dB = visually identical)
    // We'll accept >35 dB initially to account for minor differences
    REQUIRE(psnr > 35.0);
    
    // Also check pixel-perfect match at center of polygon
    uint32_t centerX = 150;
    uint32_t centerY = 150;
    uint32_t centerIdx = centerY * 320 + centerX;
    
    uint32_t gpuPixel = gpuFB[centerIdx];
    uint32_t softwarePixel = softwareFB[centerIdx];
    
    // Extract RGB
    int gpuR = (gpuPixel >> 16) & 0xFF;
    int gpuG = (gpuPixel >> 8) & 0xFF;
    int gpuB = gpuPixel & 0xFF;
    
    int softR = (softwarePixel >> 16) & 0xFF;
    int softG = (softwarePixel >> 8) & 0xFF;
    int softB = softwarePixel & 0xFF;
    
    INFO("Center pixel GPU: (" << gpuR << "," << gpuG << "," << gpuB << ")");
    INFO("Center pixel Software: (" << softR << "," << softG << "," << softB << ")");
    
    // Color should match within small tolerance (±2 due to Color555→RGB8 conversion)
    REQUIRE(std::abs(gpuR - softR) <= 2);
    REQUIRE(std::abs(gpuG - softG) <= 2);
    REQUIRE(std::abs(gpuB - softB) <= 2);
}

TEST_CASE("GPU Validation: Multiple polygons", "[gpu][validation][software_compare]") {
    auto gpuRenderer = CreateRenderer(RendererType::Vulkan);
    auto softwareRenderer = CreateRenderer(RendererType::Software);
    
    if (!gpuRenderer || !gpuRenderer->Initialize()) {
        WARN("GPU renderer not available - skipping validation test");
        return;
    }
    
    REQUIRE(softwareRenderer);
    REQUIRE(softwareRenderer->Initialize());
    
    auto testPolygons = CreateTestPolygons();
    
    for (const auto& poly : testPolygons) {
        DYNAMIC_SECTION("Testing: " << poly.description) {
            // Clear and render
            gpuRenderer->BeginFrame();
            softwareRenderer->BeginFrame();
            
            if (poly.gouraud) {
                gpuRenderer->DrawGouraudPolygon(poly.xa, poly.ya, poly.xb, poly.yb,
                                                poly.xc, poly.yc, poly.xd, poly.yd,
                                                poly.colorA, poly.colorB,
                                                poly.colorC, poly.colorD);
                softwareRenderer->DrawGouraudPolygon(poly.xa, poly.ya, poly.xb, poly.yb,
                                                     poly.xc, poly.yc, poly.xd, poly.yd,
                                                     poly.colorA, poly.colorB,
                                                     poly.colorC, poly.colorD);
            } else {
                gpuRenderer->DrawSolidPolygon(poly.xa, poly.ya, poly.xb, poly.yb,
                                              poly.xc, poly.yc, poly.xd, poly.yd,
                                              poly.color);
                softwareRenderer->DrawSolidPolygon(poly.xa, poly.ya, poly.xb, poly.yb,
                                                   poly.xc, poly.yc, poly.xd, poly.yd,
                                                   poly.color);
            }
            
            gpuRenderer->EndFrame();
            softwareRenderer->EndFrame();
            
            // Compare
            const uint32_t* gpuFB = static_cast<const uint32_t*>(gpuRenderer->GetFramebuffer());
            const uint32_t* softwareFB = static_cast<const uint32_t*>(softwareRenderer->GetFramebuffer());
            
            double psnr = CalculatePSNR(gpuFB, softwareFB, 320, 224);
            
            INFO("PSNR: " << psnr << " dB");
            
            // Each polygon should match well
            REQUIRE(psnr > 30.0); // Allow slightly lower threshold for complex cases
        }
    }
}

TEST_CASE("GPU Validation: Color555 conversion accuracy", "[gpu][validation][color]") {
    auto gpuRenderer = CreateRenderer(RendererType::Vulkan);
    auto softwareRenderer = CreateRenderer(RendererType::Software);
    
    if (!gpuRenderer || !gpuRenderer->Initialize()) {
        WARN("GPU renderer not available - skipping validation test");
        return;
    }
    
    REQUIRE(softwareRenderer);
    REQUIRE(softwareRenderer->Initialize());
    
    // Test all 5-bit values for R, G, B
    for (uint8_t r = 0; r < 32; r += 8) {  // Test every 8th value for speed
        for (uint8_t g = 0; g < 32; g += 8) {
            for (uint8_t b = 0; b < 32; b += 8) {
                Color555 color{.r = r, .g = g, .b = b, .msb = 0};
                
                // Render full-screen quad with this color
                gpuRenderer->BeginFrame();
                gpuRenderer->DrawSolidPolygon(0, 0, 320, 0, 320, 224, 0, 224, color);
                gpuRenderer->EndFrame();
                
                softwareRenderer->BeginFrame();
                softwareRenderer->DrawSolidPolygon(0, 0, 320, 0, 320, 224, 0, 224, color);
                softwareRenderer->EndFrame();
                
                // Check center pixel
                const uint32_t* gpuFB = static_cast<const uint32_t*>(gpuRenderer->GetFramebuffer());
                const uint32_t* softwareFB = static_cast<const uint32_t*>(softwareRenderer->GetFramebuffer());
                
                uint32_t centerIdx = 112 * 320 + 160;
                uint32_t gpuPixel = gpuFB[centerIdx];
                uint32_t softwarePixel = softwareFB[centerIdx];
                
                int gpuR = (gpuPixel >> 16) & 0xFF;
                int gpuG = (gpuPixel >> 8) & 0xFF;
                int gpuB = gpuPixel & 0xFF;
                
                int softR = (softwarePixel >> 16) & 0xFF;
                int softG = (softwarePixel >> 8) & 0xFF;
                int softB = softwarePixel & 0xFF;
                
                // Colors should match exactly or within ±1 (quantization)
                INFO("Color555: (" << (int)r << "," << (int)g << "," << (int)b << ")");
                INFO("GPU RGB8: (" << gpuR << "," << gpuG << "," << gpuB << ")");
                INFO("Software RGB8: (" << softR << "," << softG << "," << softB << ")");
                
                REQUIRE(std::abs(gpuR - softR) <= 1);
                REQUIRE(std::abs(gpuG - softG) <= 1);
                REQUIRE(std::abs(gpuB - softB) <= 1);
            }
        }
    }
}

TEST_CASE("GPU Validation: Coordinate transformation", "[gpu][validation][coords]") {
    auto gpuRenderer = CreateRenderer(RendererType::Vulkan);
    auto softwareRenderer = CreateRenderer(RendererType::Software);
    
    if (!gpuRenderer || !gpuRenderer->Initialize()) {
        WARN("GPU renderer not available - skipping validation test");
        return;
    }
    
    REQUIRE(softwareRenderer);
    REQUIRE(softwareRenderer->Initialize());
    
    // Test polygons at specific coordinates
    struct CoordTest {
        int32_t xa, ya, xb, yb, xc, yc, xd, yd;
        const char* description;
    };
    
    std::vector<CoordTest> coordTests = {
        {0, 0, 10, 0, 10, 10, 0, 10, "Top-left corner (0,0)"},
        {310, 0, 320, 0, 320, 10, 310, 10, "Top-right corner (320,0)"},
        {0, 214, 10, 214, 10, 224, 0, 224, "Bottom-left corner (0,224)"},
        {310, 214, 320, 214, 320, 224, 310, 224, "Bottom-right corner (320,224)"},
        {155, 107, 165, 107, 165, 117, 155, 117, "Screen center (160,112)"},
    };
    
    Color555 testColor{.r = 31, .g = 31, .b = 31, .msb = 0}; // White
    
    for (const auto& test : coordTests) {
        DYNAMIC_SECTION(test.description) {
            gpuRenderer->BeginFrame();
            gpuRenderer->DrawSolidPolygon(test.xa, test.ya, test.xb, test.yb,
                                          test.xc, test.yc, test.xd, test.yd,
                                          testColor);
            gpuRenderer->EndFrame();
            
            softwareRenderer->BeginFrame();
            softwareRenderer->DrawSolidPolygon(test.xa, test.ya, test.xb, test.yb,
                                               test.xc, test.yc, test.xd, test.yd,
                                               testColor);
            softwareRenderer->EndFrame();
            
            const uint32_t* gpuFB = static_cast<const uint32_t*>(gpuRenderer->GetFramebuffer());
            const uint32_t* softwareFB = static_cast<const uint32_t*>(softwareRenderer->GetFramebuffer());
            
            // Compare the polygons
            double psnr = CalculatePSNR(gpuFB, softwareFB, 320, 224);
            
            INFO("PSNR: " << psnr << " dB");
            
            // Should match very well
            REQUIRE(psnr > 35.0);
        }
    }
}

TEST_CASE("GPU Validation: Gouraud shading accuracy", "[gpu][validation][gouraud]") {
    auto gpuRenderer = CreateRenderer(RendererType::Vulkan);
    auto softwareRenderer = CreateRenderer(RendererType::Software);
    
    if (!gpuRenderer || !gpuRenderer->Initialize()) {
        WARN("GPU renderer not available - skipping validation test");
        return;
    }
    
    REQUIRE(softwareRenderer);
    REQUIRE(softwareRenderer->Initialize());
    
    // Test various Gouraud configurations
    struct GouraudTest {
        Color555 colorA, colorB, colorC, colorD;
        const char* description;
    };
    
    std::vector<GouraudTest> gouraudTests = {
        // Grayscale gradient
        {
            Color555{.r = 0, .g = 0, .b = 0, .msb = 0},   // Black
            Color555{.r = 10, .g = 10, .b = 10, .msb = 0}, // Dark gray
            Color555{.r = 20, .g = 20, .b = 20, .msb = 0}, // Gray
            Color555{.r = 31, .g = 31, .b = 31, .msb = 0}, // White
            "Grayscale gradient"
        },
        // Red to blue gradient
        {
            Color555{.r = 31, .g = 0, .b = 0, .msb = 0},  // Red
            Color555{.r = 31, .g = 0, .b = 0, .msb = 0},  // Red
            Color555{.r = 0, .g = 0, .b = 31, .msb = 0},  // Blue
            Color555{.r = 0, .g = 0, .b = 31, .msb = 0},  // Blue
            "Red to blue gradient"
        },
        // RGBY corners (classic test)
        {
            Color555{.r = 31, .g = 0, .b = 0, .msb = 0},   // Red
            Color555{.r = 0, .g = 31, .b = 0, .msb = 0},   // Green
            Color555{.r = 0, .g = 0, .b = 31, .msb = 0},   // Blue
            Color555{.r = 31, .g = 31, .b = 0, .msb = 0},  // Yellow
            "RGBY corners"
        },
    };
    
    for (const auto& test : gouraudTests) {
        DYNAMIC_SECTION(test.description) {
            gpuRenderer->BeginFrame();
            gpuRenderer->DrawGouraudPolygon(80, 50, 240, 50, 240, 174, 80, 174,
                                            test.colorA, test.colorB,
                                            test.colorC, test.colorD);
            gpuRenderer->EndFrame();
            
            softwareRenderer->BeginFrame();
            softwareRenderer->DrawGouraudPolygon(80, 50, 240, 50, 240, 174, 80, 174,
                                                 test.colorA, test.colorB,
                                                 test.colorC, test.colorD);
            softwareRenderer->EndFrame();
            
            const uint32_t* gpuFB = static_cast<const uint32_t*>(gpuRenderer->GetFramebuffer());
            const uint32_t* softwareFB = static_cast<const uint32_t*>(softwareRenderer->GetFramebuffer());
            
            double psnr = CalculatePSNR(gpuFB, softwareFB, 320, 224);
            
            INFO("PSNR: " << psnr << " dB");
            
            // Gouraud shading might have minor interpolation differences
            // But should still be very close
            REQUIRE(psnr > 30.0);
        }
    }
}

TEST_CASE("GPU Validation: Performance comparison", "[gpu][validation][perf]") {
    auto gpuRenderer = CreateRenderer(RendererType::Vulkan);
    auto softwareRenderer = CreateRenderer(RendererType::Software);
    
    if (!gpuRenderer || !gpuRenderer->Initialize()) {
        WARN("GPU renderer not available - skipping validation test");
        return;
    }
    
    REQUIRE(softwareRenderer);
    REQUIRE(softwareRenderer->Initialize());
    
    // Create a complex scene with many polygons
    std::vector<TestPolygon> polygons;
    for (int i = 0; i < 100; ++i) {
        int32_t x = (i * 17) % 300;
        int32_t y = (i * 23) % 200;
        uint8_t r = (i * 3) % 32;
        uint8_t g = (i * 5) % 32;
        uint8_t b = (i * 7) % 32;
        
        polygons.push_back({
            x, y, x+20, y, x+20, y+20, x, y+20,
            Color555{.r = r, .g = g, .b = b, .msb = 0},
            false, {}, {}, {}, {},
            "Stress test polygon"
        });
    }
    
    // Benchmark GPU
    auto gpuStart = std::chrono::high_resolution_clock::now();
    for (int frame = 0; frame < 60; ++frame) {
        gpuRenderer->BeginFrame();
        for (const auto& poly : polygons) {
            gpuRenderer->DrawSolidPolygon(poly.xa, poly.ya, poly.xb, poly.yb,
                                          poly.xc, poly.yc, poly.xd, poly.yd,
                                          poly.color);
        }
        gpuRenderer->EndFrame();
    }
    auto gpuEnd = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> gpuElapsed = gpuEnd - gpuStart;
    double gpuAvgMs = gpuElapsed.count() / 60.0;
    
    // Benchmark Software
    auto softStart = std::chrono::high_resolution_clock::now();
    for (int frame = 0; frame < 60; ++frame) {
        softwareRenderer->BeginFrame();
        for (const auto& poly : polygons) {
            softwareRenderer->DrawSolidPolygon(poly.xa, poly.ya, poly.xb, poly.yb,
                                               poly.xc, poly.yc, poly.xd, poly.yd,
                                               poly.color);
        }
        softwareRenderer->EndFrame();
    }
    auto softEnd = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> softElapsed = softEnd - softStart;
    double softAvgMs = softElapsed.count() / 60.0;
    
    INFO("GPU average frame time: " << gpuAvgMs << "ms (" << (1000.0/gpuAvgMs) << " FPS)");
    INFO("Software average frame time: " << softAvgMs << "ms (" << (1000.0/softAvgMs) << " FPS)");
    INFO("GPU vs Software speedup: " << (softAvgMs / gpuAvgMs) << "x");
    
    // Both should be reasonably fast
    REQUIRE(gpuAvgMs < 33.33); // At least 30 FPS
    REQUIRE(softAvgMs < 33.33); // At least 30 FPS
    
    // GPU might be slower initially due to readback, but should be competitive
    // Once we optimize, GPU should be faster
}

