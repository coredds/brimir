// Brimir SRAM Persistence Tests
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace brimir;

namespace {

constexpr uint32_t kInternalBackupRAMSize = 32 * 1024;

// Points the process temp directory at `path` for the lifetime of the object.
class ScopedTempDirOverride {
public:
    explicit ScopedTempDirOverride(const std::string &path) {
        for (const char *name : kVars) {
            const char *old = std::getenv(name);
            m_saved.push_back(old ? std::optional<std::string>(old) : std::nullopt);
            Set(name, path.c_str());
        }
    }
    ~ScopedTempDirOverride() {
        for (size_t i = 0; i < std::size(kVars); ++i) {
            Set(kVars[i], m_saved[i] ? m_saved[i]->c_str() : nullptr);
        }
    }
    ScopedTempDirOverride(const ScopedTempDirOverride &) = delete;
    ScopedTempDirOverride &operator=(const ScopedTempDirOverride &) = delete;

private:
#ifdef _WIN32
    static constexpr const char *kVars[] = {"TMP", "TEMP"};
    static void Set(const char *name, const char *value) {
        _putenv_s(name, value ? value : "");
    }
#else
    static constexpr const char *kVars[] = {"TMPDIR"};
    static void Set(const char *name, const char *value) {
        if (value) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
    }
#endif
    std::vector<std::optional<std::string>> m_saved;
};

// Creates <temp>/<name>/{saves,system} and an invalid disc image saves/<game>.iso.
std::filesystem::path MakeDummyGame(const std::filesystem::path &root, const char *game) {
    std::filesystem::create_directories(root / "saves");
    std::filesystem::create_directories(root / "system");
    const auto gamePath = root / "saves" / game;
    std::ofstream dummy(gamePath, std::ios::binary);
    dummy.write("not a real disc image", 21);
    return gamePath;
}

void RequireFormattedInternalBackupRAM(CoreWrapper &core) {
    auto &bup = core.GetSaturn()->mem.GetInternalBackupRAM();
    // GetBlockSize() is checked first: it does not touch the image, so a missing image fails
    // the test instead of crashing it.
    REQUIRE(bup.GetBlockSize() == 64);
    REQUIRE(bup.Size() == kInternalBackupRAMSize);
    REQUIRE(bup.IsHeaderValid());
}

} // namespace

TEST_CASE("Internal backup RAM exists and is formatted right after Initialize", "[sram][unit][regression]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    RequireFormattedInternalBackupRAM(core);
    REQUIRE(core.GetSRAMSize() == kInternalBackupRAMSize);
}

TEST_CASE("LoadGame does not depend on a usable temp directory for backup RAM", "[sram][unit][regression]") {
    const auto root = std::filesystem::temp_directory_path() / "brimir_sram_no_temp_test";
    std::filesystem::remove_all(root);
    const auto gamePath = MakeDummyGame(root, "dummy.iso");
    // A regular file is not a usable temp directory.
    const auto notADirectory = root / "not_a_directory";
    std::ofstream(notADirectory).put('x');

    CoreWrapper core;
    REQUIRE(core.Initialize());
    {
        ScopedTempDirOverride override(notADirectory.string());
        // The disc is invalid, so the load fails, but only in the disc loader: backup RAM setup
        // must not need the temp directory.
        REQUIRE_FALSE(core.LoadGame(gamePath.string().c_str(), (root / "saves").string().c_str(),
                                    (root / "system").string().c_str()));
    }
    INFO(core.GetLastError());
    REQUIRE(core.GetLastError().find("Exception during game load") == std::string::npos);
    RequireFormattedInternalBackupRAM(core);
    REQUIRE(core.GetSRAMData() != nullptr);

    core.Shutdown();
    std::filesystem::remove_all(root);
}

TEST_CASE("Each game load starts from a fresh backup RAM image", "[sram][unit][regression]") {
    const auto root = std::filesystem::temp_directory_path() / "brimir_sram_fresh_test";
    std::filesystem::remove_all(root);
    const auto gameA = MakeDummyGame(root, "a.iso");
    const auto gameB = MakeDummyGame(root, "b.iso");
    const auto saves = (root / "saves").string();
    const auto system = (root / "system").string();

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE_FALSE(core.LoadGame(gameA.string().c_str(), saves.c_str(), system.c_str()));
    auto &bup = core.GetSaturn()->mem.GetInternalBackupRAM();
    const std::vector<uint8_t> formatted = bup.ReadAll();
    bup.WriteByte(0x1000 * 2, 0xA5); // game A writes into backup RAM
    core.UnloadGame();

    REQUIRE_FALSE(core.LoadGame(gameB.string().c_str(), saves.c_str(), system.c_str()));
    RequireFormattedInternalBackupRAM(core);
    REQUIRE(core.GetSaturn()->mem.GetInternalBackupRAM().ReadAll() == formatted);

    core.Shutdown();
    std::filesystem::remove_all(root);
}

TEST_CASE("SRAM set/get round-trip", "[sram][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    size_t size = core.GetSRAMSize();
    REQUIRE(size > 0);

    std::vector<uint8_t> testData(size, 0xA5);
    REQUIRE(core.SetSRAMData(testData.data(), size));

    const uint8_t* out = static_cast<const uint8_t*>(core.GetSRAMData());
    REQUIRE(out != nullptr);
    REQUIRE(std::memcmp(out, testData.data(), size) == 0);
}

TEST_CASE("SRAM SetSRAMData rejects bad inputs", "[sram][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    size_t size = core.GetSRAMSize();
    REQUIRE(size > 0);

    REQUIRE_FALSE(core.SetSRAMData(nullptr, size));
    REQUIRE_FALSE(core.SetSRAMData(nullptr, 0));

    std::vector<uint8_t> smallData(size / 2, 0x42);
    REQUIRE_FALSE(core.SetSRAMData(smallData.data(), smallData.size()));

    std::vector<uint8_t> testData(size, 0x5A);
    REQUIRE(core.SetSRAMData(testData.data(), size));
}

TEST_CASE("LoadGame preserves frontend-provided SRAM buffer", "[sram][unit]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // Seed the SRAM buffer before LoadGame, simulating a frontend that supplies
    // an .srm via SetSRAMData before retro_load_game.
    size_t size = core.GetSRAMSize();
    REQUIRE(size > 0);
    std::vector<uint8_t> expected(size, 0xB5);
    REQUIRE(core.SetSRAMData(expected.data(), size));

    // Set up a temporary save/system directory and a dummy disc file. The
    // game load is expected to fail because the file is not a valid disc image,
    // but the SRAM handling in LoadGame runs before disc parsing.
    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "brimir_sram_preload_test";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir / "saves");
    std::filesystem::create_directories(tempDir / "system");

    std::filesystem::path gamePath = tempDir / "saves" / "dummy.iso";
    {
        std::ofstream dummy(gamePath, std::ios::binary);
        dummy.write("not a real disc image", 21);
    }

    REQUIRE_FALSE(core.LoadGame(gamePath.string().c_str(),
                                (tempDir / "saves").string().c_str(),
                                (tempDir / "system").string().c_str()));

    const uint8_t* after = static_cast<const uint8_t*>(core.GetSRAMData());
    REQUIRE(after != nullptr);
    REQUIRE(std::memcmp(after, expected.data(), size) == 0);

    // Shut down before removing the temp tree.
    core.Shutdown();
    std::filesystem::remove_all(tempDir);
}

TEST_CASE("GetSRAMData does not clobber frontend SRAM before first RunFrame", "[sram][unit][regression]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    size_t size = core.GetSRAMSize();
    REQUIRE(size > 0);

    // Simulate a frontend-supplied .srm (the public injection path in unit tests).
    std::vector<uint8_t> expected(size, 0xC7);
    REQUIRE(core.SetSRAMData(expected.data(), size));

    std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "brimir_sram_sync_test";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir / "saves");
    std::filesystem::create_directories(tempDir / "system");

    std::filesystem::path gamePath = tempDir / "saves" / "dummy.iso";
    {
        std::ofstream dummy(gamePath, std::ios::binary);
        dummy.write("not a real disc image", 21);
    }

    // LoadGame preserves frontend SRAM (disc parsing is expected to fail).
    REQUIRE_FALSE(core.LoadGame(gamePath.string().c_str(),
                                (tempDir / "saves").string().c_str(),
                                (tempDir / "system").string().c_str()));

    // Calling GetSRAMData to obtain the pointer must NOT push the buffer to
    // Ymir yet, because the real frontend would still be copying the .srm into
    // it. The previous implementation wrote here, which overwrote the .srm
    // with the empty/formatted backup RAM contents before the game could see it.
    const uint8_t* ptr = static_cast<const uint8_t*>(core.GetSRAMData());
    REQUIRE(ptr != nullptr);
    REQUIRE(std::memcmp(ptr, expected.data(), size) == 0);

    // UnloadGame must keep the SRAM buffer available for the frontend to save.
    // It used to clear it, preventing post-unload .srm serialization.
    core.UnloadGame();
    ptr = static_cast<const uint8_t*>(core.GetSRAMData());
    REQUIRE(ptr != nullptr);
    REQUIRE(core.GetSRAMSize() == size);

    core.Shutdown();
    std::filesystem::remove_all(tempDir);
}
