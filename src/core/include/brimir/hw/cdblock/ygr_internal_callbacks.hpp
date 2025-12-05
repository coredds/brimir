#pragma once

/**
@file
@brief Internal callback definitions used by the YGR.
*/

#include <brimir/core/types.hpp>

#include <brimir/util/callback.hpp>

namespace brimir::cdblock {

/// @brief Invoked when a sector transfer is finished
using CBSectorTransferDone = util::RequiredCallback<void()>;

} // namespace brimir::cdblock
