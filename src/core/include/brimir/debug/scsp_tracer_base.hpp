#pragma once

/**
@file
@brief Defines `brimir::debug::ISCSPTracer`, the SCSP tracer interface.
*/

#include <brimir/core/types.hpp>

namespace brimir::debug {

/// @brief Interface for SCSP tracers.
///
/// Must be implemented by users of the core library.
///
/// Attach to an instance of `brimir::cdblock::CDBlock` with its `UseTracer(ISCSPTracer *)` method.
struct ISCSPTracer {
    /// @brief Default virtual destructor. Required for inheritance.
    virtual ~ISCSPTracer() = default;

    /// @brief Invoked for each slot when the SCSP outputs a sample.
    /// @param[in] index the slot index
    /// @param[in] left the slot output sample
    virtual void SlotSample(uint32 index, sint16 sample) = 0;

    /// @brief Invoked when KYONEX is processed.
    /// @param[in] slotsMask a bitmask with state of KYONB for each slot
    virtual void KeyOnExecute(uint32 slotsMask) = 0;
};

} // namespace brimir::debug
