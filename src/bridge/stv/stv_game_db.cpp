// Brimir - ST-V game database implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Transcribed from MAME's src/mame/sega/stv.cpp. Each ROM list follows the ROM_START block of its set line by line;
// the region lists follow the country letters of the GAME title (J, U, E, T, L) and the key is the set's
// ROM_PARAMETER( ":315_5881:key", ... ). All sets are ROT0 but shienryu, ROT270: MAME turns its picture a quarter
// turn anticlockwise, which is CoreWrapper's rotation step at 270.

#include "stv_game_db.hpp"

namespace brimir::stv {

namespace {

constexpr Region kJapan[] = {Region::Japan};
constexpr Region kJET[] = {Region::Japan, Region::Europe, Region::Taiwan};
constexpr Region kUET[] = {Region::USA, Region::Europe, Region::Taiwan};
constexpr Region kJUET[] = {Region::Japan, Region::USA, Region::Europe, Region::Taiwan};
constexpr Region kJUETL[] = {Region::Japan, Region::USA, Region::Europe, Region::Taiwan, Region::LatinAmerica};

// bakubaku: ROM_LOAD16_BYTE, then ROM_RELOAD_PLAIN twice
constexpr RomEntry kBakubakuRoms[] = {
    {"fpr17969.13", 0x0000001, 0x0100000, 0xbee327e5, Load::Byte16},
    {"fpr17969.13", 0x0200000, 0x0100000, 0xbee327e5, Load::Plain},
    {"fpr17969.13", 0x0300000, 0x0100000, 0xbee327e5, Load::Plain},
    {"mpr17970.2", 0x0400000, 0x0400000, 0xbc4d6f91, Load::WordSwap},
    {"mpr17971.3", 0x0800000, 0x0400000, 0xc780a3b3, Load::WordSwap},
    {"mpr17972.4", 0x0c00000, 0x0400000, 0x8f29815a, Load::WordSwap},
    {"mpr17973.5", 0x1000000, 0x0400000, 0x5f6e0e8b, Load::WordSwap},
};

constexpr RomEntry kColmns97Roms[] = {
    {"fpr19553.13", 0x0000001, 0x0100000, 0xd4fb6a5e, Load::Byte16},
    {"fpr19553.13", 0x0200000, 0x0100000, 0xd4fb6a5e, Load::Plain},
    {"fpr19553.13", 0x0300000, 0x0100000, 0xd4fb6a5e, Load::Plain},
    {"mpr19554.2", 0x0400000, 0x0400000, 0x5a3ebcac, Load::WordSwap},
    {"mpr19555.3", 0x0800000, 0x0400000, 0x74f6e6b8, Load::WordSwap},
};

// puyosun: ROM_LOAD16_BYTE, ROM_RELOAD, then ROM_RELOAD_PLAIN twice
constexpr RomEntry kPuyosunRoms[] = {
    {"epr19531.13", 0x0000001, 0x0080000, 0xac81024f, Load::Byte16},
    {"epr19531.13", 0x0100001, 0x0080000, 0xac81024f, Load::Reload},
    {"epr19531.13", 0x0200000, 0x0080000, 0xac81024f, Load::Plain},
    {"epr19531.13", 0x0300000, 0x0080000, 0xac81024f, Load::Plain},
    {"mpr19533.2", 0x0400000, 0x0400000, 0x17ec54ba, Load::WordSwap},
    {"mpr19534.3", 0x0800000, 0x0400000, 0x820e4781, Load::WordSwap},
    {"mpr19535.4", 0x0c00000, 0x0400000, 0x94fadfa4, Load::WordSwap},
    {"mpr19536.5", 0x1000000, 0x0400000, 0x5765bc9c, Load::WordSwap},
    {"mpr19537.6", 0x1400000, 0x0400000, 0x8b736686, Load::WordSwap},
    {"mpr19532.1", 0x1800000, 0x0400000, 0x985f0c9d, Load::WordSwap},
    {"mpr19538.8", 0x1c00000, 0x0400000, 0x915a723e, Load::WordSwap},
    {"mpr19539.9", 0x2000000, 0x0400000, 0x72a297e5, Load::WordSwap},
};

constexpr RomEntry kDiehardRoms[] = {
    {"fpr19119.13", 0x0000001, 0x0100000, 0xde5c4f7c, Load::Byte16},
    {"fpr19119.13", 0x0200000, 0x0100000, 0xde5c4f7c, Load::Plain},
    {"fpr19119.13", 0x0300000, 0x0100000, 0xde5c4f7c, Load::Plain},
    {"mpr19115.2", 0x0400000, 0x0400000, 0x6fe06a30, Load::WordSwap},
    {"mpr19116.3", 0x0800000, 0x0400000, 0xaf9e627b, Load::WordSwap},
    {"mpr19117.4", 0x0c00000, 0x0400000, 0x74520ff1, Load::WordSwap},
    {"mpr19118.5", 0x1000000, 0x0400000, 0x2c9702f0, Load::WordSwap},
};

// gaxeduel: ROM_LOAD16_BYTE, ROM_RELOAD, then ROM_RELOAD_PLAIN twice
constexpr RomEntry kGaxeduelRoms[] = {
    {"epr17766.13", 0x0000001, 0x0080000, 0xa83fcd62, Load::Byte16},
    {"epr17766.13", 0x0100001, 0x0080000, 0xa83fcd62, Load::Reload},
    {"epr17766.13", 0x0200000, 0x0080000, 0xa83fcd62, Load::Plain},
    {"epr17766.13", 0x0300000, 0x0080000, 0xa83fcd62, Load::Plain},
    {"mpr17768.2", 0x0400000, 0x0400000, 0xd6808a7d, Load::WordSwap},
    {"mpr17769.3", 0x0800000, 0x0400000, 0x3471dd35, Load::WordSwap},
    {"mpr17770.4", 0x0c00000, 0x0400000, 0x06978a00, Load::WordSwap},
    {"mpr17771.5", 0x1000000, 0x0400000, 0xaea2ea3b, Load::WordSwap},
    {"mpr17772.6", 0x1400000, 0x0400000, 0xb3dc0e75, Load::WordSwap},
    {"mpr17767.1", 0x1800000, 0x0400000, 0x9ba1e7b1, Load::WordSwap},
};

constexpr RomEntry kCotton2Roms[] = {
    {"mpr20122.7", 0x0200000, 0x0200000, 0xd616f78a, Load::WordSwap},
    {"mpr20117.2", 0x0400000, 0x0400000, 0x893656ea, Load::WordSwap},
    {"mpr20118.3", 0x0800000, 0x0400000, 0x1b6a1d4c, Load::WordSwap},
    {"mpr20119.4", 0x0c00000, 0x0400000, 0x5a76e72b, Load::WordSwap},
    {"mpr20120.5", 0x1000000, 0x0400000, 0x7113dd7b, Load::WordSwap},
    {"mpr20121.6", 0x1400000, 0x0400000, 0x8c8fd521, Load::WordSwap},
    {"mpr20116.1", 0x1800000, 0x0400000, 0xd30b0175, Load::WordSwap},
    {"mpr20123.8", 0x1c00000, 0x0400000, 0x35f1b89f, Load::WordSwap},
};

constexpr RomEntry kGrdforceRoms[] = {
    {"mpr20844.7", 0x0200000, 0x0200000, 0x283e7587, Load::WordSwap},
    {"mpr20839.2", 0x0400000, 0x0400000, 0xfacd4dd8, Load::WordSwap},
    {"mpr20840.3", 0x0800000, 0x0400000, 0xfe0158e6, Load::WordSwap},
    {"mpr20841.4", 0x0c00000, 0x0400000, 0xd87ac873, Load::WordSwap},
    {"mpr20842.5", 0x1000000, 0x0400000, 0xbaebc506, Load::WordSwap},
    {"mpr20843.6", 0x1400000, 0x0400000, 0x263e49cc, Load::WordSwap},
};

constexpr RomEntry kRsgunRoms[] = {
    {"mpr20958.7", 0x0200000, 0x0200000, 0xcbe5a449, Load::WordSwap},
    {"mpr20959.2", 0x0400000, 0x0400000, 0xa953330b, Load::WordSwap},
    {"mpr20960.3", 0x0800000, 0x0400000, 0xb5ab9053, Load::WordSwap},
    {"mpr20961.4", 0x0c00000, 0x0400000, 0x0e06295c, Load::WordSwap},
    {"mpr20962.5", 0x1000000, 0x0400000, 0xf1e6c7fc, Load::WordSwap},
};

constexpr RomEntry kShienryuRoms[] = {
    {"mpr19631.7", 0x0200000, 0x0200000, 0x3a4b1abc, Load::WordSwap},
    {"mpr19632.2", 0x0400000, 0x0400000, 0x985fae46, Load::WordSwap},
    {"mpr19633.3", 0x0800000, 0x0400000, 0xe2f0b037, Load::WordSwap},
};

// ffreveng: the program ROM is ROM_LOAD16_BYTE without reloads (the even bytes of its 2 MiB stay at the gap fill)
constexpr RomEntry kFfrevengRoms[] = {
    {"ffr110.ic35", 0x0000001, 0x0100000, 0x3ffea541, Load::Byte16},
    {"opr21872.7", 0x0200000, 0x0200000, 0x32d36fee, Load::WordSwap},
    {"mpr21873.2", 0x0400000, 0x0400000, 0xdac5bd98, Load::WordSwap},
    {"mpr21874.3", 0x0800000, 0x0400000, 0x0a7be2f1, Load::WordSwap},
    {"mpr21875.4", 0x0c00000, 0x0400000, 0xccb75029, Load::WordSwap},
    {"mpr21876.5", 0x1000000, 0x0400000, 0xbb92a7fc, Load::WordSwap},
    {"mpr21877.6", 0x1400000, 0x0400000, 0xc22a4a75, Load::WordSwap},
    {"opr21878.1", 0x1800000, 0x0200000, 0x2ea4a64d, Load::WordSwap},
};

// sss: ROM_LOAD16_BYTE, ROM_RELOAD, then ROM_RELOAD_PLAIN twice
constexpr RomEntry kSssRoms[] = {
    {"epr21488.13", 0x0000001, 0x0080000, 0x71c9def1, Load::Byte16},
    {"epr21488.13", 0x0100001, 0x0080000, 0x71c9def1, Load::Reload},
    {"epr21488.13", 0x0200000, 0x0080000, 0x71c9def1, Load::Plain},
    {"epr21488.13", 0x0300000, 0x0080000, 0x71c9def1, Load::Plain},
    {"mpr21489.2", 0x0400000, 0x0400000, 0x4c85152b, Load::WordSwap},
    {"mpr21490.3", 0x0800000, 0x0400000, 0x03da67f8, Load::WordSwap},
    {"mpr21491.4", 0x0c00000, 0x0400000, 0xcf7ee784, Load::WordSwap},
    {"mpr21492.5", 0x1000000, 0x0400000, 0x57753894, Load::WordSwap},
    {"mpr21493.6", 0x1400000, 0x0400000, 0xefb2d271, Load::WordSwap},
};

constexpr GameInfo kGames[] = {
    {"bakubaku", "Baku Baku Animal", kJapan, Controls::ThreeButtons, Chip::None, 0, 0, kBakubakuRoms},
    {"colmns97", "Columns '97", kJET, Controls::ThreeButtons, Chip::None, 0, 0, kColmns97Roms},
    {"puyosun", "Puyo Puyo Sun", kJapan, Controls::ThreeButtons, Chip::None, 0, 0, kPuyosunRoms},
    {"diehard", "Die Hard Arcade", kUET, Controls::ThreeButtons, Chip::None, 0, 0, kDiehardRoms},
    {"gaxeduel", "Golden Axe: The Duel", kJUETL, Controls::SixButtons, Chip::None, 0, 0, kGaxeduelRoms},
    {"cotton2", "Cotton 2", kJUET, Controls::ThreeButtons, Chip::None, 0, 0, kCotton2Roms},
    {"grdforce", "Guardian Force", kJUET, Controls::ThreeButtons, Chip::None, 0, 0, kGrdforceRoms},
    {"rsgun", "Radiant Silvergun", kJUET, Controls::RadiantSilvergun, Chip::Crypt315_5881, 0x05272d01, 0,
     kRsgunRoms},
    // The default EEPROM sets the cabinet up for a vertical monitor: the BIOS draws its own screens turned to match
    // (with a blank one they come out sideways once the picture is rotated for the game)
    {"shienryu", "Shienryu", kJUET, Controls::ThreeButtons, Chip::None, 0, 270, kShienryuRoms,
     {"eeprom-shienryu.bin", 0x98db6925}},
    {"ffreveng", "Final Fight Revenge", kJUET, Controls::SixButtons, Chip::Crypt315_5881, 0x0524ac01, 0,
     kFfrevengRoms},
    // The default EEPROM is MAME's "preconfigured to 1 player"
    {"sss", "Steep Slope Sliders", kJUET, Controls::ThreeButtons, Chip::Crypt315_5881, 0x052b6901, 0, kSssRoms,
     {"sss.nv", 0x3473b2f3}},
};

} // namespace

std::span<const GameInfo> AllGames() {
    return kGames;
}

const GameInfo *FindGame(std::string_view shortName) {
    for (const GameInfo &game : kGames) {
        if (shortName == game.shortName) return &game;
    }
    return nullptr;
}

} // namespace brimir::stv
