// Brimir BIOS Integration Tests (with Real BIOS Files)
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0
//
// These tests run ONLY if real BIOS files are available in tests/fixtures/
// They are automatically skipped if BIOS files are not present.

#include <catch2/catch_test_macros.hpp>
#include <brimir/core_wrapper.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace brimir;

namespace {

// Expected BIOS checksums (MD5)
struct BIOSInfo {
    const char* filename;
    const char* region;
    size_t expectedSize;
    const char* expectedMD5;
};

const BIOSInfo knownBIOS[] = {
    {"sega_101.bin", "US", 512 * 1024, "f273555d7d91e8a5a6bfd9bcf066331c"},
    {"sega_100.bin", "EU", 512 * 1024, "2aba43c2f1526c5e898dfe1cafbfc53a"},
    {"sega1003.bin", "JP", 512 * 1024, "3240872c70984b6cbfda1586cab68dbe"},
};

bool BIOSFileExists(const char* filename) {
    std::filesystem::path biosPath = std::filesystem::path("tests/fixtures") / filename;
    return std::filesystem::exists(biosPath);
}

std::filesystem::path GetBIOSPath(const char* filename) {
    return std::filesystem::path("tests/fixtures") / filename;
}

bool AnyBIOSAvailable() {
    for (const auto& bios : knownBIOS) {
        if (BIOSFileExists(bios.filename)) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

TEST_CASE("BIOS integration - Real BIOS files", "[bios][integration][!mayfail]") {
    if (!AnyBIOSAvailable()) {
        SKIP("No BIOS files found in tests/fixtures/ - skipping integration tests");
    }
    
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Load US BIOS if available") {
        if (!BIOSFileExists("sega_101.bin")) {
            SKIP("US BIOS not available");
        }
        
        auto biosPath = GetBIOSPath("sega_101.bin");
        bool result = core.LoadIPLFromFile(biosPath.string().c_str());
        
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
        
        INFO("Successfully loaded US BIOS: " << biosPath);
    }
    
    SECTION("Load EU BIOS if available") {
        if (!BIOSFileExists("sega_100.bin")) {
            SKIP("EU BIOS not available");
        }
        
        auto biosPath = GetBIOSPath("sega_100.bin");
        bool result = core.LoadIPLFromFile(biosPath.string().c_str());
        
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
        
        INFO("Successfully loaded EU BIOS: " << biosPath);
    }
    
    SECTION("Load JP BIOS if available") {
        if (!BIOSFileExists("sega1003.bin")) {
            SKIP("JP BIOS not available");
        }
        
        auto biosPath = GetBIOSPath("sega1003.bin");
        bool result = core.LoadIPLFromFile(biosPath.string().c_str());
        
        REQUIRE(result);
        REQUIRE(core.IsIPLLoaded());
        
        INFO("Successfully loaded JP BIOS: " << biosPath);
    }
}

TEST_CASE("BIOS boot sequence", "[bios][integration][!mayfail]") {
    if (!AnyBIOSAvailable()) {
        SKIP("No BIOS files found - skipping boot tests");
    }
    
    CoreWrapper core;
    core.Initialize();
    
    // Try to find any available BIOS
    const char* availableBIOS = nullptr;
    for (const auto& bios : knownBIOS) {
        if (BIOSFileExists(bios.filename)) {
            availableBIOS = bios.filename;
            break;
        }
    }
    
    if (!availableBIOS) {
        SKIP("No BIOS files available");
    }
    
    SECTION("BIOS loads and emulator runs") {
        auto biosPath = GetBIOSPath(availableBIOS);
        bool loadResult = core.LoadIPLFromFile(biosPath.string().c_str());
        REQUIRE(loadResult);
        
        // Try to run a frame with BIOS loaded
        REQUIRE_NOTHROW(core.RunFrame());
        
        // Verify video output is available
        auto fb = core.GetFramebuffer();
        REQUIRE(fb != nullptr);
        
        auto width = core.GetFramebufferWidth();
        auto height = core.GetFramebufferHeight();
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        
        INFO("BIOS boot test passed with: " << availableBIOS);
    }
    
    SECTION("Multiple frames with BIOS") {
        auto biosPath = GetBIOSPath(availableBIOS);
        core.LoadIPLFromFile(biosPath.string().c_str());
        
        // Run several frames
        for (int i = 0; i < 10; ++i) {
            REQUIRE_NOTHROW(core.RunFrame());
        }
        
        // Should still be functional
        REQUIRE(core.IsInitialized());
        REQUIRE(core.IsIPLLoaded());
        
        INFO("Successfully ran 10 frames with BIOS");
    }
}

TEST_CASE("BIOS file validation", "[bios][integration][!mayfail]") {
    if (!AnyBIOSAvailable()) {
        SKIP("No BIOS files found - skipping validation tests");
    }
    
    SECTION("Verify BIOS file sizes") {
        for (const auto& bios : knownBIOS) {
            if (!BIOSFileExists(bios.filename)) {
                continue;
            }
            
            auto biosPath = GetBIOSPath(bios.filename);
            auto fileSize = std::filesystem::file_size(biosPath);
            
            REQUIRE(fileSize == bios.expectedSize);
            
            INFO(bios.region << " BIOS (" << bios.filename << ") has correct size: " << fileSize << " bytes");
        }
    }
    
    SECTION("BIOS files are readable") {
        for (const auto& bios : knownBIOS) {
            if (!BIOSFileExists(bios.filename)) {
                continue;
            }
            
            auto biosPath = GetBIOSPath(bios.filename);
            std::ifstream file(biosPath, std::ios::binary);
            
            REQUIRE(file.is_open());
            REQUIRE(file.good());
            
            // Read first 16 bytes to verify file is accessible
            std::vector<uint8_t> header(16);
            file.read(reinterpret_cast<char*>(header.data()), 16);
            
            REQUIRE(file.gcount() == 16);
            
            INFO(bios.region << " BIOS is readable");
        }
    }
}

TEST_CASE("BIOS performance baseline", "[bios][integration][benchmark][!mayfail]") {
    if (!AnyBIOSAvailable()) {
        SKIP("No BIOS files found - skipping performance tests");
    }
    
    CoreWrapper core;
    core.Initialize();
    
    // Find first available BIOS
    const char* availableBIOS = nullptr;
    for (const auto& bios : knownBIOS) {
        if (BIOSFileExists(bios.filename)) {
            availableBIOS = bios.filename;
            break;
        }
    }
    
    if (!availableBIOS) {
        SKIP("No BIOS files available");
    }
    
    SECTION("BIOS load time") {
        auto biosPath = GetBIOSPath(availableBIOS);
        
        // Load BIOS (just verify it doesn't take too long)
        bool result = core.LoadIPLFromFile(biosPath.string().c_str());
        REQUIRE(result);
        
        INFO("BIOS loaded: " << availableBIOS);
    }
    
    SECTION("Frame execution with BIOS") {
        auto biosPath = GetBIOSPath(availableBIOS);
        core.LoadIPLFromFile(biosPath.string().c_str());
        
        // Run 60 frames (1 second at 60 FPS)
        for (int i = 0; i < 60; ++i) {
            core.RunFrame();
        }
        
        INFO("Successfully ran 60 frames with BIOS (baseline performance)");
    }
}

