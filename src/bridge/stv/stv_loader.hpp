// Brimir - ST-V ROM file loader (multi-file MAME ROM set support)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#pragma once

#include "stv_game_db.hpp"

#include <ymir/core/types.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace brimir::stv {

struct STVLoadResult {
    bool succeeded = false;
    std::string errorMessage;
};

STVLoadResult LoadSTVGameROM(const std::filesystem::path &romPath,
                             std::vector<uint8> &romData,
                             const STVGameInfo *&outGameInfo);

} // namespace brimir::stv
