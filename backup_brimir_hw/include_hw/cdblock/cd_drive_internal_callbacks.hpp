#pragma once

/**
@file
@brief Internal callback definitions used by the CD drive.
*/

#include <brimir/core/types.hpp>

#include <brimir/util/callback.hpp>

namespace brimir::cdblock {

/// @brief Invoked when the CD Block changes the COMSYNC# signal state mapped to the PB2 pin on the SH-1.
using CBSetCOMSYNCn = util::RequiredCallback<void(bool level)>;

/// @brief Invoked when the CD Block changes the COMREQ# signal state mapped to the TIOCB3 pin on the SH-1.
using CBSetCOMREQn = util::RequiredCallback<void(bool level)>;

} // namespace brimir::cdblock
