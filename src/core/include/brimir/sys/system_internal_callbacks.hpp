#pragma once

/**
@file
@brief Internal callback definitions used by the system.
*/

#include "clocks.hpp"

#include <brimir/util/callback.hpp>

namespace brimir::sys {

/// @brief Invoked when the system clock speed changes.
using CBClockSpeedChange = util::RequiredCallback<void(const ClockRatios &clockRatios)>;

} // namespace brimir::sys
