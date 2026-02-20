#pragma once

/**
@file
@brief Internal callback definitions used by the CD block.
*/

#include <brimir/util/callback.hpp>

#include <brimir/core/types.hpp>

#include <span>

namespace brimir::cdblock {

/// @brief Invoked when the CD Block raises an interrupt.
using CBTriggerExternalInterrupt0 = util::RequiredCallback<void()>;

/// @brief Invoked when the CD Block reads a CDDA sector.
///
/// The callback should return how many thirds of the audio buffer are full.
using CBCDDASector = util::RequiredCallback<uint32(std::span<uint8, 2352> data)>;

/// @brief Invoked when the CD Block reads a data sector.
using CBDataSector = util::RequiredCallback<void(std::span<uint8> data)>;

} // namespace brimir::cdblock
