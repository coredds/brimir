// ST-V ZIP reader: stored and deflated entries, case-insensitive base-name lookup, damaged archives.

#include "catch_amalgamated.hpp"

#include "stv_test_zip.hpp"

#include <stv/stv_zip.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
    }
    return bytes;
}

void WriteBytes(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE("ST-V zip: lookups ignore case and directories", "[stv]") {
    TempDir dir("brimir_stv_zip_lookup");
    const auto zipPath = dir.path / "set.zip";
    brimir::test::WriteTestZip(zipPath, {
        {"a.bin", Pattern(100, 1), false, std::nullopt},
        {"b.bin", Pattern(5000, 2), true, std::nullopt},
        {"sub/c.bin", Pattern(64, 3), false, std::nullopt},
        {"MPR17970.2", Pattern(32, 4), true, std::nullopt},
    });

    brimir::stv::ZipReader zip(zipPath);
    REQUIRE(zip.IsOpen());
    CHECK(zip.HasEntry("a.bin"));
    CHECK(zip.HasEntry("A.BIN"));
    CHECK(zip.HasEntry("c.bin"));
    CHECK(zip.HasEntry("C.Bin"));
    CHECK(zip.HasEntry("mpr17970.2"));
    CHECK_FALSE(zip.HasEntry("d.bin"));
    CHECK_FALSE(zip.HasEntry("sub"));

    std::vector<uint8_t> out;
    CHECK(zip.ReadEntry("A.BIN", out));
    CHECK(out == Pattern(100, 1));
    CHECK_FALSE(zip.ReadEntry("d.bin", out));
}

TEST_CASE("ST-V zip: entry names are listed as stored, by base name", "[stv]") {
    TempDir dir("brimir_stv_zip_names");
    const auto zipPath = dir.path / "set.zip";
    brimir::test::WriteTestZip(zipPath, {
        {"a.bin", Pattern(8, 1), false, std::nullopt},
        {"sub/C.bin", Pattern(8, 2), false, std::nullopt},
        {"set\\MPR17970.2", Pattern(8, 3), true, std::nullopt},
        {"folder/", {}, false, std::nullopt},
    });

    brimir::stv::ZipReader zip(zipPath);
    REQUIRE(zip.IsOpen());
    auto names = zip.EntryNames();
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"C.bin", "MPR17970.2", "a.bin"});
}

TEST_CASE("ST-V zip: stored and deflated entries read back exactly", "[stv]") {
    TempDir dir("brimir_stv_zip_read");
    const auto zipPath = dir.path / "set.zip";
    const auto stored = Pattern(70000, 9);
    const std::vector<uint8_t> compressible(200000, 0x5A);
    brimir::test::WriteTestZip(zipPath, {
        {"stored.bin", stored, false, std::nullopt},
        {"deflated.bin", compressible, true, std::nullopt},
        {"deflated2.bin", Pattern(3000, 5), true, std::nullopt},
        {"empty.bin", {}, false, std::nullopt},
    });

    brimir::stv::ZipReader zip(zipPath);
    REQUIRE(zip.IsOpen());

    std::vector<uint8_t> out;
    REQUIRE(zip.ReadEntry("stored.bin", out));
    CHECK(out == stored);
    REQUIRE(zip.ReadEntry("deflated.bin", out));
    CHECK(out == compressible);
    REQUIRE(zip.ReadEntry("deflated2.bin", out));
    CHECK(out == Pattern(3000, 5));
    REQUIRE(zip.ReadEntry("empty.bin", out));
    CHECK(out.empty());
    // Reading again after other entries (the file position is not assumed)
    REQUIRE(zip.ReadEntry("stored.bin", out));
    CHECK(out == stored);
}

TEST_CASE("ST-V zip: a truncated archive opens but its cut entry does not read", "[stv]") {
    TempDir dir("brimir_stv_zip_truncated");
    const auto full = brimir::test::BuildTestZip({
        {"first.bin", Pattern(400, 1), false, std::nullopt},
        {"second.bin", Pattern(4000, 2), false, std::nullopt},
    });
    // Cut in the middle of the second entry's data
    const size_t cut = 30 + 9 + 400 + 30 + 10 + 1000;
    REQUIRE(cut < full.size());
    const auto zipPath = dir.path / "cut.zip";
    WriteBytes(zipPath, std::vector<uint8_t>(full.begin(), full.begin() + cut));

    brimir::stv::ZipReader zip(zipPath);
    CHECK(zip.IsOpen());

    std::vector<uint8_t> out;
    CHECK(zip.ReadEntry("first.bin", out));
    CHECK(out == Pattern(400, 1));
    CHECK_FALSE(zip.ReadEntry("second.bin", out));
}

TEST_CASE("ST-V zip: a deflated entry cut short does not read", "[stv]") {
    TempDir dir("brimir_stv_zip_cut_deflate");
    std::vector<uint8_t> noisy = Pattern(20000, 3);
    for (size_t i = 0; i < noisy.size(); ++i) {
        noisy[i] = static_cast<uint8_t>(noisy[i] * 31 + (i * i >> 5));
    }
    auto full = brimir::test::BuildTestZip({{"d.bin", noisy, true, std::nullopt}});
    const auto zipPath = dir.path / "cut.zip";
    WriteBytes(zipPath, std::vector<uint8_t>(full.begin(), full.begin() + 30 + 5 + 3000));

    brimir::stv::ZipReader zip(zipPath);
    CHECK(zip.IsOpen());
    std::vector<uint8_t> out;
    CHECK_FALSE(zip.ReadEntry("d.bin", out));
}

TEST_CASE("ST-V zip: files that are not zips have no entries", "[stv]") {
    TempDir dir("brimir_stv_zip_notzip");

    const auto text = dir.path / "text.zip";
    WriteBytes(text, std::vector<uint8_t>(500, 'x'));
    brimir::stv::ZipReader textZip(text);
    CHECK((!textZip.IsOpen() || textZip.EntryNames().empty()));
    CHECK_FALSE(textZip.HasEntry("a.bin"));

    const auto empty = dir.path / "empty.zip";
    WriteBytes(empty, {});
    brimir::stv::ZipReader emptyZip(empty);
    CHECK((!emptyZip.IsOpen() || emptyZip.EntryNames().empty()));

    brimir::stv::ZipReader missing(dir.path / "missing.zip");
    CHECK_FALSE(missing.IsOpen());
    CHECK(missing.EntryNames().empty());
    std::vector<uint8_t> out;
    CHECK_FALSE(missing.ReadEntry("a.bin", out));
}
