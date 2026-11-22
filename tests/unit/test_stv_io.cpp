// ST-V I/O board: 93C46 cabinet EEPROM driven through the SMPC PDR1/PDR2 pins, coin input, and state round trips
// of the I/O board, the 315-5881 chip and the cartridge protection.

#include "catch_amalgamated.hpp"

#include <stv/stv_cartridge.hpp>
#include <stv/stv_io.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

using brimir::stv::STVIOBoard;
using brimir::stv::STVIOBoardState;

namespace {

// PDR1 bit 2 = CS, bit 3 = CLK, bit 4 = DI. PDR2 bit 0 = DO.
constexpr uint8_t kCS = 0x04;
constexpr uint8_t kCLK = 0x08;
constexpr uint8_t kDI = 0x10;

// Bit-bangs the 93C46 the way the ST-V BIOS does: data set up with CLK low, then CLK raised.
struct EepromBus {
    STVIOBoard &board;
    bool cs = false;

    explicit EepromBus(STVIOBoard &b) : board(b) {}

    void Select() {
        board.WritePDR1(0);
        board.WritePDR1(kCS);
        cs = true;
    }

    void Deselect() {
        board.WritePDR1(0);
        cs = false;
    }

    // Clocks one bit in and returns DO sampled after the rising edge.
    bool Clock(bool di) {
        const uint8_t base = static_cast<uint8_t>(kCS | (di ? kDI : 0));
        board.WritePDR1(base);
        board.WritePDR1(static_cast<uint8_t>(base | kCLK));
        return (board.ReadPDR2() & 1) != 0;
    }

    void Bits(uint32_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            Clock(((value >> i) & 1) != 0);
        }
    }

    // Start bit, 2-bit opcode, 6-bit address.
    void Command(unsigned opcode, unsigned address) {
        Bits(1, 1);
        Bits(opcode, 2);
        Bits(address, 6);
    }

    uint16_t ReadBits(unsigned count) {
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value = (value << 1) | (Clock(false) ? 1u : 0u);
        }
        return static_cast<uint16_t>(value);
    }

    uint16_t ReadWord(unsigned address) {
        Select();
        Command(2, address);
        const uint16_t value = ReadBits(16);
        Deselect();
        return value;
    }

    void EnableWrites() {
        Select();
        Command(0, 0x30); // EWEN: 00 11xxxx
        Deselect();
    }

    void DisableWrites() {
        Select();
        Command(0, 0x00); // EWDS: 00 00xxxx
        Deselect();
    }

    // Software polls the ready line after an operation; here the chip is simply given the time its cycle needs.
    void WaitReady() { board.AdvanceTime(10'000'000); }

    void WriteWord(unsigned address, uint16_t value, bool wait = true) {
        Select();
        Command(1, address);
        Bits(value, 16);
        Deselect();
        if (wait) WaitReady();
    }

    void EraseWord(unsigned address, bool wait = true) {
        Select();
        Command(3, address);
        Deselect();
        if (wait) WaitReady();
    }

    void EraseAll(bool wait = true) {
        Select();
        Command(0, 0x20); // ERAL: 00 10xxxx
        Deselect();
        if (wait) WaitReady();
    }

    // WRAL: 00 01xxxx, then the 16 data bits
    void WriteAll(uint16_t value, bool wait = true) {
        Select();
        Command(0, 0x10);
        Bits(value, 16);
        Deselect();
        if (wait) WaitReady();
    }

    bool DO() const { return (board.ReadPDR2() & 1) != 0; }
};

uint16_t ImageWord(const STVIOBoard &board, unsigned cell) {
    const auto &eeprom = board.GetEEPROM();
    return static_cast<uint16_t>((eeprom[cell * 2] << 8) | eeprom[cell * 2 + 1]);
}

constexpr std::array<uint8_t, 8> kSettings = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
constexpr std::array<uint8_t, 2> kHeader = {0xA5, 0x5A};

void InitBoard(STVIOBoard &board) {
    board.Reset(true);
    board.InitEEPROM(kHeader.data(), kSettings.data(), 0x00);
}

// Port C (coin, start, test, service) as read through the IOGA, with STV mode on and all pins inputs.
uint8_t ReadPortC(STVIOBoard &board) {
    return board.ReadIOGAByte(0x400004);
}

} // namespace

TEST_CASE("ST-V EEPROM: READ of address 0 returns the header", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    CHECK(bus.ReadWord(0) == 0x5345); // 'S' 'E'
    CHECK(bus.ReadWord(1) == 0x4741); // 'G' 'A'
    for (unsigned cell = 0; cell < 64; ++cell) {
        INFO("cell " << cell);
        CHECK(bus.ReadWord(cell) == ImageWord(board, cell));
    }
    // The game settings and the ROM header word are in the image
    CHECK(bus.ReadWord(0x1C / 2) == 0xA55A);
    CHECK(bus.ReadWord(0x1E / 2) == 0x1122);
}

TEST_CASE("ST-V EEPROM: EWEN, WRITE and READ round trip", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    const uint16_t before4 = bus.ReadWord(4);
    const uint16_t before6 = bus.ReadWord(6);

    bus.EnableWrites();
    bus.WriteWord(5, 0xBEEF);
    CHECK(bus.ReadWord(5) == 0xBEEF);
    CHECK(bus.ReadWord(4) == before4);
    CHECK(bus.ReadWord(6) == before6);

    bus.WriteWord(5, 0x1234);
    CHECK(bus.ReadWord(5) == 0x1234);
}

TEST_CASE("ST-V EEPROM: WRITE while locked leaves the cell unchanged", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    const uint16_t original = bus.ReadWord(5);
    bus.WriteWord(5, 0xBEEF); // no EWEN yet
    CHECK(bus.ReadWord(5) == original);

    bus.EnableWrites();
    bus.WriteWord(5, 0xBEEF);
    CHECK(bus.ReadWord(5) == 0xBEEF);

    bus.DisableWrites();
    bus.WriteWord(5, 0x0F0F);
    CHECK(bus.ReadWord(5) == 0xBEEF);
}

TEST_CASE("ST-V EEPROM: GetEEPROM shows what the game wrote", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    const std::array<uint8_t, 128> before = board.GetEEPROM();
    CHECK(before[0] == 'S');
    CHECK(before[1] == 'E');

    bus.EnableWrites();
    bus.WriteWord(10, 0xBEEF);

    const std::array<uint8_t, 128> &after = board.GetEEPROM();
    CHECK(after[20] == 0xBE); // cell 10, high byte first
    CHECK(after[21] == 0xEF);
    for (size_t i = 0; i < after.size(); ++i) {
        if (i != 20 && i != 21) {
            INFO("byte " << i);
            CHECK(after[i] == before[i]);
        }
    }
}

TEST_CASE("ST-V EEPROM: LoadEEPROM makes a saved image the cells", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);
    bus.EnableWrites();
    bus.WriteWord(3, 0x1111);

    std::array<uint8_t, 128> image{};
    for (size_t i = 0; i < image.size(); ++i) {
        image[i] = static_cast<uint8_t>(i * 5 + 3);
    }
    board.LoadEEPROM(image);

    CHECK(board.GetEEPROM() == image);
    for (unsigned cell = 0; cell < 64; ++cell) {
        INFO("cell " << cell);
        CHECK(bus.ReadWord(cell) == static_cast<uint16_t>((image[cell * 2] << 8) | image[cell * 2 + 1]));
    }

    // Nothing is re-derived from the header, and the chip is back in its write-locked reset state
    bus.WriteWord(3, 0x2222);
    CHECK(board.GetEEPROM() == image);
    bus.EnableWrites();
    bus.WriteWord(3, 0x2222);
    CHECK(board.GetEEPROM()[6] == 0x22);
    CHECK(board.GetEEPROM()[7] == 0x22);

    // What GetEEPROM returns loads back into another board
    STVIOBoard other;
    other.Reset(true);
    other.LoadEEPROM(board.GetEEPROM());
    EepromBus otherBus(other);
    CHECK(otherBus.ReadWord(3) == 0x2222);
    CHECK(other.GetEEPROM() == board.GetEEPROM());
}
TEST_CASE("ST-V EEPROM: a locked chip does not erase either", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    const uint16_t original = bus.ReadWord(7);
    bus.EraseWord(7);
    CHECK(bus.ReadWord(7) == original);

    bus.EnableWrites();
    bus.EraseWord(7);
    CHECK(bus.ReadWord(7) == 0xFFFF);
}

TEST_CASE("ST-V I/O: a coin held for 10 frames counts one coin", "[stv]") {
    STVIOBoard board;
    board.Reset(true);
    board.SetSTVMode(true);

    // Coin 1 is PORT-C bit 0, active low.
    auto countCoins = [&](unsigned heldFrames, unsigned totalFrames) {
        unsigned coins = 0;
        bool wasActive = (ReadPortC(board) & 0x01) == 0;
        for (unsigned frame = 0; frame < totalFrames; ++frame) {
            board.SetCoin1(frame < heldFrames);
            board.UpdateInputs();
            const bool active = (ReadPortC(board) & 0x01) == 0;
            if (active && !wasActive) {
                ++coins;
            }
            wasActive = active;
        }
        return coins;
    };

    CHECK(countCoins(10, 30) == 1);
    CHECK(countCoins(1, 30) == 1);  // a one-frame tap counts too
    CHECK(countCoins(10, 30) == 1); // and a new press after release is a new coin

    board.UpdateInputs();
    CHECK((ReadPortC(board) & 0x03) == 0x03); // idle: nothing asserted
}

TEST_CASE("ST-V I/O: coin 2 asserts only the coin 2 input", "[stv]") {
    STVIOBoard board;
    board.Reset(true);
    board.SetSTVMode(true);

    board.SetCoin2(true);
    board.UpdateInputs();
    CHECK((ReadPortC(board) & 0x03) == 0x01); // bit 1 low, bit 0 high
}

TEST_CASE("ST-V EEPROM: READ puts a dummy 0 on DO before the first data bit", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    std::array<uint8_t, 128> allOnes;
    allOnes.fill(0xFF);
    board.LoadEEPROM(allOnes);
    EepromBus bus(board);

    bus.Select();
    bus.Bits(1, 1); // start bit
    bus.Bits(2, 2); // READ
    bus.Bits(0, 5);
    CHECK(bus.DO());      // still the pulled-up line while the address is clocked in
    bus.Bits(0, 1);       // last address bit: the command runs
    CHECK_FALSE(bus.DO()); // dummy 0
    CHECK(bus.Clock(false)); // first data bit of 0xFFFF
    CHECK(bus.ReadBits(15) == 0x7FFF);

    // A cell whose first bit is 0 (0x5345): the dummy and the first bit are both low, the second bit is 1
    bus.Deselect();
    InitBoard(board);
    bus.Select();
    bus.Command(2, 0);
    CHECK_FALSE(bus.DO());
    CHECK_FALSE(bus.Clock(false));
    CHECK(bus.Clock(false));
}

TEST_CASE("ST-V EEPROM: reading past the word does not stream into the next cell", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    // MAME's 93C46 here has streaming disabled: after the 16 data bits come 16 zeros (the register shifting out),
    // then ones. Cell 1 is never presented in the same read.
    bus.Select();
    bus.Command(2, 0);
    CHECK(bus.ReadBits(16) == 0x5345);
    CHECK(bus.ReadBits(16) == 0x0000);
    CHECK(bus.ReadBits(16) == 0xFFFF);
    CHECK(bus.ReadBits(16) == 0xFFFF);
    bus.Deselect();
    CHECK(bus.ReadWord(1) == 0x4741);
}

TEST_CASE("ST-V EEPROM: WRAL writes every cell when enabled", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    // Locked: the data is clocked in, then refused
    const std::array<uint8_t, 128> original = board.GetEEPROM();
    bus.WriteAll(0x0000);
    CHECK(board.GetEEPROM() == original);

    bus.EnableWrites();
    bus.EraseAll();
    for (unsigned cell = 0; cell < 64; ++cell) {
        REQUIRE(bus.ReadWord(cell) == 0xFFFF);
    }
    bus.WriteAll(0x1234);
    for (unsigned cell = 0; cell < 64; ++cell) {
        INFO("cell " << cell);
        CHECK(bus.ReadWord(cell) == 0x1234);
    }

    // WRAL ANDs into what is there (MAME's write_all; an ERAL is meant to come first)
    bus.WriteAll(0xF0F0);
    for (unsigned cell = 0; cell < 64; ++cell) {
        INFO("cell " << cell);
        CHECK(bus.ReadWord(cell) == 0x1030);
    }
}

TEST_CASE("ST-V EEPROM: ERAL erases every cell when enabled and not when locked", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    const std::array<uint8_t, 128> original = board.GetEEPROM();
    bus.EraseAll();
    CHECK(board.GetEEPROM() == original);

    bus.EnableWrites();
    bus.EraseAll();
    for (uint8_t byte : board.GetEEPROM()) {
        CHECK(byte == 0xFF);
    }
}

TEST_CASE("ST-V EEPROM: ready/busy shows on DO while selected after a write", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);
    bus.EnableWrites();

    struct Op {
        const char *name;
        uint64_t nanoseconds;
    };
    const Op ops[] = {{"WRITE", 1'750'000}, {"ERASE", 1'000'000}, {"WRAL", 8'000'000}, {"ERAL", 8'000'000}};
    for (const Op &op : ops) {
        INFO(op.name);
        bus.WaitReady();
        if (op.name == std::string("WRITE")) bus.WriteWord(2, 0x1111, false);
        if (op.name == std::string("ERASE")) bus.EraseWord(2, false);
        if (op.name == std::string("WRAL")) bus.WriteAll(0xFFFF, false);
        if (op.name == std::string("ERAL")) bus.EraseAll(false);

        bus.Select();
        CHECK_FALSE(bus.DO()); // busy: DO low waiting for the start bit
        board.AdvanceTime(op.nanoseconds - 1);
        CHECK_FALSE(bus.DO());
        board.AdvanceTime(1);
        CHECK(bus.DO()); // ready
        bus.Deselect();
    }
}

TEST_CASE("ST-V EEPROM: DO is high at the end of a write and a busy chip refuses commands", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);
    bus.EnableWrites();

    // Still selected after the last data bit: the chip waits for completion with DO pulled up, busy or not
    bus.Select();
    bus.Command(1, 6);
    bus.Bits(0xABCD, 16);
    CHECK(bus.DO());
    bus.Deselect();

    // Selected again while the cycle runs: DO is the busy line, and the start bit of a READ is ignored
    bus.Select();
    CHECK_FALSE(bus.DO());
    bus.Command(2, 6);
    CHECK_FALSE(bus.DO());
    CHECK(bus.ReadBits(16) == 0x0000);
    bus.Deselect();

    bus.WaitReady();
    CHECK(bus.ReadWord(6) == 0xABCD);
}

TEST_CASE("ST-V EEPROM: one PDR1 write that raises CS and CLK does not clock a start bit", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    // MAME's pdr1_output_w applies CLK, then DI, then CS: the clock edge arrives while the chip is still in reset
    board.WritePDR1(0);
    board.WritePDR1(static_cast<uint8_t>(kCS | kCLK | kDI));
    // A normal READ now has to work from its own start bit
    bus.Command(2, 0);
    CHECK(bus.ReadBits(16) == 0x5345);
}

TEST_CASE("ST-V EEPROM: a rising CLK in the same PDR1 write as a DI change samples the old DI", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    bus.Select(); // CS high, DI low, CLK low
    board.WritePDR1(static_cast<uint8_t>(kCS | kDI | kCLK)); // DI 0 -> 1 with the clock edge: the edge sees 0
    board.WritePDR1(static_cast<uint8_t>(kCS | kDI));        // CLK low again, DI stays 1
    // No start bit was taken, so a command from its own start bit works
    bus.Command(2, 0);
    CHECK(bus.ReadBits(16) == 0x5345);
}
TEST_CASE("ST-V EEPROM: SaveState and LoadState mid-READ continue the same bit sequence", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);

    bus.Select();
    bus.Command(2, 0);
    const uint16_t firstBits = bus.ReadBits(5);

    STVIOBoardState state{};
    board.SaveState(state);

    const uint16_t expectedRest = bus.ReadBits(11);
    const uint16_t expectedNext = bus.ReadBits(16); // past the word: zeros, as MAME's non-streaming 93C46 does
    CHECK(static_cast<uint16_t>((firstBits << 11) | expectedRest) == 0x5345);
    CHECK(expectedNext == 0x0000);

    // A different board, with different EEPROM contents, takes over mid-READ.
    STVIOBoard other;
    const std::array<uint8_t, 2> otherHeader = {0x00, 0x00};
    const std::array<uint8_t, 8> otherSettings = {};
    other.Reset(true);
    other.InitEEPROM(otherHeader.data(), otherSettings.data(), 0x10);
    REQUIRE(other.LoadState(state));

    EepromBus otherBus(other);
    otherBus.cs = true;
    CHECK(otherBus.ReadBits(11) == expectedRest);
    CHECK(otherBus.ReadBits(16) == expectedNext);

    // The restored image is the saved one, not the other board's own
    otherBus.Deselect();
    CHECK(otherBus.ReadWord(0) == 0x5345);
    for (unsigned cell = 0; cell < 64; ++cell) {
        INFO("cell " << cell);
        CHECK(otherBus.ReadWord(cell) == ImageWord(board, cell));
    }
}

TEST_CASE("ST-V EEPROM: SaveState carries a running write cycle and a pending WRAL", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);
    bus.EnableWrites();
    bus.WriteWord(1, 0x4242, false);
    board.AdvanceTime(500'000);

    STVIOBoardState state{};
    board.SaveState(state);

    STVIOBoard other;
    InitBoard(other);
    REQUIRE(other.LoadState(state));
    EepromBus otherBus(other);
    otherBus.Select();
    CHECK_FALSE(otherBus.DO());
    other.AdvanceTime(1'249'999);
    CHECK_FALSE(otherBus.DO());
    other.AdvanceTime(1);
    CHECK(otherBus.DO());
    otherBus.Deselect();

    // A WRAL waiting for its data bits finishes as a WRAL on the restored board
    bus.WaitReady();
    bus.EraseAll();
    bus.Select();
    bus.Command(0, 0x10);
    bus.Bits(0xAA, 8);
    board.SaveState(state);
    STVIOBoard third;
    InitBoard(third);
    REQUIRE(third.LoadState(state));
    EepromBus thirdBus(third);
    thirdBus.Bits(0xAA, 8);
    thirdBus.Deselect();
    for (size_t i = 0; i < 128; ++i) {
        INFO("byte " << i);
        CHECK(third.GetEEPROM()[i] == 0xAA);
    }
}
TEST_CASE("ST-V EEPROM: SaveState carries the write lock and written cells", "[stv]") {
    STVIOBoard board;
    InitBoard(board);
    EepromBus bus(board);
    bus.EnableWrites();
    bus.WriteWord(9, 0xCAFE);

    STVIOBoardState state{};
    board.SaveState(state);

    STVIOBoard other;
    InitBoard(other);
    REQUIRE(other.LoadState(state));
    EepromBus otherBus(other);
    CHECK(otherBus.ReadWord(9) == 0xCAFE);
    otherBus.WriteWord(10, 0x7777); // still unlocked
    CHECK(otherBus.ReadWord(10) == 0x7777);
}

TEST_CASE("ST-V I/O: SaveState keeps the coin pulse and the pins", "[stv]") {
    STVIOBoard board;
    board.Reset(true);
    board.SetSTVMode(true);
    board.WriteIOGAByte(0x400010, 0xF7); // data direction: port 3 is an output, the rest inputs
    board.SetCoin1(true);
    board.UpdateInputs();
    board.UpdateInputs();

    STVIOBoardState state{};
    board.SaveState(state);

    std::vector<uint8_t> expected;
    for (int frame = 0; frame < 8; ++frame) {
        board.UpdateInputs();
        expected.push_back(ReadPortC(board));
    }
    REQUIRE(std::any_of(expected.begin(), expected.end(), [](uint8_t v) { return (v & 1) == 0; }));

    STVIOBoard other;
    other.Reset(true);
    REQUIRE(other.LoadState(state));
    other.SetCoin1(true); // still held, as in the original
    for (int frame = 0; frame < 8; ++frame) {
        other.UpdateInputs();
        CHECK(ReadPortC(other) == expected[frame]);
    }
    CHECK(other.ReadIOGAByte(0x400010) == 0xF7);
}

TEST_CASE("ST-V I/O: LoadState rejects a state that is not valid", "[stv]") {
    STVIOBoard board;
    InitBoard(board);

    STVIOBoardState state{};
    board.SaveState(state);

    STVIOBoardState bad = state;
    bad.eepromState = 99;
    CHECK_FALSE(board.LoadState(bad));

    bad = state;
    bad.eepromAddress = 64;
    CHECK_FALSE(board.LoadState(bad));

    CHECK(board.LoadState(state));
}

TEST_CASE("ST-V state structs have no padding", "[stv]") {
    CHECK(std::is_trivially_copyable_v<STVIOBoardState>);
    CHECK(std::has_unique_object_representations_v<STVIOBoardState>);
    CHECK(std::is_trivially_copyable_v<ymir::cart::STV3155881::State>);
    CHECK(std::has_unique_object_representations_v<ymir::cart::STV3155881::State>);
    CHECK(std::is_trivially_copyable_v<ymir::cart::STVGameROMCartridge::ProtectionState>);
    CHECK(std::has_unique_object_representations_v<ymir::cart::STVGameROMCartridge::ProtectionState>);
}

namespace {

std::vector<uint8_t> NoiseROM(size_t size) {
    std::vector<uint8_t> rom(size);
    uint32_t x = 0x12345678;
    for (auto &b : rom) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(x >> 24);
    }
    return rom;
}

std::vector<uint16_t> ReadWords(ymir::cart::STV3155881 &chip, unsigned count) {
    std::vector<uint16_t> words;
    for (unsigned i = 0; i < count; ++i) {
        words.push_back(chip.ReadDecryptedWord());
    }
    return words;
}

void ConfigureChip(ymir::cart::STV3155881 &chip, std::span<const uint8_t> rom, uint32_t key) {
    chip.SetROM(rom);
    chip.Reset();
    chip.SetKey(key);
    chip.SetEnabled(true);
    chip.SetLowAddress(0x0100);
    chip.SetHighAddress(0x0000);
    chip.SetSubKey(0x00AB);
}

// The ROM is noise, so what it decrypts to is arbitrary but fixed. These keys make the first block header decode to
// an uncompressed block (a large one) and to a compressed one (108 lines of 276 bytes).
void CheckChipStateContinues(uint32_t key, bool compressed) {
    const auto rom = NoiseROM(1u << 20);

    ymir::cart::STV3155881 chip;
    ConfigureChip(chip, rom, key);
    ReadWords(chip, 37);

    ymir::cart::STV3155881::State state{};
    chip.SaveState(state);
    CHECK(((state.decHeader & 0x20000) != 0) == compressed);
    const auto expected = ReadWords(chip, 600); // several lines when compressed

    ymir::cart::STV3155881 other;
    ConfigureChip(other, rom, key);
    ReadWords(other, 5); // out of step with the original
    REQUIRE(other.LoadState(state));
    CHECK(ReadWords(other, 600) == expected);

    // Saving what was loaded gives the same state back
    ymir::cart::STV3155881 third;
    ConfigureChip(third, rom, key);
    REQUIRE(third.LoadState(state));
    ymir::cart::STV3155881::State again{};
    third.SaveState(again);
    CHECK(std::memcmp(&state, &again, sizeof state) == 0);

    ymir::cart::STV3155881::State bad = state;
    bad.bufferPos = 99;
    CHECK_FALSE(other.LoadState(bad));
    bad = state;
    bad.lineBufferSize = 513;
    CHECK_FALSE(other.LoadState(bad));
    bad = state;
    bad.bufferBit = -1;
    CHECK_FALSE(other.LoadState(bad));
}

} // namespace

TEST_CASE("ST-V 315-5881: SaveState and LoadState continue an uncompressed stream", "[stv]") {
    CheckChipStateContinues(0x01010101, false);
}

TEST_CASE("ST-V 315-5881: SaveState and LoadState continue a compressed stream", "[stv]") {
    CheckChipStateContinues(0x02020202, true);
}

TEST_CASE("ST-V 315-5881: damaged compressed data does not run past the line buffers", "[stv]") {
    // Noise decodes to arbitrary block headers (compressed and not, odd widths) and runs that overshoot the line. The
    // reads must terminate and the buffer positions must stay inside the buffers they index.
    const auto rom = NoiseROM(1u << 20);
    for (uint32_t key = 1; key <= 64; ++key) {
        ymir::cart::STV3155881 chip;
        ConfigureChip(chip, rom, key * 0x01010101u);
        for (unsigned word = 0; word < 3000; ++word) {
            ymir::cart::STV3155881::State state{};
            chip.SaveState(state);
            if (state.bufferPos > 2 || state.lineBufferSize > 512 || state.lineBufferPos > 512) {
                FAIL("key " << key << " word " << word << ": bufferPos " << state.bufferPos << " lineBufferPos "
                            << state.lineBufferPos << " lineBufferSize " << state.lineBufferSize);
            }
            chip.ReadDecryptedWord();
        }
    }
}
TEST_CASE("ST-V cartridge: protection state continues the RSG counter", "[stv]") {
    const auto rom = NoiseROM(1u << 16);
    constexpr uint32_t kCS1 = 0x04000000;

    ymir::cart::STVGameROMCartridge cart;
    cart.LoadROM(rom);
    cart.ConfigureProtection(ymir::cart::STVGameROMCartridge::ProtectionMode::RSG, 0);
    cart.WriteByte(kCS1 + 1, 1); // arms the counter
    for (int i = 0; i < 7; ++i) {
        cart.ReadWord(kCS1 + 0xC);
    }

    ymir::cart::STVGameROMCartridge::ProtectionState state{};
    cart.SaveProtectionState(state);
    std::vector<uint16_t> expected;
    for (int i = 0; i < 20; ++i) {
        expected.push_back(cart.ReadWord(kCS1 + 0xC));
    }

    ymir::cart::STVGameROMCartridge other;
    other.LoadROM(rom);
    other.ConfigureProtection(ymir::cart::STVGameROMCartridge::ProtectionMode::RSG, 0);
    REQUIRE(other.LoadProtectionState(state));
    for (int i = 0; i < 20; ++i) {
        CHECK(other.ReadWord(kCS1 + 0xC) == expected[i]);
    }
}
