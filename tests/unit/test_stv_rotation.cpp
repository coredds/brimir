// Screen Rotation "Auto": the effective rotation follows the loaded ST-V game's database entry (Shienryu, MAME's
// ROT270, is the vertical one), an explicit value (SetRotation) overrides it, and Saturn content is never rotated by
// Auto. Sessions use zero-filled sets and the idle BIOS (stv_test_bios.hpp).

#include "catch_amalgamated.hpp"

#include "unit/stv_test_bios.hpp"
#include "unit/stv_test_zip.hpp"

#include <brimir/core_wrapper.hpp>

#include <stv/stv_game_db.hpp>
#include <stv/stv_system.hpp>

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
    explicit TempDir(const std::string &name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// A zip of the set's files (zero-filled, with the database CRC-32s) and the idle BIOS
struct Session {
    TempDir dir;
    std::filesystem::path zip, saves, system;

    Session(const char *name, const char *set) : dir(name) {
        saves = dir.path / "saves";
        system = dir.path / "system";
        std::filesystem::create_directories(saves);
        std::filesystem::create_directories(system);
        const stv::GameInfo *info = stv::FindGame(set);
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
        if (info->eeprom.file != nullptr) {
            test::TestZipEntry entry;
            entry.name = info->eeprom.file;
            entry.data = test::DataWithCrc32(stv::kSTVEEPROMSize, info->eeprom.crc32);
            entries.push_back(std::move(entry));
        }
        zip = dir.path / (std::string(set) + ".zip");
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
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetRotationAuto();

    // Auto with a horizontal game
    Session plainSet("brimir_stv_rotation_auto_plain", "bakubaku");
    REQUIRE(plainSet.Load(core));
    Quiet(core);
    CHECK(core.GetEffectiveRotation() == 0);
    const Size plain = RunAndMeasure(core);
    REQUIRE(plain.width > 0);
    REQUIRE(plain.height > 0);
    REQUIRE(plain.width != plain.height);
    core.UnloadGame();

    // Auto with the vertical game: 270
    Session vertical("brimir_stv_rotation_auto_vertical", "shienryu");
    REQUIRE(vertical.Load(core));
    Quiet(core);
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

// MAME's ROT270 turns the native picture a quarter turn anticlockwise: its top-right corner is shown top left, its
// top-left corner bottom left. Under Auto, Shienryu's frame takes the same turn.
TEST_CASE("stv rotation: Shienryu's picture turns as MAME's ROT270 turns it", "[stv]") {
    Session vertical("brimir_stv_rotation_direction", "shienryu");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetRotationAuto();
    REQUIRE(vertical.Load(core));
    Quiet(core);

    constexpr uint32_t kW = 320, kH = 240;
    std::vector<uint32_t> frame(static_cast<size_t>(kW) * kH, 0xFF000000);
    frame[kW - 1] = 0xFF0000FF;                          // top right: red (XBGR)
    frame[0] = 0xFF00FF00;                               // top left: green
    frame[static_cast<size_t>(kH - 1) * kW] = 0xFFFF0000; // bottom left: blue
    core.TestInjectFrame(frame.data(), kW, kH);

    REQUIRE(core.GetFramebufferWidth() == kH);
    REQUIRE(core.GetFramebufferHeight() == kW);
    const auto *out = static_cast<const uint32_t *>(core.GetFramebuffer());
    const size_t pitch = core.GetFramebufferPitch() / 4;
    CHECK((out[0] & 0xFFFFFF) == 0xFF0000);                                    // top left: red
    CHECK((out[static_cast<size_t>(kW - 1) * pitch] & 0xFFFFFF) == 0x00FF00);  // bottom left: green
    CHECK((out[static_cast<size_t>(kW - 1) * pitch + kH - 1] & 0xFFFFFF) == 0x0000FF); // bottom right: blue
}

// The aspect ratio the core reports to the frontend turns with the picture: 4:3, and 3:4 at 90 and 270 (display only)
TEST_CASE("stv rotation: the reported aspect ratio is inverted at 90 and 270, whatever set the rotation", "[stv]") {
    constexpr float k43 = 4.0f / 3.0f;
    constexpr float k34 = 3.0f / 4.0f;
    CoreWrapper core;
    REQUIRE(core.Initialize());

    // Explicit values, Saturn content (no game)
    CHECK(core.GetDisplayAspectRatio() == k43);
    core.SetRotation(0);
    CHECK(core.GetDisplayAspectRatio() == k43);
    core.SetRotation(90);
    CHECK(core.GetDisplayAspectRatio() == k34);
    core.SetRotation(180);
    CHECK(core.GetDisplayAspectRatio() == k43);
    core.SetRotation(270);
    CHECK(core.GetDisplayAspectRatio() == k34);

    // Auto: Saturn content stays 4:3
    core.SetRotationAuto();
    CHECK(core.GetDisplayAspectRatio() == k43);

    // Auto with a horizontal ST-V game, then the vertical one in the same core
    Session plainSet("brimir_stv_rotation_aspect_plain", "bakubaku");
    REQUIRE(plainSet.Load(core));
    Quiet(core);
    CHECK(core.GetDisplayAspectRatio() == k43);
    core.UnloadGame();

    Session vertical("brimir_stv_rotation_aspect_vertical", "shienryu");
    REQUIRE(vertical.Load(core));
    Quiet(core);
    CHECK(core.GetDisplayAspectRatio() == k34);

    // an explicit value wins over the game, in both directions
    core.SetRotation(0);
    CHECK(core.GetDisplayAspectRatio() == k43);
    core.SetRotation(90);
    CHECK(core.GetDisplayAspectRatio() == k34);
    core.SetRotation(180);
    CHECK(core.GetDisplayAspectRatio() == k43);
    core.SetRotationAuto();
    CHECK(core.GetDisplayAspectRatio() == k34);

    core.UnloadGame();
    CHECK(core.GetDisplayAspectRatio() == k43);
    LoadSaturnBios(core);
    CHECK(core.GetDisplayAspectRatio() == k43);
}

TEST_CASE("stv rotation: auto never rotates Saturn content, also after a rotated ST-V game", "[stv]") {
    Session vertical("brimir_stv_rotation_saturn", "shienryu");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetRotationAuto();

    // A Saturn session on a fresh core
    LoadSaturnBios(core);
    Quiet(core);
    CHECK(core.GetEffectiveRotation() == 0);
    const Size saturn = RunAndMeasure(core);
    REQUIRE(saturn.width > 0);

    // The rotated ST-V game, then a Saturn session in the same core instance
    REQUIRE(vertical.Load(core));
    Quiet(core);
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
