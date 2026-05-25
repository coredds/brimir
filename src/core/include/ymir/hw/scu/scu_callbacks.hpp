#pragma once

/**
@file
@brief SCU callbacks.
*/

#include <ymir/core/types.hpp>

#include <ymir/util/callback.hpp>

namespace ymir::scu {

/// @brief Invoked whenever a byte is written to the debug port.
using CBDebugPortWrite = util::OptionalCallback<void(uint8 ch)>;

} // namespace ymir::scu
