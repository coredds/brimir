#pragma once

/**
@file
@brief Internal callback definitions used by the VDP.
*/

#include <brimir/util/callback.hpp>

namespace brimir::vdp {

/// @brief Invoked when the HBlank signal changes.
using CBHBlankStateChange = util::RequiredCallback<void(bool hb, bool vb)>;

/// @brief Invoked when the VBlank signal changes.
using CBVBlankStateChange = util::RequiredCallback<void(bool vb)>;

/// @brief Invoked when specific events occur while processing.
using CBTriggerEvent = util::RequiredCallback<void()>;

} // namespace brimir::vdp
