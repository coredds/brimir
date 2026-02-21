#pragma once

/**
@file
@brief Internal callback definitions used by the SCSP.
*/

#include <brimir/util/callback.hpp>

namespace brimir::scsp {

/// @brief Invoked when the SCSP needs to raise the SCU sound request interrupt signal.
using CBTriggerSoundRequestInterrupt = util::RequiredCallback<void(bool level)>;

} // namespace brimir::scsp
