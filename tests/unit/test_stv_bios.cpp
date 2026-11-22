// ST-V BIOS selection by region, and the BIOS image loader.

#include "catch_amalgamated.hpp"

#include <stv/stv_bios.hpp>
#include <stv/stv_game_db.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

TEST_CASE("stv bios: within a region the first MAME version present wins", "[stv]") {
    TempDir dir("brimir_stv_bios_version_order");
    dir.WriteBios("epr-19730.ic8");
    dir.WriteBios("epr-20091.ic8");
    dir.WriteBios("mp17951a.s");
    std::string error;
    const auto choice = SelectSTVBios(dir.path, Game("bakubaku"), "auto", error);
    REQUIRE(choice.has_value());
    CHECK(choice->path.filename() == "epr-20091.ic8"); // MAME lists epr-20091 before epr-19730 and epr-17951a
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

TEST_CASE("stv bios: the real BIOS folder serves a Japan and a USA game", "[.stv-rom]") {
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
