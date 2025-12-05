#pragma once

/**
@file
@brief Defines `brimir::debug::ICDDriveTracer`, the LLE CD drive tracer interface.
*/

#include <brimir/core/types.hpp>

#include <span>

namespace brimir::debug {

/// @brief Interface for LLE CD drive tracers.
///
/// Must be implemented by users of the core library.
///
/// Attach to an instance of `brimir::cdblock::CDDrive` with its `UseTracer(ICDDriveTracer *)` method.
struct ICDDriveTracer {
    /// @brief Default virtual destructor. Required for inheritance.
    virtual ~ICDDriveTracer() = default;

    /// @brief Invoked when the CD drive receives a command and transmits its status through the serial interface.
    /// @param[in] command the command sent to the CD drive
    /// @param[in] status the status sent to the SH-1
    virtual void RxCommandTxStatus(std::span<const uint8, 13> command, std::span<const uint8, 13> status) {}
};

} // namespace brimir::debug
