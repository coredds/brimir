#pragma once

#include <brimir/core/types.hpp>

namespace brimir::scsp {

union DSPInstr {
    uint64 u64;
    uint16 u16[4];
    struct {
        uint64 NXADR : 1; //     0 - Increments the memory address by one
        uint64 ADREB : 1; //     1 - 0=Gate the output of the address register (ADRS_REG), and make the output 0
        uint64 MASA : 5;  //   2-6 - MADRS read address
        uint64 : 1;       //     7 - (unused)
        uint64 NOFL : 1;  //     8 - 1=Do not perform a floating conversion for wave memory access
        uint64 CRA : 6;   //  9-14 - COEF read address
        uint64 : 1;       //    15 - (unused)
        uint64 BSEL : 1;  //    16 - 0=TEMP data select; 1=accumulator select
        uint64 ZERO : 1;  //    17 - 1=Assume the adder input as 0
        uint64 NEGB : 1;  //    18 - 0=addition; 1=subtraction
        uint64 YRL : 1;   //    19 - Latches INPUTS[23:4]
        uint64 SHFT0 : 1; //    20 - Shifter control 0
        uint64 SHFT1 : 1; //    21 - Shifter control 1
        uint64 FRCL : 1;  //    22 - Memory address decimal latch
        uint64 ADRL : 1;  //    23 - Memory address integer latch
        uint64 EWA : 4;   // 24-27 - Output EFREG address
        uint64 EWT : 1;   //    28 - Request to write output data to EFREG
        uint64 MRD : 1;   //    29 - Wave memory read request
        uint64 MWT : 1;   //    30 - Write request to wave memory
        uint64 TABLE : 1; //    31 - 1=Gate the output of the decrement counter (MDEC_CT), and make the output 0
        uint64 IWA : 5;   // 32-36 - Write address for the input data (INPUTS)
        uint64 IWT : 1;   //    37 - DSP input data write request
        uint64 IRA : 6;   // 38-43 - Read address for the input data (INPUTS)
        uint64 : 1;       //    44 - (unused)
        uint64 YSEL : 2;  // 45-46 - Multiplier Y input select (0=FRC_REG, 1=COEF, 2=Y_REG[23:11], 3=0|Y_REG[15:4])
        uint64 XSEL : 1;  //    47 - Multiplier X input select (0=TEMP data select, 1=INPUTS data select)
        uint64 TWA : 7;   // 48-54 - TEMP write address
        uint64 TWT : 1;   //    55 - TEMP input data write request
        uint64 TRA : 7;   // 56-62 - TEMP read address
        uint64 : 1;       //    63 - (unused)
    };
    struct {
        uint64 : 20;
        uint64 SHFT : 2; // 20-21 - Shifter control 0 and 1
    };
};
static_assert(sizeof(DSPInstr) == sizeof(uint64));

} // namespace brimir::scsp
