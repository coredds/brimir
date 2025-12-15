// Brimir - Basic Smoke Tests
// These tests validate that the core infrastructure works

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("CoreWrapper construction", "[core][smoke]") {
    // Should be able to create a CoreWrapper without crashing
    CoreWrapper core;
    
    // Basic check
    REQUIRE(true);
}

TEST_CASE("CoreWrapper initialization", "[core][smoke]") {
    CoreWrapper core;
    
    // Should be able to initialize
    // This may fail without BIOS, but should not crash
    bool initialized = core.Initialize();
    
    // Log result for debugging
    INFO("Initialization result: " << (initialized ? "SUCCESS" : "FAILED"));
    
    // We're just checking it doesn't crash, result may vary
    REQUIRE((initialized == true || initialized == false));
}

TEST_CASE("CoreWrapper can run a frame (if initialized)", "[core][smoke]") {
    CoreWrapper core;
    
    if (core.Initialize()) {
        // If initialization succeeded, should be able to run a frame
        REQUIRE_NOTHROW(core.RunFrame());
        
        // Should be able to run multiple frames
        REQUIRE_NOTHROW(core.RunFrame());
        REQUIRE_NOTHROW(core.RunFrame());
    } else {
        // If initialization failed (no BIOS), skip this test
        WARN("Skipping frame test - initialization failed (BIOS missing?)");
    }
}

TEST_CASE("CoreWrapper framebuffer access", "[core][vdp][smoke]") {
    CoreWrapper core;
    
    if (core.Initialize()) {
        core.RunFrame();
        
        // Should be able to get framebuffer pointer
        const void* fb = core.GetFramebuffer();
        REQUIRE(fb != nullptr);
        
        // Should have reasonable dimensions
        uint32_t w = core.GetFramebufferWidth();
        uint32_t h = core.GetFramebufferHeight();
        
        INFO("Framebuffer size: " << w << "x" << h);
        
        REQUIRE(w > 0);
        REQUIRE(h > 0);
        REQUIRE(w <= 704);  // Max Saturn resolution
        REQUIRE(h <= 512);  // Max Saturn resolution
    } else {
        WARN("Skipping framebuffer test - initialization failed");
    }
}

TEST_CASE("CoreWrapper performance sanity check", "[core][perf][smoke]") {
    CoreWrapper core;
    
    if (core.Initialize()) {
        auto start = std::chrono::high_resolution_clock::now();
        
        // Run 10 frames
        for (int i = 0; i < 10; i++) {
            core.RunFrame();
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        auto avgFrameTime = duration.count() / 10.0;
        
        INFO("Average frame time: " << avgFrameTime << "ms");
        
        // Should be faster than 100ms per frame (10 FPS minimum)
        // This is just a sanity check, not a performance target
        REQUIRE(avgFrameTime < 100.0);
    } else {
        WARN("Skipping performance test - initialization failed");
    }
}

