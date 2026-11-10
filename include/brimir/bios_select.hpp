#pragma once

// BIOS choice for brimir_bios = auto: the BIOS of the disc's region, else any available one.
//
// The disc's region comes from the area codes in its header: a disc for Japan only ("J"; with the Asia NTSC codes, "JT"
// or "JTK") gets the Japanese BIOS; a disc that also lists a western region (U, B, E, A or L: "JTUE", "JUE") or no
// Japan at all gets the US/EU one. No content or an unreadable disc keeps the 0.7.0 order (Japanese first).
// The SMPC area code stays region autodetection's (brimir_autodetect_region), which can differ from the BIOS region
// on a multi-region disc; the BIOS checks the disc against that area code, not against its own region.
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

} // namespace brimir
