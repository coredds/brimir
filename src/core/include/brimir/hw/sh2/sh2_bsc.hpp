#pragma once

namespace brimir::sh2 {

// addr r/w  access   init      code    name
// 1E0  R/W  16,32    03F0      BCR1    Bus Control Register 1
union RegBCR1 {
    uint16 u16;
    struct {
        uint16 DRAMn : 3;
        uint16 _rsvd3 : 1;
        uint16 A0LWn : 2;
        uint16 A1LWn : 2;
        uint16 AHLWn : 2;
        uint16 PSHR : 1;
        uint16 BSTROM : 1;
        uint16 ENDIAN : 1;
        uint16 _rsvd13 : 1;
        uint16 _rsvd14 : 1;
        uint16 MASTER : 1;
    };
    uint16 u15 : 15;
};

// 1E4  R/W  16,32    00FC      BCR2    Bus Control Register 2
union RegBCR2 {
    uint16 u16;
    struct {
        uint16 _rsvd0 : 1;
        uint16 _rsvd1 : 1;
        uint16 A1SZn : 2;
        uint16 A2SZn : 2;
        uint16 A3SZn : 2;
    };
};

// 1E8  R/W  16,32    AAFF      WCR     Wait Control Register
union RegWCR {
    uint16 u16;
    struct {
        uint16 W0n : 2;
        uint16 W1n : 2;
        uint16 W2n : 2;
        uint16 W3n : 2;
        uint16 IW0n : 2;
        uint16 IW1n : 2;
        uint16 IW2n : 2;
        uint16 IW3n : 2;
    };
};

// 1EC  R/W  16,32    0000      MCR     Individual Memory Control Register
union RegMCR {
    uint16 u16;
    struct {
        uint16 _rsvd0 : 1;
        uint16 _rsvd1 : 1;
        uint16 RMD : 1;
        uint16 RFSH : 1;
        uint16 AMX0 : 1;
        uint16 AMX1 : 1;
        uint16 SZ : 1;
        uint16 AMX2 : 1;
        uint16 _rsvd8 : 1;
        uint16 RASD : 1;
        uint16 BE : 1;
        uint16 TRASn : 2;
        uint16 TRWL : 1;
        uint16 RCD : 1;
        uint16 TRP : 1;
    };
};

// 1F0  R/W  16,32    0000      RTCSR   Refresh Timer Control/Status Register
union RegRTCSR {
    uint16 u16;
    struct {
        uint16 _rsvd0 : 1;
        uint16 _rsvd1 : 1;
        uint16 _rsvd2 : 1;
        uint16 CKSn : 3;
        uint16 CMIE : 1;
        uint16 CMF : 1;
    };
};

// 1F4  R/W  16,32    0000      RTCNT   Refresh Timer Counter
using RegRTCNT = uint8;

// 1F8  R/W  16,32    0000      RTCOR   Refresh Timer Constant Register
using RegRTCOR = uint8;

} // namespace brimir::sh2
