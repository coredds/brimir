#pragma once
// Pure transcriptions of SH-2 interpreter handlers (src/core/src/ymir/hw/sh2/sh2.cpp) used by
// every JIT backend. Reference: design/sh2-jit-handler-table.md section 9.
#include <cstdint>

namespace brimir::jit {

// DIV1 step. rmIsRn: the instruction has n == m, so Rm is read after Rn was shifted.
// Returns the new Rn; updates only Q (bit 8) and T (bit 0) of sr.
uint32_t Div1Step(uint32_t rn, uint32_t rm, bool rmIsRn, uint32_t &sr);
// MAC.W accumulate. op1 = sext16(@Rm), op2 = sext16(@Rn). Returns the new MAC (MACH:MACL).
uint64_t MacWStep(uint64_t mac, bool s, int32_t op1, int32_t op2);
// MAC.L accumulate. op1 = @Rm, op2 = @Rn. Returns the new MAC (MACH:MACL).
uint64_t MacLStep(uint64_t mac, bool s, int32_t op1, int32_t op2);

} // namespace brimir::jit
