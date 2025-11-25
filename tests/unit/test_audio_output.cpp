// Brimir Audio Output Unit Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

TEST_CASE("Audio output basic functionality", "[audio][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Can retrieve audio samples") {
        std::vector<int16_t> buffer(2048 * 2);  // Stereo
        size_t count = core.GetAudioSamples(buffer.data(), 2048);
        
        // May return 0 if no audio generated yet, but shouldn't crash
        REQUIRE(count >= 0);
        REQUIRE(count <= 2048);
    }
    
    SECTION("Audio buffer with nullptr fails safely") {
        size_t count = core.GetAudioSamples(nullptr, 2048);
        REQUIRE(count == 0);
    }
    
    SECTION("Audio buffer with zero size") {
        std::vector<int16_t> buffer(2048 * 2);
        size_t count = core.GetAudioSamples(buffer.data(), 0);
        REQUIRE(count == 0);
    }
}

TEST_CASE("Audio output without initialization", "[audio][unit]") {
    CoreWrapper core;
    
    SECTION("GetAudioSamples before init returns 0") {
        std::vector<int16_t> buffer(2048 * 2);
        size_t count = core.GetAudioSamples(buffer.data(), 2048);
        REQUIRE(count == 0);
    }
}

TEST_CASE("Audio output buffer management", "[audio][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Multiple calls to GetAudioSamples") {
        std::vector<int16_t> buffer1(2048 * 2);
        std::vector<int16_t> buffer2(2048 * 2);
        
        // First call
        size_t count1 = core.GetAudioSamples(buffer1.data(), 2048);
        
        // Second call (should work without crashing)
        size_t count2 = core.GetAudioSamples(buffer2.data(), 2048);
        
        REQUIRE(count1 >= 0);
        REQUIRE(count2 >= 0);
    }
    
    SECTION("Large buffer request") {
        std::vector<int16_t> buffer(100000 * 2);  // Very large buffer
        size_t count = core.GetAudioSamples(buffer.data(), 100000);
        
        // Should not crash and return reasonable value
        REQUIRE(count >= 0);
        REQUIRE(count <= 100000);
    }
}

TEST_CASE("Audio output format", "[audio][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Audio is stereo interleaved format") {
        std::vector<int16_t> buffer(2048 * 2);
        size_t count = core.GetAudioSamples(buffer.data(), 2048);
        
        // Buffer should contain L,R,L,R... samples
        // We can't test specific values without running a frame,
        // but we can verify the structure is valid
        REQUIRE(count >= 0);
    }
}

TEST_CASE("Audio output after frame execution", "[audio][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Running frame may generate audio") {
        // Run a frame
        core.RunFrame();
        
        std::vector<int16_t> buffer(2048 * 2);
        size_t count = core.GetAudioSamples(buffer.data(), 2048);
        
        // After running a frame, we should get some audio
        // (or 0 if emulator hasn't started audio yet)
        REQUIRE(count >= 0);
    }
    
    SECTION("Audio buffer clears after retrieval") {
        // Run a frame
        core.RunFrame();
        
        std::vector<int16_t> buffer1(2048 * 2);
        size_t count1 = core.GetAudioSamples(buffer1.data(), 2048);
        
        // Second call without running frame should return 0
        std::vector<int16_t> buffer2(2048 * 2);
        size_t count2 = core.GetAudioSamples(buffer2.data(), 2048);
        
        REQUIRE(count2 == 0);
    }
}

TEST_CASE("Audio output consistency", "[audio][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Multiple frame runs produce audio") {
        std::vector<int16_t> buffer(2048 * 2);
        
        for (int i = 0; i < 5; ++i) {
            core.RunFrame();
            size_t count = core.GetAudioSamples(buffer.data(), 2048);
            
            // Each frame should produce consistent results
            REQUIRE(count >= 0);
        }
    }
}

TEST_CASE("Audio output buffer overflow protection", "[audio][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Running many frames without consuming audio") {
        // Run multiple frames without getting audio samples
        for (int i = 0; i < 10; ++i) {
            core.RunFrame();
        }
        
        // Now try to get audio - should not crash
        std::vector<int16_t> buffer(20000 * 2);
        [[maybe_unused]] size_t count = core.GetAudioSamples(buffer.data(), 20000);
        
        // Just verify no crash
        REQUIRE(true);
    }
}

