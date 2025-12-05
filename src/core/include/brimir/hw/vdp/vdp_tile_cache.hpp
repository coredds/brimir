#pragma once

#include <cstdint>

namespace brimir::vdp {

/// @brief Compute a unique cache key for a tile
/// @param charIndex Character pattern index within the page
/// @param charAddress VRAM address of the character data (for validation)
/// @param cellIndex Cell index within character pattern (for 2x2 chars)
/// @return 64-bit unique key
inline uint64_t ComputeTileKey(uint32_t charIndex, uint32_t charAddress, uint32_t cellIndex) {
    // Combine all components into a unique 64-bit key
    // charAddress is most significant (changes less frequently)
    // charIndex changes per tile
    // cellIndex only relevant for 2x2 characters
    return ((uint64_t)charAddress << 32) | (charIndex << 4) | (cellIndex & 0xF);
}

} // namespace brimir::vdp








