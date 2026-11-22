// Brimir - ST-V content detection
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Recognizes ST-V ROM sets by the contents of a zip, never by name or extension alone.

#pragma once

#include "stv_game_db.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace brimir::stv {

enum class DetectKind {
    NotZip,     // not a .zip file, or not a readable zip
    NotSTV,     // a zip that is not (a part of) a supported ST-V set
    Incomplete, // a supported set with files missing or failing their CRC-32; game and missing are set
    Known,      // every ROM of one supported game is present; game is set
};

struct DetectResult {
    DetectKind kind = DetectKind::NotZip;
    const GameInfo *game = nullptr;
    std::vector<std::string> missing; // ROM file names (database spelling) that are absent or have the wrong CRC-32
};

DetectResult DetectSTVContent(const std::filesystem::path &path);

} // namespace brimir::stv
