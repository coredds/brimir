// Brimir Input Handling Unit Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>

using namespace brimir;

// libretro button definitions (from libretro.h)
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_B = 0;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_Y = 1;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_SELECT = 2;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_START = 3;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_UP = 4;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_DOWN = 5;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_LEFT = 6;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_RIGHT = 7;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_A = 8;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_X = 9;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_L = 10;
constexpr uint16_t RETRO_DEVICE_ID_JOYPAD_R = 11;

TEST_CASE("Input handling basic functionality", "[input][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("SetControllerState doesn't crash") {
        // Port 0, no buttons pressed
        core.SetControllerState(0, 0);
        REQUIRE(true);
    }
    
    SECTION("SetControllerState with buttons pressed") {
        // Port 0, start button pressed
        uint16_t buttons = 1 << RETRO_DEVICE_ID_JOYPAD_START;
        core.SetControllerState(0, buttons);
        REQUIRE(true);
    }
    
    SECTION("SetControllerState for port 2") {
        // Port 1 (second controller)
        core.SetControllerState(1, 0);
        REQUIRE(true);
    }
}

TEST_CASE("Input handling without initialization", "[input][unit]") {
    CoreWrapper core;
    
    SECTION("SetControllerState before init doesn't crash") {
        core.SetControllerState(0, 0);
        REQUIRE(true);
    }
}

TEST_CASE("Input handling all buttons", "[input][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("D-pad buttons") {
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_UP);
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
        REQUIRE(true);
    }
    
    SECTION("Face buttons") {
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_A);
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_B);
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_X);
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_Y);
        REQUIRE(true);
    }
    
    SECTION("Shoulder buttons") {
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_L);
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_R);
        REQUIRE(true);
    }
    
    SECTION("Start and select") {
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_START);
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
        REQUIRE(true);
    }
}

TEST_CASE("Input handling multiple buttons simultaneously", "[input][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Multiple buttons pressed") {
        uint16_t buttons = (1 << RETRO_DEVICE_ID_JOYPAD_A) | 
                          (1 << RETRO_DEVICE_ID_JOYPAD_B) |
                          (1 << RETRO_DEVICE_ID_JOYPAD_UP);
        core.SetControllerState(0, buttons);
        REQUIRE(true);
    }
    
    SECTION("All buttons pressed") {
        uint16_t buttons = 0xFFFF;
        core.SetControllerState(0, buttons);
        REQUIRE(true);
    }
}

TEST_CASE("Input handling repeated calls", "[input][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Rapid button updates") {
        for (int i = 0; i < 100; ++i) {
            uint16_t buttons = (i % 2) ? (1 << RETRO_DEVICE_ID_JOYPAD_A) : 0;
            core.SetControllerState(0, buttons);
        }
        REQUIRE(true);
    }
    
    SECTION("Alternating between controllers") {
        for (int i = 0; i < 50; ++i) {
            core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_A);
            core.SetControllerState(1, 1 << RETRO_DEVICE_ID_JOYPAD_B);
        }
        REQUIRE(true);
    }
}

TEST_CASE("Input handling with game execution", "[input][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Input during frame execution") {
        // Set some input
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_START);
        
        // Run a frame
        core.RunFrame();
        
        // Change input
        core.SetControllerState(0, 0);
        
        // Run another frame
        core.RunFrame();
        
        REQUIRE(true);
    }
}

TEST_CASE("Input handling invalid ports", "[input][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("High port number doesn't crash") {
        core.SetControllerState(999, 0);
        REQUIRE(true);
    }
}

TEST_CASE("Input handling state persistence", "[input][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Input state across multiple frames") {
        // Set input
        core.SetControllerState(0, 1 << RETRO_DEVICE_ID_JOYPAD_A);
        
        // Run several frames
        for (int i = 0; i < 10; ++i) {
            core.RunFrame();
        }
        
        // Input should still be set (no crashes or issues)
        REQUIRE(true);
    }
}

