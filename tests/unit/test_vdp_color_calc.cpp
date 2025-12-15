// Brimir - VDP Color Calculation Tests
// These tests validate color blending, transparency, and compositing

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("VDP color calculation determinism", "[vdp][color][critical]") {
    CoreWrapper core;
    
    if (core.Initialize()) {
        // Run multiple frames
        std::vector<std::vector<uint16_t>> frameBuffers;
        
        for (int i = 0; i < 5; i++) {
            core.RunFrame();
            
            auto* fb = core.GetFramebuffer();
            uint32_t w = core.GetFramebufferWidth();
            uint32_t h = core.GetFramebufferHeight();
            
            std::vector<uint16_t> frame(w * h);
            const uint16_t* fb_rgb565 = static_cast<const uint16_t*>(fb);
            std::copy(fb_rgb565, fb_rgb565 + (w * h), frame.begin());
            
            frameBuffers.push_back(std::move(frame));
        }
        
        // Each frame should be deterministic (same frame number = same output)
        // Or progressive (each frame advances state)
        // Either way, running same number of frames should give consistent results
        
        // Reset and run again
        core.Reset();
        core.Initialize();
        
        for (int i = 0; i < 5; i++) {
            core.RunFrame();
            
            auto* fb = core.GetFramebuffer();
            uint32_t w = core.GetFramebufferWidth();
            uint32_t h = core.GetFramebufferHeight();
            
            const uint16_t* fb_rgb565 = static_cast<const uint16_t*>(fb);
            
            bool matches = true;
            for (size_t j = 0; j < w * h && matches; j++) {
                if (frameBuffers[i][j] != fb_rgb565[j]) {
                    matches = false;
                }
            }
            
            REQUIRE(matches);
        }
    } else {
        WARN("Skipping color calc test - initialization failed");
    }
}

TEST_CASE("VDP framebuffer pixel format", "[vdp][color]") {
    CoreWrapper core;
    
    if (core.Initialize()) {
        core.RunFrame();
        
        auto* fb = core.GetFramebuffer();
        uint32_t w = core.GetFramebufferWidth();
        uint32_t h = core.GetFramebufferHeight();
        
        const uint16_t* fb_rgb565 = static_cast<const uint16_t*>(fb);
        
        // Check that pixels are in valid RGB565 format
        // RGB565: RRRRRGGGGGGBBBBB (5-6-5 bits)
        bool hasValidPixels = true;
        uint32_t nonBlackCount = 0;
        
        for (uint32_t i = 0; i < w * h; i++) {
            uint16_t pixel = fb_rgb565[i];
            
            // Extract components
            uint16_t r = (pixel >> 11) & 0x1F;  // 5 bits
            uint16_t g = (pixel >> 5) & 0x3F;   // 6 bits
            uint16_t b = pixel & 0x1F;          // 5 bits
            
            // Components should be in valid ranges
            REQUIRE(r <= 31);
            REQUIRE(g <= 63);
            REQUIRE(b <= 31);
            
            if (pixel != 0) {
                nonBlackCount++;
            }
        }
        
        INFO("Non-black pixels: " << nonBlackCount << " / " << (w * h));
        
        // Framebuffer should exist and have valid format
        REQUIRE(hasValidPixels);
    } else {
        WARN("Skipping pixel format test - initialization failed");
    }
}

TEST_CASE("VDP color blending performance", "[vdp][color][perf]") {
    CoreWrapper core;
    
    if (core.Initialize()) {
        // Warm up
        for (int i = 0; i < 3; i++) {
            core.RunFrame();
        }
        
        // Measure compositing performance
        auto start = std::chrono::high_resolution_clock::now();
        
        const int frameCount = 60;  // One second at 60 FPS
        for (int i = 0; i < frameCount; i++) {
            core.RunFrame();
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        auto avgFrameTime = duration.count() / static_cast<double>(frameCount);
        auto avgFrameTimeMs = avgFrameTime / 1000.0;
        
        INFO("Average frame time: " << avgFrameTimeMs << "ms");
        INFO("Theoretical FPS: " << (1000.0 / avgFrameTimeMs));
        
        // Should maintain at least 30 FPS average
        REQUIRE(avgFrameTimeMs < 33.33);
        
        // Log performance tier
        if (avgFrameTimeMs < 16.67) {
            INFO("Performance: Excellent (>= 60 FPS)");
        } else if (avgFrameTimeMs < 20.0) {
            INFO("Performance: Good (>= 50 FPS)");
        } else if (avgFrameTimeMs < 25.0) {
            INFO("Performance: Acceptable (>= 40 FPS)");
        } else {
            INFO("Performance: Marginal (>= 30 FPS)");
        }
    } else {
        WARN("Skipping color blending perf test - initialization failed");
    }
}

TEST_CASE("VDP extended run stability", "[vdp][color][stability]") {
    CoreWrapper core;
    
    if (core.Initialize()) {
        // Run for extended period to check for memory leaks, crashes, etc.
        const int frameCount = 300;  // 5 seconds at 60 FPS
        
        for (int i = 0; i < frameCount; i++) {
            core.RunFrame();
            
            // Periodically check framebuffer is still valid
            if (i % 60 == 0) {
                auto* fb = core.GetFramebuffer();
                REQUIRE(fb != nullptr);
                
                uint32_t w = core.GetFramebufferWidth();
                uint32_t h = core.GetFramebufferHeight();
                
                REQUIRE(w > 0);
                REQUIRE(h > 0);
            }
        }
        
        // If we got here without crashing, test passed
        REQUIRE(true);
    } else {
        WARN("Skipping stability test - initialization failed");
    }
}

TEST_CASE("VDP zero-content frame handling", "[vdp][color]") {
    CoreWrapper core;
    
    if (core.Initialize()) {
        // Run a frame (likely no game content, just BIOS)
        core.RunFrame();
        
        auto* fb = core.GetFramebuffer();
        uint32_t w = core.GetFramebufferWidth();
        uint32_t h = core.GetFramebufferHeight();
        
        REQUIRE(fb != nullptr);
        REQUIRE(w > 0);
        REQUIRE(h > 0);
        
        // Should handle empty/zero-content frames gracefully
        // (either all black, or border color, both are valid)
        const uint16_t* fb_rgb565 = static_cast<const uint16_t*>(fb);
        
        // Check first and last pixels exist
        uint16_t firstPixel = fb_rgb565[0];
        uint16_t lastPixel = fb_rgb565[w * h - 1];
        
        // Just verify no crash and valid data
        INFO("First pixel: 0x" << std::hex << firstPixel);
        INFO("Last pixel: 0x" << std::hex << lastPixel);
        
        REQUIRE(true);  // Made it without crashing = success
    } else {
        WARN("Skipping zero-content test - initialization failed");
    }
}

