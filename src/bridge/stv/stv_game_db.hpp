// Brimir - ST-V game database (ROM layout, encryption, control scheme)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#pragma once

#include <cstdint>
#include <filesystem>

namespace brimir::stv {

enum STVControlScheme {
    STV_CONTROL_3B = 0,
    STV_CONTROL_6B,
    STV_CONTROL_HAMMER,
    STV_CONTROL_RSG
};

enum STVROMTwiddle {
    STV_ROMTWIDDLE_NONE = 0,
    STV_ROMTWIDDLE_SANJEON
};

enum STVEChip {
    STV_EC_CHIP_NONE = 0,
    STV_EC_CHIP_315_5881,
    STV_EC_CHIP_315_5838,
    STV_EC_CHIP_RSG
};

enum STVROMMap {
    STV_MAP_BYTE = 0,
    STV_MAP_16LE,
    STV_MAP_16BE
};

struct STVROMLayoutEntry {
    uint32_t offset;     // Byte offset into the 48 MiB cart ROM buffer
    uint32_t size;       // Size of this ROM file in bytes
    unsigned  map;       // STV_MAP_BYTE, STV_MAP_16LE, or STV_MAP_16BE
    const char *fname;   // Filename within the ROM set directory
};

struct STVGameInfo {
    const char *name;
    unsigned area;          // SMPC area code (JP, NA, EU, etc.)
    unsigned control;       // STVControlScheme
    unsigned ec_chip;       // STVEChip
    unsigned romtwiddle;    // STVROMTwiddle
    bool     rotate;        // Screen rotation (tate mode)
    STVROMLayoutEntry rom_layout[16]; // Up to 16 ROM file entries
};

const STVGameInfo *LookupSTVGame(const char *filename);

// Lookup by scanning zip contents
const STVGameInfo *LookupSTVByZipEntries(class ZipReader &zip);

// Lookup by scanning a directory for matching ROM files
const STVGameInfo *LookupSTVGameInDir(const std::filesystem::path &dir);

} // namespace brimir::stv
