#pragma once

/**
@file
@brief Defines `brimir::debug::ISCUTracer`, the SCU tracer interface.
*/

#include <brimir/core/types.hpp>

namespace brimir::debug {

/// @brief Interface for SCU tracers.
///
/// Must be implemented by users of the core library.
///
/// Attach to an instance of `brimir::scu::SCU` with its `UseTracer(ISCUTracer *)` method.
struct ISCUTracer {
    /// @brief Default virtual destructor. Required for inheritance.
    virtual ~ISCUTracer() = default;

    /// @brief Invoked when the SCU raises an interrupt.
    /// @param[in] index the interrupt index; see `brimir::scu::InterruptStatus`
    /// @param[in] level the interrupt level
    virtual void RaiseInterrupt(uint8 index, uint8 level) {}

    /// @brief Invoked when the SCU acknowledges an interrupt.
    /// @param[in] index the interrupt index; see `brimir::scu::InterruptStatus`
    virtual void AcknowledgeInterrupt(uint8 index) {}

    /// @brief Invoked when a DMA transfer begins and on every indirect transfer.
    /// @param[in] channel the channel index
    /// @param[in] srcAddr the starting source address
    /// @param[in] dstAddr the starting destination address
    /// @param[in] xferCount the number of bytes to be transferred
    /// @param[in] srcAddrInc the source address increment per transfer
    /// @param[in] dstAddrInc the destination address increment per transfer
    /// @param[in] indirect indicates if this is a direct (`false`) or indirect transfer (`true`)
    /// @param[in] indirectAddr the address of the indirect transfer data
    virtual void DMA(uint8 channel, uint32 srcAddr, uint32 dstAddr, uint32 xferCount, uint32 srcAddrInc,
                     uint32 dstAddrInc, bool indirect, uint32 indirectAddr) {}

    // -----------------------------------------------------------------------------------------------------------------
    // DSP

    /// @brief Invoked when a DSP DMA transfer begins.
    /// @param[in] toD0 indicates the direction of the transfer: from DSP to D0 (`true`) or from D0 to DSP (`false`)
    /// @param[in] addrD0 the address on the D0 bus
    /// @param[in] addrDSP the addres on the DSP: 0-3 for Data RAM banks 0-3, 4 for Program RAM
    /// @param[in] count the number of longword transfers to be performed
    /// @param[in] addrInc the D0 address increment per transfer
    /// @param[in] hold indicates if the D0 address will be updated (`false`) or not (`true`) after the transfer
    virtual void DSPDMA(bool toD0, uint32 addrD0, uint8 addrDSP, uint8 count, uint8 addrInc, bool hold) {}
};

} // namespace brimir::debug
