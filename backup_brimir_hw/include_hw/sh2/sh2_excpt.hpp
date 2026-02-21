#pragma once

#include <brimir/core/types.hpp>

namespace brimir::sh2 {

// Standard SH-2 exception vectors
inline constexpr uint8 xvHardResetPC = 0x00;      // 00  00000000  Power-on reset PC value
inline constexpr uint8 xvHardResetSP = 0x01;      // 01  00000004  Power-on reset SP value
inline constexpr uint8 xvSoftResetPC = 0x02;      // 02  00000008  Manual reset PC value
inline constexpr uint8 xvSoftResetSP = 0x03;      // 03  0000000C  Manual reset SP value
inline constexpr uint8 xvGenIllegalInstr = 0x04;  // 04  00000010  General illegal instruction
inline constexpr uint8 xvSlotIllegalInstr = 0x06; // 06  00000018  Slot illegal instruction
inline constexpr uint8 xvCPUAddressError = 0x09;  // 09  00000024  CPU address error
inline constexpr uint8 xvDMAAddressError = 0x0A;  // 0A  00000028  DMA address error
inline constexpr uint8 xvIntrNMI = 0x0B;          // 0B  0000002C  NMI interrupt
inline constexpr uint8 xvIntrUserBreak = 0x0C;    // 0C  00000030  User break interrupt
inline constexpr uint8 xvIntrLevel1 = 0x40;       // 40* 00000100  IRL1
inline constexpr uint8 xvIntrLevels23 = 0x41;     // 41* 00000104  IRL2 and IRL3
inline constexpr uint8 xvIntrLevels45 = 0x42;     // 42* 00000108  IRL4 and IRL5
inline constexpr uint8 xvIntrLevels67 = 0x43;     // 43* 0000010C  IRL6 and IRL7
inline constexpr uint8 xvIntrLevels89 = 0x44;     // 44* 00000110  IRL8 and IRL9
inline constexpr uint8 xvIntrLevelsAB = 0x45;     // 45* 00000114  IRL10 and IRL11
inline constexpr uint8 xvIntrLevelsCD = 0x46;     // 46* 00000118  IRL12 and IRL13
inline constexpr uint8 xvIntrLevelsEF = 0x47;     // 47* 0000011C  IRL14 and IRL15
                                                  //
                                                  // * denote auto-vector numbers;
                                                  //   values vary when using external vector fetch
                                                  //
                                                  // vectors 05, 07, 08, 0D through 1F are reserved
                                                  // vectors 20 through 3F are reserved for TRAPA

} // namespace brimir::sh2
