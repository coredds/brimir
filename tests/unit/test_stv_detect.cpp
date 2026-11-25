// ST-V game database and zip detection: known sets, split sets, case and folders, non-ST-V content.

#include "catch_amalgamated.hpp"

#include "stv_test_zip.hpp"

#include <stv/stv_cartridge.hpp>
#include <stv/stv_detect.hpp>
#include <stv/stv_game_db.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

using brimir::stv::Chip;
using brimir::stv::DetectKind;
using brimir::stv::GameInfo;

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

const GameInfo &Game(const char *shortName) {
    const GameInfo *game = brimir::stv::FindGame(shortName);
    REQUIRE(game != nullptr);
    return *game;
}

// One zero-filled zip entry per distinct file of the game, sized as the database says, with the database CRC-32 in the
// archive. `skip` leaves a file out; `mangle` is applied to each stored name.
std::vector<brimir::test::TestZipEntry> GameEntries(const GameInfo &game, const std::string &skip = {},
                                                    std::string (*mangle)(const std::string &) = nullptr) {
    std::vector<brimir::test::TestZipEntry> entries;
    std::set<std::string> seen;
    for (const auto &rom : game.roms) {
        if (!seen.insert(rom.file).second || rom.file == skip) continue;
        brimir::test::TestZipEntry entry;
        entry.name = mangle ? mangle(rom.file) : std::string(rom.file);
        entry.data.assign(rom.size, 0);
        entry.deflate = true;
        entry.crcOverride = rom.crc32;
        entries.push_back(std::move(entry));
    }
    if (game.eeprom.file != nullptr && game.eeprom.file != skip) {
        brimir::test::TestZipEntry entry;
        entry.name = mangle ? mangle(game.eeprom.file) : std::string(game.eeprom.file);
        entry.data.assign(128, 0xFF);
        entry.crcOverride = game.eeprom.crc32;
        entries.push_back(std::move(entry));
    }
    return entries;
}

brimir::test::TestZipEntry Text(const std::string &name) {
    brimir::test::TestZipEntry entry;
    entry.name = name;
    entry.data = {'h', 'i'};
    return entry;
}

void WriteBytes(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string Upper(const std::string &name) {
    std::string out = name;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string InFolder(const std::string &name) {
    return "bakubaku/" + name;
}

size_t DistinctFiles(const GameInfo &game) {
    std::set<std::string> files;
    for (const auto &rom : game.roms) files.insert(rom.file);
    return files.size();
}

} // namespace

TEST_CASE("ST-V detect: a complete set is known", "[stv]") {
    TempDir dir("brimir_stv_detect_known");
    const auto zipPath = dir.path / "bakubaku.zip";
    brimir::test::WriteTestZip(zipPath, GameEntries(Game("bakubaku")));

    const auto result = brimir::stv::DetectSTVContent(zipPath);
    CHECK(result.kind == DetectKind::Known);
    REQUIRE(result.game != nullptr);
    CHECK(std::string(result.game->shortName) == "bakubaku");
    CHECK(result.missing.empty());
}

TEST_CASE("ST-V detect: the contents decide, not the zip's name", "[stv]") {
    TempDir dir("brimir_stv_detect_contents");
    const auto zipPath = dir.path / "something else (v2).ZIP";
    brimir::test::WriteTestZip(zipPath, GameEntries(Game("colmns97")));

    const auto result = brimir::stv::DetectSTVContent(zipPath);
    CHECK(result.kind == DetectKind::Known);
    REQUIRE(result.game != nullptr);
    CHECK(std::string(result.game->shortName) == "colmns97");
}

TEST_CASE("ST-V detect: upper-case names and names inside a folder are known", "[stv]") {
    TempDir dir("brimir_stv_detect_case");
    const auto zipPath = dir.path / "set.zip";

    SECTION("upper case") {
        brimir::test::WriteTestZip(zipPath, GameEntries(Game("bakubaku"), {}, Upper));
    }
    SECTION("inside a folder") {
        brimir::test::WriteTestZip(zipPath, GameEntries(Game("bakubaku"), {}, InFolder));
    }

    const auto result = brimir::stv::DetectSTVContent(zipPath);
    CHECK(result.kind == DetectKind::Known);
    REQUIRE(result.game != nullptr);
    CHECK(std::string(result.game->shortName) == "bakubaku");
    CHECK(result.missing.empty());
}

TEST_CASE("ST-V detect: a set with a file missing is incomplete and names it", "[stv]") {
    TempDir dir("brimir_stv_detect_missing");
    const auto zipPath = dir.path / "bakubaku.zip";
    brimir::test::WriteTestZip(zipPath, GameEntries(Game("bakubaku"), "mpr17973.5"));

    const auto result = brimir::stv::DetectSTVContent(zipPath);
    CHECK(result.kind == DetectKind::Incomplete);
    REQUIRE(result.game != nullptr);
    CHECK(std::string(result.game->shortName) == "bakubaku");
    CHECK(result.missing == std::vector<std::string>{"mpr17973.5"});
}

// MAME will not run shienryu without its default EEPROM image: a set without it is incomplete
TEST_CASE("ST-V detect: the default EEPROM image is part of the set", "[stv]") {
    TempDir dir("brimir_stv_detect_eeprom");
    const auto complete = dir.path / "complete.zip";
    brimir::test::WriteTestZip(complete, GameEntries(Game("shienryu")));
    const auto known = brimir::stv::DetectSTVContent(complete);
    CHECK(known.kind == DetectKind::Known);
    REQUIRE(known.game != nullptr);
    CHECK(std::string(known.game->shortName) == "shienryu");

    const auto zipPath = dir.path / "shienryu.zip";
    brimir::test::WriteTestZip(zipPath, GameEntries(Game("shienryu"), "eeprom-shienryu.bin"));
    const auto result = brimir::stv::DetectSTVContent(zipPath);
    CHECK(result.kind == DetectKind::Incomplete);
    REQUIRE(result.game != nullptr);
    CHECK(std::string(result.game->shortName) == "shienryu");
    CHECK(result.missing == std::vector<std::string>{"eeprom-shienryu.bin"});
}

TEST_CASE("ST-V detect: a split set holding half the files or more is incomplete", "[stv]") {
    TempDir dir("brimir_stv_detect_split");
    const auto zipPath = dir.path / "clone-without-parent.zip";
    const GameInfo &game = Game("bakubaku");

    auto entries = GameEntries(game);
    REQUIRE(entries.size() == 5);

    SECTION("three of five") {
        entries.resize(3);
    }
    SECTION("two of five is not enough") {
        entries.resize(2);
    }
    brimir::test::WriteTestZip(zipPath, entries);

    const auto result = brimir::stv::DetectSTVContent(zipPath);
    if (entries.size() == 3) {
        CHECK(result.kind == DetectKind::Incomplete);
        REQUIRE(result.game != nullptr);
        CHECK(std::string(result.game->shortName) == "bakubaku");
        CHECK(result.missing == std::vector<std::string>{"mpr17972.4", "mpr17973.5"});
    } else {
        CHECK(result.kind == DetectKind::NotSTV);
        CHECK(result.game == nullptr);
    }
}

TEST_CASE("ST-V detect: a zip named after a set but without its files is incomplete", "[stv]") {
    TempDir dir("brimir_stv_detect_stem");
    const auto zipPath = dir.path / "bakubaku.zip";
    brimir::test::WriteTestZip(zipPath, {Text("readme.txt")});

    const auto result = brimir::stv::DetectSTVContent(zipPath);
    CHECK(result.kind == DetectKind::Incomplete);
    REQUIRE(result.game != nullptr);
    CHECK(std::string(result.game->shortName) == "bakubaku");
    CHECK(result.missing.size() == DistinctFiles(*result.game));
    CHECK(result.missing.front() == "fpr17969.13");
}

TEST_CASE("ST-V detect: a file with the wrong CRC-32 is incomplete and listed", "[stv]") {
    TempDir dir("brimir_stv_detect_crc");
    const auto zipPath = dir.path / "bakubaku.zip";
    auto entries = GameEntries(Game("bakubaku"));
    for (auto &entry : entries) {
        if (entry.name == "mpr17971.3") entry.crcOverride = 0x12345678;
    }
    brimir::test::WriteTestZip(zipPath, entries);

    const auto result = brimir::stv::DetectSTVContent(zipPath);
    CHECK(result.kind == DetectKind::Incomplete);
    REQUIRE(result.game != nullptr);
    CHECK(std::string(result.game->shortName) == "bakubaku");
    CHECK(result.missing == std::vector<std::string>{"mpr17971.3"});
}

TEST_CASE("ST-V detect: other zips are not ST-V", "[stv]") {
    TempDir dir("brimir_stv_detect_other");

    SECTION("a disc image") {
        const auto zipPath = dir.path / "game.zip";
        brimir::test::WriteTestZip(zipPath, {Text("game.iso")});
        const auto result = brimir::stv::DetectSTVContent(zipPath);
        CHECK(result.kind == DetectKind::NotSTV);
        CHECK(result.game == nullptr);
        CHECK(result.missing.empty());
    }
    SECTION("an empty zip") {
        const auto zipPath = dir.path / "empty.zip";
        brimir::test::WriteTestZip(zipPath, {});
        CHECK(brimir::stv::DetectSTVContent(zipPath).kind == DetectKind::NotSTV);
    }
    SECTION("one file of a set is not half of it") {
        const auto zipPath = dir.path / "lonely.zip";
        auto entries = GameEntries(Game("rsgun"));
        entries.resize(1);
        brimir::test::WriteTestZip(zipPath, entries);
        CHECK(brimir::stv::DetectSTVContent(zipPath).kind == DetectKind::NotSTV);
    }
}

TEST_CASE("ST-V detect: files that are not zips", "[stv]") {
    TempDir dir("brimir_stv_detect_notzip");

    SECTION("cue and bin") {
        WriteBytes(dir.path / "game.cue", std::vector<uint8_t>(64, 'x'));
        WriteBytes(dir.path / "game.bin", std::vector<uint8_t>(4096, 0));
        CHECK(brimir::stv::DetectSTVContent(dir.path / "game.cue").kind == DetectKind::NotZip);
        CHECK(brimir::stv::DetectSTVContent(dir.path / "game.bin").kind == DetectKind::NotZip);
    }
    SECTION("a zip with another extension") {
        brimir::test::WriteTestZip(dir.path / "bakubaku.rom", GameEntries(Game("bakubaku")));
        CHECK(brimir::stv::DetectSTVContent(dir.path / "bakubaku.rom").kind == DetectKind::NotZip);
    }
}

// A .zip the reader cannot parse is still zip content: it is reported as such (LoadGame refuses it with a message),
// never handed to the Saturn disc loader or the Saturn BIOS selection
TEST_CASE("ST-V detect: a .zip that cannot be read is unreadable, not a disc", "[stv]") {
    TempDir dir("brimir_stv_detect_unreadable");

    SECTION("a .zip that is not a zip") {
        WriteBytes(dir.path / "bakubaku.zip", std::vector<uint8_t>(500, 'x'));
        CHECK(brimir::stv::DetectSTVContent(dir.path / "bakubaku.zip").kind == DetectKind::Unreadable);
    }
    SECTION("a missing file") {
        CHECK(brimir::stv::DetectSTVContent(dir.path / "bakubaku.zip").kind == DetectKind::Unreadable);
    }
    SECTION("a zip cut short") {
        auto bytes = brimir::test::BuildTestZip({Text("a.bin"), Text("b.bin")});
        bytes.resize(bytes.size() - 30);
        WriteBytes(dir.path / "cut.zip", bytes);
        CHECK(brimir::stv::DetectSTVContent(dir.path / "cut.zip").kind == DetectKind::Unreadable);
    }
    SECTION("a zip64 archive") {
        auto bytes = brimir::test::BuildTestZip(GameEntries(Game("bakubaku")));
        for (size_t i = 0; i < 4; ++i) bytes[bytes.size() - 6 + i] = 0xFF; // the end record's directory offset
        WriteBytes(dir.path / "bakubaku.zip", bytes);
        CHECK(brimir::stv::DetectSTVContent(dir.path / "bakubaku.zip").kind == DetectKind::Unreadable);
    }
}

// path::string() throws on Windows for characters outside the ANSI code page; the detection must not depend on it
TEST_CASE("ST-V detect: a zip with a non-ASCII name is detected", "[stv]") {
    TempDir dir("brimir_stv_detect_unicode");
    const auto known = dir.path / std::filesystem::path(u8"\u30d0\u30af\u30d0\u30af \u00e9.zip");
    brimir::test::WriteTestZip(known, GameEntries(Game("bakubaku")));
    const auto result = brimir::stv::DetectSTVContent(known);
    CHECK(result.kind == DetectKind::Known);
    REQUIRE(result.game != nullptr);
    CHECK(std::string(result.game->shortName) == "bakubaku");

    // Named after nothing and holding nothing: still a zip, not a disc
    const auto other = dir.path / std::filesystem::path(u8"\u3070\u304f.zip");
    brimir::test::WriteTestZip(other, {Text("readme.txt")});
    CHECK(brimir::stv::DetectSTVContent(other).kind == DetectKind::NotSTV);
}

TEST_CASE("ST-V game database: every game is well formed", "[stv]") {
    const auto games = brimir::stv::AllGames();
    REQUIRE(games.size() == 10);

    std::set<std::string> names;
    for (const GameInfo &game : games) {
        INFO(game.shortName);
        CHECK(names.insert(game.shortName).second);
        CHECK(std::string(game.title) != "");
        CHECK_FALSE(game.regions.empty());
        CHECK_FALSE(game.roms.empty());
        CHECK(brimir::stv::FindGame(game.shortName) == &game);

        for (size_t i = 0; i < game.roms.size(); ++i) {
            const auto &rom = game.roms[i];
            INFO(rom.file);
            CHECK(rom.size > 0);
            CHECK(uint64_t(rom.offset) + rom.size <= ymir::cart::kSTVGameROMMaxSize);
            if (rom.load == brimir::stv::Load::Reload) {
                REQUIRE(i > 0);
                CHECK(std::string(rom.file) == game.roms[i - 1].file);
                CHECK(rom.crc32 == game.roms[i - 1].crc32);
            }
        }
        CHECK(game.roms.front().load != brimir::stv::Load::Reload);
    }
    CHECK(brimir::stv::FindGame("nosuchgame") == nullptr);
}

TEST_CASE("ST-V game database: the games' details", "[stv]") {
    using brimir::stv::Controls;
    using brimir::stv::Load;
    using brimir::stv::Region;

    CHECK(Game("rsgun").chip == Chip::Crypt315_5881);
    CHECK(Game("rsgun").cryptKey == 0x05272d01);
    CHECK(Game("rsgun").controls == Controls::RadiantSilvergun);
    CHECK(Game("gaxeduel").controls == Controls::SixButtons);
    // ffreveng: ROM_PARAMETER( ":315_5881:key", "0524ac01" ), inputs stv6b
    CHECK(Game("ffreveng").chip == Chip::Crypt315_5881);
    CHECK(Game("ffreveng").cryptKey == 0x0524ac01);
    CHECK(Game("ffreveng").controls == Controls::SixButtons);
    CHECK(Game("ffreveng").eeprom.file == nullptr);
    for (const char *name : {"bakubaku", "colmns97", "puyosun", "diehard", "cotton2", "grdforce", "shienryu"}) {
        CHECK(Game(name).chip == Chip::None);
        CHECK(Game(name).controls == Controls::ThreeButtons);
    }
    // Shienryu is the one vertical game (MAME's ROT270: a quarter turn anticlockwise); the others are ROT0
    for (const auto &game : brimir::stv::AllGames()) {
        INFO(game.shortName);
        CHECK(game.rotation == (std::string(game.shortName) == "shienryu" ? 270 : 0));
    }

    const auto regionsOf = [](const char *name) {
        return std::vector<Region>(Game(name).regions.begin(), Game(name).regions.end());
    };
    CHECK(regionsOf("bakubaku") == std::vector<Region>{Region::Japan});
    CHECK(regionsOf("colmns97") == std::vector<Region>{Region::Japan, Region::Europe, Region::Taiwan});
    CHECK(regionsOf("diehard") == std::vector<Region>{Region::USA, Region::Europe, Region::Taiwan});
    CHECK(regionsOf("gaxeduel") ==
          std::vector<Region>{Region::Japan, Region::USA, Region::Europe, Region::Taiwan, Region::LatinAmerica});
    CHECK(regionsOf("rsgun") == std::vector<Region>{Region::Japan, Region::USA, Region::Europe, Region::Taiwan});
    CHECK(regionsOf("shienryu") == std::vector<Region>{Region::Japan, Region::USA, Region::Europe, Region::Taiwan});
    CHECK(regionsOf("ffreveng") == std::vector<Region>{Region::Japan, Region::USA, Region::Europe, Region::Taiwan});

    // Final Fight Revenge: the program ROM loaded once as ROM_LOAD16_BYTE (no reloads), then seven word-swapped files
    const GameInfo &ffr = Game("ffreveng");
    CHECK(DistinctFiles(ffr) == 8);
    REQUIRE(ffr.roms.size() == 8);
    CHECK(ffr.roms[0].load == Load::Byte16);
    CHECK(ffr.roms[0].offset == 0x1);
    CHECK(ffr.roms[1].offset == 0x200000);
    CHECK(ffr.roms[7].offset == 0x1800000);

    // Puyo Puyo Sun: nine files, the header file reloaded with the previous flags once and plain twice
    const GameInfo &puyo = Game("puyosun");
    CHECK(DistinctFiles(puyo) == 9);
    REQUIRE(puyo.roms.size() == 12);
    CHECK(puyo.roms[0].load == Load::Byte16);
    CHECK(puyo.roms[0].offset == 0x1);
    CHECK(puyo.roms[1].load == Load::Reload);
    CHECK(puyo.roms[1].offset == 0x100001);
    CHECK(puyo.roms[2].load == Load::Plain);
    CHECK(puyo.roms[2].offset == 0x200000);
}

TEST_CASE("ST-V detect: the developer's zips are recognized", "[.stv-rom][all-games]") {
    const char *romDir = std::getenv("BRIMIR_STV_ROM_DIR");
    if (romDir == nullptr || *romDir == '\0') SKIP("BRIMIR_STV_ROM_DIR is not set");

    for (const GameInfo &game : brimir::stv::AllGames()) {
        const auto zipPath = std::filesystem::path(romDir) / (std::string(game.shortName) + ".zip");
        if (!std::filesystem::exists(zipPath)) SKIP("missing " << zipPath.string());
    }

    for (const GameInfo &game : brimir::stv::AllGames()) {
        const auto zipPath = std::filesystem::path(romDir) / (std::string(game.shortName) + ".zip");
        INFO(zipPath.string());
        const auto result = brimir::stv::DetectSTVContent(zipPath);
        CHECK(result.kind == DetectKind::Known);
        REQUIRE(result.game != nullptr);
        CHECK(std::string(result.game->shortName) == game.shortName);
        CHECK(result.missing.empty());
    }
}
