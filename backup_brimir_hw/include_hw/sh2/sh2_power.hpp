#pragma once

#include <brimir/core/types.hpp>

namespace brimir::sh2 {

// addr r/w  access   init      code    name
// 091  R/W  8        00        SBYCR   Standby Control Register
//
//   bits   r/w  code   description
//      7   R/W  SBY    Standby (0=SLEEP -> sleep mode, 1=SLEEP -> standby mode)
//      6   R/W  HIZ    Port High Impedance (0=pin state retained in standby mode, 1=pin goes to high impedance)
//      5   R    -      Reserved - must be zero
//      4   R/W  MSTP4  Module Stop 4 - DMAC (0=DMAC runs, 1=halt)
//      3   R/W  MSTP3  Module Stop 3 - MULT (0=multiplication unit runs, 1=halt)
//      2   R/W  MSTP2  Module Stop 2 - DIVU (0=DIVU runs, 1=halt)
//      1   R/W  MSTP1  Module Stop 1 - FRT (0=FRT runs, 1=halt and reset)
//      0   R/W  MSTP0  Module Stop 0 - SCI (0=SCI runs, 1=halt and reset)
union RegSBYCR {
    uint8 u8;
    struct {
        uint8 MSTP0 : 1;
        uint8 MSTP1 : 1;
        uint8 MSTP2 : 1;
        uint8 MSTP3 : 1;
        uint8 MSTP4 : 1;
        uint8 : 1;
        uint8 HIZ : 1;
        uint8 SBY : 1;
    };
};

} // namespace brimir::sh2
