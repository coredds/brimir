#pragma once

namespace brimir::sh2 {

// addr r/w  access   init      code    name
// 000  R/W  8        00        SMR     Serial Mode Register
//
//   b  r/w  code  description
//   7  R/W  C/nA  Communication Mode (0=async, 1=clocked sync)
//   6  R/W  CHR   Character Length (0=8-bit, 1=7-bit)
//   5  R/W  PE    Parity Enable (0=disable, 1=enable)
//   4  R/W  O/nE  Parity Mode (0=even, 1=odd)
//   3  R/W  STOP  Stop Bit Length (0=one, 1=two)
//   2  R/W  MP    Multiprocessor Mode (0=disabled, 1=enabled)
//   1  R/W  CKS1  Clock Select bit 1  (00=phi/4,  01=phi/16,
//   0  R/W  CKS0  Clock Select bit 0   10=phi/64, 11=phi/256)

// 001  R/W  8        FF        BRR     Bit Rate Register

// 002  R/W  8        00        SCR     Serial Control Register

// 003  R/W  8        FF        TDR     Transmit Data Register

// 004  R/W* 8        84        SSR     Serial Status Register
//   * Can only write a 0 to clear the flags

// 005  R    8        00        RDR     Receive Data Register

} // namespace brimir::sh2
