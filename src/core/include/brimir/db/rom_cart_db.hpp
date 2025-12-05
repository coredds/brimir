#pragma once

/**
@file
@brief ROM cartridge database.
*/

#include <brimir/core/hash.hpp>

namespace brimir::db {

/// @brief Information about a ROM cartridge.
struct ROMCartInfo {
    const char *gameName; ///< The game's name
    XXH128Hash hash;      ///< Expected XXH128 hash of the full ROM image
};

/// @brief Information about The King of Fighters '95 ROM cartridge.
inline constexpr ROMCartInfo kKOF95ROMInfo = {
    "The King of Fighters '95",
    {0xD3, 0x92, 0xBA, 0x67, 0xED, 0x08, 0x4D, 0xD2, 0x8C, 0x47, 0x56, 0xFD, 0xBC, 0x04, 0x55, 0xFF},
};

/// @brief Information about the Ultraman: asdfasdflk ROM cartridge.
inline constexpr ROMCartInfo kUltramanROMInfo = {
    "Ultraman: Hikari no Kyojin Densetsu",
    {0x9E, 0x89, 0xDA, 0x01, 0x47, 0xD3, 0x03, 0x02, 0x90, 0x43, 0xDC, 0xF0, 0x93, 0xE4, 0xCA, 0x5E},
};

/// @brief Retrieves information about a ROM cartridge image given its XXH128 hash.
///
/// Returns `nullptr` if there is no information for the given hash.
///
/// @param[in] hash the ROM cartridge hash to check
/// @return a pointer to `ROMCartInfo` containing information about the catridge ROM, or `nullptr` if no matching ROM
/// was found
const ROMCartInfo *GetROMCartInfo(XXH128Hash hash);

} // namespace brimir::db
