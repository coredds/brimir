#pragma once

#include <ymir/core/types.hpp>

#include <ymir/util/callback.hpp>

namespace ymir::scu {

// Invoked when the SCU raises an interrupt.
using CBExternalInterrupt = util::RequiredCallback<void(uint8 level, uint8 vector)>;

} // namespace ymir::scu
