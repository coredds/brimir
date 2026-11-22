// Brimir - ST-V content detection implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_detect.hpp"

#include "stv_path.hpp"
#include "stv_zip.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace brimir::stv {

namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

struct Match {
    size_t total = 0;   // distinct ROM files of the game
    size_t present = 0; // of those, found in the zip by name
    std::vector<std::string> missing; // absent, or present with a different CRC-32
};

Match MatchGame(const GameInfo &game, const ZipReader &zip) {
    Match match;
    std::set<std::string> seen;
    for (const RomEntry &rom : game.roms) {
        // A repeated file (ROM_RELOAD) is the same zip entry
        if (!seen.insert(rom.file).second) continue;
        match.total++;
        if (!zip.HasEntry(rom.file)) {
            match.missing.emplace_back(rom.file);
            continue;
        }
        match.present++;
        const auto crc = zip.EntryCRC(rom.file);
        if (crc && *crc != rom.crc32) match.missing.emplace_back(rom.file);
    }
    return match;
}

} // namespace

DetectResult DetectSTVContent(const std::filesystem::path &path) {
    DetectResult result;

    if (Lower(PathToUtf8(path.extension())) != ".zip") return result;
    const ZipReader zip(path);
    if (!zip.IsOpen()) {
        result.kind = DetectKind::Unreadable;
        return result;
    }

    result.kind = DetectKind::NotSTV;

    const std::string stem = Lower(PathToUtf8(path.stem()));
    const GameInfo *named = nullptr;      // the game the zip is named after
    const GameInfo *closest = nullptr;    // the game with the most files present
    Match namedMatch, closestMatch;

    for (const GameInfo &game : AllGames()) {
        Match match = MatchGame(game, zip);
        if (match.missing.empty()) {
            result.kind = DetectKind::Known;
            result.game = &game;
            return result;
        }
        if (stem == game.shortName) {
            named = &game;
            namedMatch = match;
        }
        if (match.present > closestMatch.present) {
            closest = &game;
            closestMatch = std::move(match);
        }
    }

    // Not complete: report the game the zip is named after, else the one it holds at least half of
    if (named != nullptr) {
        result.kind = DetectKind::Incomplete;
        result.game = named;
        result.missing = std::move(namedMatch.missing);
    } else if (closest != nullptr && closestMatch.present * 2 >= closestMatch.total) {
        result.kind = DetectKind::Incomplete;
        result.game = closest;
        result.missing = std::move(closestMatch.missing);
    }
    return result;
}

} // namespace brimir::stv
