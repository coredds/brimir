// Brimir - ST-V cartridge image builder
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Builds the cartridge image from a MAME ROM set, following MAME's ROM loading macros.

#pragma once

#include "stv_game_db.hpp"
#include "stv_zip.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace brimir::stv {

// Builds the cartridge as the SH-2 sees it from 0x02000000: byte i of the image is the byte at 0x02000000 + i, in the
// SH-2's big-endian order, which is the order STVGameROMCartridge::LoadROM expects. Gaps are 0x00 (MAME's ROMREGION_ERASE00).
// The image is the highest mapped byte rounded up to a power of two, at most ymir::cart::kSTVGameROMMaxSize.
// On failure returns false with a message in error naming the missing or mis-sized files; image is then unspecified.
bool BuildCartImage(ZipReader &zip, const GameInfo &game, std::vector<uint8_t> &image, std::string &error);

// The same for a bare entry list (used by the tests).
bool BuildCartImage(ZipReader &zip, std::span<const RomEntry> roms, std::vector<uint8_t> &image, std::string &error);

// The 93C46's contents on a new cabinet (64 cells, big-endian, as STVIOBoard::LoadEEPROM takes them): the set's
// default EEPROM image (GameInfo::eeprom), or every cell 0xFFFF when it has none. False with a message in error when
// the image is missing or not 128 bytes.
bool ReadDefaultEEPROM(ZipReader &zip, const GameInfo &game, std::array<uint8_t, 128> &cells, std::string &error);

} // namespace brimir::stv
