#pragma once

#include <brimir/core/types.hpp>

#include <brimir/util/callback.hpp>

namespace brimir::scu {

// Invoked when the SCU raises an interrupt.
using CBExternalInterrupt = util::RequiredCallback<void(uint8 level, uint8 vector)>;

} // namespace brimir::scu
