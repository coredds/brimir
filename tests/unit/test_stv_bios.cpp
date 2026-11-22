// ST-V BIOS selection by region, and the BIOS image loader.

#include "catch_amalgamated.hpp"

#include <stv/stv_bios.hpp>
#include <stv/stv_game_db.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace brimir::stv;

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
    void Write(const std::string &name, size_t size, const std::vector<uint8_t> &head = {}) const {
        std::vector<uint8_t> bytes(size, 0);
        std::copy(head.begin(), head.end(), bytes.begin());
        std::ofstream out(path / name, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    void WriteBios(const std::string &name) const { Write(name, 512 * 1024); }
};

const GameInfo &Game(const char *name) {
    const GameInfo *game = FindGame(name);
    REQUIRE(game != nullptr);
    return *game;
}

std::string Lower(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

TEST_CASE("stv bios: a Japan-only game picks the Japan BIOS", "[stv]") {
    TempDir dir("brimir_stv_bios_jp");
    dir.WriteBios("epr-20091.ic8");
    std::string error;
    const auto choice = SelectSTVBios(dir.path, Game("bakubaku"), "auto", error);
    REQUIRE(choice.has_value());
    CHECK(choice->region == Region::Japan);
    CHECK(choice->path.filename() == "epr-20091.ic8");
    CHECK_FALSE(choice->reason.empty());
}

TEST_CASE("stv bios: auto skips a region the game does not support", "[stv]") {
    TempDir dir("brimir_stv_bios_auto");
    dir.WriteBios("epr-20091.ic8");
    dir.WriteBios("epr-17952a.ic8");
    std::string error;
    const auto choice = SelectSTVBios(dir.path, Game("diehard"), "auto", error); // UET
    REQUIRE(choice.has_value());
    CHECK(choice->region == Region::USA);
    CHECK(choice->path.filename() == "epr-17952a.ic8");
}

TEST_CASE("stv bios: the preference's region wins when the game supports it", "[stv]") {
    TempDir dir("brimir_stv_bios_pref");
    dir.WriteBios("epr-20091.ic8");
    dir.WriteBios("epr-17952a.ic8");
    dir.WriteBios("epr-17954a.ic8");
    std::string error;

    const auto europe = SelectSTVBios(dir.path, Game("gaxeduel"), "eu_100", error); // JUETL
    REQUIRE(europe.has_value());
    CHECK(europe->region == Region::Europe);

    const auto usa = SelectSTVBios(dir.path, Game("gaxeduel"), "us_101", error);
    REQUIRE(usa.has_value());
    CHECK(usa->region == Region::USA);

    const auto japan = SelectSTVBios(dir.path, Game("gaxeduel"), "jp_100", error);
    REQUIRE(japan.has_value());
    CHECK(japan->region == Region::Japan);

    const auto automatic = SelectSTVBios(dir.path, Game("gaxeduel"), "auto", error);
    REQUIRE(automatic.has_value());
    CHECK(automatic->region == Region::Japan);
}

TEST_CASE("stv bios: a preference the game does not support is ignored", "[stv]") {
    TempDir dir("brimir_stv_bios_pref_unsupported");
    dir.WriteBios("epr-20091.ic8");
    dir.WriteBios("epr-17954a.ic8");
    std::string error;
    // Die Hard Arcade is UET: jp_101 is not supported, so Europe is the next supported region
    const auto choice = SelectSTVBios(dir.path, Game("diehard"), "jp_101", error);
    REQUIRE(choice.has_value());
    CHECK(choice->region == Region::Europe);
}

TEST_CASE("stv bios: a preference whose file is missing falls back to the default order", "[stv]") {
    TempDir dir("brimir_stv_bios_pref_missing");
    dir.WriteBios("epr-17952a.ic8");
    std::string error;
    const auto choice = SelectSTVBios(dir.path, Game("diehard"), "eu_alt", error); // UET, only USA present
    REQUIRE(choice.has_value());
    CHECK(choice->region == Region::USA);
}

TEST_CASE("stv bios: no BIOS for the game's regions fails and names the accepted files", "[stv]") {
    TempDir dir("brimir_stv_bios_missing");
    dir.WriteBios("epr-17952a.ic8"); // USA BIOS present, but Baku Baku is Japan only
    std::string error;
    const auto choice = SelectSTVBios(dir.path, Game("bakubaku"), "auto", error);
    CHECK_FALSE(choice.has_value());
    CHECK(error.find("epr-20091.ic8") != std::string::npos);
    CHECK(error.find("20091.bin") != std::string::npos);
    CHECK(error.find("epr-17954a.ic8") == std::string::npos);
}

TEST_CASE("stv bios: the error lists the files of every region the game supports", "[stv]") {
    TempDir dir("brimir_stv_bios_missing_many");
    dir.WriteBios("epr-20091.ic8"); // Japan present, Die Hard Arcade is UET
    std::string error;
    const auto choice = SelectSTVBios(dir.path, Game("diehard"), "jp_101", error);
    CHECK_FALSE(choice.has_value());
    CHECK(error.find("epr-17952a.ic8") != std::string::npos);
    CHECK(error.find("epr-17954a.ic8") != std::string::npos);
    // The Japan BIOS that is present is reported as unused, not offered as accepted
    const auto found = error.find("Found, but");
    REQUIRE(found != std::string::npos);
    CHECK(error.find("epr-20091.ic8") > found);
}

TEST_CASE("stv bios: an empty or missing folder fails with a message", "[stv]") {
    TempDir dir("brimir_stv_bios_empty");
    std::string error;
    CHECK_FALSE(SelectSTVBios(dir.path, Game("rsgun"), "auto", error).has_value());
    CHECK_FALSE(error.empty());

    error.clear();
    CHECK_FALSE(SelectSTVBios(dir.path / "nope", Game("rsgun"), "auto", error).has_value());
    CHECK_FALSE(error.empty());
}

TEST_CASE("stv bios: older file names are accepted", "[stv]") {
    {
        TempDir dir("brimir_stv_bios_old_jp");
        dir.WriteBios("20091.bin");
        std::string error;
        const auto choice = SelectSTVBios(dir.path, Game("bakubaku"), "auto", error);
        REQUIRE(choice.has_value());
        CHECK(choice->region == Region::Japan);
        CHECK(choice->path.filename() == "20091.bin");
    }
    {
        TempDir dir("brimir_stv_bios_old_us");
        dir.WriteBios("mp17952a.s");
        std::string error;
        const auto choice = SelectSTVBios(dir.path, Game("diehard"), "auto", error);
        REQUIRE(choice.has_value());
        CHECK(choice->region == Region::USA);
    }
    {
        TempDir dir("brimir_stv_bios_old_eu");
        dir.WriteBios("mp17954a.s");
        std::string error;
        const auto choice = SelectSTVBios(dir.path, Game("diehard"), "auto", error);
        REQUIRE(choice.has_value());
        CHECK(choice->region == Region::Europe);
    }
}

TEST_CASE("stv bios: within a region the verified dumps come first, in MAME's order", "[stv]") {
    TempDir dir("brimir_stv_bios_version_order");
    dir.WriteBios("epr-19730.ic8");
    dir.WriteBios("epr-20091.ic8");
    dir.WriteBios("mp17951a.s");
    std::string error;
    const auto choice = SelectSTVBios(dir.path, Game("bakubaku"), "auto", error);
    REQUIRE(choice.has_value());
    CHECK(choice->path.filename() == "epr-20091.ic8"); // MAME lists epr-20091 before epr-19730 and epr-17951a
}

TEST_CASE("stv bios: a verified dump is chosen over an unverified one of the same region", "[stv]") {
    struct Case {
        const char *game;
        const char *unverified;
        const char *verified;
    };
    for (const Case &c : {Case{"bakubaku", "epr-23603.ic8", "mp17951a.s"},   // MAME's default Japan BIOS
                          Case{"bakubaku", "epr-17740.ic8", "epr19730.ic8"}, // MAME's jp5
                          Case{"bakubaku", "epr-17740a.ic8", "epr-17951a.ic8"},
                          Case{"diehard", "epr-17741a.ic8", "mp17952a.s"},
                          Case{"rsgun", "epr-23603.ic8", "20091.bin"}}) {
        INFO(c.game << ": " << c.unverified << " and " << c.verified);
        TempDir dir("brimir_stv_bios_verified_first");
        dir.WriteBios(c.unverified);
        dir.WriteBios(c.verified);
        std::string error;
        const auto choice = SelectSTVBios(dir.path, Game(c.game), "auto", error);
        REQUIRE(choice.has_value());
        CHECK(choice->path.filename() == c.verified);
    }

    // Taiwan (colmns97 is JET; without a Japan or Europe BIOS the Taiwan one is used)
    TempDir dir("brimir_stv_bios_verified_first_tw");
    dir.WriteBios("epr-19854.ic8");
    dir.WriteBios("epr-17742a.ic8");
    dir.WriteBios("mp17953a.ic8");
    std::string error;
    const auto taiwan = SelectSTVBios(dir.path, Game("colmns97"), "auto", error);
    REQUIRE(taiwan.has_value());
    CHECK(taiwan->region == Region::Taiwan);
    CHECK(taiwan->path.filename() == "mp17953a.ic8");
}

TEST_CASE("stv bios: an unverified dump alone is still used", "[stv]") {
    for (const char *name : {"epr-23603.ic8", "epr-17740a.ic8", "epr-17740.ic8"}) {
        INFO(name);
        TempDir dir("brimir_stv_bios_unverified_alone");
        dir.WriteBios(name);
        std::string error;
        const auto choice = SelectSTVBios(dir.path, Game("bakubaku"), "auto", error);
        REQUIRE(choice.has_value());
        CHECK(choice->path.filename() == name);
    }
}

// path::string() throws on Windows for a character outside the ANSI code page: one such file in the system folder made
// every ST-V load fail
TEST_CASE("stv bios: a file with a non-ASCII name in the folder does not break the selection", "[stv]") {
    TempDir dir("brimir_stv_bios_unicode_bystander");
    dir.WriteBios("epr-20091.ic8");
    {
        std::ofstream out(dir.path / std::filesystem::path(u8"\u65e5\u672c.txt"), std::ios::binary);
        out << "notes";
    }
    {
        std::ofstream out(dir.path / std::filesystem::path(u8"\u30bb\u30fc\u30d6 \u00fc.bin"), std::ios::binary);
        out << "x";
    }
    std::string error;
    std::optional<BiosChoice> choice;
    REQUIRE_NOTHROW(choice = SelectSTVBios(dir.path, Game("bakubaku"), "auto", error));
    INFO(error);
    REQUIRE(choice.has_value());
    CHECK(choice->path.filename() == "epr-20091.ic8");
}

TEST_CASE("stv bios: messages naming a non-ASCII path do not throw", "[stv]") {
    const std::filesystem::path folderName(u8"\u30b7\u30b9\u30c6\u30e0");
    TempDir root("brimir_stv_bios_unicode_paths");
    const std::filesystem::path folder = root.path / folderName;
    std::filesystem::create_directories(folder);
    const std::u8string folderText = folderName.u8string();
    const std::string folderUtf8(folderText.begin(), folderText.end());

    // No BIOS: the error names the folder, in UTF-8
    std::string error;
    std::optional<BiosChoice> choice;
    REQUIRE_NOTHROW(choice = SelectSTVBios(folder, Game("bakubaku"), "auto", error));
    CHECK_FALSE(choice.has_value());
    CHECK(error.find(folderUtf8) != std::string::npos);

    // A wrong-size, a missing and a folder-in-place-of-file BIOS
    const auto small = folder / std::filesystem::path(u8"\u5c0f.bin");
    {
        std::ofstream out(small, std::ios::binary);
        out << "short";
    }
    std::array<uint8_t, 512 * 1024> image{};
    error.clear();
    bool loaded = true;
    REQUIRE_NOTHROW(loaded = LoadSTVBiosImage(small, image, error));
    CHECK_FALSE(loaded);
    const std::u8string smallText = small.filename().u8string();
    CHECK(error.find(std::string(smallText.begin(), smallText.end())) != std::string::npos);

    error.clear();
    REQUIRE_NOTHROW(loaded = LoadSTVBiosImage(folder / std::filesystem::path(u8"\u7121.bin"), image, error));
    CHECK_FALSE(loaded);
    CHECK(error.find(folderUtf8) != std::string::npos);

    // A wrong-region BIOS in a non-ASCII folder is reported as found but unused
    TempDir dir("brimir_stv_bios_unicode_unused");
    const auto usFolder = dir.path / folderName;
    std::filesystem::create_directories(usFolder);
    {
        std::ofstream out(usFolder / "epr-17952a.ic8", std::ios::binary);
        out << std::string(512 * 1024, '\0');
    }
    error.clear();
    REQUIRE_NOTHROW(choice = SelectSTVBios(usFolder, Game("bakubaku"), "auto", error));
    CHECK_FALSE(choice.has_value());
    CHECK(error.find("Found, but") != std::string::npos);
}

TEST_CASE("stv bios: file names match without regard to case", "[stv]") {
    TempDir dir("brimir_stv_bios_case");
    dir.WriteBios("EPR-20091.IC8");
    std::string error;
    const auto choice = SelectSTVBios(dir.path, Game("bakubaku"), "auto", error);
    REQUIRE(choice.has_value());
    CHECK(Lower(choice->path.filename().string()) == "epr-20091.ic8");
}

TEST_CASE("stv bios: a folder named like a BIOS file is not a BIOS", "[stv]") {
    TempDir dir("brimir_stv_bios_folder");
    std::filesystem::create_directories(dir.path / "epr-20091.ic8");
    std::string error;
    CHECK_FALSE(SelectSTVBios(dir.path, Game("bakubaku"), "auto", error).has_value());
}

TEST_CASE("stv bios: image loader byte-swaps 16-bit words", "[stv]") {
    TempDir dir("brimir_stv_bios_load");
    dir.Write("bios.bin", 512 * 1024, {0x00, 0x20, 0x00, 0x02, 0x10, 0x06, 0x00, 0x00});
    std::array<uint8_t, 512 * 1024> image{};
    std::string error;
    REQUIRE(LoadSTVBiosImage(dir.path / "bios.bin", image, error));
    const std::array<uint8_t, 8> expected{0x20, 0x00, 0x02, 0x00, 0x06, 0x10, 0x00, 0x00};
    for (size_t i = 0; i < expected.size(); i++) CHECK(image[i] == expected[i]);
}

TEST_CASE("stv bios: image loader rejects a wrong-size or missing file", "[stv]") {
    TempDir dir("brimir_stv_bios_load_bad");
    dir.Write("small.bin", 256 * 1024);
    dir.Write("big.bin", 512 * 1024 + 2);
    std::array<uint8_t, 512 * 1024> image{};
    std::string error;
    CHECK_FALSE(LoadSTVBiosImage(dir.path / "small.bin", image, error));
    CHECK(error.find("small.bin") != std::string::npos);
    error.clear();
    CHECK_FALSE(LoadSTVBiosImage(dir.path / "big.bin", image, error));
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK_FALSE(LoadSTVBiosImage(dir.path / "absent.bin", image, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("stv bios: the real BIOS folder serves a Japan and a USA game", "[.stv-rom][bakubaku][diehard]") {
    const char *env = std::getenv("BRIMIR_STV_BIOS_DIR");
    if (!env || !std::filesystem::is_directory(env)) SKIP("BRIMIR_STV_BIOS_DIR is not set to a folder");
    const std::filesystem::path dir = env;

    struct Case {
        const char *game;
        Region region;
    };
    for (const Case &c : {Case{"bakubaku", Region::Japan}, Case{"diehard", Region::USA}}) {
        INFO(c.game);
        std::string error;
        const auto choice = SelectSTVBios(dir, Game(c.game), "auto", error);
        INFO(error);
        REQUIRE(choice.has_value());
        CHECK(choice->region == c.region);

        std::array<uint8_t, 512 * 1024> image{};
        REQUIRE(LoadSTVBiosImage(choice->path, image, error));
        const std::array<uint8_t, 8> expected{0x20, 0x00, 0x02, 0x00, 0x06, 0x10, 0x00, 0x00};
        for (size_t i = 0; i < expected.size(); i++) CHECK(image[i] == expected[i]);
    }
}
