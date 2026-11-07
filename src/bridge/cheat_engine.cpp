// Brimir - Sega Saturn libretro core based on Ymir
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "brimir/cheat_engine.hpp"

#include <algorithm>
#include <string>

namespace brimir {

namespace {

// The SH-2 bus decodes 27 address bits (ymir::sys::Bus::kAddressMask).
constexpr uint32_t kBusMask = 0x07FF'FFFF;
constexpr uint32_t kLowBase = 0x0020'0000;
constexpr uint32_t kHighBase = 0x0600'0000;
constexpr uint32_t kHighEnd = 0x0800'0000; // 1 MiB mirrored up to here

int HexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// The hex number in `text` (1 to 8 digits), or nullopt.
std::optional<uint32_t> ParseHex(std::string_view text) {
    if (text.empty() || text.size() > 8) {
        return std::nullopt;
    }
    uint32_t value = 0;
    for (const char c : text) {
        const int digit = HexDigit(c);
        if (digit < 0) {
            return std::nullopt;
        }
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return value;
}

CheatOp Ignored(CheatOp::IgnoreReason reason, uint32_t address = 0, uint16_t value = 0) {
    return CheatOp{CheatOp::Kind::Ignored, address, value, reason};
}

// One sub-code: an 8-digit type + address and a value of 1 to 4 digits.
CheatOp DecodeSubCode(std::string_view address, std::string_view value) {
    const std::optional<uint32_t> a = address.size() == 8 ? ParseHex(address) : std::nullopt;
    const std::optional<uint32_t> v = value.size() <= 4 ? ParseHex(value) : std::nullopt;
    if (!a || !v) {
        return Ignored(CheatOp::IgnoreReason::Malformed);
    }
    const uint32_t target = *a & 0x0FFF'FFFF;
    const auto v16 = static_cast<uint16_t>(*v);
    switch (*a >> 28) {
    case 0x0: // a plain Saturn address without a type digit (Daytona USA, Panzer Dragoon demo): written as type 1
        return CheatOp{CheatOp::Kind::Write16, target, v16, CheatOp::IgnoreReason::None, true};
    case 0x1: return CheatOp{CheatOp::Kind::Write16, target, v16};
    case 0x3: return CheatOp{CheatOp::Kind::Write8, target, v16};
    case 0xD: return CheatOp{CheatOp::Kind::IfEqual16, target, v16};
    case 0xF:
    case 0xB: return Ignored(CheatOp::IgnoreReason::MasterCode, *a, v16);
    default: return Ignored(CheatOp::IgnoreReason::UnknownType, *a, v16);
    }
}

// Half (0 Low, 1 High, 2 neither) and offset of a work RAM access.
void Locate(uint32_t address, bool word, uint8_t &half, uint32_t &offset) {
    address &= kBusMask;
    if (word) {
        address &= ~1u;
    }
    if (address >= kLowBase && address < kLowBase + CheatEngine::kHalfSize) {
        half = 0;
        offset = address - kLowBase;
    } else if (address >= kHighBase && address < kHighEnd) {
        half = 1;
        offset = address & (CheatEngine::kHalfSize - 1);
    } else {
        half = 2;
        offset = 0;
    }
}

} // namespace

std::optional<std::vector<CheatOp>> ParseCheat(std::string_view code) {
    // Separators inside a sub-code (spaces, tabs, colons, line ends) carry no meaning: the parts are re-paired by
    // length below. Besides hex digits, '?' (template values) and '.' (codes a spreadsheet turned into decimals) occur
    // in the database; they make that sub-code Ignored. Anything else means this is not an Action Replay code.
    std::string compact;
    compact.reserve(code.size());
    for (const char c : code) {
        if (c == ' ' || c == '\t' || c == ':' || c == '\r' || c == '\n') {
            continue;
        }
        if (HexDigit(c) < 0 && c != '?' && c != '.' && c != '+') {
            return std::nullopt;
        }
        compact += c;
    }

    std::vector<std::string_view> parts;
    const std::string_view text = compact;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('+', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (end > start) {
            parts.push_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    if (parts.empty()) {
        return std::nullopt;
    }

    std::vector<CheatOp> ops;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string_view part = parts[i];
        if (part.size() % 12 == 0) {
            // One 12-digit sub-code, or several typed with spaces only between them.
            for (size_t at = 0; at < part.size(); at += 12) {
                ops.push_back(DecodeSubCode(part.substr(at, 8), part.substr(at + 8, 4)));
            }
        } else if (i + 1 < parts.size() && parts[i + 1].size() <= 4) {
            // An address part followed by its value part.
            ops.push_back(DecodeSubCode(part, parts[i + 1]));
            ++i;
        } else {
            ops.push_back(Ignored(CheatOp::IgnoreReason::Malformed));
        }
    }
    return ops;
}

bool CheatAddressInWorkRam(uint32_t address) {
    uint8_t half = 0;
    uint32_t offset = 0;
    Locate(address, false, half, offset);
    return half != 2;
}

void CheatEngine::Set(unsigned index, bool enabled, std::string_view code) {
    std::optional<std::vector<CheatOp>> ops = ParseCheat(code);
    if (!ops) {
        Erase(index);
        return;
    }
    Set(index, enabled, std::move(*ops));
}

void CheatEngine::Set(unsigned index, bool enabled, std::vector<CheatOp> ops) {
    Cheat cheat;
    cheat.enabled = enabled;
    const bool malformed = std::any_of(ops.begin(), ops.end(), [](const CheatOp &op) {
        return op.kind == CheatOp::Kind::Ignored && op.reason == CheatOp::IgnoreReason::Malformed;
    });
    if (malformed) {
        m_cheats[index] = std::move(cheat); // inert
        return;
    }
    cheat.ops.reserve(ops.size());
    for (const CheatOp &op : ops) {
        Op compiled{op.kind, 2, 0, op.value};
        if (op.kind != CheatOp::Kind::Ignored) {
            Locate(op.address, op.kind != CheatOp::Kind::Write8, compiled.half, compiled.offset);
            if (compiled.half != 2 && op.kind != CheatOp::Kind::IfEqual16) {
                cheat.touchesRam = true;
            }
        }
        cheat.ops.push_back(compiled);
    }
    m_cheats[index] = std::move(cheat);
}

void CheatEngine::Erase(unsigned index) {
    m_cheats.erase(index);
}

void CheatEngine::Reset() {
    m_cheats.clear();
}

void CheatEngine::Apply(std::span<uint8_t, kHalfSize> low, std::span<uint8_t, kHalfSize> high) const {
    for (const auto &[index, cheat] : m_cheats) {
        if (!cheat.enabled) {
            continue;
        }
        bool skipNext = false;
        for (const Op &op : cheat.ops) {
            if (skipNext) {
                skipNext = false;
                continue;
            }
            uint8_t *mem = op.half == 0 ? low.data() : op.half == 1 ? high.data() : nullptr;
            switch (op.kind) {
            case CheatOp::Kind::Write8:
                if (mem != nullptr) {
                    mem[op.offset] = static_cast<uint8_t>(op.value);
                }
                break;
            case CheatOp::Kind::Write16:
                if (mem != nullptr) {
                    mem[op.offset] = static_cast<uint8_t>(op.value >> 8);
                    mem[op.offset + 1] = static_cast<uint8_t>(op.value);
                }
                break;
            case CheatOp::Kind::IfEqual16:
                // Outside work RAM nothing is known to be equal: the next sub-code does not apply.
                skipNext = mem == nullptr || ((mem[op.offset] << 8) | mem[op.offset + 1]) != op.value;
                break;
            case CheatOp::Kind::Ignored: break;
            }
        }
    }
}

size_t CheatEngine::ActiveCount() const {
    size_t count = 0;
    for (const auto &[index, cheat] : m_cheats) {
        if (cheat.enabled && cheat.touchesRam) {
            ++count;
        }
    }
    return count;
}

} // namespace brimir
