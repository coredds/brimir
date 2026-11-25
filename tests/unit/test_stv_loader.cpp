// ST-V cartridge image builder: MAME load macros, byte placement, errors, and the real sets.

#include "catch_amalgamated.hpp"

#include "stv_test_zip.hpp"

#include <stv/stv_cartridge.hpp>
#include <stv/stv_game_db.hpp>
#include <stv/stv_loader.hpp>
#include <stv/stv_zip.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace brimir::stv;
using Bytes = std::vector<uint8_t>;

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

struct BuiltImage {
    bool ok;
    Bytes image;
    std::string error;
};

BuiltImage Build(const char *dirName, const std::vector<brimir::test::TestZipEntry> &files,
                 const std::vector<RomEntry> &roms) {
    TempDir dir(dirName);
    const auto zipPath = dir.path / "set.zip";
    brimir::test::WriteTestZip(zipPath, files);
    ZipReader zip(zipPath);
    REQUIRE(zip.IsOpen());
    BuiltImage result{};
    result.ok = BuildCartImage(zip, roms, result.image, result.error);
    return result;
}

} // namespace

TEST_CASE("stv loader: WordSwap swaps each 16-bit pair", "[stv]") {
    // ROM_LOAD16_WORD_SWAP is ROM_GROUPWORD | ROM_REVERSE: bytes 01 02 of the file land as 02 01.
    const auto built = Build("brimir_stv_ldr_ws", {{"a.bin", {0x01, 0x02, 0x03, 0x04}}},
                             {{"a.bin", 0, 4, 0, Load::WordSwap}});
    REQUIRE(built.ok);
    CHECK(built.image == Bytes{0x02, 0x01, 0x04, 0x03});
}

TEST_CASE("stv loader: Byte16 at an odd offset fills odd addresses", "[stv]") {
    // ROM_LOAD16_BYTE is ROM_SKIP(1): file byte i goes to offset + 2*i. The IC13 sets use offset 1, so their header
    // bytes sit on odd addresses of the SH-2's big-endian view (MAME's stv.cpp: "IC13 roms are loaded on ODD bytes
    // only"). The even addresses between stay 0x00 (ROMREGION_ERASE00, as MAME declares the cart region of every
    // supported set; the SH-2 reads 0x00530045 at 0x02000000 in MAME's bakubaku).
    const auto built = Build("brimir_stv_ldr_b1", {{"a.bin", {0xAA, 0xBB, 0xCC}}}, {{"a.bin", 1, 3, 0, Load::Byte16}});
    REQUIRE(built.ok);
    CHECK(built.image == Bytes{0x00, 0xAA, 0x00, 0xBB, 0x00, 0xCC, 0x00, 0x00});
}

TEST_CASE("stv loader: Byte16 at an even offset fills even addresses", "[stv]") {
    const auto built = Build("brimir_stv_ldr_b0", {{"a.bin", {0xAA, 0xBB}}}, {{"a.bin", 0, 2, 0, Load::Byte16}});
    REQUIRE(built.ok);
    CHECK(built.image == Bytes{0xAA, 0x00, 0xBB, 0x00});
}

TEST_CASE("stv loader: Plain copies the file as is, also when it was loaded before", "[stv]") {
    // The Plain entry after a Byte16 entry of the same file loads it plainly (ROM_RELOAD_PLAIN).
    const auto built = Build("brimir_stv_ldr_plain", {{"a.bin", {0x11, 0x22}}},
                             {{"a.bin", 1, 2, 0, Load::Byte16}, {"a.bin", 4, 2, 0, Load::Plain}});
    REQUIRE(built.ok);
    CHECK(built.image == Bytes{0x00, 0x11, 0x00, 0x22, 0x11, 0x22, 0x00, 0x00});
}

TEST_CASE("stv loader: Reload repeats the previous file with the previous load mode", "[stv]") {
    SECTION("after Byte16") {
        const auto built = Build("brimir_stv_ldr_rl1", {{"a.bin", {0x11, 0x22}}},
                                 {{"a.bin", 1, 2, 0, Load::Byte16}, {"a.bin", 5, 2, 0, Load::Reload}});
        REQUIRE(built.ok);
        CHECK(built.image == Bytes{0x00, 0x11, 0x00, 0x22, 0x00, 0x11, 0x00, 0x22});
    }
    SECTION("after WordSwap, with another file in between the earlier entries") {
        const auto built = Build("brimir_stv_ldr_rl2", {{"a.bin", {0x01, 0x02}}, {"b.bin", {0x0A, 0x0B}}},
                                 {{"a.bin", 0, 2, 0, Load::WordSwap},
                                  {"b.bin", 2, 2, 0, Load::Plain},
                                  {"b.bin", 4, 2, 0, Load::Reload}});
        REQUIRE(built.ok);
        // Reload follows the previous entry (b.bin, Plain), not the first one.
        CHECK(built.image == Bytes{0x02, 0x01, 0x0A, 0x0B, 0x0A, 0x0B, 0x00, 0x00});
    }
}

TEST_CASE("stv loader: image size is the highest mapped byte rounded up to a power of two", "[stv]") {
    const auto built = Build("brimir_stv_ldr_size", {{"a.bin", Bytes(0x100, 0x5A)}}, {{"a.bin", 0x300, 0x100, 0, Load::Plain}});
    REQUIRE(built.ok);
    CHECK(built.image.size() == 0x400);
    CHECK(built.image[0x2FF] == 0x00);
    CHECK(built.image[0x300] == 0x5A);
    CHECK(built.image[0x3FF] == 0x5A);
}

TEST_CASE("stv loader: the image is capped at the cartridge maximum", "[stv]") {
    // Mapped end 0x2400000 would round up to 64 MiB; the cartridge window is 48 MiB.
    const auto built = Build("brimir_stv_ldr_cap", {{"a.bin", Bytes(0x400000, 0x77)}},
                             {{"a.bin", 0x2000000, 0x400000, 0, Load::Plain}});
    REQUIRE(built.ok);
    CHECK(built.image.size() == ymir::cart::kSTVGameROMMaxSize);
    CHECK(built.image[0x2000000] == 0x77);
    CHECK(built.image[0x23FFFFF] == 0x77);
    CHECK(built.image[0x2400000] == 0x00);
}

TEST_CASE("stv loader: entries beyond the cartridge window are rejected", "[stv]") {
    const auto built = Build("brimir_stv_ldr_over", {{"a.bin", Bytes(0x400000, 0x77)}},
                             {{"a.bin", 0x2C00001, 0x400000, 0, Load::Plain}});
    CHECK_FALSE(built.ok);
    CHECK(built.error.find("a.bin") != std::string::npos);
}

TEST_CASE("stv loader: missing files are reported by name", "[stv]") {
    const auto built = Build("brimir_stv_ldr_missing", {{"a.bin", {0x01, 0x02}}},
                             {{"a.bin", 0, 2, 0, Load::Plain},
                              {"gone1.bin", 2, 2, 0, Load::Plain},
                              {"gone2.bin", 4, 2, 0, Load::WordSwap}});
    CHECK_FALSE(built.ok);
    CHECK(built.error.find("gone1.bin") != std::string::npos);
    CHECK(built.error.find("gone2.bin") != std::string::npos);
}

TEST_CASE("stv loader: a file of the wrong size is rejected", "[stv]") {
    const auto built = Build("brimir_stv_ldr_badsize", {{"a.bin", {0x01, 0x02, 0x03}}}, {{"a.bin", 0, 4, 0, Load::Plain}});
    CHECK_FALSE(built.ok);
    CHECK(built.error.find("a.bin") != std::string::npos);
}

TEST_CASE("stv loader: a Reload with nothing before it is rejected", "[stv]") {
    const auto built = Build("brimir_stv_ldr_rl0", {{"a.bin", {0x01, 0x02}}}, {{"a.bin", 0, 2, 0, Load::Reload}});
    CHECK_FALSE(built.ok);
    CHECK_FALSE(built.error.empty());
}

TEST_CASE("stv loader: an empty entry list is rejected", "[stv]") {
    const auto built = Build("brimir_stv_ldr_empty", {{"a.bin", {0x01, 0x02}}}, {});
    CHECK_FALSE(built.ok);
    CHECK_FALSE(built.error.empty());
}

TEST_CASE("stv loader: the GameInfo overload names the set in errors", "[stv]") {
    TempDir dir("brimir_stv_ldr_game");
    const auto zipPath = dir.path / "set.zip";
    brimir::test::WriteTestZip(zipPath, {{"a.bin", {0x01, 0x02}}});
    ZipReader zip(zipPath);
    REQUIRE(zip.IsOpen());

    static constexpr RomEntry kRoms[] = {{"a.bin", 0, 2, 0, Load::Plain}, {"b.bin", 2, 2, 0, Load::Plain}};
    static constexpr Region kRegions[] = {Region::Japan};
    const GameInfo game{"testset", "Test", kRegions, Controls::ThreeButtons, Chip::None, 0, false, kRoms};

    Bytes image;
    std::string error;
    CHECK_FALSE(BuildCartImage(zip, game, image, error));
    CHECK(error.find("testset") != std::string::npos);
    CHECK(error.find("b.bin") != std::string::npos);
}

// MAME's init_sanjeon, on every byte of the "cart" region (0x3000000 bytes, gaps included):
//   x = v ^ 0xFF, then bitswap<8>(x, 7,2,5,1, 3,6,4,0), (4,6,5,7, 3,2,1,0), (7,6,5,4, 2,3,1,0), (7,0,5,4, 3,2,1,6),
//   (3,6,5,4, 7,2,1,0). bitswap<8>(v, b7..b0) makes result bit n = v bit b_n. Following each bit of x through the five
//   swaps (s1..s5, listing the result bits 7..0):
//     s1 = x7 x2 x5 x1 x3 x6 x4 x0
//     s2 = s1[4] s1[6] s1[5] s1[7] s1[3..0]  = x1 x2 x5 x7 x3 x6 x4 x0
//     s3 = s2 with bits 3 and 2 exchanged     = x1 x2 x5 x7 x6 x3 x4 x0
//     s4 = s3 with bits 6 and 0 exchanged     = x1 x0 x5 x7 x6 x3 x4 x2
//     s5 = s4 with bits 7 and 3 exchanged     = x6 x0 x5 x7 x1 x3 x4 x2
//   0x00: x = 0xFF, every bit set           -> 0xFF
//   0xFF: x = 0x00                          -> 0x00
//   0x01: x = 0xFE, x0 clear -> bit 6 clear -> 0xBF
//   0x80: x = 0x7F, x7 clear -> bit 4 clear -> 0xEF
//   0x02: x = 0xFD, x1 clear -> bit 3 clear -> 0xF7
//   0x40: x = 0xBF, x6 clear -> bit 7 clear -> 0x7F
// The gap fill 0x00 is in the region before the transform, so the SH-2 reads the gaps as 0xFF.
TEST_CASE("stv loader: Scramble::SanJeon applies MAME's init_sanjeon transform to the whole image", "[stv]") {
    TempDir dir("brimir_stv_ldr_sanjeon");
    const auto zipPath = dir.path / "set.zip";
    brimir::test::WriteTestZip(zipPath, {{"a.bin", {0x00, 0xFF, 0x01, 0x80, 0x02, 0x40}}, {"b.bin", {0x01, 0x80}}});
    ZipReader zip(zipPath);
    REQUIRE(zip.IsOpen());

    static constexpr RomEntry kRoms[] = {{"a.bin", 0, 6, 0, Load::Plain}, {"b.bin", 9, 2, 0, Load::Byte16}};
    static constexpr Region kRegions[] = {Region::Japan};
    GameInfo game{"testset", "Test", kRegions, Controls::ThreeButtons, Chip::None, 0, 0, kRoms};

    Bytes image;
    std::string error;
    SECTION("SanJeon") {
        game.scramble = Scramble::SanJeon;
        REQUIRE(BuildCartImage(zip, game, image, error));
        CHECK(image == Bytes{0xFF, 0x00, 0xBF, 0xEF, 0xF7, 0x7F, 0xFF, 0xFF, 0xFF, 0xBF, 0xFF, 0xEF, 0xFF, 0xFF, 0xFF,
                             0xFF});
    }
    SECTION("None leaves the image as loaded") {
        REQUIRE(game.scramble == Scramble::None);
        REQUIRE(BuildCartImage(zip, game, image, error));
        CHECK(image == Bytes{0x00, 0xFF, 0x01, 0x80, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00,
                             0x00});
    }
}

TEST_CASE("stv loader: the default EEPROM image is the set's eeprom file, byte for byte, or blank", "[stv]") {
    TempDir dir("brimir_stv_ldr_eeprom");
    const GameInfo *shienryu = FindGame("shienryu");
    REQUIRE(shienryu != nullptr);
    REQUIRE(shienryu->eeprom.file != nullptr);
    CHECK(std::string(shienryu->eeprom.file) == "eeprom-shienryu.bin");
    CHECK(shienryu->eeprom.crc32 == 0x98db6925);

    Bytes cellsFile(128);
    for (size_t i = 0; i < cellsFile.size(); ++i) {
        cellsFile[i] = static_cast<uint8_t>(i * 3 + 1);
    }
    std::array<uint8_t, 128> cells{};
    std::string error;

    // The file's bytes are the cells, big-endian (ROM_REGION16_BE): no swapping
    const auto good = dir.path / "good.zip";
    brimir::test::WriteTestZip(good, {{"eeprom-shienryu.bin", cellsFile}});
    {
        ZipReader zip(good);
        REQUIRE(ReadDefaultEEPROM(zip, *shienryu, cells, error));
        CHECK(error.empty());
        CHECK(Bytes(cells.begin(), cells.end()) == cellsFile);
    }

    // A set without one: every cell 0xFFFF, whatever the zip holds
    {
        ZipReader zip(good);
        cells.fill(0x12);
        REQUIRE(ReadDefaultEEPROM(zip, *FindGame("bakubaku"), cells, error));
        CHECK(std::all_of(cells.begin(), cells.end(), [](uint8_t b) { return b == 0xFF; }));
    }

    // Missing or of another size: refused, naming the file
    const auto missing = dir.path / "missing.zip";
    brimir::test::WriteTestZip(missing, {{"readme.txt", {0x41}}});
    {
        ZipReader zip(missing);
        CHECK_FALSE(ReadDefaultEEPROM(zip, *shienryu, cells, error));
        CHECK(error.find("eeprom-shienryu.bin") != std::string::npos);
    }
    const auto shortFile = dir.path / "short.zip";
    brimir::test::WriteTestZip(shortFile, {{"eeprom-shienryu.bin", Bytes(64, 0x00)}});
    {
        ZipReader zip(shortFile);
        CHECK_FALSE(ReadDefaultEEPROM(zip, *shienryu, cells, error));
        CHECK(error.find("eeprom-shienryu.bin") != std::string::npos);
        CHECK(error.find("64 bytes") != std::string::npos);
    }
}

TEST_CASE("stv loader: real sets build and carry the Sega header", "[.stv-rom][all-games]") {
    const char *romDir = std::getenv("BRIMIR_STV_ROM_DIR");
    if (!romDir || !std::filesystem::is_directory(romDir)) {
        SKIP("BRIMIR_STV_ROM_DIR is not set");
    }

    for (const GameInfo &game : AllGames()) {
        DYNAMIC_SECTION(game.shortName) {
            const auto zipPath = std::filesystem::path(romDir) / (std::string(game.shortName) + ".zip");
            if (!std::filesystem::exists(zipPath)) {
                SKIP(zipPath.string() << " is missing");
            }
            ZipReader zip(zipPath);
            REQUIRE(zip.IsOpen());

            Bytes image;
            std::string error;
            REQUIRE(BuildCartImage(zip, game, image, error));
            INFO(error);
            REQUIRE(image.size() >= 0x300000);
            CHECK(image.size() <= ymir::cart::kSTVGameROMMaxSize);

            // The SH-2 reads the header, "SEGA SEGASATURN " or "SEGA ST-V(TITAN)", from 0x02200000 in every set that
            // loads a file there (the IC7 position, or the plain reload of IC13); sanjeon has none.
            const bool loadsAt200000 = std::any_of(game.roms.begin(), game.roms.end(),
                                                   [](const RomEntry &rom) { return rom.offset == 0x200000; });
            if (loadsAt200000) {
                std::string text;
                for (size_t i = 0; i < 16; ++i) {
                    text.push_back(static_cast<char>(image[0x200000 + i]));
                }
                INFO("header at 0x02200000: " << text);
                CHECK(text.rfind("SEGA", 0) == 0);
            }

            // Sets with an IC13 (Byte16 at offset 1) also expose the header at 0x02000000, on odd bytes only. The
            // even bytes are the gap fill, 0x00, which sanjeon's scramble turns into 0xFF.
            if (game.roms[0].load == Load::Byte16 && (game.roms[0].offset & 1)) {
                const uint8_t gap = game.scramble == Scramble::SanJeon ? 0xFF : 0x00;
                std::string odd;
                for (size_t i = 0; i < 16; ++i) {
                    odd.push_back(static_cast<char>(image[game.roms[0].offset + 2 * i]));
                    CHECK(image[2 * i] == gap);
                }
                INFO("IC13 odd bytes: " << odd);
                CHECK(odd.rfind("SEGA", 0) == 0);
            }
        }
    }
}
