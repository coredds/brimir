#pragma once

/**
@file
@brief Internal callback definitions used by the VDP.
*/

#include <ymir/util/callback.hpp>

namespace ymir::vdp {

/// @brief Invoked when the HBlank signal changes.
using CBHBlankStateChange = util::RequiredCallback<void(bool hb, bool vb)>;

/// @brief Invoked when the VBlank signal changes.
using CBVBlankStateChange = util::RequiredCallback<void(bool vb)>;

/// @brief Invoked when specific events occur while processing.
using CBTriggerEvent = util::RequiredCallback<void()>;

} // namespace ymir::vdp
