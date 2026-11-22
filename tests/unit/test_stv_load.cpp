// Loading ST-V zips through CoreWrapper::LoadGame: the routing, the error messages, the board's wiring (area code,
// EEPROM write cycles in emulated time, the 68K reset line, the IOGA page of the bus) and the proof that a Saturn
// loaded after ST-V content in the same core finds nothing of it. Real ROMs and BIOS files are read from
// BRIMIR_STV_ROM_DIR and BRIMIR_STV_BIOS_DIR by the [.stv-rom] cases, which SKIP without them.

#include "catch_amalgamated.hpp"

#include "unit/stv_test_zip.hpp"

#include <libretro.h>

#include <brimir/core_wrapper.hpp>
#include <brimir/lockstep.hpp>

#include <ymir/core/configuration.hpp>
#include <ymir/core/scheduler.hpp>
#include <ymir/hw/cart/cart_base.hpp>
#include <ymir/hw/smpc/smpc.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/saturn.hpp>

#include <stv/stv_cartridge.hpp>
#include <stv/stv_game_db.hpp>
#include <stv/stv_input.hpp>
#include <stv/stv_io.hpp>
#include <stv/stv_system.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
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

const stv::GameInfo &Game(const char *name) {
    const stv::GameInfo *game = stv::FindGame(name);
    REQUIRE(game != nullptr);
    return *game;
}

// One zero-filled zip entry per distinct file of the game, sized as the database says, with the database CRC-32 in the
// archive (the loader trusts the archive's CRC, as the detection does).
std::vector<test::TestZipEntry> SetEntries(const stv::GameInfo &game, size_t keep = ~size_t{0}) {
    std::vector<test::TestZipEntry> entries;
    std::set<std::string> seen;
    for (const auto &rom : game.roms) {
        if (entries.size() >= keep || !seen.insert(rom.file).second) {
            continue;
        }
        test::TestZipEntry entry;
        entry.name = rom.file;
        entry.data.assign(rom.size, 0);
        entry.deflate = true;
        entry.crcOverride = rom.crc32;
        entries.push_back(std::move(entry));
    }
    return entries;
}

void WriteFile(const std::filesystem::path &path, size_t size, uint8_t fill = 0) {
    std::vector<uint8_t> bytes(size, fill);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string Str(const std::filesystem::path &path) {
    return path.string();
}

constexpr uint32_t kPDR1 = 0x010'0075;
constexpr uint32_t kPDR2 = 0x010'0077;
constexpr uint32_t kDDR1 = 0x010'0079;
constexpr uint32_t kDDR2 = 0x010'007B;

void RunFor(ymir::Saturn &saturn, uint64_t cycles) {
    uint64_t elapsed = 0;
    while (elapsed < cycles) {
        elapsed += saturn.StepMasterSH2();
    }
}

// An ST-V board around a bare Saturn: a zero BIOS and a small cartridge (the CPU idles in zeroed ROM)
struct Board {
    std::unique_ptr<ymir::Saturn> owner = std::make_unique<ymir::Saturn>();
    ymir::Saturn &saturn = *owner;
    stv::STVSystem system;
    std::array<uint8_t, stv::kSTVBiosSize> bios{};
    std::vector<uint8_t> cart = std::vector<uint8_t>(0x1000, 0xFF);

    bool Attach(const char *game = "bakubaku", uint8_t areaCode = 1) {
        std::string error;
        const bool ok = system.Attach(saturn, Game(game), bios, cart, areaCode, error);
        INFO(error);
        return ok;
    }
};

// What the 93C46 sees of one PDR1 write: CS = bit 2, CLK = bit 3, DI = bit 4
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

bool DOReady(ymir::Saturn &saturn) {
    return (saturn.mainBus.Read<uint8_t>(kPDR2) & 1) != 0;
}

} // namespace

TEST_CASE("stv load: a zip that is not an ST-V set fails with the zip message", "[stv]") {
    TempDir dir("brimir_stv_load_notstv");
    const auto zipPath = dir.path / "disc.zip";
    test::TestZipEntry entry;
    entry.name = "game.cue";
    entry.data = {'h', 'i'};
    test::WriteTestZip(zipPath, {entry});

    CHECK(CoreWrapper::DetectContent(Str(zipPath).c_str()).kind == stv::DetectKind::NotSTV);

    CoreWrapper core;
    REQUIRE(core.Initialize());
    CHECK_FALSE(core.LoadGame(Str(zipPath).c_str(), Str(dir.path).c_str(), Str(dir.path).c_str()));
    CHECK(core.GetLastError() ==
          "Zip files are only supported for ST-V games; unzip Saturn discs or convert them to CHD");
    CHECK_FALSE(core.IsSTVLoaded());
    CHECK_FALSE(core.IsGameLoaded());
    CHECK(core.GetSTVGame() == nullptr);
}

TEST_CASE("stv load: an incomplete set fails naming the set and its missing files", "[stv]") {
    TempDir dir("brimir_stv_load_incomplete");
    const stv::GameInfo &game = Game("bakubaku");
    const auto zipPath = dir.path / "bakubaku.zip";
    test::WriteTestZip(zipPath, SetEntries(game, 1)); // the first file only

    const stv::DetectResult detected = CoreWrapper::DetectContent(Str(zipPath).c_str());
    REQUIRE(detected.kind == stv::DetectKind::Incomplete);
    REQUIRE_FALSE(detected.missing.empty());

    CoreWrapper core;
    REQUIRE(core.Initialize());
    CHECK_FALSE(core.LoadGame(Str(zipPath).c_str(), Str(dir.path).c_str(), Str(dir.path).c_str()));
    const std::string &error = core.GetLastError();
    CHECK(error.rfind("ST-V set bakubaku is incomplete: missing ", 0) == 0);
    for (const std::string &name : detected.missing) {
        CHECK(error.find(name) != std::string::npos);
    }
    CHECK(error.find(game.roms[0].file) == std::string::npos); // the file that is there
    CHECK_FALSE(core.IsSTVLoaded());
}

TEST_CASE("stv load: a Saturn disc path never reaches the ST-V code", "[stv]") {
    TempDir dir("brimir_stv_load_cue");
    const auto cuePath = dir.path / "disc.cue";
    {
        std::ofstream out(cuePath);
        out << "this is not a cue sheet\n";
    }
    CHECK(CoreWrapper::DetectContent(Str(cuePath).c_str()).kind == stv::DetectKind::NotZip);
    CHECK(CoreWrapper::DetectContent(nullptr).kind == stv::DetectKind::NotZip);

    // Not even a file named like an ST-V set: a .zip that is no zip is left to the disc loader
    const auto fakeZip = dir.path / "bakubaku.zip";
    {
        std::ofstream out(fakeZip, std::ios::binary);
        out << "not a zip";
    }
    CHECK(CoreWrapper::DetectContent(Str(fakeZip).c_str()).kind == stv::DetectKind::NotZip);

    CoreWrapper core;
    REQUIRE(core.Initialize());
    for (const auto &path : {cuePath, fakeZip}) {
        CHECK_FALSE(core.LoadGame(Str(path).c_str(), Str(dir.path).c_str(), Str(dir.path).c_str()));
        CHECK_FALSE(core.IsSTVLoaded());
        CHECK(core.GetLastError().find("ST-V") == std::string::npos);
        CHECK_FALSE(core.GetSaturn()->SMPC.IsSTVMode());
    }
}

TEST_CASE("stv load: a set without a BIOS for its regions fails before the system changes", "[stv]") {
    TempDir dir("brimir_stv_load_nobios");
    const auto zipPath = dir.path / "bakubaku.zip";
    test::WriteTestZip(zipPath, SetEntries(Game("bakubaku")));

    CoreWrapper core;
    REQUIRE(core.Initialize());
    CHECK_FALSE(core.LoadGame(Str(zipPath).c_str(), Str(dir.path).c_str(), Str(dir.path).c_str()));
    CHECK(core.GetLastError().find("epr-20091.ic8") != std::string::npos); // the accepted names, from SelectSTVBios
    CHECK_FALSE(core.IsSTVLoaded());
    CHECK_FALSE(core.GetSaturn()->SMPC.IsSTVMode());
}

TEST_CASE("stv load: LoadGame builds the board and UnloadGame takes it down again", "[stv]") {
    TempDir dir("brimir_stv_load_full");
    const auto zipPath = dir.path / "bakubaku.zip";
    test::WriteTestZip(zipPath, SetEntries(Game("bakubaku")));
    WriteFile(dir.path / "epr-20091.ic8", stv::kSTVBiosSize);

    CoreWrapper core;
    REQUIRE(core.Initialize());
    ymir::Saturn &saturn = *core.GetSaturn();
    const uint8_t areaBefore = saturn.SMPC.GetAreaCode();

    REQUIRE(core.LoadGame(Str(zipPath).c_str(), Str(dir.path).c_str(), Str(dir.path).c_str()));
    CHECK(core.GetLastError().empty());
    REQUIRE(core.IsSTVLoaded());
    CHECK(core.IsGameLoaded());
    REQUIRE(core.GetSTVGame() != nullptr);
    CHECK(std::string(core.GetSTVGame()->shortName) == "bakubaku");
    CHECK(saturn.SMPC.IsSTVMode());
    CHECK(saturn.GetCartridge().OwnsCartridgeSpace());
    CHECK(saturn.SMPC.GetAreaCode() == 1); // the Japan BIOS
    CHECK(core.GetSTVIOBoardForTesting() != nullptr);
    CHECK(core.GetSTVLoadDescription().find("bakubaku") != std::string::npos);
    CHECK(core.GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);
    CHECK(core.GetCartridgeDescription().find("bakubaku") != std::string::npos);

    // The backup RAM is the board's: all zero, not a formatted Saturn image
    const auto image = saturn.mem.GetInternalBackupRAM().ReadAll();
    CHECK(std::all_of(image.begin(), image.end(), [](uint8_t b) { return b == 0; }));

    // The IOGA answers on its page: the board's idle inputs, not the Saturn's unmapped zero
    CHECK(saturn.mainBus.Read<uint8_t>(0x0040'0000) == 0xFF);

    core.UnloadGame();
    CHECK_FALSE(core.IsSTVLoaded());
    CHECK_FALSE(core.IsGameLoaded());
    CHECK_FALSE(core.IsIPLLoaded());
    CHECK(core.GetSTVGame() == nullptr);
    CHECK(core.GetSTVIOBoardForTesting() == nullptr);
    CHECK_FALSE(saturn.SMPC.IsSTVMode());
    CHECK_FALSE(saturn.GetCartridge().OwnsCartridgeSpace());
    CHECK(saturn.GetCartridge().GetType() == ymir::cart::CartType::None);
    CHECK(saturn.SMPC.GetAreaCode() == areaBefore);
    CHECK(core.GetSTVLoadDescription().empty());
    CHECK(core.GetCartridgeDescription() == "none");
    const auto restored = saturn.mem.GetInternalBackupRAM().ReadAll();
    CHECK_FALSE(std::all_of(restored.begin(), restored.end(), [](uint8_t b) { return b == 0; })); // formatted again
    CHECK(saturn.mainBus.Read<uint8_t>(0x0040'0000) == 0);

    // and it loads again
    REQUIRE(core.LoadGame(Str(zipPath).c_str(), Str(dir.path).c_str(), Str(dir.path).c_str()));
    CHECK(core.IsSTVLoaded());
}

TEST_CASE("stv load: SetSTVInput reaches the I/O ports once per emulated frame", "[stv]") {
    TempDir dir("brimir_stv_load_input");
    const auto zipPath = dir.path / "bakubaku.zip";
    test::WriteTestZip(zipPath, SetEntries(Game("bakubaku")));
    WriteFile(dir.path / "epr-20091.ic8", stv::kSTVBiosSize);

    CoreWrapper core;
    REQUIRE(core.Initialize());
    // A Saturn session has no I/O board: the call is ignored
    core.SetSTVInput(0, stv::MapSTVInput(1u << RETRO_DEVICE_ID_JOYPAD_START, stv::Controls::ThreeButtons));

    REQUIRE(core.LoadGame(Str(zipPath).c_str(), Str(dir.path).c_str(), Str(dir.path).c_str()));
    REQUIRE(core.IsSTVLoaded());
    core.SetThreadedVDP1(false);
    core.SetThreadedVDP2(false);
    stv::STVIOBoard *io = core.GetSTVIOBoardForTesting();
    REQUIRE(io != nullptr);
    const auto portA = [&] { return io->ReadIOGAByte(0x400000); };
    const auto portC = [&] { return io->ReadIOGAByte(0x400004); };

    core.SetSTVInput(0, stv::MapSTVInput(1u << RETRO_DEVICE_ID_JOYPAD_START | 1u << RETRO_DEVICE_ID_JOYPAD_LEFT,
                                         stv::Controls::ThreeButtons));
    core.SetSTVInput(2, stv::MapSTVInput(0xFFFF, stv::Controls::ThreeButtons)); // no third player
    CHECK(portC() == 0xFF); // the board latches the controls when a frame starts
    core.RunFrame();
    CHECK(portC() == static_cast<uint8_t>(~0x10));
    CHECK(portA() == static_cast<uint8_t>(~0x80));

    core.SetSTVInput(0, {});
    core.RunFrame();
    CHECK(portC() == 0xFF);
    CHECK(portA() == 0xFF);
}

TEST_CASE("stv load: the SMPC area code is MAME's 1 whatever the BIOS region", "[stv]") {
    // MAME stv_state::stv: set_region_code(1) for every set (init_stv_us's 4 is for the CD games only)
    CHECK(stv::kSTVAreaCode == 1);

    struct Case {
        const char *game;
        const char *bios;
        const char *region;
    };
    for (const Case &c : {Case{"bakubaku", "epr-20091.ic8", "Japan"}, Case{"diehard", "epr-17952a.ic8", "USA"},
                          Case{"diehard", "epr-17954a.ic8", "Europe"}}) {
        INFO(c.game << " on the " << c.region << " BIOS");
        TempDir dir("brimir_stv_load_area");
        const auto zipPath = dir.path / (std::string(c.game) + ".zip");
        test::WriteTestZip(zipPath, SetEntries(Game(c.game)));
        WriteFile(dir.path / c.bios, stv::kSTVBiosSize); // the only BIOS in the folder
        CoreWrapper core;
        REQUIRE(core.Initialize());
        core.GetSaturn()->SMPC.SetAreaCode(0x4); // what a Saturn session left
        REQUIRE(core.LoadGame(Str(zipPath).c_str(), Str(dir.path).c_str(), Str(dir.path).c_str()));
        CHECK(core.GetSaturn()->SMPC.GetAreaCode() == 1);
        CHECK(core.GetSTVLoadDescription().find(c.bios) != std::string::npos);
        CHECK(core.GetSTVLoadDescription().find("area code 1 (as MAME sets it") != std::string::npos);
        core.UnloadGame();
        CHECK(core.GetSaturn()->SMPC.GetAreaCode() == 0x4); // restored
    }
}

TEST_CASE("stv load: an EEPROM write issued through the SMPC PDR pins completes in emulated time", "[stv]") {
    Board board;
    REQUIRE(board.Attach());
    ymir::Saturn &saturn = board.saturn;

    saturn.mainBus.Write<uint8_t>(kDDR1, 0x1F);
    saturn.mainBus.Write<uint8_t>(kDDR2, 0x00);
    Pdr1(saturn, 0x00);

    // EWEN (start, 00, 11xxxx), then CS low
    ClockBit(saturn, true);
    ClockBits(saturn, 0b0011'0000, 8);
    Pdr1(saturn, 0x00);

    // WRITE cell 5 = 0xA55A
    ClockBit(saturn, true);
    ClockBits(saturn, 0b01, 2);
    ClockBits(saturn, 5, 6);
    ClockBits(saturn, 0xA55A, 16);
    Pdr1(saturn, 0x00); // CS low ...
    Pdr1(saturn, 0x04); // ... and high: DO is the ready line

    const stv::STVIOBoard &io = board.system.IO();
    CHECK(io.GetEEPROM()[10] == 0xA5);
    CHECK(io.GetEEPROM()[11] == 0x5A);
    CHECK_FALSE(DOReady(saturn)); // the cycle takes 1.75 ms

    RunFor(saturn, 20000); // 0.75 ms at 26.8 MHz
    CHECK_FALSE(DOReady(saturn));

    RunFor(saturn, 40000); // 2.2 ms in all
    CHECK(DOReady(saturn));
}

TEST_CASE("stv load: PDR2 bit 4 is the 68K's reset line", "[stv]") {
    Board board;
    REQUIRE(board.Attach());
    ymir::Saturn &saturn = board.saturn;
    const auto m68kEnabled = [&] {
        auto state = std::make_unique<ymir::savestate::SCSPSaveState>(); // holds the sound RAM: not on the stack
        saturn.SCSP.SaveState(*state);
        return state->m68kEnabled;
    };
    CHECK_FALSE(m68kEnabled()); // held in reset after the board's reset

    saturn.mainBus.Write<uint8_t>(kDDR2, 0x10);
    saturn.mainBus.Write<uint8_t>(kPDR2, 0x00); // reset line released
    CHECK(m68kEnabled());
    saturn.mainBus.Write<uint8_t>(kPDR2, 0x10); // asserted
    CHECK_FALSE(m68kEnabled());
    saturn.mainBus.Write<uint8_t>(kPDR2, 0x00);
    CHECK(m68kEnabled());
}

TEST_CASE("stv load: detaching restores the Saturn's bus page, SMPC and cartridge", "[stv]") {
    auto freshOwner = std::make_unique<ymir::Saturn>();
    ymir::Saturn &fresh = *freshOwner;
    Board board;
    board.saturn.SMPC.SetAreaCode(0x4);
    ymir::smpc::PersistentSMPCData settings;
    settings.SMEM = {1, 2, 3, 4};
    settings.STE = true;
    board.saturn.SMPC.LoadPersistentData(settings);

    REQUIRE(board.Attach("diehard", 0xC));
    ymir::Saturn &saturn = board.saturn;
    REQUIRE(saturn.SMPC.IsSTVMode());
    REQUIRE(saturn.SMPC.GetAreaCode() == 0xC);
    // the board's session changed the settings
    ymir::smpc::PersistentSMPCData during;
    saturn.SMPC.SavePersistentData(during);

    board.system.Detach();
    CHECK_FALSE(board.system.IsAttached());
    CHECK_FALSE(saturn.SMPC.IsSTVMode());
    CHECK(saturn.SMPC.GetAreaCode() == 0x4);
    ymir::smpc::PersistentSMPCData after;
    saturn.SMPC.SavePersistentData(after);
    CHECK(after.SMEM == settings.SMEM);
    CHECK(after.STE == settings.STE);
    CHECK(saturn.GetCartridge().GetType() == ymir::cart::CartType::None);

    // Page 0x40 of the main bus as a Saturn has it: reads 0, ignores writes, peeks 0, the same access cycles
    for (const uint32_t address : {0x0040'0000u, 0x0040'0001u, 0x0040'0040u, 0x0040'007Eu, 0x0040'0080u, 0x0040'FFFEu}) {
        saturn.mainBus.Write<uint8_t>(address, 0x5A);
        saturn.mainBus.Write<uint16_t>(address & ~1u, 0x5A5A);
        saturn.mainBus.Write<uint32_t>(address & ~3u, 0x5A5A5A5A);
        saturn.mainBus.Poke<uint16_t>(address & ~1u, 0x5A5A);
        CHECK(saturn.mainBus.Read<uint8_t>(address) == fresh.mainBus.Read<uint8_t>(address));
        CHECK(saturn.mainBus.Read<uint16_t>(address & ~1u) == fresh.mainBus.Read<uint16_t>(address & ~1u));
        CHECK(saturn.mainBus.Read<uint32_t>(address & ~3u) == fresh.mainBus.Read<uint32_t>(address & ~3u));
        CHECK(saturn.mainBus.Peek<uint8_t>(address) == fresh.mainBus.Peek<uint8_t>(address));
        CHECK(saturn.mainBus.Peek<uint32_t>(address & ~3u) == fresh.mainBus.Peek<uint32_t>(address & ~3u));
        CHECK(saturn.mainBus.ArrayReadPointer(address) == nullptr);
        CHECK(saturn.mainBus.GetAccessCycles<uint8_t, false>(address) ==
              fresh.mainBus.GetAccessCycles<uint8_t, false>(address));
        CHECK(saturn.mainBus.GetAccessCycles<uint16_t, true>(address & ~1u) ==
              fresh.mainBus.GetAccessCycles<uint16_t, true>(address & ~1u));
        CHECK(saturn.mainBus.GetAccessCycles<uint32_t, false>(address & ~3u) ==
              fresh.mainBus.GetAccessCycles<uint32_t, false>(address & ~3u));
        CHECK_FALSE(saturn.mainBus.IsBusWait(address, 4, false));
    }

    // The PDR pins are the Saturn's again: a write no longer reaches the board
    const auto cells = board.system.IO().GetEEPROM();
    saturn.mainBus.Write<uint8_t>(kDDR1, 0x1F);
    saturn.mainBus.Write<uint8_t>(kPDR1, 0x04);
    CHECK(saturn.mainBus.Peek<uint8_t>(kPDR1) == fresh.mainBus.Peek<uint8_t>(kPDR1));
    CHECK(board.system.IO().GetEEPROM() == cells);

    // detaching twice, and attaching again, work
    board.system.Detach();
    CHECK(board.Attach());
    CHECK(saturn.SMPC.IsSTVMode());
}

namespace {

// The SMPC's view of the system, recording the order its calls come in
class RecordingOps final : public ymir::smpc::ISMPCOperations {
public:
    std::vector<std::string> calls;
    ymir::sys::ClockSpeed speed = ymir::sys::ClockSpeed::_320;

    bool GetNMI() const override { return false; }
    void RaiseNMI() override { calls.emplace_back("NMI"); }
    void EnableAndResetSlaveSH2() override {}
    void DisableSlaveSH2() override { calls.emplace_back("slave off"); }
    void EnableAndResetM68K() override {}
    void DisableM68K() override {}
    void SoftResetSystem() override {}
    void ClockChangeSoftReset() override { calls.emplace_back("reset"); }
    ymir::sys::ClockSpeed GetClockSpeed() const override { return speed; }
    void SetClockSpeed(ymir::sys::ClockSpeed clockSpeed) override {
        speed = clockSpeed;
        calls.emplace_back(clockSpeed == ymir::sys::ClockSpeed::_352 ? "clock 352" : "clock 320");
    }
};

std::vector<std::string> ClockChangeCalls(bool stvMode, uint8_t command) {
    ymir::core::Scheduler scheduler;
    ymir::core::Configuration config;
    RecordingOps ops;
    ymir::smpc::SMPC smpc(scheduler, ops, config.rtc);
    auto bus = std::make_unique<ymir::sys::SH2Bus>();
    smpc.MapMemory(*bus);
    smpc.SetSTVMode(stvMode);

    bus->Write<uint8_t>(0x010'0063, 1);       // SF
    bus->Write<uint8_t>(0x010'001F, command); // COMREG
    scheduler.Advance(1'000'000);             // CKCHG takes 200000 cycles
    return ops.calls;
}

} // namespace

TEST_CASE("stv load: the SMPC clock change sequence is clock first, then NMI, in ST-V mode only", "[stv]") {
    constexpr uint8_t kCKCHG352 = 0x0E;
    constexpr uint8_t kCKCHG320 = 0x0F;

    // MAME's smpc_hle_device::handle_command: the dot select changes at the start of the sequence, the master NMI is
    // raised at its end
    CHECK(ClockChangeCalls(true, kCKCHG352) ==
          std::vector<std::string>{"reset", "slave off", "clock 352", "NMI"});
    CHECK(ClockChangeCalls(true, kCKCHG320) ==
          std::vector<std::string>{"reset", "slave off", "clock 320", "NMI"});

    // A Saturn keeps upstream's order
    CHECK(ClockChangeCalls(false, kCKCHG352) ==
          std::vector<std::string>{"reset", "slave off", "NMI", "clock 352"});
}

namespace {

// Copies the 512 KiB files of a folder (the BIOS files) into a scratch system directory
void CopyBiosFiles(const std::filesystem::path &from, const std::filesystem::path &to) {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(from, ec)) {
        if (entry.is_regular_file(ec) && entry.file_size(ec) == stv::kSTVBiosSize) {
            std::filesystem::copy_file(entry.path(), to / entry.path().filename(),
                                       std::filesystem::copy_options::skip_existing, ec);
        }
    }
}

std::filesystem::path EnvDir(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return {};
    }
    std::error_code ec;
    return std::filesystem::is_directory(value, ec) ? std::filesystem::path(value) : std::filesystem::path{};
}

// FNV-1a over the frame (size and pixels) and the audio the frame produced
uint64_t FrameHash(CoreWrapper &core) {
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](const void *data, size_t size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; i++) {
            hash = (hash ^ bytes[i]) * 0x100000001b3ull;
        }
    };
    const unsigned width = core.GetFramebufferWidth();
    const unsigned height = core.GetFramebufferHeight();
    const unsigned pitch = core.GetFramebufferPitch();
    mix(&width, sizeof(width));
    mix(&height, sizeof(height));
    const auto *pixels = static_cast<const uint8_t *>(core.GetFramebuffer());
    for (unsigned y = 0; pixels != nullptr && y < height; y++) {
        mix(pixels + static_cast<size_t>(y) * pitch, static_cast<size_t>(width) * 4);
    }
    std::vector<int16_t> audio(8192);
    const size_t samples = core.GetAudioSamples(audio.data(), audio.size() / 2);
    mix(&samples, sizeof(samples));
    mix(audio.data(), samples * 2 * sizeof(int16_t));
    return hash;
}

std::vector<uint64_t> RunHashed(CoreWrapper &core, int frames) {
    std::vector<uint64_t> hashes;
    for (int i = 0; i < frames; i++) {
        core.RunFrame();
        hashes.push_back(FrameHash(core));
    }
    return hashes;
}

} // namespace

TEST_CASE("stv load: bakubaku loads from the real ROM set and runs", "[.stv-rom]") {
    const auto biosDir = EnvDir("BRIMIR_STV_BIOS_DIR");
    const auto romDir = EnvDir("BRIMIR_STV_ROM_DIR");
    if (biosDir.empty() || romDir.empty() || !std::filesystem::exists(romDir / "bakubaku.zip")) {
        SKIP("BRIMIR_STV_BIOS_DIR and BRIMIR_STV_ROM_DIR with bakubaku.zip are needed");
    }
    TempDir scratch("brimir_stv_load_real");
    CopyBiosFiles(biosDir, scratch.path);
    const auto zipPath = romDir / "bakubaku.zip";

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadGame(Str(zipPath).c_str(), Str(scratch.path).c_str(), Str(scratch.path).c_str()));
    INFO(core.GetSTVLoadDescription());
    REQUIRE(core.IsSTVLoaded());
    CHECK(std::string(core.GetSTVGame()->shortName) == "bakubaku");
    CHECK(core.GetSaturn()->SMPC.IsSTVMode());

    // Still in the BIOS's check (about 1130 frames), running without an exception
    for (int i = 0; i < 600; i++) {
        core.RunFrame();
    }
    CHECK(core.GetLastError().empty());
    CHECK(core.IsSTVLoaded());
}

TEST_CASE("stv load: a Saturn session after ST-V content starts as a fresh core does", "[.stv-rom]") {
    const auto biosDir = EnvDir("BRIMIR_STV_BIOS_DIR");
    const auto romDir = EnvDir("BRIMIR_STV_ROM_DIR");
    if (biosDir.empty() || romDir.empty() || !std::filesystem::exists(romDir / "bakubaku.zip") ||
        !std::filesystem::exists(biosDir / "mpr-17933.bin")) {
        SKIP("BRIMIR_STV_BIOS_DIR with mpr-17933.bin and BRIMIR_STV_ROM_DIR with bakubaku.zip are needed");
    }
    TempDir scratch("brimir_stv_load_after");
    CopyBiosFiles(biosDir, scratch.path);
    const std::string usBios = Str(scratch.path / "mpr-17933.bin");
    const std::string systemDir = Str(scratch.path);

    // A Saturn session without content, the way retro_load_game(NULL) starts it
    const auto startSaturn = [&](CoreWrapper &core) {
        REQUIRE(core.LoadIPLFromFile(usBios.c_str()));
        core.ApplyLoadTimeOptions(systemDir.c_str());
        core.SetThreadedVDP1(false);
        core.SetThreadedVDP2(false);
        REQUIRE(brimir::ApplyFixedRtc(core, brimir::kDeterministicRtcDate));
    };

    CoreWrapper reused;
    REQUIRE(reused.Initialize());
    REQUIRE(reused.LoadGame(Str(romDir / "bakubaku.zip").c_str(), systemDir.c_str(), systemDir.c_str()));
    REQUIRE(reused.IsSTVLoaded());
    std::vector<int16_t> audio(8192);
    for (int i = 0; i < 600; i++) {
        reused.RunFrame();
        reused.GetAudioSamples(audio.data(), audio.size() / 2);
    }
    reused.UnloadGame();
    CHECK_FALSE(reused.IsSTVLoaded());
    CHECK_FALSE(reused.GetSaturn()->SMPC.IsSTVMode());
    startSaturn(reused);

    CoreWrapper fresh;
    REQUIRE(fresh.Initialize());
    startSaturn(fresh);

    CHECK(reused.GetSaturn()->SMPC.GetAreaCode() == fresh.GetSaturn()->SMPC.GetAreaCode());
    CHECK(reused.GetSaturn()->GetIPLHash() == fresh.GetSaturn()->GetIPLHash());

    const std::vector<uint64_t> a = RunHashed(reused, 300);
    const std::vector<uint64_t> b = RunHashed(fresh, 300);
    REQUIRE(a.size() == b.size());
    size_t firstDifference = a.size();
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) {
            firstDifference = i;
            break;
        }
    }
    INFO("first frame that differs: " << firstDifference);
    CHECK(firstDifference == a.size());
    CHECK(reused.GetLastError().empty());
}

namespace {

// FNV-1a over the frame's size and pixels only (the audio changes every frame even on a still screen)
uint64_t VideoHash(CoreWrapper &core) {
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](const void *data, size_t size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; i++) {
            hash = (hash ^ bytes[i]) * 0x100000001b3ull;
        }
    };
    const unsigned width = core.GetFramebufferWidth();
    const unsigned height = core.GetFramebufferHeight();
    const unsigned pitch = core.GetFramebufferPitch();
    mix(&width, sizeof(width));
    mix(&height, sizeof(height));
    const auto *pixels = static_cast<const uint8_t *>(core.GetFramebuffer());
    for (unsigned y = 0; pixels != nullptr && y < height; y++) {
        mix(pixels + static_cast<size_t>(y) * pitch, static_cast<size_t>(width) * 4);
    }
    return hash;
}

uint32_t Crc32(const uint8_t *data, size_t size, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// The frame (XRGB8888) as an RGB PNG with stored (uncompressed) deflate blocks, for a human to compare with MAME
void WritePng(CoreWrapper &core, const std::filesystem::path &path) {
    const unsigned width = core.GetFramebufferWidth();
    const unsigned height = core.GetFramebufferHeight();
    const unsigned pitch = core.GetFramebufferPitch();
    const auto *pixels = static_cast<const uint8_t *>(core.GetFramebuffer());
    if (pixels == nullptr || width == 0 || height == 0) {
        return;
    }
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
    for (unsigned y = 0; y < height; y++) {
        raw.push_back(0); // filter: none
        const uint8_t *row = pixels + static_cast<size_t>(y) * pitch;
        for (unsigned x = 0; x < width; x++) {
            raw.push_back(row[x * 4 + 2]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 0]);
        }
    }
    std::vector<uint8_t> zlib = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t len = std::min<size_t>(raw.size() - pos, 0xFFFF);
        const bool last = pos + len == raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(len));
        zlib.push_back(static_cast<uint8_t>(len >> 8));
        zlib.push_back(static_cast<uint8_t>(~len));
        zlib.push_back(static_cast<uint8_t>(~len >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                    raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
        if (last) {
            break;
        }
    }
    uint32_t a = 1, b = 0;
    for (const uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8) {
        zlib.push_back(static_cast<uint8_t>(adler >> shift));
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto be32 = [&](uint32_t v) {
        const uint8_t bytes[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                                  static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        out.write(reinterpret_cast<const char *>(bytes), 4);
    };
    const auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32(static_cast<uint32_t>(data.size()));
        out.write(type, 4);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        uint32_t crc = Crc32(reinterpret_cast<const uint8_t *>(type), 4);
        crc = Crc32(data.data(), data.size(), crc);
        be32(crc);
    };
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.write(reinterpret_cast<const char *>(kSignature), sizeof(kSignature));
    std::vector<uint8_t> ihdr;
    for (const uint32_t v : {width, height}) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            ihdr.push_back(static_cast<uint8_t>(v >> shift));
        }
    }
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
    chunk("IHDR", ihdr);
    chunk("IDAT", zlib);
    chunk("IEND", {});
}

} // namespace

TEST_CASE("stv: bakubaku reaches attract and accepts coin and start", "[.stv-rom]") {
    const auto biosDir = EnvDir("BRIMIR_STV_BIOS_DIR");
    const auto romDir = EnvDir("BRIMIR_STV_ROM_DIR");
    if (biosDir.empty() || romDir.empty() || !std::filesystem::exists(romDir / "bakubaku.zip")) {
        SKIP("BRIMIR_STV_BIOS_DIR and BRIMIR_STV_ROM_DIR with bakubaku.zip are needed");
    }
    // Optional: where to write the screenshots (attract, title with a credit, game) for comparison with MAME's
    const auto screensDir = EnvDir("BRIMIR_STV_SCREENS_DIR");
    TempDir scratch("brimir_stv_bakubaku_attract");
    CopyBiosFiles(biosDir, scratch.path);

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadGame(Str(romDir / "bakubaku.zip").c_str(), Str(scratch.path).c_str(),
                          Str(scratch.path).c_str()));
    REQUIRE(core.IsSTVLoaded());
    core.SetThreadedVDP1(false);
    core.SetThreadedVDP2(false);
    ymir::Saturn &saturn = *core.GetSaturn();

    // The credit count in the BIOS's work RAM (the coin handler at 0x06001BAE adds to it). MAME: 0 in the attract mode,
    // 1 after one coin, 0 again once Start took it.
    constexpr uint32_t kCredits = 0x0600'0740;
    const auto credits = [&] { return saturn.mainBus.Peek<uint8_t>(kCredits); };

    // The controls go through CoreWrapper::SetSTVInput as the libretro layer sets them; RunFrame latches them
    int frame = 0;
    const auto press = [&](uint16_t retropadMask) {
        core.SetSTVInput(0, stv::MapSTVInput(retropadMask, stv::Controls::ThreeButtons));
    };
    const auto runFrame = [&] {
        core.RunFrame();
        ++frame;
    };
    const auto save = [&](const char *name) {
        if (!screensDir.empty()) {
            WritePng(core, screensDir / name);
        }
    };

    // The BIOS checks the board for about 1130 frames, then starts the game (MAME about 26 frames later: it waits for
    // a CD block that its cartridge ST-V does not map). No phantom credits once the game runs.
    while (frame < 1200) {
        runFrame();
    }
    int creditFrames = 0;
    std::set<uint64_t> attract;
    while (frame < 1500) {
        runFrame();
        creditFrames += credits() != 0 ? 1 : 0;
        if (frame >= 1250) {
            attract.insert(VideoHash(core));
        }
    }
    save("brimir_frame1500_attract.png");
    CHECK(creditFrames == 0);
    CHECK(credits() == 0);
    INFO("distinct attract frames: " << attract.size());
    CHECK(attract.size() >= 30);

    // One coin, held for 5 frames, is one credit
    press(1u << RETRO_DEVICE_ID_JOYPAD_SELECT);
    while (frame < 1505) {
        runFrame();
    }
    press(0);
    while (frame < 1560) {
        runFrame();
    }
    save("brimir_frame1560_credit1.png");
    CHECK(credits() == 1);

    // Start takes the credit and leads into the game: no frame looks like the attract mode any more
    while (frame < 1600) {
        runFrame();
    }
    press(1u << RETRO_DEVICE_ID_JOYPAD_START);
    while (frame < 1605) {
        runFrame();
    }
    press(0);
    int attractLike = 0;
    std::set<uint64_t> game;
    while (frame < 2205) {
        runFrame();
        const uint64_t hash = VideoHash(core);
        attractLike += attract.count(hash) != 0 ? 1 : 0;
        game.insert(hash);
        if (frame == 1700) {
            save("brimir_frame1700_game.png");
        } else if (frame == 1900) {
            save("brimir_frame1900_game.png");
        }
    }
    CHECK(attractLike == 0);
    CHECK(game.size() >= 30);
    CHECK(credits() == 0);
    CHECK(core.GetLastError().empty());
}
