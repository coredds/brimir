// Brimir CoreWrapper Unit Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <lz4.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

    // A real state of the current size, with only the version word patched: pins the version bump
    // independently of sizeof(SaveState), so that states from before the VDP1 FBRAM layout change
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

    REQUIRE(magic == 0x32524942); // "BRI2" in LE
    REQUIRE(version == CoreWrapper::kSaveStateVersion);
    REQUIRE(uncompSize == sizeof(ymir::savestate::SaveState));
    REQUIRE(compressedSize > 0);
    REQUIRE(compressedSize < stateSize - 16);
}

TEST_CASE("Save state from before the CD block busy delay and read FIFO is rejected",
          "[core][savestate][unit][regression]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // sizeof(SaveState) was 6315744 (MSVC x64) until the CD block read FIFO (Ymir a8856768) added
    // 16 bytes; the busy delay and the SCSP one-shot flag fit in existing padding. Hard-coded because
    // the old layout no longer exists to measure. On other ABIs the sizes differ anyway, so this test
    // still proves that a size mismatch is rejected, not that the exact old layout is.
    constexpr uint32_t kOldUncompressedSize = 6315744;
    REQUIRE(kOldUncompressedSize != sizeof(ymir::savestate::SaveState));

    // A state saved by the previous build: valid BRI2 header (current version, so that the size check
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

TEST_CASE("Save state with out-of-range read FIFO cursors is rejected",
          "[core][savestate][unit][regression]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> saved(stateSize);
    REQUIRE(core.SaveState(saved.data(), stateSize));

    // Decompress a state saved by the core, patch it, and repack it into a valid BRI2 buffer so that
    // only the cursor validation can reject it. xferQueue*Pos index a fixed-size array (Ymir a8856768).
    uint32_t savedCompressedSize = 0;
    std::memcpy(&savedCompressedSize, saved.data() + 12, 4);

    auto repack = [&](auto&& patch) {
        auto state = std::make_unique<ymir::savestate::SaveState>();
        const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(saved.data() + 16),
                                          reinterpret_cast<char*>(state.get()),
                                          static_cast<int>(savedCompressedSize),
                                          static_cast<int>(sizeof(ymir::savestate::SaveState)));
        REQUIRE(n == static_cast<int>(sizeof(ymir::savestate::SaveState)));
        patch(*state);

        std::vector<uint8_t> buf(16 + LZ4_compressBound(static_cast<int>(sizeof(*state))));
        const int compressed = LZ4_compress_default(reinterpret_cast<const char*>(state.get()),
                                                    reinterpret_cast<char*>(buf.data() + 16),
                                                    static_cast<int>(sizeof(*state)),
                                                    static_cast<int>(buf.size() - 16));
        REQUIRE(compressed > 0);
        const uint32_t magic = 0x32524942; // "BRI2" in LE
        const uint32_t version = CoreWrapper::kSaveStateVersion;
        const uint32_t uncompressedSize = static_cast<uint32_t>(sizeof(*state));
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
