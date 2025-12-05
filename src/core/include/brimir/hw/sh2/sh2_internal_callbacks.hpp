#pragma once

/**
@file
@brief Internal callback definitions used by the SH2.
*/

#include <brimir/util/callback.hpp>

namespace brimir::sh2 {

/// @brief Invoked when the SH2 acknowledges an external interrupt signal.
using CBAcknowledgeExternalInterrupt = util::RequiredCallback<void()>;

} // namespace brimir::sh2
