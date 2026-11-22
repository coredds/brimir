// Brimir - ST-V BIOS selection
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Picks the ST-V BIOS file for a game by the regions the game supports, and loads it as the SH-2 sees it.

#pragma once

#include "stv_game_db.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace brimir::stv {

struct BiosChoice {
    std::filesystem::path path;
    Region region;
    std::string reason; // which rule picked this BIOS, for the log
};

// Looks in systemDir for an ST-V BIOS file (MAME's names, matched without regard to case). Regions are tried in this
// order, and the first whose BIOS file exists wins: the preference's region if the game supports it, then Japan, USA
// and Europe (those the game supports), then the game's remaining regions in MAME order. A BIOS of a region the game
// does not support is never chosen.
// preference is the brimir_bios option value ("auto", "jp_101", "jp_100", "us_101", "us_100", "eu_100", "eu_alt"); the
// prefix picks the region, anything else (including "auto") expresses no preference.
// On failure returns nullopt with a message in error naming the accepted file names.
std::optional<BiosChoice> SelectSTVBios(const std::filesystem::path &systemDir, const GameInfo &game,
                                        std::string_view preference, std::string &error);

// Reads a 512 KiB BIOS dump and byte-swaps its 16-bit words (the dumps are word-swapped, MAME's
// ROM_LOAD16_WORD_SWAP_BIOS), giving the image as the SH-2 sees it from address 0.
bool LoadSTVBiosImage(const std::filesystem::path &path, std::array<uint8_t, 512 * 1024> &out, std::string &error);

} // namespace brimir::stv
