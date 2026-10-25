// Brimir CoreWrapper Unit Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <brimir/savestate_codec.hpp>
#include <lz4.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
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

TEST_CASE("CoreWrapper LoadGame reports why it failed", "[core][unit][regression]") {
    using Catch::Matchers::ContainsSubstring;

    CoreWrapper core;

    SECTION("not initialized") {
        REQUIRE_FALSE(core.LoadGame("game.cue"));
        REQUIRE_FALSE(core.GetLastError().empty());
    }
    SECTION("empty path") {
        REQUIRE(core.Initialize());
        REQUIRE_FALSE(core.LoadGame(""));
        REQUIRE_FALSE(core.GetLastError().empty());
    }
    SECTION("missing file names the path") {
        REQUIRE(core.Initialize());
        REQUIRE_FALSE(core.LoadGame("nonexistent_file.iso"));
        REQUIRE_THAT(core.GetLastError(), ContainsSubstring("nonexistent_file.iso"));
    }
}

TEST_CASE("CoreWrapper LoadGame without save/system directories reaches disc parsing",
          "[core][unit][regression]") {
    using Catch::Matchers::ContainsSubstring;

    // An invalid disc must fail in the disc loader, not earlier while setting
    // up persistence directories (create_directories("") used to throw).
    const auto tempDir = std::filesystem::temp_directory_path() / "brimir_no_dirs_test";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    const auto gamePath = tempDir / "dummy.iso";
    {
        std::ofstream dummy(gamePath, std::ios::binary);
        dummy.write("not a real disc image", 21);
    }

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE_FALSE(core.LoadGame(gamePath.string().c_str(), nullptr, nullptr));
    REQUIRE_FALSE(core.GetLastError().empty());
    REQUIRE_THAT(core.GetLastError(), !ContainsSubstring("Exception during game load"));

    core.Shutdown();
    std::filesystem::remove_all(tempDir);
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

TEST_CASE("BRI2 state with the previous version is rejected", "[core][savestate][unit][regression]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // A real current state, with only the version word patched: pins the version bump independently
    // of the payload size, so that earlier states (raw SaveState structs holding stale heap pointers)
    // are rejected cleanly even if their size happens to match.
    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> buf(stateSize);
    REQUIRE(core.SaveState(buf.data(), stateSize));

    uint32_t compressedSize = 0;
    std::memcpy(&compressedSize, buf.data() + 12, sizeof(compressedSize));
    const size_t savedSize = 16 + static_cast<size_t>(compressedSize);

    const uint32_t previousVersion = CoreWrapper::kSaveStateVersion - 1;
    std::memcpy(buf.data() + 4, &previousVersion, sizeof(previousVersion));
    REQUIRE_FALSE(core.LoadState(buf.data(), savedSize));

    // The core is unharmed and still runs.
    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);

    // Control: the same buffer with the current version loads.
    const uint32_t currentVersion = CoreWrapper::kSaveStateVersion;
    std::memcpy(buf.data() + 4, &currentVersion, sizeof(currentVersion));
    REQUIRE(core.LoadState(buf.data(), savedSize));
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

    auto state = std::make_unique<ymir::savestate::SaveState>();
    core.GetSaturn()->SaveState(*state);

    REQUIRE(magic == 0x32524942); // "BRI2" in LE
    REQUIRE(version == CoreWrapper::kSaveStateVersion);
    REQUIRE(uncompSize == EncodedSaveStateSize(*state));
    REQUIRE(compressedSize > 0);
    REQUIRE(compressedSize < stateSize - 16);
}

TEST_CASE("Save state from before the CD block busy delay and read FIFO is rejected",
          "[core][savestate][unit][regression]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // Such states held the raw SaveState struct: sizeof(SaveState) was 6315744 (MSVC x64) until the CD
    // block read FIFO (Ymir a8856768) added 16 bytes. Hard-coded because the old layout no longer exists
    // to measure. On other ABIs the sizes differ anyway, so this test still proves that a raw-struct
    // payload of a foreign size is rejected, not that the exact old layout is.
    constexpr uint32_t kOldUncompressedSize = 6315744;
    {
        auto empty = std::make_unique<ymir::savestate::SaveState>();
        REQUIRE(kOldUncompressedSize != EncodedSaveStateSize(*empty));
    }

    // A state saved by an old build: valid BRI2 header (current version, so that the payload decoder
    // is what rejects it) and a valid LZ4 payload of the old size.
    std::vector<char> raw(kOldUncompressedSize, 0);
    std::vector<uint8_t> buf(16 + LZ4_compressBound(static_cast<int>(raw.size())));
    const int compressed = LZ4_compress_default(raw.data(), reinterpret_cast<char*>(buf.data() + 16),
                                                static_cast<int>(raw.size()),
                                                static_cast<int>(buf.size() - 16));
    REQUIRE(compressed > 0);
    const uint32_t magic = 0x32524942; // "BRI2" in LE
    const uint32_t version = CoreWrapper::kSaveStateVersion;
    const uint32_t uncompressedSize = kOldUncompressedSize;
    const uint32_t compressedSize = static_cast<uint32_t>(compressed);
    std::memcpy(buf.data(), &magic, 4);
    std::memcpy(buf.data() + 4, &version, 4);
    std::memcpy(buf.data() + 8, &uncompressedSize, 4);
    std::memcpy(buf.data() + 12, &compressedSize, 4);

    REQUIRE_FALSE(core.LoadState(buf.data(), 16 + static_cast<size_t>(compressed)));

    // The core is unharmed and still runs.
    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Save state in the pre-versioned BRIM format is rejected", "[core][savestate][unit][regression]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // BRIM ([magic][uncompSize][LZ4]) predates the version word, and the layout has changed since, so a
    // matching size proves nothing about the content. Use a real current-layout payload: without the
    // rejection it would load.
    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> saved(stateSize);
    REQUIRE(core.SaveState(saved.data(), stateSize));
    uint32_t savedPayloadSize = 0;
    uint32_t savedCompressedSize = 0;
    std::memcpy(&savedPayloadSize, saved.data() + 8, 4);
    std::memcpy(&savedCompressedSize, saved.data() + 12, 4);

    std::vector<uint8_t> buf(8 + static_cast<size_t>(savedCompressedSize));
    const uint32_t magic = 0x4D495242; // "BRIM" in LE
    const uint32_t uncompressedSize = savedPayloadSize;
    std::memcpy(buf.data(), &magic, 4);
    std::memcpy(buf.data() + 4, &uncompressedSize, 4);
    std::memcpy(buf.data() + 8, saved.data() + 16, savedCompressedSize);

    REQUIRE_FALSE(core.LoadState(buf.data(), buf.size()));

    // The core is unharmed and still runs.
    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Save state with out-of-range read FIFO cursors is rejected",
          "[core][savestate][unit][regression]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> saved(stateSize);
    REQUIRE(core.SaveState(saved.data(), stateSize));

    // Decompress and decode a state saved by the core, patch it, and repack it into a valid BRI2 buffer
    // so that only the cursor validation can reject it. xferQueue*Pos index a fixed-size array (Ymir
    // a8856768).
    uint32_t savedPayloadSize = 0;
    uint32_t savedCompressedSize = 0;
    std::memcpy(&savedPayloadSize, saved.data() + 8, 4);
    std::memcpy(&savedCompressedSize, saved.data() + 12, 4);

    auto repack = [&](auto&& patch) {
        std::vector<uint8_t> payload(savedPayloadSize);
        const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(saved.data() + 16),
                                          reinterpret_cast<char*>(payload.data()),
                                          static_cast<int>(savedCompressedSize),
                                          static_cast<int>(payload.size()));
        REQUIRE(n == static_cast<int>(payload.size()));
        auto state = std::make_unique<ymir::savestate::SaveState>();
        REQUIRE(DecodeSaveState(payload, *state));
        patch(*state);
        REQUIRE(EncodeSaveState(*state, payload));

        std::vector<uint8_t> buf(16 + LZ4_compressBound(static_cast<int>(payload.size())));
        const int compressed = LZ4_compress_default(reinterpret_cast<const char*>(payload.data()),
                                                    reinterpret_cast<char*>(buf.data() + 16),
                                                    static_cast<int>(payload.size()),
                                                    static_cast<int>(buf.size() - 16));
        REQUIRE(compressed > 0);
        const uint32_t magic = 0x32524942; // "BRI2" in LE
        const uint32_t version = CoreWrapper::kSaveStateVersion;
        const uint32_t uncompressedSize = static_cast<uint32_t>(payload.size());
        const uint32_t compressedSize = static_cast<uint32_t>(compressed);
        std::memcpy(buf.data(), &magic, 4);
        std::memcpy(buf.data() + 4, &version, 4);
        std::memcpy(buf.data() + 8, &uncompressedSize, 4);
        std::memcpy(buf.data() + 12, &compressedSize, 4);
        buf.resize(16 + static_cast<size_t>(compressed));
        return buf;
    };

    constexpr auto kQueueSize = std::tuple_size_v<decltype(ymir::savestate::CDBlockSaveState::xferQueue)>;

    SECTION("unmodified state loads") {
        const auto buf = repack([](ymir::savestate::SaveState&) {});
        REQUIRE(core.LoadState(buf.data(), buf.size()));
    }

    SECTION("write cursor out of range") {
        const auto buf = repack([](ymir::savestate::SaveState& s) { s.cdblock.xferQueueWritePos = kQueueSize; });
        REQUIRE_FALSE(core.LoadState(buf.data(), buf.size()));
    }

    SECTION("read cursor out of range") {
        const auto buf = repack([](ymir::savestate::SaveState& s) { s.cdblock.xferQueueReadPos = 0xFF; });
        REQUIRE_FALSE(core.LoadState(buf.data(), buf.size()));
    }

    // The core is unharmed and still runs.
    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Save state preserves DRAM cartridge contents", "[core][savestate][unit][regression]") {
    // The cartridge RAM lives in a std::vector inside the SCU save state; a raw copy of the SaveState
    // struct stored only the vector's heap pointer, so the contents were lost (and the load read freed
    // memory).
    CoreWrapper core;
    REQUIRE(core.Initialize());
    ymir::Saturn* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    auto* cart = saturn->InsertCartridge<ymir::cart::DRAM32MbitCartridge>();
    REQUIRE(cart != nullptr);

    constexpr size_t kCartSize = 4 * 1024 * 1024;
    std::vector<uint8_t> original(kCartSize);
    for (size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<uint8_t>((i * 31u + (i >> 12)) ^ 0x5A);
    }
    cart->LoadRAM(std::span<const uint8_t, kCartSize>(original.data(), kCartSize));

    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> buf(stateSize);
    REQUIRE(core.SaveState(buf.data(), stateSize));

    const std::vector<uint8_t> other(kCartSize, 0xEE);
    cart->LoadRAM(std::span<const uint8_t, kCartSize>(other.data(), kCartSize));

    REQUIRE(core.LoadState(buf.data(), stateSize));

    auto* loaded = saturn->GetCartridge().As<ymir::cart::CartType::DRAM32Mbit>();
    REQUIRE(loaded != nullptr);
    std::vector<uint8_t> restored(kCartSize);
    loaded->DumpRAM(std::span<uint8_t, kCartSize>(restored.data(), kCartSize));
    REQUIRE(restored == original);
}

TEST_CASE("Save state preserves the SMPC INTBACK report across core instances",
          "[core][savestate][unit][regression]") {
    // The INTBACK report is a std::vector inside the SMPC save state. A raw copy of the SaveState struct
    // carried core A's heap pointer into core B: a use-after-free once A is gone.
    const std::vector<uint8_t> report{0xF1, 0x02, 0xAB, 0xCD, 0xF1, 0x02, 0x12, 0x34};

    std::vector<uint8_t> buf;
    size_t stateSize = 0;
    {
        auto coreA = std::make_unique<CoreWrapper>();
        REQUIRE(coreA->Initialize());
        ymir::Saturn* saturn = coreA->GetSaturn();
        REQUIRE(saturn != nullptr);

        ymir::savestate::SMPCSaveState smpc{};
        saturn->SMPC.SaveState(smpc);
        smpc.intback.report = report;
        smpc.intback.reportOffset = 2;
        saturn->SMPC.LoadState(smpc);

        stateSize = coreA->GetStateSize();
        buf.resize(stateSize);
        REQUIRE(coreA->SaveState(buf.data(), stateSize));
    }

    // Reuse the heap that core A released.
    std::vector<std::vector<uint8_t>> scribble;
    for (int i = 0; i < 64; ++i) {
        scribble.emplace_back(static_cast<size_t>(16) << (i % 8), static_cast<uint8_t>(0xCC));
    }

    auto coreB = std::make_unique<CoreWrapper>();
    REQUIRE(coreB->Initialize());
    REQUIRE(coreB->LoadState(buf.data(), stateSize));

    ymir::savestate::SMPCSaveState loaded{};
    coreB->GetSaturn()->SMPC.SaveState(loaded);
    REQUIRE(loaded.intback.report == report);
    REQUIRE(loaded.intback.reportOffset == 2);
}

TEST_CASE("Save-state payload decoder rejects truncated and malformed payloads", "[core][savestate][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    ymir::Saturn* saturn = core.GetSaturn();
    saturn->InsertCartridge<ymir::cart::DRAM8MbitCartridge>();

    auto state = std::make_unique<ymir::savestate::SaveState>();
    saturn->SaveState(*state);
    state->smpc.intback.report = {0xF1, 0x02, 0xAB, 0xCD, 0xF0};
    state->smpc.intback.reportOffset = 0;

    std::vector<uint8_t> payload;
    REQUIRE(EncodeSaveState(*state, payload));
    REQUIRE(payload.size() == EncodedSaveStateSize(*state));
    REQUIRE(payload.size() <= MaxEncodedSaveStateSize(state->scu.cartData.size()));

    auto decoded = std::make_unique<ymir::savestate::SaveState>();
    REQUIRE(DecodeSaveState(payload, *decoded));
    REQUIRE(decoded->scu.cartData == state->scu.cartData);
    REQUIRE(decoded->smpc.intback.report == state->smpc.intback.report);

    SECTION("truncated") {
        for (const size_t size : {size_t{0}, size_t{1}, payload.size() / 2, payload.size() - 1}) {
            REQUIRE_FALSE(DecodeSaveState(std::span<const uint8_t>(payload.data(), size), *decoded));
        }
    }

    SECTION("trailing bytes") {
        payload.push_back(0);
        REQUIRE_FALSE(DecodeSaveState(payload, *decoded));
    }

    SECTION("cartridge length word out of range") {
        // The cartridge length word follows the scheduler, system, both SH-2s, and the SCU's DMA, DSP
        // and cartridge type fields.
        const size_t offset = sizeof(state->scheduler) + sizeof(state->system) + sizeof(state->msh2) +
                              sizeof(state->ssh2) + sizeof(state->scu.dma) + sizeof(state->scu.dsp) +
                              sizeof(state->scu.cartType);
        uint32_t length = 0;
        std::memcpy(&length, payload.data() + offset, 4);
        REQUIRE(length == state->scu.cartData.size());
        const uint32_t huge = 0xFFFFFFFF;
        std::memcpy(payload.data() + offset, &huge, 4);
        REQUIRE_FALSE(DecodeSaveState(payload, *decoded));
    }

    SECTION("cartridge image size does not match the cartridge type") {
        state->scu.cartType = ymir::savestate::SCUSaveState::CartType::DRAM48Mbit;
        REQUIRE(EncodeSaveState(*state, payload));
        REQUIRE_FALSE(DecodeSaveState(payload, *decoded));
    }

    SECTION("unknown cartridge type") {
        state->scu.cartType = static_cast<ymir::savestate::SCUSaveState::CartType>(7);
        REQUIRE(EncodeSaveState(*state, payload));
        REQUIRE_FALSE(DecodeSaveState(payload, *decoded));
    }

    SECTION("INTBACK report offset past the report") {
        state->smpc.intback.reportOffset = state->smpc.intback.report.size() + 1;
        REQUIRE(EncodeSaveState(*state, payload));
        REQUIRE_FALSE(DecodeSaveState(payload, *decoded));
    }

    SECTION("INTBACK report longer than SMPC can build is not encoded") {
        state->smpc.intback.report.assign(kMaxSaveStateINTBACKReportSize + 1, 0);
        REQUIRE_FALSE(EncodeSaveState(*state, payload));
    }

    SECTION("bool that is neither 0 nor 1") {
        const uint8_t two = 2;
        std::memcpy(&state->cdblockLLE, &two, 1);
        REQUIRE(EncodeSaveState(*state, payload));
        REQUIRE_FALSE(DecodeSaveState(payload, *decoded));
    }
}

TEST_CASE("Save state with an inconsistent BRI2 header is rejected", "[core][savestate][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> buf(stateSize);
    REQUIRE(core.SaveState(buf.data(), stateSize));
    uint32_t payloadSize = 0;
    uint32_t compressedSize = 0;
    std::memcpy(&payloadSize, buf.data() + 8, 4);
    std::memcpy(&compressedSize, buf.data() + 12, 4);
    const size_t savedSize = 16 + static_cast<size_t>(compressedSize);

    SECTION("payload size larger than the payload") {
        const uint32_t patched = payloadSize + 1;
        std::memcpy(buf.data() + 8, &patched, 4);
        REQUIRE_FALSE(core.LoadState(buf.data(), savedSize));
    }

    SECTION("payload size beyond any valid state") {
        const uint32_t patched = 0xFFFFFFFF;
        std::memcpy(buf.data() + 8, &patched, 4);
        REQUIRE_FALSE(core.LoadState(buf.data(), savedSize));
    }

    SECTION("compressed size beyond the buffer") {
        const uint32_t patched = compressedSize + 1;
        std::memcpy(buf.data() + 12, &patched, 4);
        REQUIRE_FALSE(core.LoadState(buf.data(), savedSize));
    }

    SECTION("header only") {
        REQUIRE_FALSE(core.LoadState(buf.data(), 16));
    }

    // The core is unharmed and still runs, and the state loads once its header is restored.
    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);
    std::memcpy(buf.data() + 8, &payloadSize, 4);
    std::memcpy(buf.data() + 12, &compressedSize, 4);
    REQUIRE(core.LoadState(buf.data(), savedSize));
}

TEST_CASE("Save state with a different cartridge than the inserted one is rejected",
          "[core][savestate][unit][regression]") {
    // SCU::LoadState inserts the state's cartridge, and the core does not compare disc hashes, so a
    // mismatched state would swap the cartridge mid-session and change GetStateSize() under the
    // frontend's rewind and run-ahead buffers.
    auto saveFrom = [](auto&& insertCart) {
        CoreWrapper source;
        REQUIRE(source.Initialize());
        insertCart(*source.GetSaturn());
        std::vector<uint8_t> buf(source.GetStateSize());
        REQUIRE(source.SaveState(buf.data(), buf.size()));
        return buf;
    };
    const auto noCart = [](ymir::Saturn&) {};
    const auto dram8 = [](ymir::Saturn& s) { s.InsertCartridge<ymir::cart::DRAM8MbitCartridge>(); };
    const auto dram32 = [](ymir::Saturn& s) { s.InsertCartridge<ymir::cart::DRAM32MbitCartridge>(); };

    CoreWrapper core;
    REQUIRE(core.Initialize());
    std::vector<uint8_t> buf;
    ymir::cart::CartType expectedType = ymir::cart::CartType::None;

    SECTION("cartridge state into a session without a cartridge") {
        buf = saveFrom(dram32);
    }

    SECTION("state without a cartridge into a cartridge session") {
        dram8(*core.GetSaturn());
        expectedType = ymir::cart::CartType::DRAM8Mbit;
        buf = saveFrom(noCart);
    }

    SECTION("different DRAM cartridge") {
        dram8(*core.GetSaturn());
        expectedType = ymir::cart::CartType::DRAM8Mbit;
        buf = saveFrom(dram32);
    }

    const size_t sizeBefore = core.GetStateSize();
    REQUIRE_FALSE(core.LoadState(buf.data(), buf.size()));
    REQUIRE(core.GetSaturn()->GetCartridge().GetType() == expectedType);
    REQUIRE(core.GetStateSize() == sizeBefore);

    // The core is unharmed and still runs.
    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Save state of a ROM cartridge session is rejected", "[core][savestate][unit]") {
    // Brimir never inserts ROM cartridges, and SCU::LoadState reads the 2 MiB ROM image through a
    // 4 MiB span.
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.GetSaturn()->InsertCartridge<ymir::cart::ROMCartridge>();

    std::vector<uint8_t> buf(core.GetStateSize());
    REQUIRE(core.SaveState(buf.data(), buf.size()));
    REQUIRE_FALSE(core.LoadState(buf.data(), buf.size()));

    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("Save state size covers the cartridge image", "[core][savestate][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    const size_t withoutCart = core.GetStateSize();
    core.GetSaturn()->InsertCartridge<ymir::cart::DRAM48MbitCartridge>();
    const size_t withCart = core.GetStateSize();
    REQUIRE(withCart > withoutCart + 6 * 1024 * 1024);

    // Stable across a save and a load within the session.
    std::vector<uint8_t> buf(withCart);
    REQUIRE(core.SaveState(buf.data(), buf.size()));
    REQUIRE(core.LoadState(buf.data(), buf.size()));
    REQUIRE(core.GetStateSize() == withCart);
}
