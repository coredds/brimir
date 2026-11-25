// ST-V save states: the block (I/O board with its 93C46, protection chip, time base), its encoding after the Saturn
// payload, the session-decided decoding, and the refusals. The [stv] cases build sets from zero-filled files; the
// [.stv-rom] cases run the real bakubaku (BRIMIR_STV_BIOS_DIR and BRIMIR_STV_ROM_DIR, SKIP when unset).

#include "catch_amalgamated.hpp"

#include "unit/stv_test_bios.hpp"
#include "unit/stv_test_zip.hpp"

#include <libretro.h>

#include <brimir/core_wrapper.hpp>
#include <brimir/lockstep.hpp>
#include <brimir/savestate_codec.hpp>

#include <ymir/hw/cart/cart_base.hpp>
#include <ymir/sys/saturn.hpp>

#include <stv/stv_game_db.hpp>
#include <stv/stv_input.hpp>
#include <stv/stv_io.hpp>
#include <stv/stv_state.hpp>
#include <stv/stv_system.hpp>

#include <lz4.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace brimir;

namespace {

constexpr size_t kBackup = stv::kSTVBackupRAMSize;
constexpr size_t kTotal = stv::kSTVSaveRAMSize;
constexpr uint16_t kSelect = 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;

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

// A zip of zero-filled files for an ST-V set, an idle BIOS (stv_test_bios.hpp), a system and a save folder
struct Session {
    TempDir dir;
    std::filesystem::path zip, saves, system;

    Session(const char *name, const char *game) : dir(name) {
        saves = dir.path / "saves";
        system = dir.path / "system";
        std::filesystem::create_directories(saves);
        std::filesystem::create_directories(system);
        const stv::GameInfo *info = stv::FindGame(game);
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
        zip = dir.path / (std::string(game) + ".zip");
        test::WriteTestZip(zip, entries);
        test::WriteIdleSTVBios(system / "epr-20091.ic8");
    }

    bool Load(CoreWrapper &core) const {
        return core.LoadGame(Str(zip).c_str(), Str(saves).c_str(), Str(system).c_str());
    }
};

void Quiet(CoreWrapper &core) {
    core.SetThreadedVDP1(false);
    core.SetThreadedVDP2(false);
}

std::vector<uint8_t> Save(CoreWrapper &core) {
    std::vector<uint8_t> state(core.GetStateSize());
    REQUIRE(core.SaveState(state.data(), state.size()));
    // the frontend's buffer is GetStateSize() bytes; the state is the header and the compressed payload
    uint32_t compressed = 0;
    std::memcpy(&compressed, state.data() + 12, 4);
    state.resize(16 + compressed);
    return state;
}

// The payload a state's LZ4 frame holds
std::vector<uint8_t> Payload(const std::vector<uint8_t> &state) {
    uint32_t payloadSize = 0;
    uint32_t compressed = 0;
    std::memcpy(&payloadSize, state.data() + 8, 4);
    std::memcpy(&compressed, state.data() + 12, 4);
    std::vector<uint8_t> payload(payloadSize);
    const int n = LZ4_decompress_safe(reinterpret_cast<const char *>(state.data() + 16),
                                      reinterpret_cast<char *>(payload.data()), static_cast<int>(compressed),
                                      static_cast<int>(payloadSize));
    REQUIRE(n == static_cast<int>(payloadSize));
    return payload;
}

// A state with another payload in its frame (header version as given)
std::vector<uint8_t> Frame(const std::vector<uint8_t> &payload, uint32_t version = CoreWrapper::kSaveStateVersion) {
    std::vector<uint8_t> state(16 + static_cast<size_t>(LZ4_compressBound(static_cast<int>(payload.size()))));
    const int compressed = LZ4_compress_default(reinterpret_cast<const char *>(payload.data()),
                                                reinterpret_cast<char *>(state.data() + 16),
                                                static_cast<int>(payload.size()), static_cast<int>(state.size() - 16));
    REQUIRE(compressed > 0);
    const uint32_t magic = 0x32524942; // "BRI2", as CoreWrapper writes
    std::memcpy(state.data(), &magic, 4);
    std::memcpy(state.data() + 4, &version, 4);
    const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    const uint32_t compressedSize = static_cast<uint32_t>(compressed);
    std::memcpy(state.data() + 8, &payloadSize, 4);
    std::memcpy(state.data() + 12, &compressedSize, 4);
    state.resize(16 + static_cast<size_t>(compressed));
    return state;
}

stv::STVStateBlock Capture(const CoreWrapper &core) {
    stv::STVStateBlock block;
    REQUIRE(core.CaptureSTVState(block));
    return block;
}

std::vector<uint8_t> Encoded(const stv::STVStateBlock &block) {
    std::vector<uint8_t> bytes;
    stv::EncodeSTVState(block, bytes);
    return bytes;
}

// A block with every field set to something recognisable
stv::STVStateBlock PatternBlock() {
    stv::STVStateBlock block;
    block.gameHash = stv::STVGameHash("bakubaku");
    auto *io = reinterpret_cast<uint8_t *>(&block.io);
    for (size_t i = 0; i < sizeof(block.io); i++) {
        io[i] = static_cast<uint8_t>(i * 5 + 1);
    }
    auto *protection = reinterpret_cast<uint8_t *>(&block.protection);
    for (size_t i = 0; i < sizeof(block.protection); i++) {
        protection[i] = static_cast<uint8_t>(i * 11 + 3);
    }
    auto *comp = reinterpret_cast<uint8_t *>(&block.comp5838);
    for (size_t i = 0; i < sizeof(block.comp5838); i++) {
        comp[i] = static_cast<uint8_t>(i * 7 + 5);
    }
    block.schedulerCount = 0x0123456789ABCDEFull;
    block.nsRemainderBits = 0x3FE0000000000000ull; // 0.5
    return block;
}

// A block as Brimir 0.10.0 wrote it (version 1, stv_state.cpp at v0.10.0): magic, version, game hash, I/O board,
// protection, scheduler count, remainder, flags, then the trailer (block size, 'S','T','V','1')
std::vector<uint8_t> EncodedVersion1(const stv::STVStateBlock &block) {
    std::vector<uint8_t> out;
    const auto put = [&](const auto &value) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
        out.insert(out.end(), bytes, bytes + sizeof value);
    };
    put(stv::kSTVStateMagic);
    put(uint32_t{1});
    put(block.gameHash);
    put(block.io);
    put(block.protection);
    put(block.schedulerCount);
    put(block.nsRemainderBits);
    put(block.flags);
    put(static_cast<uint32_t>(out.size()));
    out.insert(out.end(), std::begin(stv::kSTVTrailerMagic), std::end(stv::kSTVTrailerMagic));
    return out;
}

uint8_t PortC(CoreWrapper &core) {
    return core.GetSTVIOBoardForTesting()->ReadIOGAByte(0x400004);
}

} // namespace

TEST_CASE("stv state: the block round trips through its encoding", "[stv]") {
    const stv::STVStateBlock block = PatternBlock();
    const std::vector<uint8_t> bytes = Encoded(block);
    REQUIRE(bytes.size() == stv::STVStateBlockSize() + 8);
    CHECK(bytes.size() == stv::STVStateTailSize());
    CHECK(std::equal(bytes.end() - 4, bytes.end(), stv::kSTVTrailerMagic));
    uint32_t size = 0;
    std::memcpy(&size, bytes.data() + stv::STVStateBlockSize(), 4);
    CHECK(size == stv::STVStateBlockSize());

    stv::STVStateBlock back;
    REQUIRE(stv::DecodeSTVState(bytes, back));
    CHECK(back.version == stv::kSTVStateVersion);
    CHECK(back.gameHash == block.gameHash);
    CHECK(std::memcmp(&back.io, &block.io, sizeof block.io) == 0);
    CHECK(std::memcmp(&back.protection, &block.protection, sizeof block.protection) == 0);
    CHECK(std::memcmp(&back.comp5838, &block.comp5838, sizeof block.comp5838) == 0);
    CHECK(back.schedulerCount == block.schedulerCount);
    CHECK(back.nsRemainderBits == block.nsRemainderBits);
    CHECK(back.flags == 0);
    CHECK(Encoded(back) == bytes);

    // appended to what is there
    std::vector<uint8_t> payload = {1, 2, 3};
    stv::EncodeSTVState(block, payload);
    REQUIRE(payload.size() == 3 + bytes.size());
    CHECK(std::equal(bytes.begin(), bytes.end(), payload.begin() + 3));
    CHECK(stv::HasSTVTrailer(payload));
    CHECK_FALSE(stv::HasSTVTrailer(std::vector<uint8_t>(payload.begin(), payload.end() - 1)));
}

TEST_CASE("stv state: a block with a wrong magic, version, size or trailer is refused", "[stv]") {
    const std::vector<uint8_t> good = Encoded(PatternBlock());
    stv::STVStateBlock out;
    REQUIRE(stv::DecodeSTVState(good, out));

    const auto refused = [&](const std::function<void(std::vector<uint8_t> &)> &damage) {
        std::vector<uint8_t> bytes = good;
        damage(bytes);
        stv::STVStateBlock untouched = PatternBlock();
        untouched.schedulerCount = 77;
        stv::STVStateBlock target = untouched;
        const bool ok = stv::DecodeSTVState(bytes, target);
        if (!ok) {
            CHECK(target.schedulerCount == 77); // a refused block leaves the target alone
        }
        return ok;
    };

    CHECK_FALSE(refused([](auto &b) { b[0] ^= 1; }));                       // magic
    CHECK_FALSE(refused([](auto &b) { b[4] = 3; }));                        // version
    CHECK_FALSE(refused([](auto &b) { b[4] = 1; }));                        // version 1 with version 2's size
    CHECK_FALSE(refused([](auto &b) { b[4] = 0; }));
    CHECK_FALSE(refused([](auto &b) { b.pop_back(); }));                    // size
    CHECK_FALSE(refused([](auto &b) { b.push_back(0); }));
    CHECK_FALSE(refused([](auto &b) { b.erase(b.begin()); }));
    CHECK_FALSE(refused([](auto &b) { b.clear(); }));
    CHECK_FALSE(refused([](auto &b) { b[b.size() - 1] = '2'; }));           // trailer magic
    CHECK_FALSE(refused([](auto &b) { b[b.size() - 4] = 'X'; }));
    CHECK_FALSE(refused([](auto &b) { b[b.size() - 8] ^= 1; }));            // trailer block size
    CHECK_FALSE(refused([](auto &b) { b[b.size() - 5] = 1; }));
    CHECK_FALSE(refused([](auto &b) { b[stv::STVStateBlockSize() - 4] = 1; })); // an unknown flag
    CHECK(refused([](auto &) {}));
}

TEST_CASE("stv state: a version 1 block (Brimir 0.10.0) decodes with the 315-5838 at its reset value", "[stv]") {
    stv::STVStateBlock block = PatternBlock();
    const std::vector<uint8_t> v1 = EncodedVersion1(block);
    REQUIRE(v1.size() == stv::STVStateTailSize(1));
    CHECK(stv::STVStateTailSize(1) < stv::STVStateTailSize());
    CHECK(stv::STVStateTailSize(2) == stv::STVStateTailSize());
    CHECK(stv::STVStateTailSize(3) == 0);

    std::vector<uint8_t> payload = {9, 8, 7};
    payload.insert(payload.end(), v1.begin(), v1.end());
    CHECK(stv::HasSTVTrailer(payload));
    CHECK(stv::STVStateTailSizeIn(payload) == v1.size());

    stv::STVStateBlock back = PatternBlock(); // the 5838 part holds a pattern: the decoding must reset it
    REQUIRE(stv::DecodeSTVState(v1, back));
    CHECK(back.version == 1);
    CHECK(back.gameHash == block.gameHash);
    CHECK(std::memcmp(&back.io, &block.io, sizeof block.io) == 0);
    CHECK(std::memcmp(&back.protection, &block.protection, sizeof block.protection) == 0);
    const ymir::cart::STV3155838::State reset{};
    CHECK(std::memcmp(&back.comp5838, &reset, sizeof reset) == 0);
    CHECK(back.schedulerCount == block.schedulerCount);
    CHECK(back.nsRemainderBits == block.nsRemainderBits);

    // the next save is a version 2 block
    const std::vector<uint8_t> again = Encoded(back);
    CHECK(again.size() == stv::STVStateTailSize());
    stv::STVStateBlock v2;
    REQUIRE(stv::DecodeSTVState(again, v2));
    CHECK(v2.version == stv::kSTVStateVersion);

    // a version 1 block with damage is still refused
    std::vector<uint8_t> bad = v1;
    bad[4] = 2; // version 2 with version 1's size
    CHECK_FALSE(stv::DecodeSTVState(bad, back));
    bad = v1;
    bad[v1.size() - 12] = 1; // an unknown flag
    CHECK_FALSE(stv::DecodeSTVState(bad, back));
}

TEST_CASE("stv state: a block of an unknown version is refused with a message naming it", "[stv]") {
    std::vector<uint8_t> bytes = Encoded(PatternBlock());
    bytes[4] = 3;
    stv::STVStateBlock out;
    std::string why;
    CHECK_FALSE(stv::DecodeSTVState(bytes, out, &why));
    CHECK(why.find("version 3") != std::string::npos);
}

TEST_CASE("stv state: a state is tied to its game by the hash of the short name", "[stv]") {
    CHECK(stv::STVGameHash("bakubaku") == stv::STVGameHash("bakubaku"));
    for (const char *other : {"colmns97", "puyosun", "diehard", "gaxeduel", "cotton2", "grdforce", "rsgun", ""}) {
        CHECK(stv::STVGameHash("bakubaku") != stv::STVGameHash(other));
    }
}

TEST_CASE("stv state: a Saturn session's state is the Saturn payload and nothing else", "[stv]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    Quiet(core);
    for (int i = 0; i < 3; i++) {
        core.RunFrame();
    }

    // Size: the LZ4 bound of the largest Saturn payload and the header, as before ST-V
    CHECK(core.GetStateSize() ==
          static_cast<size_t>(LZ4_compressBound(static_cast<int>(MaxEncodedSaveStateSize(0)))) + 16);

    const std::vector<uint8_t> state = Save(core);
    const std::vector<uint8_t> payload = Payload(state);

    // The payload is EncodeSaveState's bytes of the state it decodes to: no block, no trailer
    auto decoded = std::make_unique<ymir::savestate::SaveState>();
    REQUIRE(DecodeSaveState(payload, *decoded, CoreWrapper::kSaveStateVersion));
    std::vector<uint8_t> encoded;
    REQUIRE(EncodeSaveState(*decoded, encoded));
    CHECK(encoded == payload);
    CHECK(payload.size() == EncodedSaveStateSize(*decoded));
    CHECK_FALSE(stv::HasSTVTrailer(payload));

    // and it loads back
    REQUIRE(core.LoadState(state.data(), state.size()));
    CHECK(core.GetLastStateError().empty());
}

TEST_CASE("stv state: an ST-V state is the Saturn payload, the block and the trailer", "[stv]") {
    Session session("brimir_stv_state_layout", "bakubaku");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);
    for (int i = 0; i < 3; i++) {
        core.RunFrame();
    }

    // the frontend's buffer holds the largest state of this session, the cabinet's block included
    CHECK(core.GetStateSize() == static_cast<size_t>(LZ4_compressBound(
                                     static_cast<int>(MaxEncodedSaveStateSize(0) + stv::STVStateTailSize()))) + 16);

    const std::vector<uint8_t> state = Save(core);
    const std::vector<uint8_t> payload = Payload(state);
    REQUIRE(payload.size() > stv::STVStateTailSize());
    CHECK(stv::HasSTVTrailer(payload));

    const std::span<const uint8_t> saturnPart = std::span<const uint8_t>(payload).first(payload.size() - stv::STVStateTailSize());
    auto decoded = std::make_unique<ymir::savestate::SaveState>();
    REQUIRE(DecodeSaveState(saturnPart, *decoded, CoreWrapper::kSaveStateVersion));
    CHECK(saturnPart.size() == EncodedSaveStateSize(*decoded));
    // the cartridge is the ST-V ROM one (a ROM cartridge type, no image in the state)
    CHECK(decoded->scu.cartType == ymir::savestate::SCUSaveState::CartType::ROM);
    CHECK(decoded->scu.cartData.empty());

    stv::STVStateBlock block;
    REQUIRE(stv::DecodeSTVState(std::span<const uint8_t>(payload).last(stv::STVStateTailSize()), block));
    CHECK(block.gameHash == stv::STVGameHash("bakubaku"));
    CHECK(Encoded(block) == Encoded(Capture(core)));
}

TEST_CASE("stv state: the cabinet's state round trips: in place, in a fresh core and by reuse", "[stv]") {
    Session session("brimir_stv_state_roundtrip", "bakubaku");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);

    // Something in every part of the block: a coin pulse and a button, an EEPROM command half clocked in through the
    // SMPC pins (which also moves the I/O board's clock to the emulated time)
    REQUIRE(core.GetSTVIOBoardForTesting() != nullptr);
    core.SetSTVInput(0, stv::MapSTVInput(kSelect | (1u << RETRO_DEVICE_ID_JOYPAD_B), stv::Controls::ThreeButtons));
    for (int i = 0; i < 7; i++) {
        core.RunFrame();
    }
    ymir::Saturn &saturn = *core.GetSaturn();
    saturn.mainBus.Write<uint8_t>(0x010'0079, 0x1F); // DDR1
    saturn.mainBus.Write<uint8_t>(0x010'007B, 0x00); // DDR2
    const auto clockBit = [&](bool bit) {
        const uint8_t base = static_cast<uint8_t>(0x04 | (bit ? 0x10 : 0));
        for (const uint8_t value : {base, static_cast<uint8_t>(base | 0x08), base}) {
            saturn.mainBus.Write<uint8_t>(0x010'0075, value); // PDR1
        }
    };
    saturn.mainBus.Write<uint8_t>(0x010'0075, 0x00);
    clockBit(true); // the start bit
    clockBit(true); // the opcode's first bit, then an address bit: the command is half in
    clockBit(false);
    clockBit(true);

    const stv::STVStateBlock saved = Capture(core);
    const std::vector<uint8_t> savedBytes = Encoded(saved);
    CHECK(saved.schedulerCount != 0);
    CHECK(saved.io.eepromBits != 0);
    CHECK(saved.io.coinHeld[0] == 1);
    const std::vector<uint8_t> state = Save(core);

    // moved on, then loaded back in the same core (the reuse path: it holds exactly the bytes it saved)
    core.SetSTVInput(0, {});
    for (int i = 0; i < 5; i++) {
        core.RunFrame();
    }
    CHECK(Encoded(Capture(core)) != savedBytes);
    const uint64_t reusedBefore = core.GetReusedStateLoads();
    REQUIRE(core.LoadState(state.data(), state.size()));
    CHECK(core.GetReusedStateLoads() == reusedBefore + 1);
    CHECK(Encoded(Capture(core)) == savedBytes);

    // after another core saved over it (no reuse: the bytes are decompressed and decoded)
    CoreWrapper other;
    REQUIRE(other.Initialize());
    REQUIRE(session.Load(other));
    Quiet(other);
    for (int i = 0; i < 4; i++) {
        other.RunFrame();
    }
    CHECK(Encoded(Capture(other)) != savedBytes);
    REQUIRE(other.LoadState(state.data(), state.size()));
    CHECK(other.GetReusedStateLoads() == 0);
    CHECK(Encoded(Capture(other)) == savedBytes);
    CHECK(other.GetSaturn()->GetSchedulerCount() == core.GetSaturn()->GetSchedulerCount());

    // and the same core, with its own state saved since, through the decoding path
    core.SetSTVInput(0, {});
    core.RunFrame();
    const std::vector<uint8_t> later = Save(core);
    REQUIRE(core.LoadState(state.data(), state.size()));
    CHECK(Encoded(Capture(core)) == savedBytes);
    REQUIRE(core.LoadState(later.data(), later.size()));
    CHECK(Encoded(Capture(core)) != savedBytes);
}

TEST_CASE("stv state: the session decides the layout: Saturn and ST-V states are refused by the other", "[stv]") {
    Session session("brimir_stv_state_sessions", "bakubaku");
    Session puyo("brimir_stv_state_sessions_other", "puyosun");

    CoreWrapper saturn;
    REQUIRE(saturn.Initialize());
    Quiet(saturn);
    saturn.RunFrame();
    const std::vector<uint8_t> saturnState = Save(saturn);

    CoreWrapper baku;
    REQUIRE(baku.Initialize());
    REQUIRE(session.Load(baku));
    Quiet(baku);
    baku.RunFrame();
    const std::vector<uint8_t> bakuState = Save(baku);

    CoreWrapper other;
    REQUIRE(other.Initialize());
    REQUIRE(puyo.Load(other));
    Quiet(other);
    other.RunFrame();
    const std::vector<uint8_t> puyoState = Save(other);

    // each loads into a session of its own kind
    CHECK(saturn.LoadState(saturnState.data(), saturnState.size()));
    CHECK(baku.LoadState(bakuState.data(), bakuState.size()));
    CHECK(other.LoadState(puyoState.data(), puyoState.size()));

    // an ST-V state in a Saturn session
    CHECK_FALSE(saturn.LoadState(bakuState.data(), bakuState.size()));
    CHECK(saturn.GetLastStateError().find("ST-V") != std::string::npos);
    // a Saturn state in an ST-V session
    CHECK_FALSE(baku.LoadState(saturnState.data(), saturnState.size()));
    CHECK(baku.GetLastStateError().find("not an ST-V state") != std::string::npos);
    // the state of another ST-V game
    CHECK_FALSE(baku.LoadState(puyoState.data(), puyoState.size()));
    CHECK(baku.GetLastStateError().find("another ST-V game") != std::string::npos);
    CHECK_FALSE(other.LoadState(bakuState.data(), bakuState.size()));
    CHECK(other.GetLastStateError().find("another ST-V game") != std::string::npos);

    // ST-V content ended: the core is a Saturn again and refuses the ST-V state; its own Saturn state loads
    baku.UnloadGame();
    CHECK_FALSE(baku.LoadState(bakuState.data(), bakuState.size()));
    CHECK(baku.LoadState(saturnState.data(), saturnState.size()));
}

TEST_CASE("stv state: a damaged ST-V state is refused and changes nothing", "[stv]") {
    Session session("brimir_stv_state_damaged", "bakubaku");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);
    core.RunFrame();
    const std::vector<uint8_t> state = Save(core);
    const std::vector<uint8_t> payload = Payload(state);
    REQUIRE(core.LoadState(state.data(), state.size()));

    // a different state to be loaded: the core ran on from the one in `state`
    for (int i = 0; i < 4; i++) {
        core.RunFrame();
    }
    const uint64_t count = core.GetSaturn()->GetSchedulerCount();
    const std::vector<uint8_t> blockBefore = Encoded(Capture(core));

    const size_t blockAt = payload.size() - stv::STVStateTailSize();
    const auto refused = [&](const std::function<void(std::vector<uint8_t> &)> &damage, uint32_t version = 12) {
        std::vector<uint8_t> damaged = payload;
        damage(damaged);
        const std::vector<uint8_t> bytes = Frame(damaged, version);
        const bool ok = core.LoadState(bytes.data(), bytes.size());
        CHECK(core.GetSaturn()->GetSchedulerCount() == count);
        CHECK(Encoded(Capture(core)) == blockBefore);
        return !ok;
    };

    // offsets in the block: 4 magic, 4 version, 8 game hash, then the I/O board
    CHECK(refused([](auto &) {}, 11));                                               // an older format version
    CHECK(refused([](auto &p) { p.pop_back(); }));                                   // no trailer
    CHECK(refused([](auto &p) { p.erase(p.begin() + 100); }));                       // a byte missing
    CHECK(refused([&](auto &p) { p[blockAt] ^= 1; }));                               // magic
    CHECK(refused([&](auto &p) { p[blockAt + 4] = 9; }));                            // version
    CHECK(refused([&](auto &p) { p[blockAt + 8] ^= 1; }));                           // game hash
    CHECK(refused([&](auto &p) { p[blockAt + 16 + offsetof(stv::STVIOBoardState, eepromState)] = 99; })); // the board
    CHECK(refused([&](auto &p) {
        // the chip's flags: protectionEnabled 2 is not a state the cartridge can be in
        const size_t at = blockAt + 16 + sizeof(stv::STVIOBoardState) +
                          offsetof(ymir::cart::STVGameROMCartridge::ProtectionState, protectionEnabled);
        p[at] = 2;
    }));
    CHECK(refused([&](auto &p) {
        // a time base remainder of 2.0 nanoseconds
        const size_t at = blockAt + 16 + sizeof(stv::STVIOBoardState) +
                          sizeof(ymir::cart::STVGameROMCartridge::ProtectionState) + 8;
        const uint64_t bits = 0x4000000000000000ull;
        std::memcpy(p.data() + at, &bits, 8);
    }));

    CHECK(refused([&](auto &p) {
        // a 315-5838 state that is not the reset value, for a cartridge without the chip
        const size_t at = blockAt + stv::STVStateBlockSize() - 4 - sizeof(ymir::cart::STV3155838::State) +
                          offsetof(ymir::cart::STV3155838::State, active);
        p[at] = 1;
    }));

    // the undamaged state still loads
    REQUIRE(core.LoadState(state.data(), state.size()));
}

TEST_CASE("stv state: a Brimir 0.10.0 state (block version 1) loads for a plain and a 315-5881 game", "[stv]") {
    for (const char *game : {"bakubaku", "rsgun"}) {
        INFO(game);
        Session session(game == std::string("rsgun") ? "brimir_stv_state_v1_rsgun" : "brimir_stv_state_v1_plain",
                        game);
        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(session.Load(core));
        Quiet(core);
        core.SetSTVInput(0, stv::MapSTVInput(kSelect, stv::Controls::ThreeButtons));
        for (int i = 0; i < 4; i++) {
            core.RunFrame();
        }
        const stv::STVStateBlock saved = Capture(core);
        const uint64_t count = core.GetSaturn()->GetSchedulerCount();
        const std::vector<uint8_t> payload = Payload(Save(core));

        // the same payload with the block as 0.10.0 wrote it
        std::vector<uint8_t> old(payload.begin(), payload.end() - static_cast<std::ptrdiff_t>(stv::STVStateTailSize()));
        const std::vector<uint8_t> v1 = EncodedVersion1(saved);
        old.insert(old.end(), v1.begin(), v1.end());
        const std::vector<uint8_t> state = Frame(old);

        core.SetSTVInput(0, {});
        for (int i = 0; i < 3; i++) {
            core.RunFrame();
        }
        REQUIRE(core.LoadState(state.data(), state.size()));
        CHECK(core.GetLastStateError().empty());
        CHECK(Encoded(Capture(core)) == Encoded(saved));
        CHECK(core.GetSaturn()->GetSchedulerCount() == count);

        // and in a fresh core
        CoreWrapper fresh;
        REQUIRE(fresh.Initialize());
        REQUIRE(session.Load(fresh));
        Quiet(fresh);
        fresh.RunFrame();
        REQUIRE(fresh.LoadState(state.data(), state.size()));
        CHECK(Encoded(Capture(fresh)) == Encoded(saved));

        // a block of a version this core does not know is refused, naming it
        std::vector<uint8_t> future = payload;
        future[future.size() - stv::STVStateTailSize() + 4] = 3;
        const std::vector<uint8_t> futureState = Frame(future);
        CHECK_FALSE(fresh.LoadState(futureState.data(), futureState.size()));
        CHECK(fresh.GetLastStateError().find("version 3") != std::string::npos);
    }
}

TEST_CASE("stv state: loading a state the cartridge slot keeps the ST-V cartridge", "[stv]") {
    // SCU::LoadState with CartType::ROM would insert a new plain ROM cartridge, reading 4 MiB from the state's (empty)
    // image: the bridge saves the ST-V cartridge as a ROM one with no image and loads it as no cartridge
    Session session("brimir_stv_state_cart", "bakubaku");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);
    core.RunFrame();

    ymir::Saturn &saturn = *core.GetSaturn();
    ymir::cart::BaseCartridge *cart = &saturn.GetCartridge();
    REQUIRE(cart->GetType() == ymir::cart::CartType::ROM);
    REQUIRE(cart->OwnsCartridgeSpace());

    // Saturn::SaveState with the ST-V cartridge: a ROM cartridge with no image
    auto raw = std::make_unique<ymir::savestate::SaveState>();
    saturn.SaveState(*raw);
    CHECK(raw->scu.cartType == ymir::savestate::SCUSaveState::CartType::ROM);
    CHECK(raw->scu.cartData.empty());

    const std::vector<uint8_t> state = Save(core);
    core.RunFrame();
    REQUIRE(core.LoadState(state.data(), state.size()));
    CHECK(&saturn.GetCartridge() == cart);
    CHECK(saturn.GetCartridge().OwnsCartridgeSpace());
}

TEST_CASE("stv state: the synthetic session's BIOS idles the master SH-2", "[stv]") {
    // A BIOS of zeros is not idle: opcode 0x0000 is illegal, and each exception pushes SR and PC through SP 0 into the
    // on-chip registers at the top of the address space, which logs every write (about 1 GB of output per 600 frames)
    Session session("brimir_stv_state_idle", "bakubaku");
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);
    for (int i = 0; i < 3; i++) {
        core.RunFrame();
    }
    auto &master = core.GetSaturn()->masterSH2.GetProbe();
    const uint32_t pc = master.PC() & 0x07FF'FFFF;
    CHECK(pc >= test::kIdleSTVBiosLoop);
    CHECK(pc < test::kIdleSTVBiosLoop + 4);
    CHECK(master.R(15) == 0x0610'0000);
}

TEST_CASE("stv state: a held coin is one coin across a state load", "[stv]") {
    Session session("brimir_stv_state_coin", "bakubaku");

    // Frames of Select held, with the coin input (PORTC bit 0, active low) read after each; the edges after the load
    const auto edgesAfterLoad = [&](CoreWrapper &loader, const std::vector<uint8_t> &state, int framesAfter) {
        REQUIRE(loader.LoadState(state.data(), state.size()));
        unsigned edges = 0;
        bool wasActive = (PortC(loader) & 0x01) == 0;
        for (int i = 0; i < framesAfter; i++) {
            loader.SetSTVInput(0, stv::MapSTVInput(kSelect, stv::Controls::ThreeButtons));
            loader.RunFrame();
            const bool active = (PortC(loader) & 0x01) == 0;
            edges += active && !wasActive ? 1 : 0;
            wasActive = active;
        }
        return edges;
    };

    // one frame in (the pulse has just started), and well after the pulse has ended with the button still down
    for (const int heldFrames : {1, 3, 20, 40}) {
        INFO("saved after " << heldFrames << " frames of Select held");
        CoreWrapper core;
        REQUIRE(core.Initialize());
        REQUIRE(session.Load(core));
        Quiet(core);
        core.RunFrame(); // the first frame
        unsigned edgesBefore = 0;
        bool wasActive = (PortC(core) & 0x01) == 0;
        for (int i = 0; i < heldFrames; i++) {
            core.SetSTVInput(0, stv::MapSTVInput(kSelect, stv::Controls::ThreeButtons));
            core.RunFrame();
            const bool active = (PortC(core) & 0x01) == 0;
            edgesBefore += active && !wasActive ? 1 : 0;
            wasActive = active;
        }
        CHECK(edgesBefore == 1);
        const std::vector<uint8_t> state = Save(core);

        // the same core, loading its own state after the button was held on
        for (int i = 0; i < 5; i++) {
            core.SetSTVInput(0, stv::MapSTVInput(kSelect, stv::Controls::ThreeButtons));
            core.RunFrame();
        }
        CHECK(edgesAfterLoad(core, state, 60) == 0);

        // a new core, which has never seen the button pressed
        CoreWrapper fresh;
        REQUIRE(fresh.Initialize());
        REQUIRE(session.Load(fresh));
        Quiet(fresh);
        fresh.RunFrame();
        CHECK(edgesAfterLoad(fresh, state, 60) == 0);

        // released and pressed again: a new coin
        fresh.SetSTVInput(0, {});
        for (int i = 0; i < 20; i++) {
            fresh.RunFrame();
        }
        wasActive = (PortC(fresh) & 0x01) == 0;
        unsigned again = 0;
        for (int i = 0; i < 30; i++) {
            fresh.SetSTVInput(0, stv::MapSTVInput(kSelect, stv::Controls::ThreeButtons));
            fresh.RunFrame();
            const bool active = (PortC(fresh) & 0x01) == 0;
            again += active && !wasActive ? 1 : 0;
            wasActive = active;
        }
        CHECK(again == 1);
    }
}

TEST_CASE("stv state: loading a state replaces the .srm not yet copied down, and the buffer follows", "[stv]") {
    Session session("brimir_stv_state_srm", "bakubaku");

    // A state with an EEPROM cell the game wrote and a backup RAM record
    std::vector<uint8_t> state;
    std::vector<uint8_t> expected;
    {
        CoreWrapper source;
        REQUIRE(source.Initialize());
        REQUIRE(session.Load(source));
        Quiet(source);
        source.RunFrame();
        source.GetSaturn()->mem.GetInternalBackupRAM().WriteByte(2 * 4321, 0x6B);
        std::array<uint8_t, stv::kSTVEEPROMSize> cells;
        cells.fill(0xFF);
        cells[10] = 0xA5;
        cells[11] = 0x5A;
        source.GetSTVIOBoardForTesting()->LoadEEPROM(cells);
        state = Save(source);
        const auto *image = static_cast<const uint8_t *>(source.GetSRAMData());
        expected.assign(image, image + source.GetSRAMSize());
        REQUIRE(expected.size() == kTotal);
        REQUIRE(expected[4321] == 0x6B);
        REQUIRE(expected[kBackup + 10] == 0xA5);
    }

    // The session the state is loaded into: the frontend has put its .srm in the buffer (before the first frame)
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core));
    Quiet(core);
    uint8_t *buffer = static_cast<uint8_t *>(core.GetSRAMData());
    REQUIRE(buffer != nullptr);
    std::vector<uint8_t> srm(kTotal, 0x11);
    std::memcpy(buffer, srm.data(), srm.size());

    REQUIRE(core.LoadState(state.data(), state.size()));
    // the frontend's buffer (same address) holds the state's save RAM, not its own file
    CHECK(core.GetSRAMData() == buffer);
    CHECK(std::vector<uint8_t>(buffer, buffer + kTotal) == expected);

    core.RunFrame(); // the first frame must not copy the .srm down over the loaded state
    CHECK(core.GetSaturn()->mem.GetInternalBackupRAM().ReadAll()[4321] == 0x6B);
    const auto &cells = core.GetSTVIOBoardForTesting()->GetEEPROM();
    CHECK(cells[10] == 0xA5);
    CHECK(cells[11] == 0x5A);
    CHECK(std::vector<uint8_t>(buffer, buffer + kTotal) == expected);
}

TEST_CASE("stv state: lockstep cores load the cabinet's block and keep their ST-V cartridge", "[stv]") {
    Session session("brimir_stv_state_lockstep", "bakubaku");
    CoreWrapper a;
    CoreWrapper b;
    for (CoreWrapper *core : {&a, &b}) {
        REQUIRE(core->Initialize());
        REQUIRE(session.Load(*core));
        PrepareLockstepCore(*core);
    }
    for (int i = 0; i < 3; i++) {
        a.RunFrame();
    }
    a.SetSTVInput(0, stv::MapSTVInput(kSelect, stv::Controls::ThreeButtons));
    for (int i = 0; i < 3; i++) {
        a.RunFrame();
    }
    REQUIRE(Encoded(Capture(a)) != Encoded(Capture(b)));

    ymir::cart::BaseCartridge *cartB = &b.GetSaturn()->GetCartridge();
    REQUIRE(SyncLockstepCores(a, b));
    CHECK(Encoded(Capture(a)) == Encoded(Capture(b)));
    CHECK(&b.GetSaturn()->GetCartridge() == cartB);
    CHECK(b.GetSaturn()->GetCartridge().OwnsCartridgeSpace());
    a.RunFrame();
    b.RunFrame();
    CHECK(CompareCores(a, b).empty());

    // a state and the block, loaded into both after they ran on
    auto raw = std::make_unique<ymir::savestate::SaveState>();
    a.GetSaturn()->SaveState(*raw);
    const stv::STVStateBlock block = Capture(a);
    for (int i = 0; i < 5; i++) {
        a.RunFrame();
        b.RunFrame();
    }
    CHECK(CompareCores(a, b).empty());
    REQUIRE(LoadLockstepState(a, b, *raw, &block));
    CHECK(Encoded(Capture(a)) == Encoded(block));
    CHECK(Encoded(Capture(b)) == Encoded(block));
    CHECK(CompareCores(a, b).empty());
    CHECK(b.GetSaturn()->GetCartridge().OwnsCartridgeSpace());

    // the comparison sees a difference in the cabinet
    stv::STVStateBlock changed = block;
    changed.io.eepromData[3] ^= 0x55;
    REQUIRE(b.RestoreSTVState(changed));
    CHECK(CompareCores(a, b).find("ST-V") != std::string::npos);
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

// A scratch folder with the BIOS files, and a save folder of its own per core
struct RomSession {
    TempDir scratch;
    std::filesystem::path zip;

    RomSession(const char *name, const std::filesystem::path &biosDir, const std::filesystem::path &romZip)
        : scratch(name), zip(romZip) {
        CopyBiosFiles(biosDir, scratch.path);
    }
    bool Load(CoreWrapper &core, const char *saves) const {
        const auto dir = scratch.path / saves;
        std::filesystem::create_directories(dir);
        return core.LoadGame(Str(zip).c_str(), Str(dir).c_str(), Str(scratch.path).c_str());
    }
};

} // namespace

TEST_CASE("stv state: bakubaku continues identically after a state load", "[.stv-rom][bakubaku]") {
    const auto biosDir = EnvDir("BRIMIR_STV_BIOS_DIR");
    const auto romDir = EnvDir("BRIMIR_STV_ROM_DIR");
    if (biosDir.empty() || romDir.empty() || !std::filesystem::exists(romDir / "bakubaku.zip")) {
        SKIP("BRIMIR_STV_BIOS_DIR and BRIMIR_STV_ROM_DIR with bakubaku.zip are needed");
    }
    RomSession session("brimir_stv_state_bakubaku", biosDir, romDir / "bakubaku.zip");

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core, "saves_a"));
    Quiet(core);
    RunHashed(core, 2000); // drains the audio every frame, as a frontend does
    const std::vector<uint8_t> state = Save(core);
    CHECK(state.size() < core.GetStateSize());
    const std::vector<uint64_t> expected = RunHashed(core, 600);

    // the same core (the saved bytes are reused)
    REQUIRE(core.LoadState(state.data(), state.size()));
    CHECK(RunHashed(core, 600) == expected);

    // a fresh core
    CoreWrapper fresh;
    REQUIRE(fresh.Initialize());
    REQUIRE(session.Load(fresh, "saves_b"));
    Quiet(fresh);
    REQUIRE(fresh.LoadState(state.data(), state.size()));
    CHECK(fresh.GetLastStateError().empty());
    CHECK(RunHashed(fresh, 600) == expected);

    // The frontend's buffer, loaded from the .srm before the first frame, is replaced by the state's save RAM
    CoreWrapper late;
    REQUIRE(late.Initialize());
    REQUIRE(session.Load(late, "saves_c"));
    Quiet(late);
    std::memset(late.GetSRAMData(), 0x22, late.GetSRAMSize());
    REQUIRE(late.LoadState(state.data(), state.size()));
    late.RunFrame();
    CHECK(static_cast<const uint8_t *>(late.GetSRAMData())[kBackup + 1] != 0x22); // an EEPROM byte
    CHECK(late.GetSTVIOBoardForTesting()->GetEEPROM()[0] == 'S'); // the BIOS wrote its header ("SEGA", big-endian cells)

    // refused by a Saturn session
    CoreWrapper saturn;
    REQUIRE(saturn.Initialize());
    CHECK_FALSE(saturn.LoadState(state.data(), state.size()));

    // and by a session running another ST-V game
    if (std::filesystem::exists(romDir / "puyosun.zip")) {
        RomSession other("brimir_stv_state_puyosun", biosDir, romDir / "puyosun.zip");
        CoreWrapper puyo;
        REQUIRE(puyo.Initialize());
        REQUIRE(other.Load(puyo, "saves"));
        Quiet(puyo);
        CHECK_FALSE(puyo.LoadState(state.data(), state.size()));
        CHECK(puyo.GetLastStateError().find("another ST-V game") != std::string::npos);
    }
}

TEST_CASE("stv state: a coin held across a state load is one credit in bakubaku", "[.stv-rom][bakubaku]") {
    const auto biosDir = EnvDir("BRIMIR_STV_BIOS_DIR");
    const auto romDir = EnvDir("BRIMIR_STV_ROM_DIR");
    if (biosDir.empty() || romDir.empty() || !std::filesystem::exists(romDir / "bakubaku.zip")) {
        SKIP("BRIMIR_STV_BIOS_DIR and BRIMIR_STV_ROM_DIR with bakubaku.zip are needed");
    }
    RomSession session("brimir_stv_state_credit", biosDir, romDir / "bakubaku.zip");

    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(session.Load(core, "saves"));
    Quiet(core);
    ymir::Saturn &saturn = *core.GetSaturn();
    constexpr uint32_t kCredits = 0x0600'0740; // the credit count in the BIOS's work RAM (see test_stv_load.cpp)
    const auto credits = [&] { return saturn.mainBus.Peek<uint8_t>(kCredits); };
    const auto hold = [&](int frames) {
        for (int i = 0; i < frames; i++) {
            core.SetSTVInput(0, stv::MapSTVInput(kSelect, stv::Controls::ThreeButtons));
            core.RunFrame();
        }
    };

    for (int i = 0; i < 1500; i++) {
        core.RunFrame();
    }
    REQUIRE(credits() == 0);
    hold(3);
    const std::vector<uint8_t> state = Save(core);
    hold(10);
    REQUIRE(core.LoadState(state.data(), state.size()));
    hold(60); // the button stays down
    core.SetSTVInput(0, {});
    for (int i = 0; i < 60; i++) {
        core.RunFrame();
    }
    CHECK(credits() == 1);
}
