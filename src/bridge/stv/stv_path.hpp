// Brimir - File system paths as text for the ST-V code
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// path::string() is in the ANSI code page on Windows and throws for a character the code page cannot hold (a Japanese
// file name on a Western system). The ST-V code names paths in messages and compares file names, so it works on the
// UTF-8 text of a path instead, which every path has.

#pragma once

#include <filesystem>
#include <string>

namespace brimir::stv {

inline std::string PathToUtf8(const std::filesystem::path &path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace brimir::stv
