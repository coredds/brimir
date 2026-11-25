// The ST-V save RAM (the libretro .srm): the internal backup RAM followed by the cabinet EEPROM, 32896 bytes. The
// layout, the first-frame handshake with the frontend, the cold state, the rejection of buffers and files of another
// size, and the proof that an ST-V session leaves the console's Saturn settings and RTC files alone. The [stv] cases
// build a set from zero-filled files; the [.stv-rom] cases boot the real bakubaku (BRIMIR_STV_BIOS_DIR and
// BRIMIR_STV_ROM_DIR, SKIP when unset).

#include "catch_amalgamated.hpp"

#include "unit/stv_test_bios.hpp"
#include "unit/stv_test_zip.hpp"

#include <brimir/core_wrapper.hpp>

#include <ymir/hw/smpc/smpc.hpp>
#include <ymir/sys/saturn.hpp>

#include <stv/stv_game_db.hpp>
#include <stv/stv_io.hpp>
#include <stv/stv_system.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace brimir;

namespace {

constexpr size_t kBackup = stv::kSTVBackupRAMSize;
constexpr size_t kTotal = stv::kSTVSaveRAMSize;
constexpr uint32_t kPDR1 = 0x010'0075;
constexpr uint32_t kDDR1 = 0x010'0079;
constexpr uint32_t kDDR2 = 0x010'007B;

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

std::string Str(const std::filesystem::path &path) {
    return path.string();
}

void WriteBytes(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> ReadBytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// What is in a folder: file name to contents
std::map<std::string, std::vector<uint8_t>> Snapshot(const std::filesystem::path &dir) {
    std::map<std::string, std::vector<uint8_t>> files;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files[entry.path().filename().string()] = ReadBytes(entry.path());
        }
    }
    return files;
}

// A recognisable image: the backup RAM bytes count up, the EEPROM cells are 0xE0 + the byte offset
std::vector<uint8_t> PatternImage() {
    std::vector<uint8_t> image(kTotal);
    for (size_t i = 0; i < kBackup; i++) {
        image[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    for (size_t i = 0; i < stv::kSTVEEPROMSize; i++) {
        image[kBackup + i] = static_cast<uint8_t>(0xE0 + i);
    }
    return image;
}

// A zip of zero-filled files (bakubaku's unless another set is named; a default EEPROM image is there, with the
// database CRC-32) and an idle BIOS (stv_test_bios.hpp), a system and a save folder
struct Session {
    TempDir dir;
    std::string set;
    std::filesystem::path zip, saves, system;
    std::vector<uint8_t> eepromFile; // the set's default EEPROM image as written, when it has one

    explicit Session(const char *name, const char *setName = "bakubaku") : dir(name), set(setName) {
        saves = dir.path / "saves";
        system = dir.path / "system";
        std::filesystem::create_directories(saves);
        std::filesystem::create_directories(system);
        const stv::GameInfo *game = stv::FindGame(set);
        REQUIRE(game != nullptr);
        std::vector<test::TestZipEntry> entries;
        std::set<std::string> seen;
        for (const auto &rom : game->roms) {
            if (!seen.insert(rom.file).second) {
                continue;
            }
            test::TestZipEntry entry;
            entry.name = rom.file;
            entry.data = test::DataWithCrc32(rom.size, rom.crc32);
            entry.deflate = true;
            entries.push_back(std::move(entry));
        }
        if (game->eeprom.file != nullptr) {
            test::TestZipEntry entry;
            entry.name = game->eeprom.file;
            entry.data = test::DataWithCrc32(stv::kSTVEEPROMSize, game->eeprom.crc32);
            eepromFile = entry.data;
            entries.push_back(std::move(entry));
        }
        zip = dir.path / (set + ".zip");
        test::WriteTestZip(zip, entries);
        test::WriteIdleSTVBios(system / "epr-20091.ic8");
    }

    bool Load(CoreWrapper &core) const {
        return core.LoadGame(Str(zip).c_str(), Str(saves).c_str(), Str(system).c_str());
    }
    std::filesystem::path Srm() const {
        return saves / (set + ".srm");
    }
};

void Pdr1(ymir::Saturn &saturn, uint8_t value) {
    saturn.mainBus.Write<uint8_t>(kPDR1, value);
}

void ClockBit(ymir::Saturn &saturn, bool bit) {
    const uint8_t base = static_cast<uint8_t>(0x04 | (bit ? 0x10 : 0));
    Pdr1(saturn, base);
    Pdr1(saturn, base | 0x08);
    Pdr1(saturn, base);
}

void ClockBits(ymir::Saturn &saturn, uint32_t value, unsigned count) {
    for (unsigned i = count; i-- > 0;) {
        ClockBit(saturn, ((value >> i) & 1) != 0);
    }
}

// EWEN, then WRITE of one cell, the way the BIOS drives the 93C46 through the SMPC pins
void WriteEEPROMCell(ymir::Saturn &saturn, unsigned cell, uint16_t value) {
    saturn.mainBus.Write<uint8_t>(kDDR1, 0x1F);
    saturn.mainBus.Write<uint8_t>(kDDR2, 0x00);
    Pdr1(saturn, 0x00);
    ClockBit(saturn, true);
    ClockBits(saturn, 0b0011'0000, 8);
    Pdr1(saturn, 0x00);
    ClockBit(saturn, true);
    ClockBits(saturn, 0b01, 2);
    ClockBits(saturn, cell, 6);
    ClockBits(saturn, value, 16);
    Pdr1(saturn, 0x00);
}

std::vector<uint8_t> Image(CoreWrapper &core) {
    const auto *data = static_cast<const uint8_t *>(core.GetSRAMData());
    REQUIRE(data != nullptr);
    return std::vector<uint8_t>(data, data + core.GetSRAMSize());
}

std::vector<uint8_t> BackupRAM(CoreWrapper &core) {
    return core.GetSaturn()->mem.GetInternalBackupRAM().ReadAll();
}

std::vector<uint8_t> EEPROM(CoreWrapper &core) {
    const auto &cells = core.GetSTVIOBoardForTesting()->GetEEPROM();
    return std::vector<uint8_t>(cells.begin(), cells.end());
}

bool AllEqual(const std::vector<uint8_t> &bytes, size_t from, size_t to, uint8_t value) {
    return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.begin() + static_cast<std::ptrdiff_t>(to),
                       [value](uint8_t b) { return b == value; });
}

// Starts a session the way the libretro layer does for a frame loop: no threaded VDP threads
void Quiet(CoreWrapper &core) {
    core.SetThreadedVDP1(false);
    core.SetThreadedVDP2(false);
}

} // namespace

TEST_CASE("stv sram: the save RAM is the backup RAM and the EEPROM from the moment the game is loaded", "[stv]") {
    Session session("brimir_stv_sram_layout");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    CHECK(core.GetSRAMSize() == 32768); // a Saturn's

    REQUIRE(session.Load(core));
    REQUIRE(core.IsSTVLoaded());
    CHECK(core.GetSRAMSize() == 32896);

    // Before the first frame, when a frontend reads size and pointer and loads its .srm into the buffer: the cold state,
    // all-zero backup RAM (not a formatted Saturn image) and a blank (0xFF) EEPROM, which the BIOS initialises
    void *pointer = core.GetSRAMData();
    REQUIRE(pointer != nullptr);
    const std::vector<uint8_t> cold = Image(core);
    REQUIRE(cold.size() == kTotal);
    CHECK(AllEqual(cold, 0, kBackup, 0x00));
    CHECK(AllEqual(cold, kBackup, kTotal, 0xFF));
    CHECK(core.GetSRAMData() == pointer);

    Quiet(core);
    core.RunFrame();
    CHECK(core.GetSRAMData() == pointer); // never moves: a frontend's autosave thread keeps it
    CHECK(core.GetSRAMSize() == 32896);

    core.UnloadGame();
    CHECK(core.GetSRAMSize() == 32768); // a Saturn's again
}

// MAME's eeprom_base_device::nvram_default: with no NVRAM the chip holds the set's "eeprom" region (shienryu's sets
// the cabinet up for its vertical monitor); a saved .srm still replaces it
TEST_CASE("stv sram: a set with a default EEPROM image starts with it, and a .srm still wins", "[stv]") {
    Session session("brimir_stv_sram_default_eeprom", "shienryu");
    REQUIRE(session.eepromFile.size() == stv::kSTVEEPROMSize);
    REQUIRE_FALSE(AllEqual(session.eepromFile, 0, stv::kSTVEEPROMSize, 0xFF));

    {
        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(session.Load(core));
        REQUIRE(core.IsSTVLoaded());
        Quiet(core);
        const std::vector<uint8_t> cold = Image(core);
        CHECK(AllEqual(cold, 0, kBackup, 0x00));
        CHECK(std::vector<uint8_t>(cold.begin() + kBackup, cold.end()) == session.eepromFile);
        CHECK(EEPROM(core) == session.eepromFile);
        core.RunFrame();
        CHECK(EEPROM(core) == session.eepromFile);
        CHECK(core.GetLastError().empty());
    }

    // A save of the board replaces the default at the first frame
    const std::vector<uint8_t> saved = PatternImage();
    WriteBytes(session.Srm(), saved);
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);
    core.RunFrame();
    CHECK(EEPROM(core) == std::vector<uint8_t>(saved.begin() + kBackup, saved.end()));
}

TEST_CASE("stv sram: an EEPROM write by the 93C46 protocol is in the save RAM at offset 32768", "[stv]") {
    Session session("brimir_stv_sram_eeprom");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);
    core.RunFrame(); // the first frame: the handshake with the frontend's buffer is done

    WriteEEPROMCell(*core.GetSaturn(), 5, 0xA55A);
    core.RunFrame();
    const std::vector<uint8_t> image = Image(core);
    CHECK(image[kBackup + 10] == 0xA5);
    CHECK(image[kBackup + 11] == 0x5A);
    CHECK(image[kBackup + 12] == 0xFF); // the neighbour cells are as they were
    CHECK(image[kBackup + 9] == 0xFF);
    CHECK(std::equal(image.begin() + static_cast<std::ptrdiff_t>(kBackup), image.end(), EEPROM(core).begin()));

    // a write to the internal backup RAM shows at the same offset as in a Saturn's save RAM: byte n of the image is
    // the backup RAM's byte n
    core.GetSaturn()->mem.GetInternalBackupRAM().WriteByte(2 * 100, 0x77);
    CHECK(static_cast<const uint8_t *>(core.GetSRAMData())[100] == 0x77);
}

TEST_CASE("stv sram: the frontend's buffer is copied in at the first frame, both parts", "[stv]") {
    Session session("brimir_stv_sram_handshake");
    const std::vector<uint8_t> saved = PatternImage();
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);

    // What RetroArch does: read size and pointer after retro_load_game, load the .srm into the buffer, then run
    uint8_t *pointer = static_cast<uint8_t *>(core.GetSRAMData());
    REQUIRE(pointer != nullptr);
    REQUIRE(core.GetSRAMSize() == saved.size());
    std::memcpy(pointer, saved.data(), saved.size());

    // The buffer is the frontend's until the first frame: asking again must not refresh it from the emulator
    CHECK(core.GetSRAMData() == pointer);
    CHECK(Image(core) == saved);
    CHECK(AllEqual(BackupRAM(core), 0, kBackup, 0x00));

    core.RunFrame();
    CHECK(BackupRAM(core) == std::vector<uint8_t>(saved.begin(), saved.begin() + kBackup));
    CHECK(EEPROM(core) == std::vector<uint8_t>(saved.begin() + kBackup, saved.end()));
    CHECK(core.GetSRAMData() == pointer);
    CHECK(Image(core) == saved);
    CHECK(core.TakeBackupRAMNotice().empty());
}

TEST_CASE("stv sram: SetSRAMData restores both parts and a save made by one session is the next one's", "[stv]") {
    Session session("brimir_stv_sram_roundtrip");
    const std::vector<uint8_t> saved = PatternImage();
    {
        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(session.Load(core));
        Quiet(core);
        REQUIRE(core.SetSRAMData(saved.data(), saved.size()));
        core.RunFrame();
        CHECK(BackupRAM(core) == std::vector<uint8_t>(saved.begin(), saved.begin() + kBackup));
        CHECK(EEPROM(core) == std::vector<uint8_t>(saved.begin() + kBackup, saved.end()));
        core.UnloadGame(); // the core writes the .srm itself, as for a Saturn
    }
    REQUIRE(std::filesystem::exists(session.Srm()));
    CHECK(ReadBytes(session.Srm()) == saved);

    // A new session finds it without the frontend's help
    CoreWrapper next;
    REQUIRE(next.Initialize());
    REQUIRE(session.Load(next));
    Quiet(next);
    CHECK(Image(next) == saved);
    next.RunFrame();
    CHECK(BackupRAM(next) == std::vector<uint8_t>(saved.begin(), saved.begin() + kBackup));
    CHECK(EEPROM(next) == std::vector<uint8_t>(saved.begin() + kBackup, saved.end()));
}

TEST_CASE("stv sram: an all-zero image is not replaced by a formatted one", "[stv]") {
    Session session("brimir_stv_sram_zero");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);

    const std::vector<uint8_t> zeros(kTotal, 0);
    REQUIRE(core.SetSRAMData(zeros.data(), zeros.size()));
    core.RunFrame();
    CHECK(AllEqual(BackupRAM(core), 0, kBackup, 0x00)); // a Saturn would format it
    CHECK(AllEqual(Image(core), 0, kBackup, 0x00));
    CHECK(core.TakeBackupRAMNotice().empty());
}

TEST_CASE("stv sram: a buffer of another size is rejected and the cold state is kept", "[stv]") {
    Session session("brimir_stv_sram_wrongsize");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);

    const std::vector<uint8_t> saturnSized(32768, 0x5A); // a Saturn's, or an earlier build's
    CHECK_FALSE(core.SetSRAMData(saturnSized.data(), saturnSized.size()));
    const std::string notice = core.TakeBackupRAMNotice();
    CHECK(notice.find("32768") != std::string::npos);
    CHECK(notice.find("32896") != std::string::npos);

    const std::vector<uint8_t> tooLong(kTotal + 1, 0x5A);
    CHECK_FALSE(core.SetSRAMData(tooLong.data(), tooLong.size()));
    CHECK_FALSE(core.TakeBackupRAMNotice().empty());

    core.RunFrame();
    CHECK(AllEqual(BackupRAM(core), 0, kBackup, 0x00));
    const std::vector<uint8_t> image = Image(core);
    CHECK(AllEqual(image, 0, kBackup, 0x00));
    CHECK(AllEqual(image, kBackup, kTotal, 0xFF));
}

TEST_CASE("stv sram: a .srm file of another size is rejected, logged, and replaced at unload", "[stv]") {
    Session session("brimir_stv_sram_foreignfile");
    // What an earlier build's frontend wrote: the raw 32 KiB backup RAM
    WriteBytes(session.Srm(), std::vector<uint8_t>(32768, 0x5A));

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);
    const std::string notice = core.TakeBackupRAMNotice();
    CHECK(notice.find("32768") != std::string::npos);
    CHECK(notice.find("32896") != std::string::npos);
    CHECK(notice.find("bakubaku.srm") != std::string::npos);

    // RetroArch loads a file of another size into the start of the buffer all the same (a partial load): the core
    // keeps the cold state
    uint8_t *pointer = static_cast<uint8_t *>(core.GetSRAMData());
    REQUIRE(pointer != nullptr);
    std::memset(pointer, 0x5A, 32768);
    core.RunFrame();
    CHECK(AllEqual(BackupRAM(core), 0, kBackup, 0x00));
    const std::vector<uint8_t> image = Image(core);
    CHECK(AllEqual(image, 0, kBackup, 0x00));
    CHECK(AllEqual(image, kBackup, kTotal, 0xFF));

    core.UnloadGame();
    const std::vector<uint8_t> written = ReadBytes(session.Srm());
    REQUIRE(written.size() == kTotal);
    CHECK(AllEqual(written, 0, kBackup, 0x00));
}

TEST_CASE("stv sram: no save RAM from a session that did not run is lost or invented", "[stv]") {
    Session session("brimir_stv_sram_norun");
    const std::vector<uint8_t> saved = PatternImage();
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    // The frontend's buffer, never run: the unload still passes it on (a Saturn's safety net)
    std::memcpy(core.GetSRAMData(), saved.data(), saved.size());
    core.UnloadGame();
    CHECK(ReadBytes(session.Srm()) == saved);
}

TEST_CASE("stv sram: an ST-V session leaves the Saturn settings and RTC files alone", "[stv]") {
    const auto rtcFile = [](const char *suffix) { return std::string("brimir_saturn_rtc_") + suffix + ".smpc"; };

    SECTION("existing files stay byte-identical") {
        Session session("brimir_stv_sram_rtcfiles");
        uint8_t fill = 0x11;
        for (const char *name : {"none", "us_eu", "jp", "asia", "other"}) {
            std::vector<uint8_t> bytes(21, fill);
            fill = static_cast<uint8_t>(fill + 0x11);
            WriteBytes(session.system / rtcFile(name), bytes);
        }
        WriteBytes(session.system / "brimir_saturn_rtc.smpc", std::vector<uint8_t>(21, 0x99));
        const auto before = Snapshot(session.system);

        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(session.Load(core));
        Quiet(core);
        for (int i = 0; i < 3; i++) {
            core.RunFrame();
        }
        core.GetSaturn()->SMPC.PersistData();
        core.UnloadGame();
        CHECK(Snapshot(session.system) == before);
    }

    SECTION("none are created") {
        Session session("brimir_stv_sram_nortcfiles");
        const auto before = Snapshot(session.system);
        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(session.Load(core));
        Quiet(core);
        for (int i = 0; i < 3; i++) {
            core.RunFrame();
        }
        core.UnloadGame();
        CHECK(Snapshot(session.system) == before);
        const auto saves = Snapshot(session.saves);
        CHECK(saves.size() == 1); // bakubaku.srm only
        CHECK(saves.count("bakubaku.srm") == 1);
    }
}

TEST_CASE("stv sram: an ST-V session starts from MAME's SMPC defaults, not the console's settings", "[stv]") {
    // MAME's smpc_hle_device: SMEM is NVRAM with DEFAULT_ALL_0 and the clock is the host's. The console's settings in
    // this instance (a Saturn session before) are not the board's.
    Session session("brimir_stv_sram_smpcdefaults");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    ymir::smpc::PersistentSMPCData console{};
    console.SMEM = {0x01, 0x02, 0x03, 0x04};
    console.STE = true;
    core.GetSaturn()->SMPC.LoadPersistentData(console);

    REQUIRE(session.Load(core));
    ymir::smpc::PersistentSMPCData inSession{};
    core.GetSaturn()->SMPC.SavePersistentData(inSession);
    CHECK(inSession.SMEM == std::array<uint8_t, 4>{});
    CHECK_FALSE(inSession.STE);

    core.UnloadGame();
    ymir::smpc::PersistentSMPCData after{};
    core.GetSaturn()->SMPC.SavePersistentData(after);
    CHECK(after.SMEM == console.SMEM); // the console's, as they were
    CHECK(after.STE);
}

namespace {

std::filesystem::path EnvDir(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return {};
    }
    std::error_code ec;
    return std::filesystem::is_directory(value, ec) ? std::filesystem::path(value) : std::filesystem::path{};
}

void CopyBiosFiles(const std::filesystem::path &from, const std::filesystem::path &to) {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(from, ec)) {
        if (entry.is_regular_file(ec) && entry.file_size(ec) == stv::kSTVBiosSize) {
            std::filesystem::copy_file(entry.path(), to / entry.path().filename(),
                                       std::filesystem::copy_options::skip_existing, ec);
        }
    }
}

// The 93C46 of MAME's bakubaku after its first boot (the BIOS writes it into the blank chip: header, settings, a CRC and
// the mirror at 0x44), as MAME's nvram file holds it (each 16-bit cell little-endian: "SEGA" reads "ESAG")
constexpr std::array<uint8_t, 128> kMameFirstBootEEPROM = {
    0x45, 0x53, 0x41, 0x47, 0xff, 0xff, 0xff, 0xff, 0x0e, 0x9f, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0xfd, 0x08, 0x81, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x0e, 0x9f, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0xfd, 0x08, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

} // namespace

TEST_CASE("stv sram: bakubaku's BIOS initialises the blank EEPROM as it does in MAME", "[.stv-rom][bakubaku]") {
    const auto biosDir = EnvDir("BRIMIR_STV_BIOS_DIR");
    const auto romDir = EnvDir("BRIMIR_STV_ROM_DIR");
    if (biosDir.empty() || romDir.empty() || !std::filesystem::exists(romDir / "bakubaku.zip")) {
        SKIP("BRIMIR_STV_BIOS_DIR and BRIMIR_STV_ROM_DIR with bakubaku.zip are needed");
    }
    TempDir scratch("brimir_stv_sram_real");
    const auto saves = scratch.path / "saves";
    std::filesystem::create_directories(saves);
    CopyBiosFiles(biosDir, scratch.path);

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadGame(Str(romDir / "bakubaku.zip").c_str(), Str(saves).c_str(), Str(scratch.path).c_str()));
    REQUIRE(core.IsSTVLoaded());
    REQUIRE(core.GetSRAMSize() == kTotal);
    Quiet(core);

    // MAME's chip is blank until the BIOS has run
    CHECK(AllEqual(Image(core), kBackup, kTotal, 0xFF));
    for (int i = 0; i < 1300; i++) {
        core.RunFrame();
    }
    const std::vector<uint8_t> image = Image(core);
    const std::vector<uint8_t> eeprom(image.begin() + static_cast<std::ptrdiff_t>(kBackup), image.end());
    // MAME's file holds each 16-bit cell in the host's byte order (little-endian); the board's cells are big-endian
    std::vector<uint8_t> expected(kMameFirstBootEEPROM.begin(), kMameFirstBootEEPROM.end());
    for (size_t i = 0; i + 1 < expected.size(); i += 2) {
        std::swap(expected[i], expected[i + 1]);
    }
    for (size_t i = 0; i < expected.size(); i++) {
        INFO("byte " << i << " Brimir " << static_cast<int>(eeprom[i]) << " MAME " << static_cast<int>(expected[i]));
        CHECK(eeprom[i] == expected[i]);
    }
    CHECK(core.GetLastError().empty());
}

TEST_CASE("stv sram: bakubaku's EEPROM and backup RAM survive a restart", "[.stv-rom][bakubaku]") {
    const auto biosDir = EnvDir("BRIMIR_STV_BIOS_DIR");
    const auto romDir = EnvDir("BRIMIR_STV_ROM_DIR");
    if (biosDir.empty() || romDir.empty() || !std::filesystem::exists(romDir / "bakubaku.zip")) {
        SKIP("BRIMIR_STV_BIOS_DIR and BRIMIR_STV_ROM_DIR with bakubaku.zip are needed");
    }
    TempDir scratch("brimir_stv_sram_restart");
    const auto saves = scratch.path / "saves";
    std::filesystem::create_directories(saves);
    CopyBiosFiles(biosDir, scratch.path);
    const std::string zip = Str(romDir / "bakubaku.zip");

    std::vector<uint8_t> first;
    {
        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(core.LoadGame(zip.c_str(), Str(saves).c_str(), Str(scratch.path).c_str()));
        Quiet(core);
        for (int i = 0; i < 1300; i++) {
            core.RunFrame();
        }
        core.GetSaturn()->mem.GetInternalBackupRAM().WriteByte(2 * 1234, 0xC3); // a record the game would keep
        first = Image(core);
        core.UnloadGame();
    }
    REQUIRE(first.size() == kTotal);
    CHECK(first[1234] == 0xC3);
    CHECK(ReadBytes(saves / "bakubaku.srm") == first);

    // Restarted: the frontend loads the .srm into the buffer; the game runs on from it
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadGame(zip.c_str(), Str(saves).c_str(), Str(scratch.path).c_str()));
    Quiet(core);
    void *pointer = core.GetSRAMData();
    REQUIRE(pointer != nullptr);
    std::memcpy(pointer, first.data(), first.size());
    core.RunFrame();
    CHECK(EEPROM(core) == std::vector<uint8_t>(first.begin() + static_cast<std::ptrdiff_t>(kBackup), first.end()));
    CHECK(BackupRAM(core)[1234] == 0xC3);
    for (int i = 0; i < 1300; i++) {
        core.RunFrame();
    }
    CHECK(BackupRAM(core)[1234] == 0xC3);
    CHECK(EEPROM(core) == std::vector<uint8_t>(first.begin() + static_cast<std::ptrdiff_t>(kBackup), first.end()));
    CHECK(core.GetLastError().empty());
}
