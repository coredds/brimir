#pragma once

/**
@file
@brief CD-ROM error detection code calculation routines.
*/

#include <brimir/core/types.hpp>

#include <span>

namespace brimir::media {

/// @brief Calculates the CRC for the given sector.
/// @param[in] sector the sector to checksum
/// @return the CD-ROM ECC for the sector
uint32 CalcCRC(std::span<uint8, 2064> sector);

} // namespace brimir::media
