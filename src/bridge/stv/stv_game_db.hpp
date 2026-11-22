// Brimir - ST-V game database
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// The supported ST-V games, transcribed from MAME's src/mame/sega/stv.cpp (ROM_START blocks, GAME lines, input ports
// and the 315-5881 key). Short names are MAME set names.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace brimir::stv {

enum class Region : uint8_t { Japan, USA, Europe, Taiwan, Asia, Korea, Brazil, LatinAmerica };

enum class Controls : uint8_t { ThreeButtons, SixButtons, RadiantSilvergun };

enum class Chip : uint8_t { None, Crypt315_5881 };

// MAME's ROM loading macros, as used by the supported sets:
//   WordSwap  ROM_LOAD16_WORD_SWAP
//   Byte16    ROM_LOAD16_BYTE
//   Plain     ROM_LOAD, and ROM_RELOAD_PLAIN (the previous file again, loaded without the previous entry's flags)
//   Reload    ROM_RELOAD (the previous file again, at a new offset, with the previous entry's flags)
// Offsets and sizes are MAME's, unchanged. A repeated file carries the same file name and CRC-32 as the entry it
// repeats.
enum class Load : uint8_t { WordSwap, Byte16, Plain, Reload };

struct RomEntry {
    const char *file;
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    Load load;
};

struct GameInfo {
    const char *shortName; // MAME set name, e.g. "bakubaku"; also the save-state identity
    const char *title;
    std::span<const Region> regions; // MAME order
    Controls controls;
    Chip chip;
    uint32_t cryptKey;
    bool rotated;
    std::span<const RomEntry> roms;
};

std::span<const GameInfo> AllGames();
const GameInfo *FindGame(std::string_view shortName);

} // namespace brimir::stv
