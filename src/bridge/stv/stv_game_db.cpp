// Brimir - ST-V game database implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_game_db.hpp"
#include "stv_zip.hpp"

#include <cstring>

namespace brimir::stv {

static const STVGameInfo kGameDB[] = {
    {
        "Radiant Silvergun", 0x1, STV_CONTROL_RSG, STV_EC_CHIP_315_5881, 0x05272d01, STV_ROMTWIDDLE_NONE, false,
        { STVROMLayoutEntry{0x0200000, 0x0200000, STV_MAP_16LE, "mpr20958.7"},
          STVROMLayoutEntry{0x0400000, 0x0400000, STV_MAP_16LE, "mpr20959.2"},
          STVROMLayoutEntry{0x0800000, 0x0400000, STV_MAP_16LE, "mpr20960.3"},
          STVROMLayoutEntry{0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20961.4"},
          STVROMLayoutEntry{0x1000000, 0x0400000, STV_MAP_16LE, "mpr20962.5"} }
    },
    {
        "Baku Baku Animal", 0x1, STV_CONTROL_3B, STV_EC_CHIP_NONE, 0, STV_ROMTWIDDLE_NONE, false,
        { STVROMLayoutEntry{0x0000001, 0x0100000, STV_MAP_BYTE, "fpr17969.13"},
          STVROMLayoutEntry{0x0400000, 0x0400000, STV_MAP_16LE, "mpr17970.2"},
          STVROMLayoutEntry{0x0800000, 0x0400000, STV_MAP_16LE, "mpr17971.3"},
          STVROMLayoutEntry{0x0C00000, 0x0400000, STV_MAP_16LE, "mpr17972.4"},
          STVROMLayoutEntry{0x1000000, 0x0400000, STV_MAP_16LE, "mpr17973.5"} }
    },
    {
        "Cotton 2", 0x1, STV_CONTROL_3B, STV_EC_CHIP_NONE, 0, STV_ROMTWIDDLE_NONE, false,
        { STVROMLayoutEntry{0x0200000, 0x0200000, STV_MAP_16LE, "mpr20122.7"},
          STVROMLayoutEntry{0x0400000, 0x0400000, STV_MAP_16LE, "mpr20117.2"},
          STVROMLayoutEntry{0x0800000, 0x0400000, STV_MAP_16LE, "mpr20118.3"},
          STVROMLayoutEntry{0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20119.4"},
          STVROMLayoutEntry{0x1000000, 0x0400000, STV_MAP_16LE, "mpr20120.5"},
          STVROMLayoutEntry{0x1400000, 0x0400000, STV_MAP_16LE, "mpr20121.6"},
          STVROMLayoutEntry{0x1800000, 0x0400000, STV_MAP_16LE, "mpr20116.1"},
          STVROMLayoutEntry{0x1C00000, 0x0400000, STV_MAP_16LE, "mpr20123.8"} }
    },
    {
        "Die Hard Arcade", 0x4, STV_CONTROL_3B, STV_EC_CHIP_NONE, 0, STV_ROMTWIDDLE_NONE, false,
        { STVROMLayoutEntry{0x0000001, 0x0100000, STV_MAP_BYTE, "fpr19119.13"},
          STVROMLayoutEntry{0x0400000, 0x0400000, STV_MAP_16LE, "mpr19115.2"},
          STVROMLayoutEntry{0x0800000, 0x0400000, STV_MAP_16LE, "mpr19116.3"},
          STVROMLayoutEntry{0x0C00000, 0x0400000, STV_MAP_16LE, "mpr19117.4"},
          STVROMLayoutEntry{0x1000000, 0x0400000, STV_MAP_16LE, "mpr19118.5"} }
    },
    {
        "Golden Axe: The Duel", 0x1, STV_CONTROL_6B, STV_EC_CHIP_NONE, 0, STV_ROMTWIDDLE_NONE, false,
        { STVROMLayoutEntry{0x0000001, 0x0080000, STV_MAP_BYTE, "epr17766.13"},
          STVROMLayoutEntry{0x0100001, 0x0080000, STV_MAP_BYTE, "epr17766.13"},
          STVROMLayoutEntry{0x0400000, 0x0400000, STV_MAP_16LE, "mpr17768.2"},
          STVROMLayoutEntry{0x0800000, 0x0400000, STV_MAP_16LE, "mpr17769.3"},
          STVROMLayoutEntry{0x0C00000, 0x0400000, STV_MAP_16LE, "mpr17770.4"},
          STVROMLayoutEntry{0x1000000, 0x0400000, STV_MAP_16LE, "mpr17771.5"},
          STVROMLayoutEntry{0x1400000, 0x0400000, STV_MAP_16LE, "mpr17772.6"},
          STVROMLayoutEntry{0x1800000, 0x0400000, STV_MAP_16LE, "mpr17767.1"} }
    },
    {
        "Guardian Force", 0x1, STV_CONTROL_3B, STV_EC_CHIP_NONE, 0, STV_ROMTWIDDLE_NONE, false,
        { STVROMLayoutEntry{0x0200000, 0x0200000, STV_MAP_16LE, "mpr20844.7"},
          STVROMLayoutEntry{0x0400000, 0x0400000, STV_MAP_16LE, "mpr20839.2"},
          STVROMLayoutEntry{0x0800000, 0x0400000, STV_MAP_16LE, "mpr20840.3"},
          STVROMLayoutEntry{0x0C00000, 0x0400000, STV_MAP_16LE, "mpr20841.4"},
          STVROMLayoutEntry{0x1000000, 0x0400000, STV_MAP_16LE, "mpr20842.5"},
          STVROMLayoutEntry{0x1400000, 0x0400000, STV_MAP_16LE, "mpr20843.6"} }
    },
    {
        "Puyo Puyo Sun", 0x1, STV_CONTROL_3B, STV_EC_CHIP_NONE, 0, STV_ROMTWIDDLE_NONE, false,
        { STVROMLayoutEntry{0x0000001, 0x0080000, STV_MAP_BYTE, "epr19531.13"},
          STVROMLayoutEntry{0x0100001, 0x0080000, STV_MAP_BYTE, "epr19531.13"},
          STVROMLayoutEntry{0x0400000, 0x0400000, STV_MAP_16LE, "mpr19533.2"},
          STVROMLayoutEntry{0x0800000, 0x0400000, STV_MAP_16LE, "mpr19534.3"},
          STVROMLayoutEntry{0x0C00000, 0x0400000, STV_MAP_16LE, "mpr19535.4"},
          STVROMLayoutEntry{0x1000000, 0x0400000, STV_MAP_16LE, "mpr19536.5"},
          STVROMLayoutEntry{0x1400000, 0x0400000, STV_MAP_16LE, "mpr19537.6"},
          STVROMLayoutEntry{0x1800000, 0x0400000, STV_MAP_16LE, "mpr19538.1"} }
    },
    {
        "Columns '97", 0x1, STV_CONTROL_3B, STV_EC_CHIP_NONE, 0, STV_ROMTWIDDLE_NONE, false,
        { STVROMLayoutEntry{0x0000001, 0x0100000, STV_MAP_BYTE, "fpr19553.13"},
          STVROMLayoutEntry{0x0400000, 0x0400000, STV_MAP_16LE, "mpr19554.2"},
          STVROMLayoutEntry{0x0800000, 0x0400000, STV_MAP_16LE, "mpr19555.3"} }
    },
};

const STVGameInfo *LookupSTVGame(const char *filename) {
    if (!filename) return nullptr;
    const char *base = strrchr(filename, '/');
    if (!base) base = strrchr(filename, '\\');
    if (base) base++; else base = filename;

    for (auto &game : kGameDB) {
        for (auto &rom : game.rom_layout) {
            if (!rom.fname) break;
#ifdef _MSC_VER
            if (_stricmp(base, rom.fname) == 0)
#else
            if (strcasecmp(base, rom.fname) == 0)
#endif
                return &game;
        }
    }
    return nullptr;
}

const STVGameInfo *LookupSTVByZipEntries(ZipReader &zip) {
    for (auto &game : kGameDB) {
        bool allFound = true;
        for (auto &rom : game.rom_layout) {
            if (!rom.fname) break;
            if (!zip.HasEntry(rom.fname)) {
                allFound = false;
                break;
            }
        }
        if (allFound) return &game;
    }
    return nullptr;
}

const STVGameInfo *LookupSTVGameInDir(const std::filesystem::path &dir) {
    for (auto &game : kGameDB) {
        bool allFound = true;
        for (auto &rom : game.rom_layout) {
            if (!rom.fname) break;
            if (!std::filesystem::exists(dir / rom.fname)) {
                allFound = false;
                break;
            }
        }
        if (allFound) return &game;
    }
    return nullptr;
}

} // namespace brimir::stv
