// Brimir - ST-V BIOS selection implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_bios.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace brimir::stv {

namespace {

constexpr size_t kBiosSize = 512 * 1024;

// The ST-V BIOS files by region, transcribed from MAME's "stvbios" ROM set (src/mame/sega/stv.cpp, the STV_BIOS
// macro: ROM_SYSTEM_BIOS / ROM_LOAD16_WORD_SWAP_BIOS entries). Within a region the files are in MAME's order, its
// default (newest) first. The second name of a pair is the one older MAME versions used for the same dump (same
// CRC-32): 20091.bin = epr-20091.ic8, epr19730.ic8 = epr-19730.ic8, mp17951a.s = epr-17951a.ic8,
// mp17952a.s = epr-17952a.ic8, mp17953a.ic8 = epr-17953a.ic8, mp17954a.s = epr-17954a.ic8.
// MAME's Taiwan BIOS also serves the Philippines and Asia, so Asia shares the Taiwan files. MAME has no BIOS dump
// for Korea, Brazil or Latin America. MAME's two development BIOSes (stv110.bin, stv1061.bin) belong to no region
// and are not used.
constexpr const char *kJapan[] = {"epr-23603.ic8", "epr-20091.ic8",  "20091.bin",   "epr-19730.ic8",
                                  "epr19730.ic8",  "epr-17951a.ic8", "mp17951a.s", "epr-17740a.ic8",
                                  "epr-17740.ic8"};
constexpr const char *kUSA[] = {"epr-17952a.ic8", "mp17952a.s", "epr-17741a.ic8"};
constexpr const char *kEurope[] = {"epr-17954a.ic8", "mp17954a.s"};
constexpr const char *kTaiwan[] = {"epr-19854.ic8", "epr-17953a.ic8", "mp17953a.ic8", "epr-17742a.ic8"};

std::span<const char *const> BiosNames(Region region) {
    switch (region) {
    case Region::Japan: return kJapan;
    case Region::USA: return kUSA;
    case Region::Europe: return kEurope;
    case Region::Taiwan:
    case Region::Asia: return kTaiwan;
    case Region::Korea:
    case Region::Brazil:
    case Region::LatinAmerica: break;
    }
    return {};
}

const char *RegionName(Region region) {
    switch (region) {
    case Region::Japan: return "Japan";
    case Region::USA: return "USA";
    case Region::Europe: return "Europe";
    case Region::Taiwan: return "Taiwan";
    case Region::Asia: return "Asia";
    case Region::Korea: return "Korea";
    case Region::Brazil: return "Brazil";
    case Region::LatinAmerica: return "Latin America";
    }
    return "?";
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return text;
}

// The regular files of a folder by lower-case name, so that "EPR-20091.IC8" matches on case-sensitive file systems
std::map<std::string, std::filesystem::path> ListFiles(const std::filesystem::path &dir) {
    std::map<std::string, std::filesystem::path> files;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code statEc;
        if (!it->is_regular_file(statEc)) continue;
        files.emplace(Lower(it->path().filename().string()), it->path());
    }
    return files;
}

std::optional<std::filesystem::path> FindBios(const std::map<std::string, std::filesystem::path> &files,
                                              Region region) {
    for (const char *name : BiosNames(region)) {
        const auto it = files.find(name);
        if (it != files.end()) return it->second;
    }
    return std::nullopt;
}

bool Supports(const GameInfo &game, Region region) {
    return std::find(game.regions.begin(), game.regions.end(), region) != game.regions.end();
}

std::optional<Region> PreferredRegion(std::string_view preference) {
    if (preference.starts_with("jp_")) return Region::Japan;
    if (preference.starts_with("us_")) return Region::USA;
    if (preference.starts_with("eu_")) return Region::Europe;
    return std::nullopt;
}

std::string Join(const std::vector<std::string> &items) {
    std::string text;
    for (const std::string &item : items) {
        if (!text.empty()) text += ", ";
        text += item;
    }
    return text;
}

} // namespace

std::optional<BiosChoice> SelectSTVBios(const std::filesystem::path &systemDir, const GameInfo &game,
                                        std::string_view preference, std::string &error) {
    struct Candidate {
        Region region;
        std::string reason;
    };
    std::vector<Candidate> order;
    const auto add = [&](Region region, const std::string &reason) {
        if (!Supports(game, region)) return;
        const bool seen = std::any_of(order.begin(), order.end(), [&](const Candidate &c) { return c.region == region; });
        if (!seen) order.push_back({region, reason});
    };

    if (const auto preferred = PreferredRegion(preference)) {
        add(*preferred, "the " + std::string(preference) + " preference");
    }
    for (Region region : {Region::Japan, Region::USA, Region::Europe}) {
        add(region, "the default order (Japan, USA, Europe)");
    }
    for (Region region : game.regions) {
        add(region, std::string("one of the game's other regions (MAME order)"));
    }

    const auto files = ListFiles(systemDir);
    for (const Candidate &candidate : order) {
        if (const auto path = FindBios(files, candidate.region)) {
            return BiosChoice{*path, candidate.region,
                              std::string(RegionName(candidate.region)) + " BIOS, chosen by " + candidate.reason};
        }
    }

    std::vector<std::string> accepted;
    std::vector<std::string> regionNames;
    for (Region region : game.regions) {
        regionNames.emplace_back(RegionName(region));
        for (const char *name : BiosNames(region)) {
            if (std::find(accepted.begin(), accepted.end(), name) == accepted.end()) accepted.emplace_back(name);
        }
    }

    error = std::string("No ST-V BIOS for ") + game.title + " (" + game.shortName + ") in " + systemDir.string() +
            ". It needs a BIOS for one of its regions: " + Join(regionNames) + ". Accepted file names: " + (accepted.empty() ? std::string("none known") : Join(accepted)) + ".";

    // Say what was found but is not used, so a wrong-region BIOS is never mistaken for a working one
    std::vector<std::string> unused;
    for (Region region : {Region::Japan, Region::USA, Region::Europe, Region::Taiwan}) {
        if (Supports(game, region) || (region == Region::Taiwan && Supports(game, Region::Asia))) continue;
        if (const auto path = FindBios(files, region)) {
            unused.push_back(path->filename().string() + " (" + RegionName(region) + ")");
        }
    }
    if (!unused.empty()) {
        error += " Found, but for a region this game does not run on: " + Join(unused) + ".";
    }
    return std::nullopt;
}

bool LoadSTVBiosImage(const std::filesystem::path &path, std::array<uint8_t, kBiosSize> &out, std::string &error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "Cannot open the ST-V BIOS " + path.string();
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size != static_cast<std::streamoff>(kBiosSize)) {
        error = "The ST-V BIOS " + path.filename().string() + " is " + std::to_string(size) +
                " bytes, expected " + std::to_string(kBiosSize);
        return false;
    }
    file.seekg(0);
    file.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(kBiosSize));
    if (!file) {
        error = "Cannot read the ST-V BIOS " + path.string();
        return false;
    }
    for (size_t i = 0; i < kBiosSize; i += 2) std::swap(out[i], out[i + 1]);
    return true;
}

} // namespace brimir::stv
