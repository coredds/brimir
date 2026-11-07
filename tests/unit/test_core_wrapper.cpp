// Brimir CoreWrapper Unit Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "unit/frame_path_cases.hpp"
#include <brimir/core_wrapper.hpp>
#include <brimir/savestate_codec.hpp>
#include <lz4.h>
#include <ymir/hw/smpc/smpc_defs.hpp>
#include <ymir/util/date_time.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
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

TEST_CASE("BRI2 state with a version before 4 is rejected", "[core][savestate][unit][regression]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // A real current state, with only the version word patched: pins the oldest loadable version
    // independently of the payload size, so that states before version 4 (raw SaveState structs
    // holding stale heap pointers) are rejected cleanly even if their size happens to match.
    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> buf(stateSize);
    REQUIRE(core.SaveState(buf.data(), stateSize));

    uint32_t compressedSize = 0;
    std::memcpy(&compressedSize, buf.data() + 12, sizeof(compressedSize));
    const size_t savedSize = 16 + static_cast<size_t>(compressedSize);

    STATIC_REQUIRE(CoreWrapper::kOldestLoadableSaveStateVersion == 4);
    STATIC_REQUIRE(CoreWrapper::kSaveStateVersion == 10);

    for (const uint32_t version : {0u, 1u, 2u, 3u, CoreWrapper::kSaveStateVersion + 1}) {
        CAPTURE(version);
        std::memcpy(buf.data() + 4, &version, sizeof(version));
        REQUIRE_FALSE(core.LoadState(buf.data(), savedSize));
    }

    // The core is unharmed and still runs.
    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);

    // Control: the same buffer loads as the current version.
    const uint32_t current = CoreWrapper::kSaveStateVersion;
    std::memcpy(buf.data() + 4, &current, sizeof(current));
    REQUIRE(core.LoadState(buf.data(), savedSize));
}

namespace {

std::vector<uint8_t> RepackStateAs(const std::vector<uint8_t>& saved, uint32_t version);

} // namespace

TEST_CASE("Version 4, 5 and 6 states (without all of the Brimir extension) still load", "[core][savestate][unit][brimir]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> saved(stateSize);
    REQUIRE(core.SaveState(saved.data(), stateSize));
    for (const uint32_t version : {4u, 5u, 6u}) {
        CAPTURE(version);
        const auto buf = RepackStateAs(saved, version);
        REQUIRE(core.LoadState(buf.data(), buf.size()));
        // The payload of the current version is the older one plus the extension: an older header on it is refused.
        std::vector<uint8_t> patched = saved;
        std::memcpy(patched.data() + 4, &version, sizeof(version));
        REQUIRE_FALSE(core.LoadState(patched.data(), patched.size()));
    }
    // Version 7 has the version 8 layout (only backup memory and ROM cartridge images differ); both end before the
    // peripherals' state of version 9, which ends before the Virtua Gun jitter of version 10.
    for (const uint32_t version : {7u, 8u, 9u}) {
        CAPTURE(version);
        const auto buf = RepackStateAs(saved, version);
        REQUIRE(core.LoadState(buf.data(), buf.size()));
        std::vector<uint8_t> patched = saved;
        std::memcpy(patched.data() + 4, &version, sizeof(version));
        REQUIRE_FALSE(core.LoadState(patched.data(), patched.size()));
    }
    core.RunFrame();
    REQUIRE(core.GetFramebuffer() != nullptr);
}

TEST_CASE("A version 6 state leaves the internal backup RAM alone", "[core][savestate][unit][brimir][regression]") {
    // Version 6 states hold no backup RAM; the decoder reuses the core's scratch state, which held the last saved
    // state's backup RAM.
    CoreWrapper core;
    REQUIRE(core.Initialize());
    auto& bup = core.GetSaturn()->mem.GetInternalBackupRAM();
    constexpr uint32_t kAddress = 0x1234 << 1;
    bup.WriteByte(kAddress, 0x11);
    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> saved(stateSize);
    REQUIRE(core.SaveState(saved.data(), stateSize));
    bup.WriteByte(kAddress, 0x22);

    const auto v6 = RepackStateAs(saved, 6);
    REQUIRE(core.LoadState(v6.data(), v6.size()));
    CHECK(core.GetSaturn()->mem.GetInternalBackupRAM().ReadByte(kAddress) == 0x22);

    // Control: the current version restores it.
    REQUIRE(core.LoadState(saved.data(), saved.size()));
    CHECK(core.GetSaturn()->mem.GetInternalBackupRAM().ReadByte(kAddress) == 0x11);
}

namespace {

// Decodes a BRI2 state saved by the core, patches the decoded SaveState and repacks it with the given version.
template <typename Patch>
std::vector<uint8_t> RepackState(const std::vector<uint8_t>& saved, uint32_t version, Patch&& patch) {
    uint32_t savedPayloadSize = 0;
    uint32_t savedCompressedSize = 0;
    std::memcpy(&savedPayloadSize, saved.data() + 8, 4);
    std::memcpy(&savedCompressedSize, saved.data() + 12, 4);

    std::vector<uint8_t> payload(savedPayloadSize);
    const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(saved.data() + 16),
                                      reinterpret_cast<char*>(payload.data()),
                                      static_cast<int>(savedCompressedSize), static_cast<int>(payload.size()));
    REQUIRE(n == static_cast<int>(payload.size()));
    auto state = std::make_unique<ymir::savestate::SaveState>();
    REQUIRE(DecodeSaveState(payload, *state));
    patch(*state);
    REQUIRE(EncodeSaveState(*state, payload));
    if (version < 6) {
        // Versions 4 and 5 end before the Brimir extension, the payload's last member.
        payload.resize(payload.size() - sizeof(ymir::savestate::BrimirSaveState));
    } else if (version == 6) {
        // Version 6 ends before its scspSlots.
        payload.resize(payload.size() - sizeof(ymir::savestate::BrimirSaveState) +
                       ymir::savestate::kBrimirSaveStateV6Size);
    } else if (version < 9) {
        // Versions 7 and 8 end before the peripherals.
        payload.resize(payload.size() - sizeof(ymir::savestate::BrimirSaveState) +
                       ymir::savestate::kBrimirSaveStateV8Size);
    } else if (version == 9) {
        // Version 9 ends before the Virtua Gun jitter.
        payload.resize(payload.size() - sizeof(ymir::savestate::BrimirSaveState) +
                       ymir::savestate::kBrimirSaveStateV9Size);
    }

    std::vector<uint8_t> buf(16 + LZ4_compressBound(static_cast<int>(payload.size())));
    const int compressed = LZ4_compress_default(reinterpret_cast<const char*>(payload.data()),
                                                reinterpret_cast<char*>(buf.data() + 16),
                                                static_cast<int>(payload.size()), static_cast<int>(buf.size() - 16));
    REQUIRE(compressed > 0);
    const uint32_t magic = 0x32524942; // "BRI2" in LE
    const uint32_t uncompressedSize = static_cast<uint32_t>(payload.size());
    const uint32_t compressedSize = static_cast<uint32_t>(compressed);
    std::memcpy(buf.data(), &magic, 4);
    std::memcpy(buf.data() + 4, &version, 4);
    std::memcpy(buf.data() + 8, &uncompressedSize, 4);
    std::memcpy(buf.data() + 12, &compressedSize, 4);
    buf.resize(16 + static_cast<size_t>(compressed));
    return buf;
}

std::vector<uint8_t> RepackStateAs(const std::vector<uint8_t>& saved, uint32_t version) {
    return RepackState(saved, version, [](ymir::savestate::SaveState&) {});
}

} // namespace

TEST_CASE("Version 4 states have their VDP2 fetcher palette numbers converted on load",
          "[core][savestate][unit][regression][brimir]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    const size_t stateSize = core.GetStateSize();
    std::vector<uint8_t> saved(stateSize);
    REQUIRE(core.SaveState(saved.data(), stateSize));

    // Version 4 stored Character::palNum >> 4; version 5 stores all 7 bits.
    const auto patch = [](ymir::savestate::SaveState& s) {
        for (size_t field = 0; field < 2; ++field) {
            for (size_t i = 0; i < 6; ++i) {
                auto& fetcher = s.vdp.renderer.vramFetchers[field][i];
                fetcher.currChar.palNum = static_cast<uint8_t>((field * 6 + i) & 7);
                fetcher.nextChar.palNum = static_cast<uint8_t>((field * 6 + i + 3) & 7);
            }
        }
    };

    auto reloadedPalNums = [&](uint32_t version) {
        const auto buf = RepackState(saved, version, patch);
        REQUIRE(core.LoadState(buf.data(), buf.size()));
        std::vector<uint8_t> out(stateSize);
        REQUIRE(core.SaveState(out.data(), stateSize));
        std::vector<std::pair<uint8_t, uint8_t>> palNums;
        RepackState(out, CoreWrapper::kSaveStateVersion, [&](ymir::savestate::SaveState& s) {
            for (const auto& field : s.vdp.renderer.vramFetchers) {
                for (const auto& fetcher : field) {
                    palNums.emplace_back(fetcher.currChar.palNum, fetcher.nextChar.palNum);
                }
            }
        });
        return palNums;
    };

    SECTION("version 4: shifted into the upper palette bits, as version 4 loaded them") {
        const auto palNums = reloadedPalNums(4);
        for (size_t k = 0; k < palNums.size(); ++k) {
            CAPTURE(k);
            CHECK(int{palNums[k].first} == static_cast<int>((k & 7) << 4));
            CHECK(int{palNums[k].second} == static_cast<int>(((k + 3) & 7) << 4));
        }
    }

    SECTION("version 5: kept as stored") {
        const auto palNums = reloadedPalNums(CoreWrapper::kSaveStateVersion);
        for (size_t k = 0; k < palNums.size(); ++k) {
            CAPTURE(k);
            CHECK(int{palNums[k].first} == static_cast<int>(k & 7));
            CHECK(int{palNums[k].second} == static_cast<int>((k + 3) & 7));
        }
    }
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

    SECTION("invalid Brimir extension") {
        // The extension is the payload's last member, written raw.
        const size_t ext = payload.size() - sizeof(ymir::savestate::BrimirSaveState);
        using B = ymir::savestate::BrimirSaveState;
        const auto patched = [&](size_t offset, const void* bytes, size_t size) {
            std::vector<uint8_t> p = payload;
            std::memcpy(p.data() + ext + offset, bytes, size);
            return p;
        };
        const uint8_t two = 2;
        const uint8_t zero = 0;
        const uint32_t pastVram = 0x80000 + 0x20;
        const uint32_t field = 2;
        const uint8_t longProgram = 129;
        REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, valid), &zero, 1), *decoded));
        REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, valid), &two, 1), *decoded));
        REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, vdp) + offsetof(B::VDP, TVMDDirty), &two, 1), *decoded));
        REQUIRE_FALSE(
            DecodeSaveState(patched(offsetof(B, vdp) + offsetof(B::VDP, vdp1InInfiniteLoop), &two, 1), *decoded));
        REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, vdp) + offsetof(B::VDP, VTimingField), &field, 4), *decoded));
        REQUIRE_FALSE(
            DecodeSaveState(patched(offsetof(B, vdp) + offsetof(B::VDP, vdp1ReturnAddress), &pastVram, 4), *decoded));
        REQUIRE_FALSE(
            DecodeSaveState(patched(offsetof(B, scsp) + offsetof(B::SCSP, dspProgramLength), &longProgram, 1), *decoded));
        REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, scspSlots) + offsetof(B::SCSPSlots, valid), &zero, 1), *decoded));
        REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, scspSlots) + offsetof(B::SCSPSlots, valid), &two, 1), *decoded));
        {
            using P = B::Peripherals;
            const size_t port1 = offsetof(B, peripherals) + offsetof(P, ports) + sizeof(P::Port);
            const uint8_t badType = 7;
            const uint8_t badPos = 22;
            REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, peripherals) + offsetof(P, valid), &zero, 1), *decoded));
            REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, peripherals) + offsetof(P, valid), &two, 1), *decoded));
            REQUIRE_FALSE(DecodeSaveState(patched(port1 + offsetof(P::Port, type), &badType, 1), *decoded));
            REQUIRE_FALSE(DecodeSaveState(patched(port1 + offsetof(P::Port, reportPos), &badPos, 1), *decoded));
            REQUIRE_FALSE(DecodeSaveState(patched(port1 + offsetof(P::Port, tl), &two, 1), *decoded));
            REQUIRE_FALSE(DecodeSaveState(patched(port1 + offsetof(P::Port, analogMode), &two, 1), *decoded));
            const uint8_t lastPos = 21;
            REQUIRE(DecodeSaveState(patched(port1 + offsetof(P::Port, reportPos), &lastPos, 1), *decoded));
        }
        {
            using G = B::VirtuaGun;
            const uint32_t zeroState = 0;
            const uint32_t badJitter = 4;
            REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, virtuaGun) + offsetof(G, valid), &zero, 1), *decoded));
            REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, virtuaGun) + offsetof(G, jitterState), &zeroState, 4),
                                          *decoded));
            REQUIRE_FALSE(DecodeSaveState(patched(offsetof(B, virtuaGun) + offsetof(G, lastJitterY), &badJitter, 4),
                                          *decoded));
        }
        // Without the extension (versions 4 and 5) the same bytes are not read.
        REQUIRE(DecodeSaveState(std::span<const uint8_t>(payload.data(), ext), *decoded, 5));
        REQUIRE_FALSE(decoded->brimir.valid);
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

    SECTION("backup memory and ROM cartridge image sizes per version") {
        using CartType = ymir::savestate::SCUSaveState::CartType;
        const auto decodes = [&](CartType type, size_t size, uint32_t version) {
            state->scu.cartType = type;
            state->scu.cartData.assign(size, 0);
            REQUIRE(EncodeSaveState(*state, payload));
            // Versions 7 and 8 end before the extension's peripherals (version 9), 9 before the Virtua Gun (10).
            const size_t ext = payload.size() - sizeof(ymir::savestate::BrimirSaveState);
            const size_t length = version < 9    ? ext + ymir::savestate::kBrimirSaveStateV8Size
                                  : version == 9 ? ext + ymir::savestate::kBrimirSaveStateV9Size
                                                 : payload.size();
            return DecodeSaveState(std::span<const uint8_t>(payload.data(), length), *decoded, version);
        };
        for (const size_t size : {512u * 1024u, 1024u * 1024u, 2u * 1024u * 1024u, 4u * 1024u * 1024u}) {
            CHECK(decodes(CartType::BackupMemory, size, 8));
            CHECK_FALSE(decodes(CartType::BackupMemory, size, 7));
        }
        CHECK_FALSE(decodes(CartType::BackupMemory, 0, 8));
        CHECK_FALSE(decodes(CartType::BackupMemory, 1000, 8));
        CHECK(decodes(CartType::BackupMemory, 0, 7));
        CHECK(decodes(CartType::ROM, 0, 8));
        CHECK_FALSE(decodes(CartType::ROM, 2 * 1024 * 1024, 8));
        CHECK(decodes(CartType::ROM, 2 * 1024 * 1024, 7));
        CHECK_FALSE(decodes(CartType::ROM, 0, 7));
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

TEST_CASE("cartridge: ROM cartridge session states hold no ROM and keep the inserted one",
          "[core][savestate][unit][cartridge]") {
    // The ROM is the content's dump: a state does not carry it (and SCU::LoadState would read a 2 MiB image through
    // a 4 MiB span).
    CoreWrapper core;
    REQUIRE(core.Initialize());
    const size_t withoutCart = core.GetStateSize();
    auto* rom = core.GetSaturn()->InsertCartridge<ymir::cart::ROMCartridge>();
    std::vector<uint8_t> image(2 * 1024 * 1024);
    for (size_t i = 0; i < image.size(); ++i) {
        image[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    rom->LoadROM(image);
    REQUIRE(core.GetStateSize() == withoutCart);

    std::vector<uint8_t> buf(core.GetStateSize());
    REQUIRE(core.SaveState(buf.data(), buf.size()));
    REQUIRE(core.LoadState(buf.data(), buf.size()));
    auto& cart = core.GetSaturn()->GetCartridge();
    REQUIRE(cart.GetType() == ymir::cart::CartType::ROM);
    CHECK(cart.ReadByte(0x0200'0000 + 0x12345) == image[0x12345]);
    CHECK(cart.ReadByte(0x0200'0000 + 0x1FFFFF) == image[0x1FFFFF]);

    // A version 7 state of a ROM session holds the 2 MiB image; it loads and the inserted ROM stays.
    const auto v7 = RepackState(buf, 7, [](ymir::savestate::SaveState& s) {
        s.scu.cartData.assign(2 * 1024 * 1024, 0xEE);
    });
    REQUIRE(core.LoadState(v7.data(), v7.size()));
    CHECK(core.GetSaturn()->GetCartridge().ReadByte(0x0200'0000 + 0x12345) == image[0x12345]);

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

// ============================================================
// Software frame path characterization: XBGR->XRGB, overscan crop, rotation
// ============================================================
//
// The expected pixels below are defined from the output side, independently of the bridge's loops:
//   1. Colour: native XBGR 0xAABBGGRR becomes XRGB 0x00RRGGBB (alpha byte dropped).
//   2. Crop: if the frame is wider than cropH+32 and taller than cropV+32 it is cut to (w-cropH) x (h-cropV),
//      starting cropH/2 px from the left and cropV/2 px from the top (integer division); otherwise it is untouched.
//   3. Rotation of the cropped image C (cw x ch): 90 is clockwise, the output is ch wide and cw tall.
//        90:  out(x, y) = C(y, ch-1-x)       180: out(x, y) = C(cw-1-x, ch-1-y)       270: out(x, y) = C(cw-1-y, x)

namespace {

using brimir::test::FramePathCase;

uint32_t ExpectedXRGB(uint32_t xbgr) {
    const uint32_t r = (xbgr >> 0) & 0xFF;
    const uint32_t g = (xbgr >> 8) & 0xFF;
    const uint32_t b = (xbgr >> 16) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

// Checks one case against the plain description above; returns an empty string on success.
std::string CheckFramePathCase(const FramePathCase& c) {
    uint32_t outW = 0, outH = 0;
    const std::vector<uint32_t> out = brimir::test::CpuFramePath(c, &outW, &outH);

    const bool crops = c.w > c.cropH + 32 && c.h > c.cropV + 32;
    const uint32_t cw = crops ? c.w - c.cropH : c.w;
    const uint32_t ch = crops ? c.h - c.cropV : c.h;
    const uint32_t left = crops ? c.cropH / 2 : 0;
    const uint32_t top = crops ? c.cropV / 2 : 0;

    const bool swap = c.rotation == 90 || c.rotation == 270;
    const uint32_t expectW = swap ? ch : cw;
    const uint32_t expectH = swap ? cw : ch;
    if (outW != expectW || outH != expectH) {
        return "size " + std::to_string(outW) + "x" + std::to_string(outH) + ", expected " +
               std::to_string(expectW) + "x" + std::to_string(expectH);
    }
    if (out.size() != static_cast<size_t>(expectW) * expectH) {
        return "pixel count " + std::to_string(out.size());
    }

    for (uint32_t y = 0; y < expectH; ++y) {
        for (uint32_t x = 0; x < expectW; ++x) {
            uint32_t cx = 0, cy = 0;
            switch (c.rotation) {
            case 0:   cx = x;           cy = y;           break;
            case 90:  cx = y;           cy = ch - 1 - x;  break;
            case 180: cx = cw - 1 - x;  cy = ch - 1 - y;  break;
            case 270: cx = cw - 1 - y;  cy = x;           break;
            }
            const uint32_t expected =
                ExpectedXRGB(brimir::test::PatternXBGR(cx + left, cy + top, c.w, c.h));
            if (out[static_cast<size_t>(y) * expectW + x] != expected) {
                char msg[160];
                std::snprintf(msg, sizeof msg, "pixel (%u,%u) = 0x%08X, expected 0x%08X", x, y,
                              out[static_cast<size_t>(y) * expectW + x], expected);
                return msg;
            }
        }
    }
    return {};
}

std::string DescribeFramePathCase(const FramePathCase& c) {
    return std::to_string(c.w) + "x" + std::to_string(c.h) + " crop " + std::to_string(c.cropH) + "," +
           std::to_string(c.cropV) + " rot " + std::to_string(c.rotation);
}

} // namespace

TEST_CASE("Frame path: XBGR to XRGB drops alpha and swaps R and B", "[core][frame-path]") {
    CoreWrapper core;
    const uint32_t frame[4] = {0xFF332211, 0x00FF0000, 0x800000FF, 0xAB00FF00};
    core.TestInjectFrame(frame, 4, 1);

    REQUIRE(core.GetFramebufferWidth() == 4);
    REQUIRE(core.GetFramebufferHeight() == 1);
    REQUIRE(core.GetFramebufferPitch() == 16);
    const uint32_t* out = static_cast<const uint32_t*>(core.GetFramebuffer());
    CHECK(out[0] == 0x00112233);
    CHECK(out[1] == 0x000000FF);
    CHECK(out[2] == 0x00FF0000);
    CHECK(out[3] == 0x0000FF00);
}

TEST_CASE("Frame path: pattern pixels are distinct", "[core][frame-path]") {
    const std::vector<uint32_t> frame = brimir::test::MakePatternFrame(704, 512);
    std::vector<uint32_t> sorted = frame;
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK(std::all_of(frame.begin(), frame.end(), [](uint32_t px) { return (px >> 24) == 0xFF; }));
}

TEST_CASE("Frame path: size and pixels for every size, overscan and rotation", "[core][frame-path]") {
    const auto& cases = brimir::test::FramePathCases();
    REQUIRE(cases.size() == 4 * 5 * 4 * 4);
    for (const FramePathCase& c : cases) {
        const std::string error = CheckFramePathCase(c);
        INFO(DescribeFramePathCase(c));
        INFO(error);
        REQUIRE(error.empty());
    }
}

TEST_CASE("Frame path: non-square crops and the crop guard", "[core][frame-path]") {
    for (const FramePathCase& c : brimir::test::FramePathExtraCases()) {
        const std::string error = CheckFramePathCase(c);
        INFO(DescribeFramePathCase(c));
        INFO(error);
        REQUIRE(error.empty());
    }
}

TEST_CASE("Frame path: pitch is tight and settings apply to the next frame", "[core][frame-path]") {
    CoreWrapper core;
    const std::vector<uint32_t> frame = brimir::test::MakePatternFrame(320, 224);

    core.TestInjectFrame(frame.data(), 320, 224);
    CHECK(core.GetFramebufferWidth() == 320);
    CHECK(core.GetFramebufferPitch() == 320 * 4);

    core.SetRotation(90);
    core.SetOverscanCrop(32, 32);
    CHECK(core.GetFramebufferWidth() == 320); // unchanged until the next frame

    core.TestInjectFrame(frame.data(), 320, 224);
    CHECK(core.GetFramebufferWidth() == 224 - 32);
    CHECK(core.GetFramebufferHeight() == 320 - 32);
    CHECK(core.GetFramebufferPitch() == (224 - 32) * 4);

    core.SetRotation(0);
    core.SetOverscanCrop(0, 0);
    core.TestInjectFrame(frame.data(), 320, 224);
    CHECK(core.GetFramebufferWidth() == 320);
    CHECK(core.GetFramebufferHeight() == 224);
    CHECK(core.GetFramebufferPitch() == 320 * 4);
}
TEST_CASE("Frame path: GPU output keeps the raw frame and skips the CPU transforms", "[core][frame-path]") {
    CoreWrapper core;
    const std::vector<uint32_t> first = brimir::test::MakePatternFrame(320, 224);
    core.TestInjectFrame(first.data(), 320, 224);
    const auto* sw = static_cast<const uint32_t*>(core.GetFramebuffer());
    const std::vector<uint32_t> softwareFrame(sw, sw + 320 * 224);
    CHECK_FALSE(core.IsGpuOutputActive());
    CHECK(core.RawFrame() == nullptr);

    core.SetGpuOutputActive(true);
    CHECK(core.IsGpuOutputActive());
    CHECK(core.RawFrame() == nullptr); // nothing captured yet
    core.SetRotation(90);
    core.SetOverscanCrop(32, 16);
    const std::vector<uint32_t> second = brimir::test::MakePatternFrame(352, 240);
    core.TestInjectFrame(second.data(), 352, 240);

    REQUIRE(core.RawFrame() != nullptr);
    CHECK(core.RawFrameWidth() == 352);
    CHECK(core.RawFrameHeight() == 240);
    CHECK(std::equal(second.begin(), second.end(), core.RawFrame())); // native XBGR, untouched
    CHECK(core.RawFrameSettings().cropH == 32);
    CHECK(core.RawFrameSettings().cropV == 16);
    CHECK(core.RawFrameSettings().rotation == 90);

    // The software output is neither converted nor cropped nor rotated while the GPU output is active.
    CHECK(core.GetFramebufferWidth() == 320);
    CHECK(core.GetFramebufferHeight() == 224);
    CHECK(core.GetFramebufferPitch() == 320 * 4);
    sw = static_cast<const uint32_t*>(core.GetFramebuffer());
    CHECK(std::equal(softwareFrame.begin(), softwareFrame.end(), sw));

    // Like the software path, settings changed after a frame apply to the next one.
    core.SetRotation(180);
    CHECK(core.RawFrameSettings().rotation == 90);

    core.SetGpuOutputActive(false);
    CHECK(core.RawFrame() == nullptr);
    core.TestInjectFrame(second.data(), 352, 240);
    CHECK(core.GetFramebufferWidth() == 352 - 32);
    CHECK(core.GetFramebufferHeight() == 240 - 16);
}

// ============================================================
// RTC mode
// ============================================================

namespace {

int64_t HostEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A temporary content folder: saves/, system/ and an audio CD image (one silent WAV track behind a CUE sheet), which
// LoadGame accepts as a disc. Removed on destruction.
struct RtcContent {
    std::filesystem::path root;
    std::filesystem::path saves;
    std::filesystem::path system;
    std::filesystem::path cue;

    explicit RtcContent(const char* name) {
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(root);
        saves = root / "saves";
        system = root / "system";
        std::filesystem::create_directories(saves);
        std::filesystem::create_directories(system);
        const uint32_t dataSize = 2352 * 75; // one second of CD audio
        {
            std::ofstream wav(root / "track.wav", std::ios::binary);
            const auto u32 = [&](uint32_t v) { wav.write(reinterpret_cast<const char*>(&v), 4); };
            const auto u16 = [&](uint16_t v) { wav.write(reinterpret_cast<const char*>(&v), 2); };
            wav.write("RIFF", 4);
            u32(36 + dataSize);
            wav.write("WAVEfmt ", 8);
            u32(16);
            u16(1);
            u16(2);
            u32(44100);
            u32(44100 * 4);
            u16(4);
            u16(16);
            wav.write("data", 4);
            u32(dataSize);
            const std::vector<char> silence(dataSize, 0);
            wav.write(silence.data(), silence.size());
        }
        cue = root / "audio.cue";
        std::ofstream(cue, std::ios::binary) << "FILE \"track.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n";
    }
    ~RtcContent() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    bool Load(CoreWrapper& core) const {
        return core.LoadGame(cue.string().c_str(), saves.string().c_str(), system.string().c_str());
    }

    std::filesystem::path RtcFile() const { return system / "brimir_saturn_rtc_us_eu.smpc"; }
};

std::filesystem::path UsBiosFixture() {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / "mpr-17933.bin";
}

ymir::smpc::PersistentSMPCData PersistentData(CoreWrapper& core) {
    ymir::smpc::PersistentSMPCData data{};
    core.GetSaturn()->SMPC.SavePersistentData(data);
    return data;
}

// The RTC file layout: version, 3 reserved bytes, SMEM[4], STE, offset (int64 LE), timestamp (int64 LE).
void WriteRtcFile(const std::filesystem::path& path, uint8_t ste, int64_t offset, int64_t timestamp) {
    std::ofstream out(path, std::ios::binary);
    const char header[9] = {1, 0, 0, 0, 0, 0, 0, 0, static_cast<char>(ste)};
    out.write(header, sizeof(header));
    out.write(reinterpret_cast<const char*>(&offset), 8);
    out.write(reinterpret_cast<const char*>(&timestamp), 8);
}

int64_t RtcFileTimestamp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes(25);
    in.read(bytes.data(), bytes.size());
    REQUIRE(in);
    int64_t timestamp = 0;
    std::memcpy(&timestamp, bytes.data() + 17, 8);
    return timestamp;
}

} // namespace

TEST_CASE("rtc: virtual mode seeds from host time once and persists", "[core][rtc]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    RtcContent content("brimir_rtc_virtual_test");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadIPLFromFile(UsBiosFixture().string().c_str()));
    core.SetRTCMode(RTCMode::Virtual);
    REQUIRE(content.Load(core));

    ymir::Saturn& saturn = *core.GetSaturn();
    CHECK(*saturn.configuration.rtc.mode == ymir::core::config::rtc::Mode::Virtual);
    CHECK(saturn.configuration.rtc.virtHardResetStrategy == ymir::core::config::rtc::HardResetStrategy::Preserve);

    // No RTC file: the virtual clock starts at the host's local time.
    const auto seeded = PersistentData(core);
    CHECK(std::llabs(seeded.rtc.timestamp - HostEpochSeconds()) <= 120);

    // A date far from the host's (2001-09-09), then 600 frames (10 s of emulated time) and unload: the file keeps the
    // virtual clock where emulation left it.
    constexpr int64_t kStart = 1000000000;
    auto data = seeded;
    data.rtc.timestamp = kStart;
    saturn.SMPC.LoadPersistentData(data);
    for (int i = 0; i < 600; ++i) {
        core.RunFrame();
    }
    core.UnloadGame();
    REQUIRE(std::filesystem::exists(content.RtcFile()));
    const int64_t saved = RtcFileTimestamp(content.RtcFile());
    CHECK(saved >= kStart + 9);
    CHECK(saved <= kStart + 11);

    // Reloading continues from the file, not from the host's time.
    REQUIRE(content.Load(core));
    CHECK(PersistentData(core).rtc.timestamp == saved);
    const util::datetime::DateTime guest = saturn.SMPC.GetRTC().GetDateTime();
    CHECK(guest.year == 2001);
}

TEST_CASE("rtc: virtual mode starts an existing host-mode RTC file at the date it showed", "[core][rtc]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    // A file written in Host mode holds the user's offset to the host clock and no virtual timestamp: 0 (older
    // versions) or Ymir's default date.
    const int64_t stored = GENERATE(int64_t{0}, util::datetime::to_timestamp(util::datetime::kDefaultDateTime));
    INFO("stored timestamp " << stored);
    RtcContent content("brimir_rtc_hostfile_test");
    constexpr int64_t kOffset = -3600 * 24 * 365;
    WriteRtcFile(content.RtcFile(), 1, kOffset, stored);
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadIPLFromFile(UsBiosFixture().string().c_str()));
    core.SetRTCMode(RTCMode::Virtual);
    REQUIRE(content.Load(core));
    const auto data = PersistentData(core);
    CHECK(data.STE);
    CHECK(data.rtc.offset == kOffset);
    CHECK(std::llabs(data.rtc.timestamp - (HostEpochSeconds() + kOffset)) <= 120);
}

TEST_CASE("rtc: host mode follows host clock", "[core][rtc]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    RtcContent content("brimir_rtc_host_test");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadIPLFromFile(UsBiosFixture().string().c_str()));
    core.SetRTCMode(RTCMode::Host);
    REQUIRE(content.Load(core));

    ymir::Saturn& saturn = *core.GetSaturn();
    CHECK(*saturn.configuration.rtc.mode == ymir::core::config::rtc::Mode::Host);
    // Today's behaviour: the virtual timestamp keeps Ymir's default (not seeded), the guest reads the host clock
    // (offset 0 without a file).
    CHECK(PersistentData(core).rtc.timestamp == util::datetime::to_timestamp(util::datetime::kDefaultDateTime));
    const util::datetime::DateTime guest = saturn.SMPC.GetRTC().GetDateTime();
    CHECK(std::llabs(util::datetime::to_timestamp(guest) - HostEpochSeconds()) <= 2 + 3600);
    for (int i = 0; i < 60; ++i) {
        core.RunFrame();
    }
    const util::datetime::DateTime later = saturn.SMPC.GetRTC().GetDateTime();
    CHECK(std::llabs(util::datetime::to_timestamp(later) - HostEpochSeconds()) <= 2 + 3600);
}

TEST_CASE("rtc: a first boot in virtual mode is configured (no BIOS setup screen)", "[core][rtc]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    SECTION("virtual, no RTC file: clock set (STE), default settings (SMEM zero: English)") {
        RtcContent content("brimir_rtc_firstboot_virtual");
        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(core.LoadIPLFromFile(UsBiosFixture().string().c_str()));
        core.SetRTCMode(RTCMode::Virtual);
        REQUIRE(content.Load(core));
        const auto data = PersistentData(core);
        CHECK(data.STE);
        CHECK(data.SMEM == std::array<uint8_t, 4>{0, 0, 0, 0});
        core.UnloadGame(); // persisted for the next session
        REQUIRE(std::filesystem::exists(content.RtcFile()));
    }
    SECTION("virtual, existing RTC file: read unchanged") {
        RtcContent content("brimir_rtc_firstboot_existing");
        WriteRtcFile(content.RtcFile(), 0, 0, 1000000000);
        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(core.LoadIPLFromFile(UsBiosFixture().string().c_str()));
        core.SetRTCMode(RTCMode::Virtual);
        REQUIRE(content.Load(core));
        CHECK_FALSE(PersistentData(core).STE);
    }
    SECTION("host, no RTC file: as before (the BIOS asks)") {
        RtcContent content("brimir_rtc_firstboot_host");
        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(core.LoadIPLFromFile(UsBiosFixture().string().c_str()));
        core.SetRTCMode(RTCMode::Host);
        REQUIRE(content.Load(core));
        CHECK_FALSE(PersistentData(core).STE);
    }
}

TEST_CASE("rtc: the RTC mode defaults to virtual", "[core][rtc]") {
    CoreWrapper core;
    CHECK(core.GetRTCMode() == RTCMode::Virtual);
    REQUIRE(core.Initialize());
    CHECK(*core.GetSaturn()->configuration.rtc.mode == ymir::core::config::rtc::Mode::Virtual);
    CHECK(std::llabs(PersistentData(core).rtc.timestamp - HostEpochSeconds()) <= 120);
}

// ============================================================
// Cartridges
// ============================================================

namespace {

std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / name;
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A core with the US BIOS and the given cartridge choice; the content is the audio CD of RtcContent.
struct CartSession {
    RtcContent content;
    CoreWrapper core;

    CartSession(const char* name, CartridgeChoice choice, const char* productNumber = "")
        : content(name) {
        REQUIRE(core.Initialize());
        REQUIRE(core.LoadIPLFromFile(UsBiosFixture().string().c_str()));
        core.SetCartridgeOverride(choice);
        core.TestSetProductNumber(productNumber);
    }

    bool Load() { return content.Load(core); }
    ymir::cart::CartType Type() { return core.GetSaturn()->GetCartridge().GetType(); }
};

} // namespace

TEST_CASE("cartridge: auto inserts KoF95 rom when dump present", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture()) || !std::filesystem::exists(Fixture("mpr-18811-mx.ic1"))) {
        SKIP("BIOS or KoF95 ROM fixture missing");
    }
    // T-3101G: The King of Fighters '95 (Japan) in the game database
    CartSession s("brimir_cart_kof95", CartridgeChoice::Auto, "T-3101G");
    std::filesystem::copy_file(Fixture("mpr-18811-mx.ic1"), s.content.system / "mpr-18811-mx.ic1");
    REQUIRE(s.Load());
    REQUIRE(s.Type() == ymir::cart::CartType::ROM);
    CHECK(s.core.GetLastCartridgeMessage().empty());
    CHECK(s.core.GetCartridgeDescription().find("unrecognised") == std::string::npos);

    const auto rom = ReadAllBytes(Fixture("mpr-18811-mx.ic1"));
    REQUIRE(rom.size() == 2 * 1024 * 1024);
    auto& cart = s.core.GetSaturn()->GetCartridge();
    for (const uint32_t offset : {0u, 1u, 0x1000u, 0x1FFFFEu, 0x1FFFFFu}) {
        CAPTURE(offset);
        CHECK(cart.ReadByte(0x0200'0000 + offset) == rom[offset]);
    }
    CHECK(cart.ReadWord(0x0200'0000) == ((rom[0] << 8) | rom[1])); // the dump is in bus (big-endian) order

    s.core.RunFrame();
    REQUIRE(s.core.GetFramebuffer() != nullptr);
}

TEST_CASE("cartridge: the Ultraman rom loads from its dump", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture()) || !std::filesystem::exists(Fixture("mpr-19367-mx.ic1"))) {
        SKIP("BIOS or Ultraman ROM fixture missing");
    }
    SECTION("auto, from the game database") {
        CartSession s("brimir_cart_ultraman_auto", CartridgeChoice::Auto, "T-13308G");
        std::filesystem::copy_file(Fixture("mpr-19367-mx.ic1"), s.content.system / "mpr-19367-mx.ic1");
        REQUIRE(s.Load());
        REQUIRE(s.Type() == ymir::cart::CartType::ROM);
        CHECK(s.core.GetLastCartridgeMessage().empty());
        CHECK(s.core.GetCartridgeDescription().find("unrecognised") == std::string::npos);
    }
    SECTION("override, for a game without a cartridge") {
        CartSession s("brimir_cart_ultraman_override", CartridgeChoice::ROM_Ultraman);
        std::filesystem::copy_file(Fixture("mpr-19367-mx.ic1"), s.content.system / "mpr-19367-mx.ic1");
        REQUIRE(s.Load());
        REQUIRE(s.Type() == ymir::cart::CartType::ROM);
        const auto rom = ReadAllBytes(Fixture("mpr-19367-mx.ic1"));
        CHECK(s.core.GetSaturn()->GetCartridge().ReadByte(0x0200'0000 + 0x4321) == rom[0x4321]);
    }
}

TEST_CASE("cartridge: missing rom dump starts without cart", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    CartSession s("brimir_cart_missing", CartridgeChoice::Auto, "T-3101G");
    REQUIRE(s.Load());
    CHECK(s.Type() == ymir::cart::CartType::None);
    const std::string& message = s.core.GetLastCartridgeMessage();
    CHECK(message.find("mpr-18811-mx.ic1") != std::string::npos);
    CHECK(message.find("without the cartridge") != std::string::npos);
    CHECK(s.core.GetCartridgeDescription() == "none");

    s.core.RunFrame();
    REQUIRE(s.core.GetFramebuffer() != nullptr);

    // The next load with the dump in place clears the message.
    s.core.UnloadGame();
    if (std::filesystem::exists(Fixture("mpr-18811-mx.ic1"))) {
        std::filesystem::copy_file(Fixture("mpr-18811-mx.ic1"), s.content.system / "mpr-18811-mx.ic1");
        REQUIRE(s.Load());
        CHECK(s.Type() == ymir::cart::CartType::ROM);
        CHECK(s.core.GetLastCartridgeMessage().empty());
    }
}

TEST_CASE("cartridge: wrong-size rom dump rejected", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    SECTION("wrong size") {
        CartSession s("brimir_cart_wrong_size", CartridgeChoice::ROM_KOF95);
        {
            std::ofstream out(s.content.system / "mpr-18811-mx.ic1", std::ios::binary);
            const std::vector<char> half(1024 * 1024, 0x55);
            out.write(half.data(), half.size());
        }
        REQUIRE(s.Load());
        CHECK(s.Type() == ymir::cart::CartType::None);
        CHECK(s.core.GetLastCartridgeMessage().find("1048576 bytes") != std::string::npos);
    }
    SECTION("the other game's dump") {
        if (!std::filesystem::exists(Fixture("mpr-19367-mx.ic1"))) {
            SKIP("Ultraman ROM fixture missing");
        }
        CartSession s("brimir_cart_other_dump", CartridgeChoice::ROM_KOF95);
        std::filesystem::copy_file(Fixture("mpr-19367-mx.ic1"), s.content.system / "mpr-18811-mx.ic1");
        REQUIRE(s.Load());
        CHECK(s.Type() == ymir::cart::CartType::None);
        CHECK(s.core.GetLastCartridgeMessage().find("Ultraman") != std::string::npos);
    }
    SECTION("an unknown dump of the right size is used") {
        CartSession s("brimir_cart_unknown_dump", CartridgeChoice::ROM_KOF95);
        {
            std::ofstream out(s.content.system / "mpr-18811-mx.ic1", std::ios::binary);
            const std::vector<char> rom(2 * 1024 * 1024, 0x55);
            out.write(rom.data(), rom.size());
        }
        REQUIRE(s.Load());
        CHECK(s.Type() == ymir::cart::CartType::ROM);
        CHECK(s.core.GetLastCartridgeMessage().empty());
        CHECK(s.core.GetCartridgeDescription().find("unrecognised") != std::string::npos);
    }
}

TEST_CASE("cartridge: backup cart persists to <game>.bcr", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    CartSession s("brimir_cart_backup", CartridgeChoice::Backup);
    const auto bcr = s.content.saves / "audio.bcr";
    REQUIRE(s.Load());
    REQUIRE(s.Type() == ymir::cart::CartType::BackupMemory);
    CHECK(s.core.GetSaturn()->GetCartridge().GetID() == 0x21); // 4 Mbit backup memory
    s.core.RunFrame();

    // Data byte k at bus address 2k + 1 (A-bus CS0, 0x04000000)
    auto& cart = s.core.GetSaturn()->GetCartridge();
    cart.WriteByte(0x0400'0000 + (0x1000 << 1) + 1, 0x5A);
    cart.WriteByte(0x0400'0000 + (0x7FFFF << 1) + 1, 0xA5);
    s.core.UnloadGame();

    REQUIRE(std::filesystem::exists(bcr));
    CHECK(std::filesystem::file_size(bcr) == CoreWrapper::kBackupCartSize);
    const auto file = ReadAllBytes(bcr);
    REQUIRE(file.size() == 512 * 1024);
    CHECK(std::string(file.begin(), file.begin() + 16) == "BackUpRam Format"); // formatted, raw data bytes
    CHECK(file[0x1000] == 0x5A);
    CHECK(file[0x7FFFF] == 0xA5);

    REQUIRE(s.Load());
    REQUIRE(s.Type() == ymir::cart::CartType::BackupMemory);
    auto& reloaded = s.core.GetSaturn()->GetCartridge();
    CHECK(reloaded.ReadByte(0x0400'0000 + (0x1000 << 1) + 1) == 0x5A);
    CHECK(reloaded.ReadByte(0x0400'0000 + (0x7FFFF << 1) + 1) == 0xA5);
    s.core.UnloadGame();
}

TEST_CASE("cartridge: a .bcr file in Beetle's layout loads unchanged", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    // Beetle Saturn's backup.c: 512 KiB of data bytes (byte i at bus address 2i + 1), "BackUpRam Format" repeated over
    // the first 512 bytes, written as is (no byte swap).
    CartSession s("brimir_cart_beetle_bcr", CartridgeChoice::Backup);
    std::vector<uint8_t> image(512 * 1024, 0);
    const char header[] = "BackUpRam Format";
    for (size_t i = 0; i < 0x200; i += 0x10) {
        std::memcpy(image.data() + i, header, 16);
    }
    image[0x400] = 0x80; // a save's first block
    image[0x404] = 'B';
    image[0x405] = 'R';
    {
        std::ofstream out(s.content.saves / "audio.bcr", std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), image.size());
    }
    REQUIRE(s.Load());
    REQUIRE(s.Type() == ymir::cart::CartType::BackupMemory);
    auto& cart = s.core.GetSaturn()->GetCartridge();
    for (const uint32_t i : {0u, 15u, 0x1FFu, 0x400u, 0x404u, 0x405u, 0x7FFFFu}) {
        CAPTURE(i);
        CHECK(cart.ReadByte(0x0400'0000 + (i << 1) + 1) == image[i]);
    }
    s.core.UnloadGame();
    const bool unchanged = ReadAllBytes(s.content.saves / "audio.bcr") == image;
    CHECK(unchanged); // saved back unchanged
}

TEST_CASE("cartridge: a .bcr file of the wrong size is left alone", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    CartSession s("brimir_cart_bad_bcr", CartridgeChoice::Backup);
    const std::vector<uint8_t> odd(1000, 0x42);
    {
        std::ofstream out(s.content.saves / "audio.bcr", std::ios::binary);
        out.write(reinterpret_cast<const char*>(odd.data()), odd.size());
    }
    REQUIRE(s.Load());
    REQUIRE(s.Type() == ymir::cart::CartType::BackupMemory);
    CHECK(s.core.GetLastCartridgeMessage().find("1000 bytes") != std::string::npos);
    s.core.UnloadGame();
    const bool leftAlone = ReadAllBytes(s.content.saves / "audio.bcr") == odd;
    CHECK(leftAlone);
}

TEST_CASE("cartridge: override none disables auto dram", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    // T-1245G: Dungeons and Dragons Collection (Japan), a 4 MB RAM cartridge game
    SECTION("auto") {
        CartSession s("brimir_cart_auto_dram", CartridgeChoice::Auto, "T-1245G");
        REQUIRE(s.Load());
        CHECK(s.Type() == ymir::cart::CartType::DRAM32Mbit);
        CHECK(s.core.GetCartridgeDescription() == "4 MB RAM");
    }
    SECTION("none") {
        CartSession s("brimir_cart_none_dram", CartridgeChoice::None, "T-1245G");
        REQUIRE(s.Load());
        CHECK(s.Type() == ymir::cart::CartType::None);
        CHECK(s.core.GetLastCartridgeMessage().empty());
    }
    SECTION("1 MB RAM for a game without a cartridge") {
        CartSession s("brimir_cart_dram1", CartridgeChoice::DRAM8Mbit);
        REQUIRE(s.Load());
        CHECK(s.Type() == ymir::cart::CartType::DRAM8Mbit);
    }
    SECTION("the option applies at the next content load; unloading removes the cartridge") {
        CartSession s("brimir_cart_next_load", CartridgeChoice::DRAM32Mbit);
        REQUIRE(s.Load());
        CHECK(s.Type() == ymir::cart::CartType::DRAM32Mbit);
        s.core.SetCartridgeOverride(CartridgeChoice::None);
        CHECK(s.Type() == ymir::cart::CartType::DRAM32Mbit);
        s.core.UnloadGame();
        CHECK(s.Type() == ymir::cart::CartType::None);
        REQUIRE(s.Load());
        CHECK(s.Type() == ymir::cart::CartType::None);
    }
}

TEST_CASE("cartridge: save states hold the backup cartridge's contents", "[core][cartridge][savestate]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    // A game's write to the cartridge between a save and a load (a run-ahead frame) must not survive the load.
    CartSession s("brimir_cart_backup_state", CartridgeChoice::Backup);
    REQUIRE(s.Load());
    for (int i = 0; i < 3; ++i) {
        s.core.RunFrame();
    }
    auto& cart = s.core.GetSaturn()->GetCartridge();
    constexpr uint32_t kAddress = 0x0400'0000 + (0x2345 << 1) + 1;
    cart.WriteByte(kAddress, 0x11);
    const size_t stateSize = s.core.GetStateSize();
    std::vector<uint8_t> saved(stateSize);
    REQUIRE(s.core.SaveState(saved.data(), stateSize));

    s.core.RunFrame();
    cart.WriteByte(kAddress, 0x22);
    REQUIRE(s.core.LoadState(saved.data(), saved.size()));
    CHECK(s.core.GetSaturn()->GetCartridge().ReadByte(kAddress) == 0x11);
    CHECK(s.core.GetStateSize() == stateSize);

    // Saving right after the load gives the same state.
    std::vector<uint8_t> again(stateSize);
    REQUIRE(s.core.SaveState(again.data(), stateSize));
    const bool same = again == saved;
    CHECK(same);

    // A version 7 state of the session (no cartridge image) loads and leaves the cartridge as it is.
    cart.WriteByte(kAddress, 0x33);
    const auto v7 = RepackState(saved, 7, [](ymir::savestate::SaveState& st) { st.scu.cartData.clear(); });
    REQUIRE(s.core.LoadState(v7.data(), v7.size()));
    CHECK(s.core.GetSaturn()->GetCartridge().ReadByte(kAddress) == 0x33);

    // A state without the cartridge does not load into the session.
    CoreWrapper bare;
    REQUIRE(bare.Initialize());
    std::vector<uint8_t> noCart(bare.GetStateSize());
    REQUIRE(bare.SaveState(noCart.data(), noCart.size()));
    CHECK_FALSE(s.core.LoadState(noCart.data(), noCart.size()));
    CHECK(s.core.GetSaturn()->GetCartridge().GetType() == ymir::cart::CartType::BackupMemory);
    s.core.UnloadGame();
}

// Cost of the backup cartridge in a state (run-ahead saves and loads one every frame). Hidden: timing only.
TEST_CASE("cartridge: backup cart save/load cost", "[.][cartridge-bench]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    const auto measure = [](CartridgeChoice choice, const char* name) {
        CartSession s(name, choice);
        REQUIRE(s.Load());
        for (int i = 0; i < 10; ++i) {
            s.core.RunFrame();
        }
        std::vector<uint8_t> buf(s.core.GetStateSize());
        constexpr int kRounds = 200;
        double save = 0, load = 0;
        for (int i = 0; i < kRounds; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            REQUIRE(s.core.SaveState(buf.data(), buf.size()));
            const auto t1 = std::chrono::steady_clock::now();
            REQUIRE(s.core.LoadState(buf.data(), buf.size()));
            const auto t2 = std::chrono::steady_clock::now();
            save += std::chrono::duration<double, std::milli>(t1 - t0).count();
            load += std::chrono::duration<double, std::milli>(t2 - t1).count();
        }
        std::printf("cartridge bench %-8s save %.3f ms load %.3f ms\n", name, save / kRounds, load / kRounds);
        return std::make_pair(save / kRounds, load / kRounds);
    };
    const auto none = measure(CartridgeChoice::None, "none");
    const auto backup = measure(CartridgeChoice::Backup, "backup");
    std::printf("cartridge bench backup extra: save %.3f ms load %.3f ms\n", backup.first - none.first,
                backup.second - none.second);

    // The backup cartridge's own work in SaveState (the image) and LoadState (compare and restore)
    CartSession s("bupwork", CartridgeChoice::Backup);
    REQUIRE(s.Load());
    auto* bupCart = s.core.GetSaturn()->GetCartridge().As<ymir::cart::CartType::BackupMemory>();
    REQUIRE(bupCart != nullptr);
    auto& memory = static_cast<ymir::bup::BackupMemory&>(bupCart->GetBackupMemory());
    std::vector<uint8_t> image(memory.Size());
    constexpr int kRounds = 200;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRounds; ++i) {
        memory.BrimirReadData(image); // as SaveState
    }
    const bool read = image == memory.ReadAll();
    REQUIRE(read);

    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRounds; ++i) {
        image[0x1234] ^= 0xFF; // one byte differs each time, as after a frame that wrote the cartridge
        CoreWrapper::RestoreBackupCartData(*bupCart, image);
    }
    const bool restored = memory.ReadAll() == image;
    CHECK(restored);
    const auto t2 = std::chrono::steady_clock::now();
    std::printf("cartridge bench backup image %.3f ms, restore %.3f ms\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count() / kRounds,
                std::chrono::duration<double, std::milli>(t2 - t1).count() / kRounds);
}

TEST_CASE("cartridge: a second session's backup cart save replaces the first one's file", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    CartSession s("brimir_cart_backup_twice", CartridgeChoice::Backup);
    const auto bcr = s.content.saves / "audio.bcr";
    constexpr uint32_t kAddress = 0x0400'0000 + (0x3000 << 1) + 1;
    REQUIRE(s.Load());
    s.core.GetSaturn()->GetCartridge().WriteByte(kAddress, 0x11);
    s.core.UnloadGame();
    CHECK(s.core.TakeCartridgeSaveError().empty());
    REQUIRE(ReadAllBytes(bcr).at(0x3000) == 0x11);

    REQUIRE(s.Load());
    CHECK(s.core.GetSaturn()->GetCartridge().ReadByte(kAddress) == 0x11);
    s.core.GetSaturn()->GetCartridge().WriteByte(kAddress, 0x22);
    s.core.UnloadGame(); // renames over the existing file
    CHECK(s.core.TakeCartridgeSaveError().empty());
    const auto file = ReadAllBytes(bcr);
    REQUIRE(file.size() == 512 * 1024);
    CHECK(file[0x3000] == 0x22);
    CHECK_FALSE(std::filesystem::exists(s.content.saves / "audio.bcr.tmp"));
}

TEST_CASE("cartridge: flushing writes the cartridge save only when it changed", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    SECTION("backup memory") {
        CartSession s("brimir_cart_flush_backup", CartridgeChoice::Backup);
        const auto bcr = s.content.saves / "audio.bcr";
        REQUIRE(s.Load());
        REQUIRE(s.core.FlushCartridgeSave());
        CHECK_FALSE(std::filesystem::exists(bcr)); // a fresh cartridge the game has not written
        constexpr uint32_t kAddress = 0x0400'0000 + (0x100 << 1) + 1;
        s.core.GetSaturn()->GetCartridge().WriteByte(kAddress, 0x5A);
        REQUIRE(s.core.FlushCartridgeSave());
        REQUIRE(std::filesystem::exists(bcr));
        CHECK(ReadAllBytes(bcr).at(0x100) == 0x5A);
        // Unchanged since: not rewritten
        std::filesystem::remove(bcr);
        REQUIRE(s.core.FlushCartridgeSave());
        CHECK_FALSE(std::filesystem::exists(bcr));
        s.core.GetSaturn()->GetCartridge().WriteByte(kAddress, 0x5B);
        REQUIRE(s.core.FlushCartridgeSave());
        CHECK(ReadAllBytes(bcr).at(0x100) == 0x5B);
    }
    SECTION("1 MB RAM") {
        CartSession s("brimir_cart_flush_dram", CartridgeChoice::DRAM8Mbit);
        const auto cart = s.content.saves / "audio.cart";
        REQUIRE(s.Load());
        s.core.GetSaturn()->GetCartridge().WriteByte(0x0240'0010, 0x77);
        REQUIRE(s.core.FlushCartridgeSave());
        REQUIRE(std::filesystem::exists(cart));
        CHECK(std::filesystem::file_size(cart) == 1024 * 1024);
    }
    SECTION("no cartridge: nothing to do") {
        CartSession s("brimir_cart_flush_none", CartridgeChoice::None);
        REQUIRE(s.Load());
        CHECK(s.core.FlushCartridgeSave());
        CHECK(s.core.TakeCartridgeSaveError().empty());
    }
}

TEST_CASE("cartridge: a failed cartridge save is reported and the data kept", "[core][cartridge]") {
    if (!std::filesystem::exists(UsBiosFixture())) {
        SKIP("US BIOS fixture missing");
    }
    CartSession s("brimir_cart_save_fails", CartridgeChoice::Backup);
    const auto bcr = s.content.saves / "audio.bcr";
    const auto tmp = s.content.saves / "audio.bcr.tmp";
    REQUIRE(s.Load());
    // A directory where the file goes: the rename into place fails (also on the retry)
    std::filesystem::create_directories(bcr);
    s.core.GetSaturn()->GetCartridge().WriteByte(0x0400'0000 + (0x200 << 1) + 1, 0x42);
    CHECK_FALSE(s.core.FlushCartridgeSave());
    const std::string error = s.core.TakeCartridgeSaveError();
    CHECK(error.find("audio.bcr.tmp") != std::string::npos);
    CHECK(s.core.TakeCartridgeSaveError().empty()); // taken
    REQUIRE(std::filesystem::exists(tmp));
    CHECK(ReadAllBytes(tmp).at(0x200) == 0x42);

    s.core.UnloadGame(); // fails the same way
    CHECK(s.core.TakeCartridgeSaveError().find("audio.bcr.tmp") != std::string::npos);
    std::filesystem::remove(bcr);
}