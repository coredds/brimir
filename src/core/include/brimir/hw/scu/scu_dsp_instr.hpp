#pragma once

#include <brimir/core/types.hpp>

namespace brimir::scu {

union DSPInstr {
    uint32 u32 = 0u;

    constexpr bool operator==(const DSPInstr &rhs) const {
        return u32 == rhs.u32;
    }
    constexpr auto operator<=>(const DSPInstr &rhs) const {
        return u32 <=> rhs.u32;
    }
    struct InstructionControl {
        uint32 instructionControl : 30; // 00-29 - Instruction control bits
        uint32 instructionClass : 2;    // 30-31 - Instruction class
    } instructionInfo;

    // Arithmetic operations
    struct ALUInstr {
        sint32 d1BusImm : 8;  // 00-07 - D1-Bus Immediate
        uint32 d1BusDest : 4; // 08-11 - D1-Bus Destination
        uint32 d1BusOp : 2;   // 12-13 - D1-Bus Operation

        uint32 yBusSource : 3; // 14-16 - Y-Bus Control
        uint32 yBusOp : 3;     // 17-19 - Y-Bus Operation

        uint32 xBusSource : 3; // 20-22 - X-Bus Source
        uint32 xBusOp : 3;     // 23-25 - X-Bus Operation

        uint32 aluOp : 4; // 26-29 - ALU Operation
    } aluInfo;

    // Load operations
    union LoadInstr {
        struct LoadControl {
            uint32 loadImmControl : 25; // 00-24 - Conditional/Unconditional Control bits
            uint32 conditionalLoad : 1; // 25 - Conditional/Unconditional load
            uint32 storageLocation : 4; // 26-29 - Destination
        } loadControl;

        struct LoadUnconditional {
            sint32 imm : 25; // 00-24 - Immediate
        } unconditional;

        struct LoadConditional {
            sint32 imm : 19;      // 00-18 - Immediate
            uint32 condition : 6; // 19-24 - Condition
        } conditional;
    } loadInfo;

    // Special operations
    union SpecialInstr {
        struct SpecialControl {
            uint32 specialControl : 28; // 00-27 - Special-operation control bits
            uint32 specialClass : 2;    // 28-29 - Special-operation class
        } specialControl;

        struct DMAControl {
            uint32 imm : 8;        // 00-07 - Immediate
            uint32 address : 3;    // 08-10 - Transfer address
            uint32 unused : 1;     // 11 - Unused
            uint32 direction : 1;  // 12 - Transfer direction
            uint32 sizeSource : 1; // 13 - Transfer size source (Immediate/Memory)
            uint32 hold : 1;       // 14 - Hold DMA address
            uint32 stride : 3;     // 15-17 - Address stride
        } dmaInfo;

        struct JumpControl {
            uint32 target : 8;      // 00-07 - Jump Target
            uint32 unused : 11;     // 08-18 - Unused
            uint32 condition : 6;   // 19-24 - Jump Condition
            uint32 conditional : 1; //    25 - Conditional jump
        } jumpInfo;

        struct LoopControl {
            uint32 unused : 27; // 0-26 - Unused
            uint32 repeat : 1;  // 27 - Repeat loop
        } loopInfo;

        struct EndControl {
            uint32 unused : 27;   // 0-26 - Unused
            uint32 interrupt : 1; // 27 - Signal DSP End interrupt
        } endInfo;

    } specialInfo;
};

static_assert(sizeof(DSPInstr) == sizeof(uint32));

} // namespace brimir::scu