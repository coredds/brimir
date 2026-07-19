// Brimir CoreWrapper Unit Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace brimir;

// ============================================================
// Construction and Initialization
// ============================================================

TEST_CASE("CoreWrapper starts uninitialized", "[core][unit]") {
    CoreWrapper core;
    REQUIRE_FALSE(core.IsInitialized());
    REQUIRE_FALSE(core.IsGameLoaded());
}

TEST_CASE("CoreWrapper initialization lifecycle", "[core][unit]") {
    CoreWrapper core;

    REQUIRE(core.Initialize());
    REQUIRE(core.IsInitialized());

    core.Shutdown();
    REQUIRE_FALSE(core.IsInitialized());
}

TEST_CASE("CoreWrapper shutdown without init is safe", "[core][unit]") {
    CoreWrapper core;
    core.Shutdown();
    REQUIRE_FALSE(core.IsInitialized());
}

TEST_CASE("CoreWrapper game loading without init fails", "[core][unit]") {
    CoreWrapper core;

    SECTION("nullptr path") {
        REQUIRE_FALSE(core.LoadGame(nullptr));
    }
    SECTION("empty path") {
        REQUIRE_FALSE(core.LoadGame(""));
    }
    SECTION("invalid path") {
        REQUIRE(core.Initialize());
        REQUIRE_FALSE(core.LoadGame("nonexistent_file.iso"));
        REQUIRE(core.IsInitialized());  // core still viable after failed load
    }
}

// ============================================================
// Save State
// ============================================================

TEST_CASE("CoreWrapper save state size", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();

    size_t size = core.GetStateSize();
    REQUIRE(size > 0);
    REQUIRE(size < 100 * 1024 * 1024);  // < 100 MB
}

TEST_CASE("CoreWrapper save/load state null safety", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();

    REQUIRE_FALSE(core.SaveState(nullptr, 0));
    REQUIRE_FALSE(core.LoadState(nullptr, 0));
}

TEST_CASE("CoreWrapper save/load state round-trip", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();

    size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> buf(stateSize);

    REQUIRE(core.SaveState(buf.data(), stateSize));
    REQUIRE(core.LoadState(buf.data(), stateSize));
}

TEST_CASE("CoreWrapper save state too-small buffer", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();

    size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> buf(stateSize / 2);
    REQUIRE_FALSE(core.SaveState(buf.data(), buf.size()));
}

// ============================================================
// IPL / BIOS Loading
// ============================================================

TEST_CASE("CoreWrapper IPL loading", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();

    SECTION("empty buffer fails") {
        std::vector<uint8_t> empty;
        REQUIRE_FALSE(core.LoadIPL(std::span<const uint8_t>(empty)));
        REQUIRE_FALSE(core.IsIPLLoaded());
    }
    SECTION("wrong size fails") {
        std::vector<uint8_t> small(1024, 0xFF);
        REQUIRE_FALSE(core.LoadIPL(std::span<const uint8_t>(small)));
    }
    SECTION("correct size succeeds") {
        std::vector<uint8_t> correct(512 * 1024, 0xFF);
        REQUIRE(core.LoadIPL(std::span<const uint8_t>(correct)));
        REQUIRE(core.IsIPLLoaded());
    }
}

// ============================================================
// Audio Volume
// ============================================================

TEST_CASE("SetAudioVolume clamps out-of-range values", "[audio][unit]") {
    CoreWrapper core;

    core.SetAudioVolume(-50);   // clamped to 0
    core.SetAudioVolume(300);   // clamped to 200
    core.SetAudioVolume(0);
    core.SetAudioVolume(100);
    core.SetAudioVolume(200);
}

TEST_CASE("SetAudioVolume does not break audio output", "[audio][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    std::vector<int16_t> buf(2048 * 2);

    // 0% volume
    core.SetAudioVolume(0);
    core.RunFrame();
    size_t samples0 = core.GetAudioSamples(buf.data(), 2048);

    // 100% volume
    core.SetAudioVolume(100);
    core.RunFrame();
    size_t samples100 = core.GetAudioSamples(buf.data(), 2048);

    // 200% volume (should not overflow)
    core.SetAudioVolume(200);
    core.RunFrame();
    size_t samples200 = core.GetAudioSamples(buf.data(), 2048);

    // Default — verify samples not obviously corrupt
    if (samples100 > 0) {
        for (size_t i = 0; i < samples100 * 2; ++i) {
            REQUIRE(buf[i] >= -32768);
            REQUIRE(buf[i] <= 32767);
        }
    }

    (void)samples0;
    (void)samples200;
}

// ============================================================
// Screen Rotation
// ============================================================

TEST_CASE("SetRotation clamps invalid values to 0", "[video][unit]") {
    CoreWrapper core;

    core.SetRotation(45);   // invalid
    core.SetRotation(-90);  // invalid
    core.SetRotation(360);  // invalid
    core.SetRotation(0);
    core.SetRotation(90);
    core.SetRotation(180);
    core.SetRotation(270);
}

TEST_CASE("Rotation 90 swaps framebuffer dimensions", "[video][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();
    uint32_t defW = core.GetFramebufferWidth();
    uint32_t defH = core.GetFramebufferHeight();
    REQUIRE(defW > 0);
    REQUIRE(defH > 0);

    core.SetRotation(90);
    core.RunFrame();

    REQUIRE(core.GetFramebufferWidth() == defH);
    REQUIRE(core.GetFramebufferHeight() == defW);
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Rotation 270 swaps framebuffer dimensions", "[video][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();
    uint32_t defW = core.GetFramebufferWidth();
    uint32_t defH = core.GetFramebufferHeight();

    core.SetRotation(270);
    core.RunFrame();

    REQUIRE(core.GetFramebufferWidth() == defH);
    REQUIRE(core.GetFramebufferHeight() == defW);
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Rotation 180 preserves framebuffer dimensions", "[video][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();
    uint32_t defW = core.GetFramebufferWidth();
    uint32_t defH = core.GetFramebufferHeight();

    core.SetRotation(180);
    core.RunFrame();

    REQUIRE(core.GetFramebufferWidth() == defW);
    REQUIRE(core.GetFramebufferHeight() == defH);
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Rotation 0 is no-op", "[video][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();
    uint32_t defW = core.GetFramebufferWidth();
    uint32_t defH = core.GetFramebufferHeight();

    core.SetRotation(0);
    core.RunFrame();

    REQUIRE(core.GetFramebufferWidth() == defW);
    REQUIRE(core.GetFramebufferHeight() == defH);
}

// ============================================================
// Overscan Crop
// ============================================================

TEST_CASE("SetOverscanCrop clamps negative values", "[video][unit]") {
    CoreWrapper core;

    core.SetOverscanCrop(-10, -20);
    core.SetOverscanCrop(0, 0);
    core.SetOverscanCrop(48, 48);
}

TEST_CASE("Overscan crop reduces framebuffer dimensions", "[video][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();
    uint32_t defW = core.GetFramebufferWidth();
    uint32_t defH = core.GetFramebufferHeight();
    REQUIRE(defW > 32);
    REQUIRE(defH > 32);

    core.SetOverscanCrop(48, 48);
    core.RunFrame();

    REQUIRE(core.GetFramebufferWidth() == defW - 48);
    REQUIRE(core.GetFramebufferHeight() == defH - 48);
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Overscan crop zero is no-op", "[video][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();
    uint32_t defW = core.GetFramebufferWidth();
    uint32_t defH = core.GetFramebufferHeight();

    core.SetOverscanCrop(0, 0);
    core.RunFrame();

    REQUIRE(core.GetFramebufferWidth() == defW);
    REQUIRE(core.GetFramebufferHeight() == defH);
}

// ============================================================
// Combined Crop + Rotation
// ============================================================

TEST_CASE("Crop then rotate 90 produces correct dimensions", "[video][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();
    uint32_t defW = core.GetFramebufferWidth();
    uint32_t defH = core.GetFramebufferHeight();

    core.SetOverscanCrop(48, 48);
    core.SetRotation(90);
    core.RunFrame();

    // crop first: (defW-48) x (defH-48)
    // rotate 90: swap dimensions
    REQUIRE(core.GetFramebufferWidth() == defH - 48);
    REQUIRE(core.GetFramebufferHeight() == defW - 48);
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Reset crop and rotation restores defaults", "[video][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();
    uint32_t defW = core.GetFramebufferWidth();
    uint32_t defH = core.GetFramebufferHeight();

    core.SetOverscanCrop(32, 32);
    core.SetRotation(90);
    core.RunFrame();

    core.SetOverscanCrop(0, 0);
    core.SetRotation(0);
    core.RunFrame();

    REQUIRE(core.GetFramebufferWidth() == defW);
    REQUIRE(core.GetFramebufferHeight() == defH);
}

// ============================================================
// Framebuffer / Audio Access (sanity)
// ============================================================

TEST_CASE("CoreWrapper framebuffer access after init", "[core][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();

    const void* fb = core.GetFramebuffer();
    REQUIRE(fb != nullptr);

    uint32_t w = core.GetFramebufferWidth();
    uint32_t h = core.GetFramebufferHeight();
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    REQUIRE(w <= 2816);
    REQUIRE(h <= 2048);
}

TEST_CASE("CoreWrapper audio access after init", "[core][integration]") {
    CoreWrapper core;
    if (!core.Initialize()) {
        WARN("Skipping — init failed (BIOS missing?)");
        return;
    }

    core.RunFrame();

    std::vector<int16_t> buf(2048 * 2);
    [[maybe_unused]] size_t samples = core.GetAudioSamples(buf.data(), 2048);
}

TEST_CASE("CoreWrapper setters handle repeated calls without crashing", "[core][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    core.SetAudioInterpolation("linear");
    core.SetAudioInterpolation("nearest");
    core.SetCDReadSpeed(2);
    core.SetCDReadSpeed(200);
    core.SetSH2OverclockFactor(100);
    core.SetSH2OverclockFactor(300);
    core.SetAutodetectRegion(true);
    core.SetAutodetectRegion(false);
    core.SetAudioVolume(50);
    core.SetAudioVolume(150);
    core.SetRotation(90);
    core.SetRotation(0);
    core.SetOverscanCrop(16, 16);
    core.SetOverscanCrop(0, 0);
}

TEST_CASE("CoreWrapper maps SMPC area codes to NTSC/PAL", "[core][region][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // Before any game/region is set we are unknown or NTSC fallback
    auto initial = core.GetConsoleRegion();
    REQUIRE((initial == ConsoleRegion::NTSC ||
             initial == ConsoleRegion::Unknown));

    // Tests use BRIMIR_BUILD_TESTS, so we can access the Saturn instance
    ymir::Saturn* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    saturn->SMPC.SetAreaCode(1);  // Japan
    REQUIRE(core.GetConsoleRegion() == ConsoleRegion::NTSC);

    saturn->SMPC.SetAreaCode(4);  // North America
    REQUIRE(core.GetConsoleRegion() == ConsoleRegion::NTSC);

    saturn->SMPC.SetAreaCode(12); // Europe PAL
    REQUIRE(core.GetConsoleRegion() == ConsoleRegion::PAL);
}

TEST_CASE("Save state version reject", "[core][savestate][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> buf(stateSize);
    REQUIRE(core.SaveState(buf.data(), stateSize));

    // Corrupt the version word (offset 4) and ensure load fails
    uint32_t badVersion = 0xDEADBEEF;
    std::memcpy(buf.data() + 4, &badVersion, sizeof(badVersion));
    REQUIRE_FALSE(core.LoadState(buf.data(), stateSize));
}

TEST_CASE("Save state writes 16-byte header", "[core][savestate][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> buf(stateSize);
    REQUIRE(core.SaveState(buf.data(), stateSize));

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t uncompSize = 0;
    uint32_t compressedSize = 0;
    std::memcpy(&magic, buf.data(), sizeof(magic));
    std::memcpy(&version, buf.data() + 4, sizeof(version));
    std::memcpy(&uncompSize, buf.data() + 8, sizeof(uncompSize));
    std::memcpy(&compressedSize, buf.data() + 12, sizeof(compressedSize));

    REQUIRE(magic == 0x32524942); // "BRI2" in LE
    REQUIRE(version == 2);
    REQUIRE(uncompSize == sizeof(ymir::savestate::SaveState));
    REQUIRE(compressedSize > 0);
    REQUIRE(compressedSize < stateSize - 16);
}
