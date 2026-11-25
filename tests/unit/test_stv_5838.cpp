// The 315-5838 compression chip (Decathlete): the cipher, decompression against vectors from the reference core, the
// register window in the cartridge, and the chip's state. No ROMs: the source data is a generated pattern.
//
// The decompression vectors were produced by libretro-stv's Chip5838 (the Mednafen-based core that runs Decathlete)
// fed with the same generated ROM and tables as below, in a local harness outside this repository; the record of how
// is in .superpowers/sdd/2026-11-24-stv-edge-cases/task-6-report.md.

#include "catch_amalgamated.hpp"

#include <stv/stv_315_5838.hpp>
#include <stv/stv_cartridge.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <vector>

using ymir::cart::STV3155838;
using ymir::cart::STVGameROMCartridge;

namespace {

// 32 MiB of big-endian words from a xorshift32 (seed 0x5838), the generator the reference harness used
const std::vector<uint8_t> &PatternROM() {
    static const std::vector<uint8_t> rom = [] {
        std::vector<uint8_t> bytes(32u << 20);
        uint32_t x = 0x5838u;
        for (size_t i = 0; i < bytes.size(); i += 2) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            const uint16_t word = static_cast<uint16_t>(x >> 8);
            bytes[i] = static_cast<uint8_t>(word >> 8);
            bytes[i + 1] = static_cast<uint8_t>(word);
        }
        return bytes;
    }();
    return rom;
}

// A canonical code of lengths 2, 3, 4, 6, 8, 10 and 12 (the last entry repeated, as Decathlete's tables do), as
// (len << 8 | idx, pattern) pairs
constexpr uint16_t kTree[12][2] = {{0x0200, 0x0000}, {0x0301, 0x0400}, {0x0403, 0x0800}, {0x0607, 0x0C00},
                                   {0x080F, 0x0E00}, {0x0A27, 0x0F80}, {0x0C3F, 0x0FE0}, {0x0C3F, 0x0FE0},
                                   {0x0C3F, 0x0FE0}, {0x0C3F, 0x0FE0}, {0x0C3F, 0x0FE0}, {0x0C3F, 0x0FE0}};

uint16_t DictionaryWord(int i, int salt) {
    const auto a = static_cast<uint8_t>(i * 37 + salt);
    const auto b = static_cast<uint8_t>((i + 1) * 37 + salt);
    return static_cast<uint16_t>(a << 8 | b);
}

void UploadTables(STV3155838 &chip, uint16_t treeMode, uint16_t dictMode, int salt) {
    chip.SetTableUploadMode(treeMode);
    for (const auto &node : kTree) {
        chip.UploadTableData(node[0]);
        chip.UploadTableData(node[1]);
    }
    chip.SetTableUploadMode(dictMode);
    for (int i = 0; i < 256; i += 2) {
        chip.UploadTableData(DictionaryWord(i, salt));
    }
}

struct Vector {
    uint16_t treeMode, dictMode;
    int salt;
    uint8_t bank;
    uint32_t source;
    std::vector<uint16_t> words;
    uint32_t sourceAfter;
};

// The reference core's output
const std::vector<Vector> &Vectors() {
    static const std::vector<Vector> vectors = {
        {0x8000, 0x8080, 11, 2, 0x012345,
         {0x30C4, 0x0B9F, 0x0B0B, 0xC40B, 0xA29F, 0x55A8, 0xA255, 0xE9E9, 0x0B30, 0x5E0B, 0x2FE9, 0x64C4, 0xD9E9,
          0x300B, 0x30C4, 0x55E9, 0x110B, 0x7A0B, 0x301A, 0xA255, 0x550B, 0x0BEC, 0xEC55, 0x3030, 0x55C4, 0xF50B,
          0x30EC, 0x5E0B, 0x7A55, 0x9F55, 0xE955, 0x0B0B, 0xEF0B, 0x0B30, 0xE955, 0x0B0B, 0xA59F, 0xA29F, 0x0BAB,
          0x0B0B, 0x559F, 0xE961, 0x7A0B, 0xC40E, 0x587A, 0xD05B, 0x0E0B, 0xC430, 0x3055, 0x3055, 0x0B30, 0x300B,
          0xF530, 0x0B0E, 0x557A, 0xC70B, 0x0B55, 0x0B7D, 0xC455, 0x7A7A, 0x7AE9, 0x30EC, 0x307A, 0xC47A},
         0x012366},
        {0x0000, 0x0080, 101, 1, 0x2ABCDE,
         {0xAFB2, 0x43AF, 0x659C, 0xF98A, 0x6B65, 0x028A, 0xF943, 0x4FDD, 0x8A65, 0xD465, 0x998A,
          0xAF8A, 0xAF65, 0x938A, 0x8AB2, 0x658A, 0x6565, 0x6565, 0x65C4, 0x6865, 0x1EAF, 0x1E65,
          0x651E, 0xBE65, 0x4343, 0x8D1E, 0x65F9, 0x9965, 0xF965, 0x6565, 0x65C1, 0x93AF},
         0x2ABCEF},
        {0x8000, 0x8080, 11, 0, 0x000100,
         {0x30E9, 0x0B9F, 0x307A, 0x30E9, 0xC755, 0xC455, 0x3055, 0x3055, 0xF53F, 0xE90B, 0x559F,
          0x5511, 0x5530, 0x557A, 0x339F, 0xC70E, 0x0B0B, 0x0B7A, 0x300B, 0x550B, 0x300B, 0xA50B,
          0x0B55, 0x0B0B, 0xE97A, 0x550B, 0x0BEC, 0x307D, 0x5530, 0x55C4, 0x7DD0, 0x0BEC},
         0x00010F},
    };
    return vectors;
}

STV3155838 PatternChip() {
    STV3155838 chip;
    chip.SetROM(PatternROM());
    chip.Reset();
    return chip;
}

std::vector<uint8_t> Bytes(const STV3155838::State &state) {
    std::vector<uint8_t> bytes(sizeof state);
    std::memcpy(bytes.data(), &state, sizeof state);
    return bytes;
}

std::vector<uint8_t> Saved(const STV3155838 &chip) {
    STV3155838::State state;
    chip.SaveState(state);
    return Bytes(state);
}

} // namespace

TEST_CASE("stv 5838: the cipher gives the value validated on real hardware", "[stv]") {
    // decipher(0x1533) == 0x16be: libretro-stv's chip315_5838.h ("Validated: decipher(0x1533) = 0x16be (confirmed on
    // real hardware)"); MAME's 315-5838_317-0229_comp.cpp does not state it
    CHECK(STV3155838::Decipher(0x1533) == 0x16BE);
    // and the reference core's values for a few more words
    CHECK(STV3155838::Decipher(0x0000) == 0xCF8E);
    CHECK(STV3155838::Decipher(0xFFFF) == 0x2989);
    CHECK(STV3155838::Decipher(0x1234) == 0xDE5E);
    CHECK(STV3155838::Decipher(0x8001) == 0x9D8E);
    CHECK(STV3155838::Decipher(0xA5A5) == 0x0258);
}

TEST_CASE("stv 5838: a table upload and a decompression reproduce the reference core's output", "[stv]") {
    STV3155838 chip = PatternChip();
    for (const Vector &v : Vectors()) {
        INFO("bank " << int(v.bank) << " source " << v.source);
        UploadTables(chip, v.treeMode, v.dictMode, v.salt);
        chip.SetBank(v.bank);
        chip.SetSourceAddress(v.source, 0xFFFFFFFFu);
        std::vector<uint16_t> words;
        for (size_t i = 0; i < v.words.size(); i++) {
            words.push_back(chip.ReadData());
        }
        CHECK(words == v.words);
        STV3155838::State state;
        chip.SaveState(state);
        CHECK(state.srcOffset == v.sourceAfter);
    }
}

TEST_CASE("stv 5838: the source address takes the written lanes and restarts the decoder", "[stv]") {
    STV3155838 chip = PatternChip();
    const Vector &v = Vectors()[0];
    UploadTables(chip, v.treeMode, v.dictMode, v.salt);
    chip.SetBank(v.bank);
    // two word writes (MAME's COMBINE_DATA): the high half, then the low half
    chip.SetSourceAddress(0x7FFF0000u, 0xFFFF0000u);
    chip.ReadData();
    chip.SetSourceAddress(v.source & 0xFFFFu, 0x0000FFFFu);
    chip.SetSourceAddress((v.source >> 16) << 16, 0xFFFF0000u);
    std::vector<uint16_t> words;
    for (int i = 0; i < 4; i++) {
        words.push_back(chip.ReadData());
    }
    CHECK(words == std::vector<uint16_t>(v.words.begin(), v.words.begin() + 4));
    // the address is a 23-bit word offset
    chip.SetSourceAddress(0xFF812345u, 0xFFFFFFFFu);
    STV3155838::State state;
    chip.SaveState(state);
    CHECK(state.srcOffset == 0x012345u);
    CHECK(state.srcStart == 0x012345u);
    CHECK(state.active == 1);
}

TEST_CASE("stv 5838: a decompression that wraps to where it started gives 0xFF bytes", "[stv]") {
    // MAME's failsafe: a transfer that has read the whole 8M-word window aborts. A tree that never matches (all
    // lengths 0) reads on until it does.
    STV3155838 chip = PatternChip();
    chip.SetTableUploadMode(0x8000);
    for (int i = 0; i < 24; i++) {
        chip.UploadTableData(0);
    }
    chip.SetBank(0);
    chip.SetSourceAddress(0x000010, 0xFFFFFFFFu);
    CHECK(chip.ReadData() == 0xFFFF);
    STV3155838::State state;
    chip.SaveState(state);
    CHECK(state.abort == 1);
    CHECK(state.srcOffset == 0x000010u);
    CHECK(chip.ReadData() == 0xFFFF);
    // a new source address ends the abort
    UploadTables(chip, 0x8000, 0x8080, 11);
    chip.SetSourceAddress(0x000100, 0xFFFFFFFFu);
    CHECK(chip.ReadData() == Vectors()[2].words[0]);
}

TEST_CASE("stv 5838: uploads past the tables are ignored", "[stv]") {
    // MAME asserts the indices stay inside the tables; here the extra words are dropped
    STV3155838 chip = PatternChip();
    UploadTables(chip, 0x8000, 0x8080, 11);
    const std::vector<uint8_t> before = Saved(chip);
    chip.SetTableUploadMode(0x8080);
    for (int i = 0; i < 128; i++) {
        chip.UploadTableData(DictionaryWord(i * 2, 11));
    }
    chip.UploadTableData(0x1234); // a 129th dictionary word
    STV3155838::State state;
    chip.SaveState(state);
    CHECK(state.dictIndex == 256);
    chip.SetTableUploadMode(0x8000);
    for (const auto &node : kTree) {
        chip.UploadTableData(node[0]);
        chip.UploadTableData(node[1]);
    }
    chip.UploadTableData(0x0C00); // a 13th tree entry would overwrite the sentinel
    chip.SaveState(state);
    CHECK(state.treeIndex == 24);
    CHECK(state.tree[12].len == 0);
    CHECK(state.tree[12].pattern == 0);
    // the tables are as before (the modes and indices aside)
    STV3155838::State first;
    std::memcpy(&first, before.data(), sizeof first);
    CHECK(std::memcmp(&first.tree, &state.tree, sizeof state.tree) == 0);
    CHECK(std::memcmp(&first.dictionary, &state.dictionary, sizeof state.dictionary) == 0);
}

TEST_CASE("stv 5838: a state saved in the middle of a block continues identically", "[stv]") {
    const Vector &v = Vectors()[0];
    STV3155838 chip = PatternChip();
    UploadTables(chip, v.treeMode, v.dictMode, v.salt);
    chip.SetBank(v.bank);
    chip.SetSourceAddress(v.source, 0xFFFFFFFFu);
    for (int i = 0; i < 9; i++) {
        chip.ReadData();
    }
    STV3155838::State mid;
    chip.SaveState(mid);
    CHECK(mid.numBitsCompressed != 0); // between source words: the decoder holds part of one
    REQUIRE(chip.ValidateState(mid));

    std::vector<uint16_t> rest;
    for (size_t i = 9; i < v.words.size(); i++) {
        rest.push_back(chip.ReadData());
    }
    CHECK(rest == std::vector<uint16_t>(v.words.begin() + 9, v.words.end()));

    // the same chip after it ran on, and a fresh one
    REQUIRE(chip.LoadState(mid));
    CHECK(Saved(chip) == Bytes(mid));
    STV3155838 fresh = PatternChip();
    REQUIRE(fresh.LoadState(mid));
    for (STV3155838 *c : {&chip, &fresh}) {
        std::vector<uint16_t> again;
        for (size_t i = 9; i < v.words.size(); i++) {
            again.push_back(c->ReadData());
        }
        CHECK(again == rest);
    }
}

TEST_CASE("stv 5838: a damaged state is refused and changes nothing", "[stv]") {
    const Vector &v = Vectors()[0];
    STV3155838 chip = PatternChip();
    UploadTables(chip, v.treeMode, v.dictMode, v.salt);
    chip.SetBank(v.bank);
    chip.SetSourceAddress(v.source, 0xFFFFFFFFu);
    chip.ReadData();
    STV3155838::State good;
    chip.SaveState(good);
    REQUIRE(chip.ValidateState(good));
    REQUIRE(chip.ValidateState(STV3155838::State{})); // the reset value

    STV3155838 target = PatternChip();
    const std::vector<uint8_t> before = Saved(target);
    const auto refused = [&](const std::function<void(STV3155838::State &)> &damage) {
        STV3155838::State state = good;
        damage(state);
        const bool valid = target.ValidateState(state);
        const bool loaded = target.LoadState(state);
        CHECK(valid == loaded);
        if (!loaded) {
            CHECK(Saved(target) == before);
        }
        return !loaded;
    };
    CHECK(refused([](auto &s) { s.srcOffset = 0x800000; }));
    CHECK(refused([](auto &s) { s.srcStart = 0x800000; }));
    CHECK(refused([](auto &s) { s.numBitsCompressed = 17; }));
    CHECK(refused([](auto &s) { s.numBitsCompressed = -1; }));
    CHECK(refused([](auto &s) { s.numBits = 1; }));      // between reads the decoder has no code half read
    CHECK(refused([](auto &s) { s.val = 1; }));
    CHECK(refused([](auto &s) { s.treeIndex = 25; }));
    CHECK(refused([](auto &s) { s.dictIndex = 3; }));
    CHECK(refused([](auto &s) { s.dictIndex = 258; }));
    CHECK(refused([](auto &s) { s.abort = 2; }));
    CHECK(refused([](auto &s) { s.bank = 3; }));
    CHECK(refused([](auto &s) { s.active = 2; }));
    CHECK(refused([](auto &s) { s.reserved[1] = 1; }));
    CHECK(refused([](auto &s) { s.tree[12].len = 12; })); // the sentinel is never written
    CHECK(refused([](auto &s) {
        s.abort = 1;
        s.numBits = 0x7FFFFFFF;
    }));
    CHECK_FALSE(refused([](auto &s) {
        s.abort = 1; // an aborted transfer stops wherever it was
        s.numBits = 1000;
        s.val = 0x1234;
    }));
}

namespace {

constexpr uint32_t kBank2Window = 0x037FFFF0u;

struct Cart {
    STVGameROMCartridge cart;
    Cart() {
        cart.LoadROM(PatternROM());
        cart.ConfigureProtection(STVGameROMCartridge::ProtectionMode::Comp3155838, 0);
    }
    void Upload(uint32_t window, uint16_t treeMode, uint16_t dictMode, int salt) {
        cart.WriteWord(window + 4, treeMode);
        for (const auto &node : kTree) {
            cart.WriteWord(window + 6, node[0]);
            cart.WriteWord(window + 6, node[1]);
        }
        cart.WriteWord(window + 4, dictMode);
        for (int i = 0; i < 256; i += 2) {
            cart.WriteWord(window + 6, DictionaryWord(i, salt));
        }
    }
    uint32_t ROMLong(uint32_t address) const {
        const auto &rom = PatternROM();
        const size_t at = address - 0x02000000u;
        return static_cast<uint32_t>(rom[at]) << 24 | static_cast<uint32_t>(rom[at + 1]) << 16 |
               static_cast<uint32_t>(rom[at + 2]) << 8 | rom[at + 3];
    }
};

} // namespace

TEST_CASE("stv 5838: the cartridge's register window decompresses as the reference core does", "[stv]") {
    const Vector &v = Vectors()[0]; // bank 2: the window at 0x037FFFF0, as Decathlete uses it
    Cart c;
    c.Upload(kBank2Window, v.treeMode, v.dictMode, v.salt);
    c.cart.WriteLong(kBank2Window, v.source); // the source address (a long write: both halves)
    std::vector<uint16_t> words;
    for (size_t i = 0; i < v.words.size(); i += 2) {
        const uint32_t data = c.cart.ReadLong(kBank2Window + 8);
        words.push_back(static_cast<uint16_t>(data >> 16));
        words.push_back(static_cast<uint16_t>(data));
    }
    CHECK(words == v.words);
}

TEST_CASE("stv 5838: the cartridge reads the chip only once a source address was written", "[stv]") {
    // libretro-stv's `active` gate: before the game starts the chip, these addresses read the ROM
    Cart c;
    for (const uint32_t at : {0x027FFFF8u, 0x02FFFFF8u, 0x037FFFF8u}) {
        CHECK(c.cart.ReadLong(at) == c.ROMLong(at));
    }
    const Vector &v = Vectors()[2];
    c.Upload(0x027FFFF0u, v.treeMode, v.dictMode, v.salt);
    CHECK(c.cart.ReadLong(0x027FFFF8u) == c.ROMLong(0x027FFFF8u)); // tables alone do not start it
    c.cart.WriteWord(0x027FFFF0u, static_cast<uint16_t>(v.source >> 16));
    c.cart.WriteWord(0x027FFFF2u, static_cast<uint16_t>(v.source));

    // MAME's three read windows, word and byte reads one chip word each; a peek does not take data
    CHECK(c.cart.PeekLong(0x027FFFF8u) == c.ROMLong(0x027FFFF8u));
    CHECK(c.cart.ReadWord(0x02FFFFF8u) == v.words[0]);
    CHECK(c.cart.ReadWord(0x037FFFFAu) == v.words[1]);
    CHECK(c.cart.ReadByte(0x027FFFF8u) == (v.words[2] >> 8));
    CHECK(c.cart.ReadByte(0x027FFFF9u) == (v.words[3] & 0xFF));
    CHECK(c.cart.ReadLong(0x027FFFF8u) == (uint32_t(v.words[4]) << 16 | v.words[5]));
    // outside them, the ROM: the next long, the data register, the fourth 8 MiB window and CS1
    for (const uint32_t at : {0x027FFFFCu, 0x027FFFF4u, 0x03FFFFF8u}) {
        CHECK(c.cart.ReadLong(at) == c.ROMLong(at));
    }
    CHECK(c.cart.ReadLong(0x047FFFF8u) == c.ROMLong(0x027FFFF8u)); // CS1 shows image 0x02000000 on, mirrored
    CHECK(c.cart.ReadLong(0x027FFFF8u) == (uint32_t(v.words[6]) << 16 | v.words[7]));
}

TEST_CASE("stv 5838: the bank is the one of the source address write", "[stv]") {
    // libretro-stv sets the bank when the source address is written; MAME on every write to the cartridge space. Table
    // writes through another window after the source address leave the bank alone.
    const Vector &v = Vectors()[0];
    Cart c;
    c.Upload(0x027FFFF0u, v.treeMode, v.dictMode, v.salt);
    c.cart.WriteLong(kBank2Window, v.source);
    c.Upload(0x027FFFF0u, v.treeMode, v.dictMode, v.salt); // bank 0's window
    c.cart.WriteWord(0x02000000u, 0x1234);                  // and a write to the ROM
    std::vector<uint16_t> words;
    for (size_t i = 0; i < 8; i++) {
        words.push_back(c.cart.ReadWord(0x027FFFF8u));
    }
    CHECK(words == std::vector<uint16_t>(v.words.begin(), v.words.begin() + 8));
}

TEST_CASE("stv 5838: a cartridge without the chip has no register window", "[stv]") {
    STVGameROMCartridge cart;
    cart.LoadROM(PatternROM());
    cart.ConfigureProtection(STVGameROMCartridge::ProtectionMode::None, 0);
    cart.WriteLong(kBank2Window, 0x012345);
    CHECK(cart.ReadLong(kBank2Window + 8) == Cart{}.ROMLong(kBank2Window + 8));
    STV3155838::State state;
    cart.SaveComp5838State(state);
    CHECK(Bytes(state) == Bytes(STV3155838::State{}));
}

TEST_CASE("stv 5838: the cartridge's chip state round trips and a reset stops the chip", "[stv]") {
    const Vector &v = Vectors()[0];
    Cart c;
    c.Upload(kBank2Window, v.treeMode, v.dictMode, v.salt);
    c.cart.WriteLong(kBank2Window, v.source);
    c.cart.ReadLong(kBank2Window + 8);
    STV3155838::State mid;
    c.cart.SaveComp5838State(mid);
    REQUIRE(c.cart.ValidateComp5838State(mid));

    Cart other;
    REQUIRE(other.cart.LoadComp5838State(mid));
    for (size_t i = 2; i < 10; i += 2) {
        const uint32_t data = other.cart.ReadLong(kBank2Window + 8);
        CHECK(static_cast<uint16_t>(data >> 16) == v.words[i]);
        CHECK(static_cast<uint16_t>(data) == v.words[i + 1]);
    }

    // a reset (MAME's device_reset) stops it; the tables stay
    other.cart.Reset(true);
    CHECK(other.cart.ReadLong(kBank2Window + 8) == other.ROMLong(kBank2Window + 8));
    STV3155838::State after;
    other.cart.SaveComp5838State(after);
    CHECK(after.active == 0);
    CHECK(after.srcOffset == 0);
    CHECK(std::memcmp(&after.dictionary, &mid.dictionary, sizeof mid.dictionary) == 0);

    // a damaged state is refused
    STV3155838::State bad = mid;
    bad.bank = 7;
    CHECK_FALSE(other.cart.ValidateComp5838State(bad));
    CHECK_FALSE(other.cart.LoadComp5838State(bad));

    // a cartridge without the chip takes only the reset value
    STVGameROMCartridge plain;
    plain.LoadROM(PatternROM());
    plain.ConfigureProtection(STVGameROMCartridge::ProtectionMode::None, 0);
    CHECK(plain.ValidateComp5838State(STV3155838::State{}));
    CHECK_FALSE(plain.ValidateComp5838State(mid));
}
