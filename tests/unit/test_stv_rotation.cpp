// Screen Rotation "Auto": the effective rotation follows the loaded ST-V game's database entry, an explicit value
// (SetRotation) overrides it, and Saturn content is never rotated by Auto. No ST-V game of the database is rotated
// until a vertical set is added, so the cases below give the loaded game a rotation through the test hook
// (CoreWrapper::SetSTVRotationForTesting). Sessions use zero-filled sets and the idle BIOS (stv_test_bios.hpp).

#include "catch_amalgamated.hpp"

#include "unit/stv_test_bios.hpp"
#include "unit/stv_test_zip.hpp"

#include <brimir/core_wrapper.hpp>

#include <stv/stv_game_db.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace brimir;

namespace {

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char *name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

struct Session {
    TempDir dir;
    std::filesystem::path zip, saves, system;

    explicit Session(const char *name) : dir(name) {
        saves = dir.path / "saves";
        system = dir.path / "system";
        std::filesystem::create_directories(saves);
        std::filesystem::create_directories(system);
        const stv::GameInfo *info = stv::FindGame("bakubaku");
        REQUIRE(info != nullptr);
        std::vector<test::TestZipEntry> entries;
        std::set<std::string> seen;
        for (const auto &rom : info->roms) {
            if (!seen.insert(rom.file).second) {
                continue;
            }
            test::TestZipEntry entry;
            entry.name = rom.file;
            entry.data = test::DataWithCrc32(rom.size, rom.crc32);
            entry.deflate = true;
            entries.push_back(std::move(entry));
        }
        zip = dir.path / "bakubaku.zip";
        test::WriteTestZip(zip, entries);
        test::WriteIdleSTVBios(system / "epr-20091.ic8");
    }

    bool Load(CoreWrapper &core) const {
        return core.LoadGame(zip.string().c_str(), saves.string().c_str(), system.string().c_str());
    }
};

void Quiet(CoreWrapper &core) {
    core.SetThreadedVDP1(false);
    core.SetThreadedVDP2(false);
}

// A Saturn session: the idle image as the BIOS, no disc
void LoadSaturnBios(CoreWrapper &core) {
    std::vector<uint8_t> bios(stv::kSTVBiosSize);
    test::FillIdleSTVBiosImage(std::span<uint8_t, stv::kSTVBiosSize>(bios.data(), bios.size()));
    REQUIRE(core.LoadIPL(std::span<const uint8_t>(bios)));
}

struct Size {
    unsigned width, height;
};

Size RunAndMeasure(CoreWrapper &core) {
    core.RunFrame();
    return {core.GetFramebufferWidth(), core.GetFramebufferHeight()};
}

} // namespace

TEST_CASE("stv rotation: auto follows a rotated game, an explicit value overrides it", "[stv]") {
    Session session("brimir_stv_rotation_auto");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);

    // Auto with the database's own rotation (0 for this game)
    core.SetRotationAuto();
    CHECK(core.GetEffectiveRotation() == 0);
    const Size plain = RunAndMeasure(core);
    REQUIRE(plain.width > 0);
    REQUIRE(plain.height > 0);
    REQUIRE(plain.width != plain.height);

    // Auto with a game that needs 270
    core.SetSTVRotationForTesting(270);
    CHECK(core.GetEffectiveRotation() == 270);
    const Size turned = RunAndMeasure(core);
    CHECK(turned.width == plain.height);
    CHECK(turned.height == plain.width);

    // 0 is an explicit "no rotation", not "auto": a saved brimir_rotation = 0 keeps overriding
    core.SetRotation(0);
    CHECK(core.GetEffectiveRotation() == 0);
    const Size none = RunAndMeasure(core);
    CHECK(none.width == plain.width);
    CHECK(none.height == plain.height);

    core.SetRotation(90);
    CHECK(core.GetEffectiveRotation() == 90);
    const Size ninety = RunAndMeasure(core);
    CHECK(ninety.width == plain.height);
    CHECK(ninety.height == plain.width);

    core.SetRotation(180);
    CHECK(core.GetEffectiveRotation() == 180);
    const Size half = RunAndMeasure(core);
    CHECK(half.width == plain.width);
    CHECK(half.height == plain.height);

    // and back to Auto
    core.SetRotationAuto();
    CHECK(core.GetEffectiveRotation() == 270);
    const Size again = RunAndMeasure(core);
    CHECK(again.width == plain.height);
    CHECK(again.height == plain.width);
}

TEST_CASE("stv rotation: auto never rotates Saturn content, also after a rotated ST-V game", "[stv]") {
    Session session("brimir_stv_rotation_saturn");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetRotationAuto();

    // A Saturn session on a fresh core
    LoadSaturnBios(core);
    Quiet(core);
    CHECK(core.GetEffectiveRotation() == 0);
    const Size saturn = RunAndMeasure(core);
    REQUIRE(saturn.width > 0);

    // A rotated ST-V game, then a Saturn session in the same core instance
    REQUIRE(session.Load(core));
    Quiet(core);
    core.SetSTVRotationForTesting(270);
    CHECK(core.GetEffectiveRotation() == 270);
    const Size turned = RunAndMeasure(core);
    CHECK(turned.width < turned.height); // 320x224 turned

    core.UnloadGame();
    CHECK(core.GetEffectiveRotation() == 0);

    LoadSaturnBios(core);
    Quiet(core);
    CHECK(core.GetEffectiveRotation() == 0);
    const Size after = RunAndMeasure(core);
    CHECK(after.width == saturn.width);
    CHECK(after.height == saturn.height);

    // An explicit value still applies to Saturn content
    core.SetRotation(90);
    CHECK(core.GetEffectiveRotation() == 90);
    const Size ninety = RunAndMeasure(core);
    CHECK(ninety.width == saturn.height);
    CHECK(ninety.height == saturn.width);
}
