#pragma once

// Saturn Pro Action Replay codes, as RetroArch's Saturn cheat database stores them (retro_cheat_set). A code is one or
// more sub-codes "TAAAAAAA VVVV" (T: code type, AAAAAAA: address, VVVV: value), written as 12 hex digits
// ("16033c7400c0"), with spaces ("1601dba4 0900") or with the address and the value as separate '+'-joined parts
// ("D6033C60+270F"); several sub-codes are joined with '+'. Code types:
//
//   1AAAAAAA VVVV   16-bit write of VVVV at AAAAAAA
//   3AAAAAAA 00VV   8-bit write of VV at AAAAAAA (the value's high byte is not used)
//   DAAAAAAA VVVV   if the 16-bit word at AAAAAAA equals VVVV, the next sub-code of the same cheat applies
//   F... / B...     master (enable) codes for the real cartridge's hooks: accepted, nothing to do here
//   0AAAAAAA VVVV   a plain Saturn address with no type digit (a few database files): a 16-bit write, as type 1
//                   (and as Beetle Saturn's raw-address form)
//
// Any other type, and sub-codes the database mangled (template values like "00??" meant to be edited, addresses with
// 7 or 9 digits, numbers a spreadsheet rewrote as decimals), parse as Ignored with the reason, so one bad sub-code
// never stops a whole file. A code with characters that cannot appear in any of these forms is not a code (nullopt).
//
// Addresses go through the SH-2 bus's 27-bit mask; Work RAM Low is 0x00200000-0x002FFFFF, Work RAM High
// 0x06000000-0x07FFFFFF (1 MiB mirrored). Anything else is skipped (a conditional there is false). 16-bit accesses use
// the even address below an odd one, as the bus does. Values are written in Saturn (big-endian) byte order.
//
// The engine applies the enabled cheats once per frame in cheat index order, at the same point every frame (before
// emulation; hidden run-ahead frames included), so it stays deterministic under save states, rewind and run-ahead.
// Cheats are frontend state: they are not in save states.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace brimir {

struct CheatOp {
    enum class Kind { Write8, Write16, IfEqual16, Ignored };
    // Why a sub-code is Ignored (None for the other kinds).
    enum class IgnoreReason : uint8_t { None, MasterCode, UnknownType, Malformed };
    Kind kind;
    uint32_t address; // Write8/Write16/IfEqual16: the Saturn address; Ignored: the type digit and address if readable
    uint16_t value;   // as written (Write8 uses the low byte)
    IgnoreReason reason = IgnoreReason::None;
};

std::optional<std::vector<CheatOp>> ParseCheat(std::string_view code);

// Whether a write/compare at this Saturn address reaches work RAM (after the bus mask and alignment).
bool CheatAddressInWorkRam(uint32_t address);

class CheatEngine {
public:
    static constexpr size_t kHalfSize = 0x100000;

    // Replaces cheat `index`. A code that does not parse leaves the slot empty.
    void Set(unsigned index, bool enabled, std::string_view code);
    void Set(unsigned index, bool enabled, std::vector<CheatOp> ops);
    void Reset();

    // Applies the enabled cheats to work RAM (Saturn byte order).
    void Apply(std::span<uint8_t, kHalfSize> low, std::span<uint8_t, kHalfSize> high) const;

    // Enabled cheats with at least one op that touches work RAM.
    size_t ActiveCount() const;

private:
    struct Op {
        CheatOp::Kind kind;
        uint8_t half;    // 0: Low, 1: High, 2: outside work RAM
        uint32_t offset; // in the half (even for 16-bit ops)
        uint16_t value;
    };
    struct Cheat {
        bool enabled = false;
        bool touchesRam = false;
        std::vector<Op> ops;
    };
    std::map<unsigned, Cheat> m_cheats;
};

} // namespace brimir
