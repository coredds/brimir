#pragma once

/**
@file
@brief Internal callback definitions used by the SH2.
*/

#include <ymir/util/callback.hpp>

namespace ymir::sh2 {

/// @brief Invoked when the SH2 acknowledges an external interrupt signal.
using CBAcknowledgeExternalInterrupt = util::RequiredCallback<void()>;

} // namespace ymir::sh2
