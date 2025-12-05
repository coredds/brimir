#pragma once

/**
@file
@brief SCU callbacks.
*/

#include <brimir/core/types.hpp>

#include <brimir/util/callback.hpp>

namespace brimir::scu {

/// @brief Invoked whenever a byte is written to the debug port.
using CBDebugPortWrite = util::OptionalCallback<void(uint8 ch)>;

} // namespace brimir::scu
