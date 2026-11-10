// Brimir - Sega Saturn libretro core based on Ymir
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "brimir/bios_select.hpp"

#include <array>

namespace brimir {

namespace {

struct BiosFile {
    const char* name;
    DiscRegion region; // by name
};

// The 0.7.0 order. sega_101.bin and sega_100.bin are the Japanese v1.01 and v1.00 under Mednafen's names.
// Note: sega1003.bin (JP v1.003) is NOT included - it has compatibility issues
constexpr std::array<BiosFile, 6> kBiosFiles = {{
    {"Sega Saturn BIOS v1.01 (JAP).bin", DiscRegion::Japan},
    {"Sega Saturn BIOS v1.00 (JAP).bin", DiscRegion::Japan},
    {"sega_101.bin", DiscRegion::Japan},
    {"mpr-17933.bin", DiscRegion::UsEu},
    {"sega_100.bin", DiscRegion::Japan},
    {"Sega Saturn BIOS (EUR).bin", DiscRegion::UsEu},
}};

// ymir::media::AreaCode bits
constexpr uint16_t kAreaJapan = 1u << 0x1; // J
constexpr uint16_t kAreaWest = (1u << 0x4)  // U North America
                             | (1u << 0x5)  // B Central/South America NTSC
                             | (1u << 0xA)  // A Asia PAL
                             | (1u << 0xC)  // E Europe PAL
                             | (1u << 0xD); // L Central/South America PAL

} // namespace

DiscRegion DiscRegionFromAreaCodes(uint16_t areaCodes) {
    if (areaCodes == 0) {
        return DiscRegion::Unknown;
    }
    return (areaCodes & kAreaJapan) && !(areaCodes & kAreaWest) ? DiscRegion::Japan : DiscRegion::UsEu;
}

std::vector<std::string> BiosCandidatesForRegion(DiscRegion region) {
    std::vector<std::string> candidates;
    candidates.reserve(kBiosFiles.size());
    if (region != DiscRegion::Unknown) {
        for (const BiosFile& file : kBiosFiles) {
            if (file.region == region) {
                candidates.emplace_back(file.name);
            }
        }
    }
    for (const BiosFile& file : kBiosFiles) {
        if (region == DiscRegion::Unknown || file.region != region) {
            candidates.emplace_back(file.name);
        }
    }
    return candidates;
}

DiscRegion BiosRegionFromFileName(const std::string& fileName) {
    for (const BiosFile& file : kBiosFiles) {
        if (fileName == file.name) {
            return file.region;
        }
    }
    return DiscRegion::Unknown;
}

std::optional<std::string> ChooseAutoBios(DiscRegion region, const BiosIdentify& identify) {
    std::optional<std::string> fallback;
    for (const std::string& name : BiosCandidatesForRegion(region)) {
        const std::optional<DiscRegion> identified = identify(name);
        if (!identified) {
            continue;
        }
        if (region == DiscRegion::Unknown) {
            return name;
        }
        const DiscRegion biosRegion = *identified != DiscRegion::Unknown ? *identified : BiosRegionFromFileName(name);
        if (biosRegion == region) {
            return name;
        }
        if (!fallback) {
            fallback = name;
        }
    }
    return fallback;
}

} // namespace brimir
