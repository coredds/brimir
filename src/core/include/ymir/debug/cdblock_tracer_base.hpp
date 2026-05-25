#pragma once

/**
@file
@brief Defines `ymir::debug::ICDBlockTracer`, the CD Block tracer interface.
*/

#include <ymir/core/types.hpp>

namespace ymir::debug {

/// @brief Interface for CD Block tracers.
///
/// Must be implemented by users of the core library.
///
/// Attach to an instance of `ymir::cdblock::CDBlock` with its `UseTracer(ICDBlockTracer *)` method.
struct ICDBlockTracer {
    /// @brief Default virtual destructor. Required for inheritance.
    virtual ~ICDBlockTracer() = default;

    /// @brief Invoked when the CD Block processes a command.
    /// @param[in] cr1 the value of CR1
    /// @param[in] cr2 the value of CR2
    /// @param[in] cr3 the value of CR3
    /// @param[in] cr4 the value of CR4
    virtual void ProcessCommand(uint16 cr1, uint16 cr2, uint16 cr3, uint16 cr4) {}

    /// @brief Invoked when the CD Block sends a command response.
    /// @param[in] cr1 the value of CR1
    /// @param[in] cr2 the value of CR2
    /// @param[in] cr3 the value of CR3
    /// @param[in] cr4 the value of CR4
    virtual void ProcessCommandResponse(uint16 cr1, uint16 cr2, uint16 cr3, uint16 cr4) {}
};

} // namespace ymir::debug
