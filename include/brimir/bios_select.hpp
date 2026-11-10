#pragma once

// BIOS choice for brimir_bios = auto: the BIOS of the disc's region, else any available one.
//
// The disc's region comes from the area codes in its header: a disc that lists only Japanese-hardware regions (J, T Asia
// NTSC, K Korea: "J", "JT", "T") gets the Japanese BIOS; a disc that lists a western region (U, B, E, A or L: "TU",
// "JTU", "JTUE") gets the US/EU one. No content or an unreadable disc keeps the 0.7.0 order (Japanese first).
// Region autodetection (brimir_autodetect_region) then picks the SMPC area code in the order of the loaded BIOS's
// region (PreferredAreaOrder), so a multi-region disc runs in the BIOS's region.
//
// File names do not say a BIOS's region reliably (sega_101.bin and sega_100.bin are Japanese dumps under Mednafen's
// names), so the files are identified by hash (Ymir's IPL ROM database) as they are tried; only a file the database
// does not know is taken at its name's word.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace brimir {

enum class DiscRegion {
    Unknown, // no content, unreadable header, no area code
    Japan,
    UsEu,
};

// The region a disc header's compatible area codes (ymir::media::AreaCode bitmap; 0 = none) select.
DiscRegion DiscRegionFromAreaCodes(uint16_t areaCodes);

// The BIOS files brimir_bios = auto tries, in order: the region's files first, then the others. Unknown: the 0.7.0 order.
std::vector<std::string> BiosCandidatesForRegion(DiscRegion region);

// The region a BIOS file name suggests (used only for files the IPL ROM database does not identify).
DiscRegion BiosRegionFromFileName(const std::string& fileName);

// Loads a candidate: nullopt when it cannot be loaded, else the region the database gives it (Unknown if not listed).
using BiosIdentify = std::function<std::optional<DiscRegion>(const std::string& fileName)>;

// The file to use for `region`: the first candidate of the region (by hash, else by name); none of them available:
// the first candidate that loads; nothing loads: nullopt. Unknown region: the first candidate that loads.
std::optional<std::string> ChooseAutoBios(DiscRegion region, const BiosIdentify& identify);

// Region autodetection's preferred SMPC area codes (Japan 0x1, Asia NTSC 0x2, North America 0x4, Europe PAL 0xC) for a
// BIOS of `biosRegion`: its own regions first. Unknown: Ymir's default order (North America, Japan, Europe, Asia NTSC).
// A disc listing none of them gets the first area code it lists (Saturn::AutodetectRegion).
std::vector<uint8_t> PreferredAreaOrder(DiscRegion biosRegion);

// Whether retro_load_game chooses the BIOS: always for brimir_bios = auto (nullptr: the default, auto), so each content
// gets its region's BIOS; an explicit choice only while no BIOS is loaded (as in 0.7.0).
bool ShouldSelectBios(const char* biosOption, bool iplLoaded);

} // namespace brimir
