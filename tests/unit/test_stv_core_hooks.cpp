// ST-V hooks in the core (cartridge long-word access, SMPC ST-V mode) and the proof that Saturn does not notice them.
// The Saturn golden values below were captured from the code before the hooks existed.

#include "catch_amalgamated.hpp"

#include "unit/stv_test_bios.hpp"

#include <ymir/hw/cart/cart_base.hpp>
#include <ymir/hw/smpc/smpc.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/saturn.hpp>

#include <stv/stv_315_5881.hpp>
#include <stv/stv_cartridge.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

using namespace ymir;

namespace {

constexpr uint32 kIREG0 = 0x010'0001;
constexpr uint32 kCOMREG = 0x010'001F;
constexpr uint32 kOREG0 = 0x010'0021;
constexpr uint32 kSF = 0x010'0063;
constexpr uint32 kPDR1 = 0x010'0075;
constexpr uint32 kPDR2 = 0x010'0077;
constexpr uint32 kDDR1 = 0x010'0079;
constexpr uint32 kDDR2 = 0x010'007B;

constexpr uint32 kCartBase = 0x200'0000;
constexpr uint32 kCartLastLong = 0x4FF'FFFC;

void RunFor(Saturn &saturn, uint64 cycles) {
    uint64 elapsed = 0;
    while (elapsed < cycles) {
        elapsed += saturn.StepMasterSH2();
    }
}

void Command(Saturn &saturn, std::initializer_list<uint8> iregs, uint8 command) {
    uint32 address = kIREG0;
    for (const uint8 value : iregs) {
        saturn.mainBus.Write<uint8>(address, value);
        address += 2;
    }
    saturn.mainBus.Write<uint8>(kSF, 1);
    saturn.mainBus.Write<uint8>(kCOMREG, command);
    RunFor(saturn, 20000);
}

// INTBACK with the status report only; returns OREG0
uint8 IntbackOREG0(Saturn &saturn) {
    Command(saturn, {0x01, 0x00, 0xF0}, 0x10);
    return saturn.mainBus.Peek<uint8>(kOREG0);
}

// Two control pads and the TH/TR handshake bits as outputs, as a game reading the pads directly sets them up
void SetUpPads(Saturn &saturn) {
    saturn.SMPC.GetPeripheralPort1().ConnectControlPad();
    saturn.SMPC.GetPeripheralPort2().ConnectControlPad();
    saturn.mainBus.Write<uint8>(kDDR1, 0x60);
    saturn.mainBus.Write<uint8>(kDDR2, 0x60);
}

// A cartridge that counts the accesses it gets. Opts in to long accesses unless told otherwise.
class CountingCartridge final : public cart::BaseCartridge {
public:
    explicit CountingCartridge(bool handlesLong)
        : BaseCartridge(0xFFu, cart::CartType::ROM) {
        SetOwnsCartridgeSpace(handlesLong);
    }

    mutable int readLongs = 0;
    mutable int readWords = 0;
    int writeLongs = 0;
    int writeWords = 0;
    mutable int peekLongs = 0;
    mutable int peekWords = 0;
    int pokeLongs = 0;
    int pokeWords = 0;
    std::vector<uint32> wordWrites;

    static uint16 WordAt(uint32 address) {
        return static_cast<uint16>(0x1000u + (address & 0xFFFEu) * 3u);
    }

    uint8 ReadByte(uint32 address) const override {
        return static_cast<uint8>(WordAt(address) >> ((address & 1) ? 0 : 8));
    }
    uint16 ReadWord(uint32 address) const override {
        ++readWords;
        return WordAt(address);
    }
    uint32 ReadLong(uint32 address) const override {
        ++readLongs;
        return 0xA5A50000u | (address & 0xFFFFu);
    }
    void WriteByte(uint32, uint8) override {}
    void WriteWord(uint32 address, uint16 value) override {
        ++writeWords;
        wordWrites.push_back(address);
        wordWrites.push_back(value);
    }
    void WriteLong(uint32, uint32) override {
        ++writeLongs;
    }
    uint8 PeekByte(uint32 address) const override {
        return ReadByte(address);
    }
    uint16 PeekWord(uint32 address) const override {
        ++peekWords;
        return WordAt(address);
    }
    uint32 PeekLong(uint32 address) const override {
        ++peekLongs;
        return 0x5A5A0000u | (address & 0xFFFFu);
    }
    void PokeByte(uint32, uint8) override {}
    void PokeWord(uint32, uint16) override {
        ++pokeWords;
    }
    void PokeLong(uint32, uint32) override {
        ++pokeLongs;
    }
};

uint32 TwoWords(uint32 address) {
    return (static_cast<uint32>(CountingCartridge::WordAt(address)) << 16u) |
           CountingCartridge::WordAt(address + 2);
}

} // namespace

TEST_CASE("stv hooks: a cartridge that handles long accesses gets one ReadLong per 32-bit SCU read", "[stv]") {
    auto saturn = std::make_unique<Saturn>();
    auto *cart = saturn->InsertCartridge<CountingCartridge>(true);

    CHECK(saturn->mainBus.Read<uint32>(kCartBase + 0x1230) == 0xA5A51230u);
    CHECK(cart->readLongs == 1);
    CHECK(cart->readWords == 0);

    // the end of the cartridge space, where the SCU answers 16-bit reads with the cartridge ID, goes to the cartridge too
    CHECK(saturn->mainBus.Read<uint32>(kCartLastLong) == 0xA5A5FFFCu);
    CHECK(cart->readLongs == 2);

    saturn->mainBus.Write<uint32>(kCartBase + 0x40, 0x11223344u);
    CHECK(cart->writeLongs == 1);
    CHECK(cart->writeWords == 0);

    CHECK(saturn->mainBus.Peek<uint32>(kCartBase + 0x1230) == 0x5A5A1230u);
    CHECK(cart->peekLongs == 1);
    CHECK(cart->peekWords == 0);

    saturn->mainBus.Poke<uint32>(kCartBase + 0x40, 0x11223344u);
    CHECK(cart->pokeLongs == 1);
    CHECK(cart->pokeWords == 0);

    // 16-bit reads are unchanged, and the cartridge owns the end of the space: no cartridge ID there
    CHECK(saturn->mainBus.Read<uint16>(kCartBase + 0x1230) == CountingCartridge::WordAt(kCartBase + 0x1230));
    CHECK(cart->readLongs == 2);
    CHECK(saturn->mainBus.Read<uint16>(0x4FF'FFFE) == CountingCartridge::WordAt(0x4FF'FFFE));
    CHECK(saturn->mainBus.Read<uint8>(0x4FF'FFFF) == static_cast<uint8>(CountingCartridge::WordAt(0x4FF'FFFE)));
    CHECK(saturn->mainBus.Read<uint8>(0x4FF'FFFE) == static_cast<uint8>(CountingCartridge::WordAt(0x4FF'FFFE) >> 8));
}

TEST_CASE("stv hooks: a cartridge that does not handle long accesses sees the upstream two-word split", "[stv]") {
    auto saturn = std::make_unique<Saturn>();
    auto *cart = saturn->InsertCartridge<CountingCartridge>(false);

    CHECK(saturn->mainBus.Read<uint32>(kCartBase + 0x1230) == TwoWords(kCartBase + 0x1230));
    CHECK(cart->readWords == 2);
    CHECK(cart->readLongs == 0);

    // the last long of the space: the second word is the cartridge ID, not the cartridge's
    cart->readWords = 0;
    const uint32 last = saturn->mainBus.Read<uint32>(kCartLastLong);
    CHECK(last == ((static_cast<uint32>(CountingCartridge::WordAt(kCartLastLong)) << 16u) | 0xFFFFu));
    CHECK(cart->readWords == 1);
    CHECK(cart->readLongs == 0);

    // 16- and 8-bit reads of the last word are the cartridge ID, as upstream
    CHECK(saturn->mainBus.Read<uint16>(0x4FF'FFFE) == 0xFFFFu);
    CHECK(saturn->mainBus.Read<uint8>(0x4FF'FFFE) == 0xFFu);
    CHECK(saturn->mainBus.Read<uint8>(0x4FF'FFFF) == 0xFFu);

    saturn->mainBus.Write<uint32>(kCartBase + 0x40, 0x11223344u);
    CHECK(cart->writeWords == 2);
    CHECK(cart->writeLongs == 0);
    REQUIRE(cart->wordWrites.size() == 4);
    CHECK(cart->wordWrites[0] == kCartBase + 0x40);
    CHECK(cart->wordWrites[1] == 0x1122u);
    CHECK(cart->wordWrites[2] == kCartBase + 0x42);
    CHECK(cart->wordWrites[3] == 0x3344u);

    CHECK(saturn->mainBus.Peek<uint32>(kCartBase + 0x1230) == TwoWords(kCartBase + 0x1230));
    CHECK(cart->peekWords == 2);
    CHECK(cart->peekLongs == 0);

    saturn->mainBus.Poke<uint32>(kCartBase + 0x40, 0x11223344u);
    CHECK(cart->pokeWords == 2);
    CHECK(cart->pokeLongs == 0);
}

TEST_CASE("stv hooks: the default long accesses are two word accesses, high word first", "[stv]") {
    // a cartridge that overrides no long access
    class WordsOnly final : public cart::BaseCartridge {
    public:
        WordsOnly()
            : BaseCartridge(0xFFu, cart::CartType::ROM) {}
        mutable std::vector<uint32> order;
        std::vector<std::pair<uint32, uint16>> writes;
        uint8 ReadByte(uint32) const override {
            return 0;
        }
        uint16 ReadWord(uint32 address) const override {
            order.push_back(address);
            return CountingCartridge::WordAt(address);
        }
        void WriteByte(uint32, uint8) override {}
        void WriteWord(uint32 address, uint16 value) override {
            writes.emplace_back(address, value);
        }
        uint8 PeekByte(uint32) const override {
            return 0;
        }
        uint16 PeekWord(uint32 address) const override {
            order.push_back(address | 0x8000'0000u);
            return CountingCartridge::WordAt(address);
        }
        void PokeByte(uint32, uint8) override {}
        void PokeWord(uint32 address, uint16 value) override {
            writes.emplace_back(address | 0x8000'0000u, value);
        }
    } cart;

    CHECK_FALSE(cart.OwnsCartridgeSpace());

    CHECK(cart.ReadLong(0x300'0010) == TwoWords(0x300'0010));
    CHECK(cart.order == std::vector<uint32>{0x300'0010, 0x300'0012});

    cart.order.clear();
    CHECK(cart.PeekLong(0x300'0010) == TwoWords(0x300'0010));
    CHECK(cart.order == std::vector<uint32>{0x8000'0000u | 0x300'0010, 0x8000'0000u | 0x300'0012});

    cart.WriteLong(0x300'0020, 0xDEADBEEFu);
    cart.PokeLong(0x300'0020, 0xDEADBEEFu);
    REQUIRE(cart.writes.size() == 4);
    CHECK(cart.writes[0] == std::make_pair(uint32{0x300'0020}, uint16{0xDEAD}));
    CHECK(cart.writes[1] == std::make_pair(uint32{0x300'0022}, uint16{0xBEEF}));
    CHECK(cart.writes[2] == std::make_pair(0x8000'0000u | 0x300'0020u, uint16{0xDEAD}));
    CHECK(cart.writes[3] == std::make_pair(0x8000'0000u | 0x300'0022u, uint16{0xBEEF}));
}

TEST_CASE("stv hooks: the ST-V cartridge handles long accesses", "[stv]") {
    cart::STVGameROMCartridge cart;
    CHECK(cart.OwnsCartridgeSpace());

    // through the SCU: 32-bit reads see the ROM everywhere, also in the last long of the cartridge space
    std::vector<uint8> rom(0x10000);
    for (size_t i = 0; i < rom.size(); ++i) {
        rom[i] = static_cast<uint8>((i * 7 + (i >> 8)) & 0xFF);
    }
    auto saturn = std::make_unique<Saturn>();
    auto *stv = saturn->InsertCartridge<cart::STVGameROMCartridge>();
    stv->LoadROM(rom);
    auto longAt = [&](size_t offset) {
        return (uint32{rom[offset]} << 24u) | (uint32{rom[offset + 1]} << 16u) | (uint32{rom[offset + 2]} << 8u) |
               uint32{rom[offset + 3]};
    };
    CHECK(saturn->mainBus.Read<uint32>(kCartBase + 0x1230) == longAt(0x1230));
    CHECK(saturn->mainBus.Peek<uint32>(kCartBase + 0x1230) == longAt(0x1230));
    CHECK(saturn->mainBus.Read<uint32>(kCartLastLong) == longAt(0xFFFC));
    CHECK(saturn->mainBus.Peek<uint32>(kCartLastLong) == longAt(0xFFFC));
}

namespace {

constexpr uint32 kProt = 0x4FF'FFF0; // the 315-5881 registers, MAME's install_common_protection()
constexpr uint32 kProtKey = 0x01010101;
constexpr uint16 kProtLow = 0x0100;
constexpr uint16 kProtHigh = 0x0000;
constexpr uint16 kProtSubKey = 0x00AB;

std::vector<uint8> ProtROM() {
    std::vector<uint8> rom(1u << 20);
    uint32 x = 0x12345678;
    for (auto &b : rom) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<uint8>(x >> 24);
    }
    return rom;
}

// The chip as MAME's handlers set it up, read without the cartridge: the words a game gets in order
struct ReferenceChip {
    cart::STV3155881 chip;

    explicit ReferenceChip(const std::vector<uint8> &rom) {
        chip.SetROM(rom);
        chip.Reset();
        chip.SetKey(kProtKey);
        chip.SetEnabled(true);
        chip.SetLowAddress(kProtLow);
        chip.SetHighAddress(kProtHigh);
        chip.SetSubKey(kProtSubKey);
    }

    // common_prot_r: two decryptions, each byte swapped, the first in the high half
    uint32 NextLong() {
        const uint16 first = chip.ReadDecryptedWord();
        const uint16 second = chip.ReadDecryptedWord();
        const auto swap = [](uint16 w) { return static_cast<uint16>((w >> 8) | (w << 8)); };
        return (uint32{swap(first)} << 16) | swap(second);
    }
};

std::unique_ptr<Saturn> ChipSaturn(const std::vector<uint8> &rom, cart::STVGameROMCartridge *&cart) {
    auto saturn = std::make_unique<Saturn>();
    cart = saturn->InsertCartridge<cart::STVGameROMCartridge>();
    cart->LoadROM(rom);
    cart->ConfigureProtection(cart::STVGameROMCartridge::ProtectionMode::Chip3155881, kProtKey);
    return saturn;
}

void ProgramChipWithLongs(Saturn &saturn) {
    saturn.mainBus.Write<uint32>(kProt + 0x0, 0x0001'0000);
    saturn.mainBus.Write<uint32>(kProt + 0x8, (uint32{kProtLow} << 16) | kProtHigh);
    saturn.mainBus.Write<uint32>(kProt + 0xC, uint32{kProtSubKey} << 16);
}

} // namespace

TEST_CASE("stv hooks: 315-5881 registers decode as MAME's common_prot_w and common_prot_r do", "[stv]") {
    const auto rom = ProtROM();
    cart::STVGameROMCartridge *cart = nullptr;

    SECTION("long writes enable and program the chip; long reads return the decrypted words in MAME's order") {
        auto saturn = ChipSaturn(rom, cart);
        ProgramChipWithLongs(*saturn);
        ReferenceChip reference(rom);
        for (int i = 0; i < 40; ++i) {
            CHECK(saturn->mainBus.Read<uint32>(kProt + 0xC) == reference.NextLong());
        }
    }

    SECTION("word writes decode as lanes of the long registers") {
        auto saturn = ChipSaturn(rom, cart);
        // bit 16 of register 0 is bit 0 of its high word
        saturn->mainBus.Write<uint16>(kProt + 0x2, 0x0001);
        CHECK(saturn->mainBus.Read<uint32>(kProt + 0x0) == 0x0000'0001); // stored, but not the enable
        saturn->mainBus.Write<uint16>(kProt + 0x0, 0x0001);
        saturn->mainBus.Write<uint16>(kProt + 0x8, kProtLow);
        saturn->mainBus.Write<uint16>(kProt + 0xA, kProtHigh);
        saturn->mainBus.Write<uint16>(kProt + 0xC, kProtSubKey);
        ReferenceChip reference(rom);
        for (int i = 0; i < 10; ++i) {
            CHECK(saturn->mainBus.Read<uint32>(kProt + 0xC) == reference.NextLong());
        }
    }

    SECTION("byte writes decode as lanes of the long registers") {
        auto saturn = ChipSaturn(rom, cart);
        saturn->mainBus.Write<uint8>(kProt + 0x1, 0x01); // bits 16-23 of register 0
        // each byte write sets the whole address half it touches from its own lane (the other lane is 0 in the access's
        // data), so the last byte written decides: low = byte at +8 << 8 | byte at +9
        saturn->mainBus.Write<uint8>(kProt + 0x9, 0x00);
        saturn->mainBus.Write<uint8>(kProt + 0x8, 0x01);
        saturn->mainBus.Write<uint8>(kProt + 0xB, 0x00);
        saturn->mainBus.Write<uint8>(kProt + 0xA, 0x00);
        saturn->mainBus.Write<uint8>(kProt + 0xC, 0x00);
        saturn->mainBus.Write<uint8>(kProt + 0xD, kProtSubKey);
        ReferenceChip reference(rom);
        for (int i = 0; i < 10; ++i) {
            CHECK(saturn->mainBus.Read<uint32>(kProt + 0xC) == reference.NextLong());
        }
    }

    SECTION("the subkey is the high half of register 3, whichever half is written") {
        auto saturn = ChipSaturn(rom, cart);
        saturn->mainBus.Write<uint32>(kProt + 0x0, 0x0001'0000);
        saturn->mainBus.Write<uint32>(kProt + 0x8, (uint32{kProtLow} << 16) | kProtHigh);
        saturn->mainBus.Write<uint32>(kProt + 0xC, (uint32{kProtSubKey} << 16) | 0x1234);
        saturn->mainBus.Write<uint16>(kProt + 0xE, 0x5678); // restarts the stream with the same subkey
        ReferenceChip reference(rom);
        for (int i = 0; i < 10; ++i) {
            CHECK(saturn->mainBus.Read<uint32>(kProt + 0xC) == reference.NextLong());
        }
    }

    SECTION("word and byte reads of the data register take a lane of a whole two-word decryption") {
        auto saturn = ChipSaturn(rom, cart);
        ProgramChipWithLongs(*saturn);
        ReferenceChip reference(rom);
        const uint32 first = reference.NextLong();
        const uint32 second = reference.NextLong();
        const uint32 third = reference.NextLong();
        const uint32 fourth = reference.NextLong();
        CHECK(saturn->mainBus.Read<uint16>(kProt + 0xC) == (first >> 16));
        CHECK(saturn->mainBus.Read<uint16>(kProt + 0xE) == (second & 0xFFFF));
        CHECK(saturn->mainBus.Read<uint8>(kProt + 0xD) == ((third >> 16) & 0xFF));
        CHECK(saturn->mainBus.Read<uint8>(kProt + 0xF) == (fourth & 0xFF));
    }

    SECTION("other registers read back what was written; peeks do not run the chip") {
        auto saturn = ChipSaturn(rom, cart);
        ProgramChipWithLongs(*saturn);
        saturn->mainBus.Write<uint32>(kProt + 0x4, 0x1234'5678);
        CHECK(saturn->mainBus.Read<uint32>(kProt + 0x4) == 0x1234'5678);
        CHECK(saturn->mainBus.Read<uint16>(kProt + 0x4) == 0x1234);
        CHECK(saturn->mainBus.Read<uint8>(kProt + 0x7) == 0x78);
        CHECK(saturn->mainBus.Read<uint32>(kProt + 0x8) == ((uint32{kProtLow} << 16) | kProtHigh));

        const uint32 peeked = saturn->mainBus.Peek<uint32>(kProt + 0xC);
        CHECK(peeked == (uint32{kProtSubKey} << 16));
        ReferenceChip reference(rom);
        CHECK(saturn->mainBus.Read<uint32>(kProt + 0xC) == reference.NextLong()); // the peek did not consume
    }

    SECTION("while the chip is disabled the registers read back what was written, else the ROM") {
        auto saturn = ChipSaturn(rom, cart);
        const auto romLong = [&](size_t offset) {
            return (uint32{rom[offset]} << 24) | (uint32{rom[offset + 1]} << 16) | (uint32{rom[offset + 2]} << 8) |
                   uint32{rom[offset + 3]};
        };
        // the ROM view is the end of the 16 MiB bank, mirrored into the 1 MiB test ROM
        CHECK(saturn->mainBus.Read<uint32>(kProt + 0x4) == romLong(0xFFFF4));
        CHECK(saturn->mainBus.Read<uint32>(kProt + 0xC) == romLong(0xFFFFC));
        CHECK(saturn->mainBus.Read<uint16>(0x4FF'FFFE) == (romLong(0xFFFFC) & 0xFFFF));
        saturn->mainBus.Write<uint32>(kProt + 0x4, 0xCAFE'F00D);
        CHECK(saturn->mainBus.Read<uint32>(kProt + 0x4) == 0xCAFE'F00D);
        saturn->mainBus.Write<uint32>(kProt + 0x0, 0x0001'0000);
        saturn->mainBus.Write<uint32>(kProt + 0x0, 0x0000'0000); // disabled again, nonzero registers still win
        saturn->mainBus.Write<uint32>(kProt + 0x4, 0x0000'0000);
        CHECK(saturn->mainBus.Read<uint32>(kProt + 0x4) == romLong(0xFFFF4));
    }

    SECTION("a cartridge without the chip does not intercept the registers") {
        auto saturn = std::make_unique<Saturn>();
        auto *plain = saturn->InsertCartridge<cart::STVGameROMCartridge>();
        plain->LoadROM(rom);
        saturn->mainBus.Write<uint32>(kProt + 0x4, 0x1234'5678);
        CHECK(saturn->mainBus.Read<uint32>(kProt + 0x4) != 0x1234'5678);
    }

    SECTION("the registers are part of the protection state") {
        auto saturn = ChipSaturn(rom, cart);
        ProgramChipWithLongs(*saturn);
        saturn->mainBus.Write<uint32>(kProt + 0x4, 0x1234'5678);
        ReferenceChip reference(rom);
        for (int i = 0; i < 5; ++i) {
            REQUIRE(saturn->mainBus.Read<uint32>(kProt + 0xC) == reference.NextLong());
        }

        cart::STVGameROMCartridge::ProtectionState state{};
        cart->SaveProtectionState(state);
        CHECK(state.abus[1] == 0x1234'5678);
        CHECK(state.protectionEnabled == 1);

        cart::STVGameROMCartridge::ProtectionState mismatch = state;
        mismatch.abus[0] = 0;
        CHECK_FALSE(cart->LoadProtectionState(mismatch));

        cart::STVGameROMCartridge::ProtectionState reserved = state;
        reserved.reserved[2] = 1;
        CHECK_FALSE(cart->LoadProtectionState(reserved));

        cart::STVGameROMCartridge *other = nullptr;
        auto otherSaturn = ChipSaturn(rom, other);
        REQUIRE(other->LoadProtectionState(state));
        CHECK(otherSaturn->mainBus.Read<uint32>(kProt + 0x4) == 0x1234'5678);
        for (int i = 0; i < 10; ++i) {
            CHECK(otherSaturn->mainBus.Read<uint32>(kProt + 0xC) == reference.NextLong());
        }
    }
}
TEST_CASE("stv hooks: Saturn PDR1, PDR2 and INTBACK are what they were before the hooks", "[stv][smpc]") {
    // Captured from the unmodified code: pads on both ports, TH/TR walked through the handshake positions
    struct Step {
        uint8 write;
        uint8 pdr1;
        uint8 pdr2;
    };
    constexpr std::array<Step, 6> kGolden = {{
        {0x60, 0x7C, 0x7C},
        {0x20, 0x3F, 0x3F},
        {0x40, 0x5F, 0x5F},
        {0x00, 0x1F, 0x1F},
        {0x60, 0x7C, 0x7C},
        {0x20, 0x3F, 0x3F},
    }};

    auto check = [&](Saturn &saturn) {
        CHECK(saturn.mainBus.Read<uint8>(kPDR1) == 0x00);
        CHECK(saturn.mainBus.Read<uint8>(kPDR2) == 0x00);
        SetUpPads(saturn);
        for (const Step &step : kGolden) {
            saturn.mainBus.Write<uint8>(kPDR1, step.write);
            saturn.mainBus.Write<uint8>(kPDR2, step.write);
            CHECK(saturn.mainBus.Read<uint8>(kPDR1) == step.pdr1);
            CHECK(saturn.mainBus.Read<uint8>(kPDR2) == step.pdr2);
            CHECK(saturn.mainBus.Peek<uint8>(kPDR1) == step.pdr1);
            CHECK(saturn.mainBus.Peek<uint8>(kPDR2) == step.pdr2);
        }
        CHECK(IntbackOREG0(saturn) == 0x40);
        Command(saturn, {0x01, 0x02, 0x03, 0x04}, 0x17); // SETSMEM
        CHECK(IntbackOREG0(saturn) == 0xC0);
    };

    SECTION("a fresh core") {
        auto saturn = std::make_unique<Saturn>();
        CHECK_FALSE(saturn->SMPC.IsSTVMode());
        check(*saturn);
    }

    SECTION("ST-V mode set and cleared again, handlers left installed") {
        auto saturn = std::make_unique<Saturn>();
        int calls = 0;
        saturn->SMPC.SetSTVPDRHandlers([&] { ++calls; return uint8{0x55}; }, [&](uint8) { ++calls; },
                                       [&] { ++calls; return uint8{0x55}; }, [&](uint8) { ++calls; });
        saturn->SMPC.SetSTVMode(true);
        saturn->SMPC.SetSTVMode(false);
        check(*saturn);
        CHECK(calls == 0);
    }
}

TEST_CASE("stv hooks: ST-V mode routes PDR1 and PDR2 to the handlers as MAME does", "[stv][smpc]") {
    auto saturn = std::make_unique<Saturn>();

    std::vector<uint8> pdr1Writes;
    std::vector<uint8> pdr2Writes;
    uint8 pdr1Value = 0x2B;
    uint8 pdr2Value = 0x35;
    int pdr1Reads = 0;
    int pdr2Reads = 0;
    saturn->SMPC.SetSTVPDRHandlers([&] { ++pdr1Reads; return pdr1Value; }, [&](uint8 v) { pdr1Writes.push_back(v); },
                                   [&] { ++pdr2Reads; return pdr2Value; }, [&](uint8 v) { pdr2Writes.push_back(v); });
    saturn->SMPC.SetSTVMode(true);
    CHECK(saturn->SMPC.IsSTVMode());

    // smpc_hle_device::pdr1_w: the handler gets the written value masked by DDR (bit 7 never)
    saturn->mainBus.Write<uint8>(kDDR1, 0x1C);
    saturn->mainBus.Write<uint8>(kDDR2, 0x10);
    saturn->mainBus.Write<uint8>(kPDR1, 0xFF);
    REQUIRE(pdr1Writes.size() == 1);
    CHECK(pdr1Writes[0] == 0x1C);
    CHECK(pdr2Writes.empty());
    saturn->mainBus.Write<uint8>(kPDR2, 0xFF);
    REQUIRE(pdr2Writes.size() == 1);
    CHECK(pdr2Writes[0] == 0x10);
    saturn->mainBus.Write<uint8>(kPDR1, 0x93);
    REQUIRE(pdr1Writes.size() == 2);
    CHECK(pdr1Writes[1] == 0x10);

    // smpc_hle_device::pdr1_r: (handler() & ~DDR) | readback, the readback being the written output pins plus bit 7
    CHECK(saturn->mainBus.Read<uint8>(kPDR1) == ((0x2B & ~0x1C) | 0x10 | 0x80));
    CHECK(pdr1Reads == 1);
    CHECK(saturn->mainBus.Read<uint8>(kPDR2) == ((0x35 & ~0x10) | 0x10 | 0x80));
    CHECK(pdr2Reads == 1);

    // the handler's input bits pass (bit 7 included), its bits on output pins do not
    saturn->mainBus.Write<uint8>(kPDR1, 0x00);
    pdr1Value = 0xFF;
    CHECK(saturn->mainBus.Read<uint8>(kPDR1) == 0xE3);

    // the readback is taken at the write: a later DDR change does not alter it, but masks the handler's bits
    saturn->mainBus.Write<uint8>(kPDR1, 0xFF);
    pdr1Value = 0xFF;
    saturn->mainBus.Write<uint8>(kDDR1, 0x7F);
    CHECK(saturn->mainBus.Read<uint8>(kPDR1) == (0x80 | 0x1C));
    // DDR bit 7 is not a pin
    saturn->mainBus.Write<uint8>(kDDR1, 0xFF);
    saturn->mainBus.Write<uint8>(kPDR1, 0xFF);
    CHECK(pdr1Writes.back() == 0x7F);
    pdr1Value = 0x00;
    CHECK(saturn->mainBus.Read<uint8>(kPDR1) == 0xFF);

    // a read does not write the pins again
    const size_t writes1 = pdr1Writes.size();
    const size_t writes2 = pdr2Writes.size();
    saturn->mainBus.Read<uint8>(kPDR1);
    saturn->mainBus.Read<uint8>(kPDR2);
    CHECK(pdr1Writes.size() == writes1);
    CHECK(pdr2Writes.size() == writes2);

    // peeks and pokes have no side effects: they see the readback
    const int readsBefore = pdr1Reads + pdr2Reads;
    CHECK(saturn->mainBus.Peek<uint8>(kPDR2) == (0x10 | 0x80));
    saturn->mainBus.Poke<uint8>(kPDR1, 0x04);
    CHECK(saturn->mainBus.Peek<uint8>(kPDR1) == 0x04);
    CHECK(pdr1Reads + pdr2Reads == readsBefore);
    CHECK(pdr1Writes.size() == writes1);

    // turning ST-V mode off restores the Saturn port behaviour (the pad answers, the handlers are left alone)
    saturn->SMPC.SetSTVMode(false);
    SetUpPads(*saturn);
    saturn->mainBus.Write<uint8>(kPDR1, 0x60);
    saturn->mainBus.Write<uint8>(kPDR2, 0x60);
    CHECK(saturn->mainBus.Read<uint8>(kPDR1) == 0x7C);
    CHECK(saturn->mainBus.Read<uint8>(kPDR2) == 0x7C);
    CHECK(pdr1Writes.size() == writes1);
    CHECK(pdr1Reads + pdr2Reads == readsBefore);
}
TEST_CASE("stv hooks: ST-V mode without handlers keeps the Saturn ports", "[stv][smpc]") {
    auto saturn = std::make_unique<Saturn>();
    SetUpPads(*saturn);
    saturn->SMPC.SetSTVMode(true);
    saturn->mainBus.Write<uint8>(kPDR1, 0x60);
    saturn->mainBus.Write<uint8>(kPDR2, 0x60);
    CHECK(saturn->mainBus.Read<uint8>(kPDR1) == 0x7C);
    CHECK(saturn->mainBus.Read<uint8>(kPDR2) == 0x7C);
}

TEST_CASE("stv hooks: INTBACK OREG0 bit 7 follows SETSMEM in ST-V mode as on a Saturn", "[stv][smpc]") {
    // MAME's smpc_hle_device::resolve_intback: OREG0 bit 7 = SMEM[4] bit 7, 0 on a first boot, 0xFF after SETSMEM
    auto saturn = std::make_unique<Saturn>();
    CHECK(IntbackOREG0(*saturn) == 0x40); // STE false: the Saturn asks for the setup screen

    saturn->SMPC.SetSTVMode(true);
    CHECK(IntbackOREG0(*saturn) == 0x40);
    Command(*saturn, {0x01, 0x02, 0x03, 0x04}, 0x17); // SETSMEM
    CHECK(IntbackOREG0(*saturn) == 0xC0);

    saturn->SMPC.SetSTVMode(false);
    CHECK(IntbackOREG0(*saturn) == 0xC0);
}

TEST_CASE("stv hooks: ST-V mode survives a reset and is not part of any save state", "[stv][smpc]") {
    auto saturn = std::make_unique<Saturn>();
    saturn->SMPC.SetSTVMode(true);
    saturn->Reset(true);
    CHECK(saturn->SMPC.IsSTVMode());
    saturn->SMPC.Reset(false);
    CHECK(saturn->SMPC.IsSTVMode());

    // the SMPC state is the same bytes in both modes
    savestate::SMPCSaveState on;
    saturn->SMPC.SaveState(on);
    saturn->SMPC.SetSTVMode(false);
    savestate::SMPCSaveState off;
    saturn->SMPC.SaveState(off);
    CHECK(on.IREG == off.IREG);
    CHECK(on.OREG == off.OREG);
    CHECK(on.COMREG == off.COMREG);
    CHECK(on.SR == off.SR);
    CHECK(on.SF == off.SF);
    CHECK(on.PDR1 == off.PDR1);
    CHECK(on.PDR2 == off.PDR2);
    CHECK(on.DDR1 == off.DDR1);
    CHECK(on.DDR2 == off.DDR2);
    CHECK(on.IOSEL == off.IOSEL);
    CHECK(on.EXLE == off.EXLE);
    CHECK(on.busValue == off.busValue);
    CHECK(on.intback.report == off.intback.report);

    // loading a state does not change the mode either way
    saturn->SMPC.SetSTVMode(true);
    saturn->SMPC.LoadState(off);
    CHECK(saturn->SMPC.IsSTVMode());
    saturn->SMPC.SetSTVMode(false);
    saturn->SMPC.LoadState(on);
    CHECK_FALSE(saturn->SMPC.IsSTVMode());
}

TEST_CASE("stv hooks: SNDON and SNDOFF drive the 68K on a Saturn but not in ST-V mode", "[stv][smpc]") {
    constexpr uint8 kSNDON = 0x06;
    constexpr uint8 kSNDOFF = 0x07;
    auto m68kEnabled = [](Saturn &saturn) {
        auto state = std::make_unique<savestate::SCSPSaveState>();
        saturn.SCSP.SaveState(*state);
        return state->m68kEnabled;
    };

    auto saturn = std::make_unique<Saturn>();
    REQUIRE_FALSE(m68kEnabled(*saturn));

    Command(*saturn, {}, kSNDON);
    CHECK(m68kEnabled(*saturn));
    Command(*saturn, {}, kSNDOFF);
    CHECK_FALSE(m68kEnabled(*saturn));

    saturn->SMPC.SetSTVMode(true);
    Command(*saturn, {}, kSNDON);
    CHECK_FALSE(m68kEnabled(*saturn));
    saturn->SCSP.SetCPUEnabled(true); // what the PDR2 handler does
    Command(*saturn, {}, kSNDOFF);
    CHECK(m68kEnabled(*saturn));

    saturn->SMPC.SetSTVMode(false);
    Command(*saturn, {}, kSNDOFF);
    CHECK_FALSE(m68kEnabled(*saturn));
}

TEST_CASE("stv hooks: a hold-line IRL is taken once, a Saturn IRL until it is lowered", "[stv][sh2]") {
    // The slave runs `bra $` in high work RAM with every interrupt level open; its level-2 handler (vector 0x41)
    // counts in R14 and returns. The IRL is raised once and never lowered.
    constexpr uint32 kVBR = 0x0600'0000;
    constexpr uint32 kLoop = 0x0600'4000;
    constexpr uint32 kHandler = 0x0600'5000;
    const auto run = [&](bool holdLine) {
        auto saturn = std::make_unique<Saturn>();
        saturn->mainBus.Write<uint16>(kLoop + 0, 0xAFFE);     // bra $
        saturn->mainBus.Write<uint16>(kLoop + 2, 0x0009);     // nop
        saturn->mainBus.Write<uint16>(kLoop + 0x100, 0xAFFE); // the master's loop
        saturn->mainBus.Write<uint16>(kLoop + 0x102, 0x0009);
        saturn->mainBus.Write<uint16>(kHandler + 0, 0x7E01); // add #1, r14
        saturn->mainBus.Write<uint16>(kHandler + 2, 0x002B); // rte
        saturn->mainBus.Write<uint16>(kHandler + 4, 0x0009); // nop
        saturn->mainBus.Write<uint32>(kVBR + 0x41 * 4, kHandler);
        saturn->masterSH2.GetProbe().PC() = kLoop + 0x100;
        saturn->slaveSH2Enabled = true;
        auto &slave = saturn->slaveSH2.GetProbe();
        slave.PC() = kLoop;
        slave.VBR() = kVBR;
        slave.SR().ILevel = 0;
        slave.R(14) = 0;
        saturn->slaveSH2.SetIRLHoldLine(holdLine);
        saturn->slaveSH2.CbExtIntr(2, 0x41);
        RunFor(*saturn, 400);
        return slave.R(14);
    };

    CHECK(run(true) == 1);
    CHECK(run(false) > 5);
}

TEST_CASE("stv hooks: a clock change keeps the sound RAM in ST-V mode and clears it on a Saturn", "[stv][smpc]") {
    // The ST-V BIOS leaves the coin inputs at sound RAM 0x4F1 for the game across its CKCHG320 (bakubaku reads them
    // at 0x060024B0); MAME's clock change resets the SCSP device but leaves the sound RAM alone.
    constexpr uint32 kSoundRAM = 0x5A0'04F0;
    constexpr uint8 kCKCHG352 = 0x0E;
    constexpr uint8 kCKCHG320 = 0x0F;
    auto clockChange = [](Saturn &saturn, uint8 command) {
        Command(saturn, {}, command);
        RunFor(saturn, 250000); // the core runs SYSRES/CKCHG 200000 cycles after the write
        // the clock change's NMI lands in the idle BIOS's loop, so the CPU runs no garbage (which the core logs)
        const uint32 pc = saturn.masterSH2.GetProbe().PC() & 0x07FF'FFFF;
        CHECK(pc >= brimir::test::kIdleSTVBiosLoop);
        CHECK(pc < brimir::test::kIdleSTVBiosLoop + 4);
    };

    auto saturn = std::make_unique<Saturn>();
    std::vector<uint8> ipl(sys::kIPLSize);
    brimir::test::FillIdleSTVBiosImage(std::span<uint8, sys::kIPLSize>(ipl.data(), ipl.size()));
    saturn->LoadIPL(std::span<uint8, sys::kIPLSize>(ipl.data(), ipl.size()));
    saturn->Reset(true);
    saturn->mainBus.Write<uint16>(kSoundRAM, 0x01FF);
    clockChange(*saturn, kCKCHG320);
    CHECK(saturn->mainBus.Peek<uint16>(kSoundRAM) == 0x0000);

    saturn->SMPC.SetSTVMode(true);
    saturn->mainBus.Write<uint16>(kSoundRAM, 0x01FF);
    clockChange(*saturn, kCKCHG352);
    CHECK(saturn->mainBus.Peek<uint16>(kSoundRAM) == 0x01FF);
    clockChange(*saturn, kCKCHG320);
    CHECK(saturn->mainBus.Peek<uint16>(kSoundRAM) == 0x01FF);

    // a reset still clears it
    saturn->Reset(true);
    CHECK(saturn->mainBus.Peek<uint16>(kSoundRAM) == 0x0000);

    saturn->SMPC.SetSTVMode(false);
    saturn->mainBus.Write<uint16>(kSoundRAM, 0x01FF);
    clockChange(*saturn, kCKCHG352);
    CHECK(saturn->mainBus.Peek<uint16>(kSoundRAM) == 0x0000);
}
