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

// Crypt315_5881: MAME's install_common_protection (cryptKey is the game's key); Comp315_5838: init_decathlt
enum class Chip : uint8_t { None, Crypt315_5881, Comp315_5838 };

// A fixed transform of every cartridge byte, applied once to the built image (MAME's driver init):
//   SanJeon  init_sanjeon: each byte XOR 0xFF, then a sequence of bit swaps
enum class Scramble : uint8_t { None, SanJeon };

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

// The set's ROM_REGION16_BE( 0x80, "eeprom", 0 ): the 93C46's contents on a new cabinet (MAME's
// eeprom_base_device::nvram_default copies the region when there is no NVRAM file). The file is 128 bytes, the 64
// cells big-endian. A set without one starts with every cell 0xFFFF.
struct DefaultEEPROM {
    const char *file = nullptr;
    uint32_t crc32 = 0;
};

struct GameInfo {
    const char *shortName; // MAME set name, e.g. "bakubaku"; also the save-state identity
    const char *title;
    std::span<const Region> regions; // MAME order
    Controls controls;
    Chip chip;
    uint32_t cryptKey;
    uint16_t rotation; // degrees (0, 90, 180, 270): the libretro rotation the game needs to appear upright
    std::span<const RomEntry> roms;
    DefaultEEPROM eeprom = {};
    Scramble scramble = Scramble::None;
};

std::span<const GameInfo> AllGames();
const GameInfo *FindGame(std::string_view shortName);

} // namespace brimir::stv
