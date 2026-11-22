// ST-V ZIP reader: stored and deflated entries, case-insensitive base-name lookup, damaged archives.

#include "catch_amalgamated.hpp"

#include "stv_test_zip.hpp"

#include <stv/stv_zip.hpp>

#include <zlib.h>

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

uint32_t Crc(const std::vector<uint8_t> &bytes) {
    return static_cast<uint32_t>(crc32(0, bytes.empty() ? Z_NULL : bytes.data(), static_cast<uInt>(bytes.size())));
}

// Overwrites a little-endian 32-bit field of the named entry's central directory header (offset from the header start).
void PatchCentralU32(std::vector<uint8_t> &zip, const std::string &name, size_t fieldOffset, uint32_t value) {
    for (size_t i = 0; i + 46 <= zip.size(); ++i) {
        if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 1 && zip[i + 3] == 2) {
            const size_t nameLen = zip[i + 28] | (zip[i + 29] << 8);
            if (std::string(zip.begin() + i + 46, zip.begin() + i + 46 + nameLen) == name) {
                for (size_t b = 0; b < 4; ++b) {
                    zip[i + fieldOffset + b] = static_cast<uint8_t>(value >> (8 * b));
                }
                return;
            }
        }
    }
    FAIL("central directory entry not found: " << name);
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

TEST_CASE("ST-V zip: a truncated archive is not open", "[stv]") {
    TempDir dir("brimir_stv_zip_truncated");
    const auto full = brimir::test::BuildTestZip({
        {"first.bin", Pattern(400, 1), false, std::nullopt},
        {"second.bin", Pattern(4000, 2), false, std::nullopt},
    });
    // Cut in the middle of the second entry's data: the central directory is gone
    const size_t cut = 30 + 9 + 400 + 30 + 10 + 1000;
    REQUIRE(cut < full.size());
    const auto zipPath = dir.path / "cut.zip";
    WriteBytes(zipPath, std::vector<uint8_t>(full.begin(), full.begin() + cut));

    brimir::stv::ZipReader zip(zipPath);
    CHECK_FALSE(zip.IsOpen());
    CHECK_FALSE(zip.HasEntry("first.bin"));
    CHECK(zip.EntryNames().empty());
    std::vector<uint8_t> out;
    CHECK_FALSE(zip.ReadEntry("first.bin", out));
}

TEST_CASE("ST-V zip: an entry whose data is missing does not read", "[stv]") {
    TempDir dir("brimir_stv_zip_cut_entry");
    const auto zipPath = dir.path / "bad.zip";
    auto bytes = brimir::test::BuildTestZip({
        {"good.bin", Pattern(300, 1), false, std::nullopt},
        {"big.bin", Pattern(500, 2), false, std::nullopt},
        {"lie.bin", Pattern(2000, 3), true, std::nullopt},
    });
    // The central directory claims more data than the file holds for big.bin, and a wrong decompressed size for lie.bin
    PatchCentralU32(bytes, "big.bin", 20, 0x7FFFFFF0u);
    PatchCentralU32(bytes, "big.bin", 24, 0x7FFFFFF0u);
    PatchCentralU32(bytes, "lie.bin", 24, 2100);
    WriteBytes(zipPath, bytes);

    brimir::stv::ZipReader zip(zipPath);
    REQUIRE(zip.IsOpen());
    std::vector<uint8_t> out;
    CHECK(zip.HasEntry("big.bin"));
    CHECK_FALSE(zip.ReadEntry("big.bin", out));
    CHECK(out.empty());
    CHECK_FALSE(zip.ReadEntry("lie.bin", out));
    CHECK(out.empty());
    REQUIRE(zip.ReadEntry("good.bin", out));
    CHECK(out == Pattern(300, 1));
}

TEST_CASE("ST-V zip: entries written with a data descriptor read back", "[stv]") {
    TempDir dir("brimir_stv_zip_descriptor");
    const auto zipPath = dir.path / "dd.zip";
    const auto stored = Pattern(1234, 7);
    const std::vector<uint8_t> compressible(50000, 0x33);
    brimir::test::WriteTestZip(zipPath, {
        {"stored.bin", stored, false, std::nullopt, true},
        {"deflated.bin", compressible, true, std::nullopt, true},
        {"plain.bin", Pattern(10, 1), true, std::nullopt, false},
    });

    brimir::stv::ZipReader zip(zipPath);
    REQUIRE(zip.IsOpen());
    CHECK(zip.EntryNames() == std::vector<std::string>{"stored.bin", "deflated.bin", "plain.bin"});
    std::vector<uint8_t> out;
    REQUIRE(zip.ReadEntry("stored.bin", out));
    CHECK(out == stored);
    REQUIRE(zip.ReadEntry("deflated.bin", out));
    CHECK(out == compressible);
    REQUIRE(zip.ReadEntry("plain.bin", out));
    CHECK(out == Pattern(10, 1));
    // The stored CRC comes from the central directory (the local header has zero)
    CHECK(zip.EntryCRC("stored.bin") == Crc(stored));
    CHECK(zip.EntryCRC("deflated.bin") == Crc(compressible));
}

TEST_CASE("ST-V zip: an archive comment does not hide the entries", "[stv]") {
    TempDir dir("brimir_stv_zip_comment");
    const auto data = Pattern(777, 4);
    const std::vector<brimir::test::TestZipEntry> entries = {
        {"a.bin", data, false, std::nullopt},
        {"b.bin", Pattern(3000, 5), true, std::nullopt},
    };

    for (const std::string &comment :
         {std::string("made by a ROM manager"), std::string(60000, 'c'),
          // a comment that itself holds an end-record signature
          std::string("xx") + std::string("PK\x05\x06", 4) + std::string(30, '\0') + "yy"}) {
        INFO("comment of " << comment.size() << " bytes");
        const auto zipPath = dir.path / "comment.zip";
        brimir::test::WriteTestZip(zipPath, entries, comment);

        brimir::stv::ZipReader zip(zipPath);
        REQUIRE(zip.IsOpen());
        std::vector<uint8_t> out;
        REQUIRE(zip.ReadEntry("a.bin", out));
        CHECK(out == data);
        REQUIRE(zip.ReadEntry("b.bin", out));
        CHECK(out == Pattern(3000, 5));
    }
}

TEST_CASE("ST-V zip: EntryCRC returns the stored CRC-32", "[stv]") {
    TempDir dir("brimir_stv_zip_crc");
    const auto zipPath = dir.path / "crc.zip";
    const auto a = Pattern(100, 1);
    const auto b = Pattern(5000, 2);
    brimir::test::WriteTestZip(zipPath, {
        {"a.bin", a, false, std::nullopt},
        {"sub/B.bin", b, true, std::nullopt},
        {"wrong.bin", a, false, 0xDEADBEEFu},
        {"empty.bin", {}, false, std::nullopt},
    });

    brimir::stv::ZipReader zip(zipPath);
    REQUIRE(zip.IsOpen());
    CHECK(zip.EntryCRC("a.bin") == Crc(a));
    CHECK(zip.EntryCRC("A.BIN") == Crc(a));
    CHECK(zip.EntryCRC("b.bin") == Crc(b));
    CHECK(zip.EntryCRC("wrong.bin") == 0xDEADBEEFu); // reported as stored, not verified
    CHECK(zip.EntryCRC("empty.bin") == 0u);
    CHECK_FALSE(zip.EntryCRC("missing.bin").has_value());
}

TEST_CASE("ST-V zip: a zip64 archive is refused cleanly", "[stv]") {
    TempDir dir("brimir_stv_zip_zip64");
    const auto zipPath = dir.path / "z64.zip";

    SECTION("an entry with a zip64 size marker") {
        auto bytes = brimir::test::BuildTestZip({{"a.bin", Pattern(100, 1), false, std::nullopt}});
        PatchCentralU32(bytes, "a.bin", 20, 0xFFFFFFFFu);
        PatchCentralU32(bytes, "a.bin", 24, 0xFFFFFFFFu);
        WriteBytes(zipPath, bytes);
    }
    SECTION("an entry with a zip64 offset marker") {
        auto bytes = brimir::test::BuildTestZip({{"a.bin", Pattern(100, 1), false, std::nullopt}});
        PatchCentralU32(bytes, "a.bin", 42, 0xFFFFFFFFu);
        WriteBytes(zipPath, bytes);
    }
    SECTION("an end record with a zip64 directory offset") {
        auto bytes = brimir::test::BuildTestZip({{"a.bin", Pattern(100, 1), false, std::nullopt}});
        for (size_t i = 0; i < 4; ++i) bytes[bytes.size() - 6 + i] = 0xFF; // central directory offset
        WriteBytes(zipPath, bytes);
    }

    brimir::stv::ZipReader zip(zipPath);
    CHECK_FALSE(zip.IsOpen());
    CHECK_FALSE(zip.HasEntry("a.bin"));
    CHECK(zip.EntryNames().empty());
    CHECK_FALSE(zip.EntryCRC("a.bin").has_value());
    std::vector<uint8_t> out;
    CHECK_FALSE(zip.ReadEntry("a.bin", out));
}
TEST_CASE("ST-V zip: a malformed central directory is not open", "[stv]") {
    TempDir dir("brimir_stv_zip_bad_central");
    const auto zipPath = dir.path / "bad.zip";
    auto bytes = brimir::test::BuildTestZip({
        {"first.bin", Pattern(100, 1), false, std::nullopt},
        {"second.bin", Pattern(100, 2), false, std::nullopt},
    });

    SECTION("the second central header has a bad signature") {
        PatchCentralU32(bytes, "second.bin", 0, 0xDEADBEEFu);
    }
    SECTION("the second central header's name runs past the directory") {
        PatchCentralU32(bytes, "second.bin", 28, 0xFFFFFFFFu);
    }
    SECTION("the end record counts more entries than the directory holds") {
        const size_t count = bytes.size() - 22 + 8; // entries on this disk, then total entries
        bytes[count] = bytes[count + 2] = 3;
    }
    WriteBytes(zipPath, bytes);

    brimir::stv::ZipReader zip(zipPath);
    CHECK_FALSE(zip.IsOpen());
    CHECK_FALSE(zip.HasEntry("first.bin"));
    CHECK(zip.EntryNames().empty());
    CHECK_FALSE(zip.EntryCRC("first.bin").has_value());
    std::vector<uint8_t> out;
    CHECK_FALSE(zip.ReadEntry("first.bin", out));
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
