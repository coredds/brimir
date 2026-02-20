#pragma once

#include <brimir/util/size_ops.hpp>

#include <brimir/core/types.hpp>

namespace brimir::m68k {

inline constexpr std::size_t kM68KWRAMSize = 512_KiB;

enum class ExceptionVector : uint32 {
    ResetSSP = 0x0,
    ResetPC = 0x1,
    BusError = 0x2,
    AddressError = 0x3,
    IllegalInstruction = 0x4,
    ZeroDivide = 0x5,
    CHKInstruction = 0x6,
    TRAPVInstruction = 0x7,
    PrivilegeViolation = 0x8,
    Trace = 0x9,
    Line1010Emulator = 0xA,
    Line1111Emulator = 0xB,

    UninitializedInterrupt = 0xF,

    SpuriousInterrupt = 0x18,

    BaseAutovector = 0x18, // for automatic vector generation
    Level1InterruptAutovector = 0x19,
    Level2InterruptAutovector = 0x1A,
    Level3InterruptAutovector = 0x1B,
    Level4InterruptAutovector = 0x1C,
    Level5InterruptAutovector = 0x1D,
    Level6InterruptAutovector = 0x1E,
    Level7InterruptAutovector = 0x1F,

    TRAPVector0 = 0x20,
    TRAPVector1 = 0x21,
    TRAPVector2 = 0x22,
    TRAPVector3 = 0x23,
    TRAPVector4 = 0x24,
    TRAPVector5 = 0x25,
    TRAPVector6 = 0x26,
    TRAPVector7 = 0x27,
    TRAPVector8 = 0x28,
    TRAPVector9 = 0x29,
    TRAPVectorA = 0x2A,
    TRAPVectorB = 0x2B,
    TRAPVectorC = 0x2C,
    TRAPVectorD = 0x2D,
    TRAPVectorE = 0x2E,
    TRAPVectorF = 0x2F,

    // Special value used by external devices during the MC68EC000 interrupt acknowledge cycle to request automatic
    // vector generation.
    AutoVectorRequest = 0xFFFFFFFF,
};

} // namespace brimir::m68k
