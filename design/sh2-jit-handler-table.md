# M1d — SH-2 handler table for the JIT front end

Source of truth: `src/core/src/ymir/hw/sh2/sh2.cpp` (line numbers below are that file as of 2026-10-01).
Decoder: `src/core/src/ymir/hw/sh2/sh2_decode.cpp`. Target: `src/jit/src/frontend.cpp` `LowerPlain` / `LowerDelayedBranch`.

## 0. Shared facts (read first)

### Decode macros (sh2.cpp:2597-2656)

| Macro | Fields |
|---|---|
| `DECODE_NM` | `rn = bits[11:8]`, `rm = bits[7:4]` |
| `DECODE_N` | `rn = bits[11:8]` |
| `DECODE_M` | `rm = bits[11:8]` (**note: M format puts Rm in bits 11:8**, like JMP; frontend uses `Rn(instr)` for it) |
| `DECODE_MD(s)` | `rm = bits[7:4]`, `disp = bits[3:0] << s` (unsigned) |
| `DECODE_ND4(s)` | `rn = bits[7:4]`, `disp = bits[3:0] << s` (unsigned) |
| `DECODE_NMD(s)` | `rn = bits[11:8]`, `rm = bits[7:4]`, `disp = bits[3:0] << s` (unsigned) |
| `DECODE_D_U(s)` | `disp = bits[7:0] << s` (unsigned) |
| `DECODE_D_S(s)` | `disp = sext8(bits[7:0]) << s` |
| `DECODE_D12(s)` | `disp = sext12(bits[11:0]) << s` |
| `DECODE_ND8(s)` | `rn = bits[11:8]`, `disp = bits[7:0] << s` (unsigned) |
| `DECODE_I_U(s)` | `imm = bits[7:0] << s` (unsigned, zero-extended) |
| `DECODE_I_S(s)` | `imm = sext8(bits[7:0]) << s` (as uint32) |
| `DECODE_NI` | `rn = bits[11:8]`, `imm = sext8(bits[7:0])` |

`bit::sign_extend<B>` returns the signed type of the input (int16 for the uint16 opcode); assigned to `uint32` it is sign-extended to 32 bits. So `SImm8()` in frontend.cpp matches `DECODE_I_S(0)`/`DECODE_NI`.

### Helpers

- `WritebackCycles(regs...)` (sh2.cpp:1830-1833): `((regs == m_wbReg) || ...) ? 1 : 0` — at most 1 per call, regardless of how many regs match. `kWBRegPR = 0x10`, `kWBRegNone = 0xFF` (sh2.hpp:724-725). IR `WbStall(mask)` is exactly this (bit16 = PR).
- `AccessCycles<T,write,emulateCache>(addr)` (sh2.cpp:1007-1040). The JIT callback `JitAccessCycles` (sh2.cpp:4753) dispatches on size 1/2/4 and write — so `AddAccessCycles(addr, size, write)` reproduces *any* `AccessCycles<T,…>` term, including the mismatched-size ones in OR_M/XOR_M (see quirks). With emulateCache=false: partition 000 → 1; 001/101 → bus table `GetAccessCycles<T,write>`; 010/011/100/110 → 1; 111 → 4.
- `AccessCyclesRMWByte` (sh2.cpp:1042-1056) is **used only by TAS** (sh2.cpp:4424). AND_M / OR_M / XOR_M / TST_M do **not** use it. No new RMW cycle op is needed.
- `AdvancePC<delaySlot>` (sh2.cpp:2210-2223): non-slot `PC += 2`; slot: `PC = m_delaySlotTarget; if (PC & 2) RefillPipeline(); m_delaySlot = false; m_intrFlags.pending = INTC.pending.level > SR.ILevel`. IR: non-slot = nothing (block tracks pc), slot = `EndDelaySlot`.
- `SetupDelaySlot(target)` (sh2.cpp:2204-2208): `m_delaySlot = true; m_delaySlotTarget = target; m_intrFlags.pending = false`.
- `InterpretNext` (sh2.cpp:2250-2277): if `pending && allow` → interrupt entry; else **`m_intrFlags.allow = true`** then fetch/execute. So an instruction that clears `allow` blocks interrupt acceptance only *before the next instruction*.
- Interpreter cycles are returned at the end; `m_cyclesExecuted` is updated by the caller. So during any memory access in a handler, the timers see the count *before* this instruction → emit `SyncCycles` before the instruction's first `AddCycles`/`AddAccessCycles`/`WbStall` (as existing MOVL_L does). One `SyncCycles` covers both accesses of an RMW.
- Bus-wait convention (existing MOVL_L/MOVL_S lowering): cycles added *before* `ExitIfBusWait` are charged on the wait exit; `ExitIfBusWait(addr,size,write,pc_of_this_instr,retiredBefore)`. On wait the interpreter changes nothing (registers, memory, PC, `m_delaySlot`, `m_wbReg` all untouched) and returns only the cycles summed so far.
- In every non-branch handler below, all memory accesses happen **before** `AdvancePC` (matters in delay slots: the store/load precedes the delay-slot refill done by `EndDelaySlot`).
- Delay-slot capability: every non-branch opcode in this list is registered with `setOpcode` (sh2_decode.cpp) ⇒ has a `Delay_` variant dispatched with `delaySlot=true` (sh2.cpp:2445-2582). BSR, BRAF, BSRF, JSR use `setNonDelayOpcode` ⇒ `IllegalSlot` in a delay slot. Only MOVW_I, MOVL_I and MOVA have delaySlot-dependent *semantics* (PC base `m_delaySlotTarget - 2`); LDC SR has a delaySlot-dependent `pending` expression (net effect identical, see §LDC).

### Notation for the IR rows

- `adv()` = the existing `advance` lambda (`EndDelaySlot` if delaySlot).
- `R(x)` = `GetReg(x)`, `C(k)` = `Const(k)`, `B(x)` = `RegBit(x)`, `PRB` = `kWbPRBit`.
- `BW(addr,sz,w)` = `ExitIfBusWait(addr, sz, w, pc, retiredBefore)`.
- "ALU template" =
  `<compute/SetReg/SetT>; adv(); WbStall(mask); AddCycles(1); SetWb(kWbNone);`
  (interpreter computes `WritebackCycles` after `AdvancePC`; `AdvancePC` never touches `m_wbReg`, so the order only has to keep `WbStall` before `SetWb`).

---

## 1. Proposed new IR ops (minimal set)

All values are uint32; booleans are 0/1.

| Op | Operands | Semantics (exact) |
|---|---|---|
| `And` | a, b | `a & b` |
| `Or` | a, b | `a \| b` |
| `Xor` | a, b | `a ^ b` |
| `Not` | a | `~a` |
| `Shl` | a, imm (1..31) | `a << imm` |
| `Shr` | a, imm (1..31) | `a >> imm` (logical) |
| `Sar` | a, imm (1..31) | `uint32(int32(a) >> imm)` (arithmetic) |
| `CmpGtU` | a, b | `a > b ? 1 : 0` (unsigned) |
| `CmpGeU` | a, b | `a >= b ? 1 : 0` (unsigned) |
| `CmpGtS` | a, b | `int32(a) > int32(b) ? 1 : 0` |
| `CmpGeS` | a, b | `int32(a) >= int32(b) ? 1 : 0` |
| `CmpStr` *(optional; can be composed, see CMP_STR)* | a, b | `t = a ^ b; ((t>>24)&0xFF)==0 \|\| ((t>>16)&0xFF)==0 \|\| ((t>>8)&0xFF)==0 \|\| (t&0xFF)==0 ? 1 : 0` |
| `GetGBR` / `SetGBR` | – / a | `*ctx.GBR` read / `*ctx.GBR = a` |
| `GetVBR` / `SetVBR` | – / a | `*ctx.VBR` read / `*ctx.VBR = a` |
| `SetPR` | a | `*ctx.PR = a` |
| `GetSR` | – | `*ctx.SR` (full RegSR::u32) |
| `SetSR` | a, flag=delaySlot | via new callback: `SR.u32 = a & 0x3F3; m_intrFlags.pending = !delaySlot && INTC.pending.level > SR.ILevel; m_intrFlags.allow = false` (LDC SR, sh2.cpp:3337-3341). Needs a new `SH2JitContext` callback (INTC is not exposed). |
| `GetMACH` / `GetMACL` | – | `MAC.H` / `MAC.L` |
| `SetMACH` / `SetMACL` | a | `MAC.H = a` / `MAC.L = a` (CLRMAC = both 0). **`SH2JitContext` has no MAC pointer today** — add `uint32 *MACL, *MACH` (or `uint64 *MAC`, RegMAC layout: L = low word, H = high word; sh2_regs.hpp:8-14). |
| `ClearIntrAllow` | – | `*ctx.intrAllow = false` |
| `SetIntrAllow` | – | `*ctx.intrAllow = true` (emit right after the `CheckBoundary` of the instruction following an allow-clearing one; see §6) |
| `GetDelayTarget` | – | `*ctx.delaySlotTarget` — only needed for MOVA/MOVW_I/MOVL_I in the slot of a *dynamic-target* branch (JMP, RTS, BRAF, BSRF, JSR). For BRA/BT/S/BF/S/BSR the target is a compile-time constant. Alternative: pass the target `ValueId` from `LowerDelayedBranch` into `LowerPlain`. |

Everything else (ADDC/SUBC/NEGC carry, ADDV/SUBV overflow, rotates, swaps) composes bit-exactly from the ops above plus existing `Add/Sub/CmpEq/GetT/SetT/SExt8/SExt16`; formulas are given per opcode. No fused carry/overflow op is required (optional optimisation only).

---

## 2. Data movement — loads

### MOVW_L — `mov.w @Rm,Rn` — 0110nnnnmmmm0001 — sh2.cpp:2719-2732
1. `DECODE_NM`. Delay_ variant: yes, no slot-specific behaviour.
2. Steps: `address = R[rm]`; `cycles = AccessCycles<u16,read>(address)`; **if !IsBusWait(address,2,false)**: `R[rn] = sext16(MemReadWord(address))`; AdvancePC; `cycles += WB(rm)`; `m_wbReg = rn`.
3. Cycles: `Access16R(R[rm])` + `WB(rm)` (after the wait check, success only).
4. m_wbReg: `rn`; on wait unchanged.
5. Bus-wait: yes, size 2, read. Wait: nothing changed, returns `Access16R` only.
6. IR: `SyncCycles; a=R(m); AddAccessCycles(a,2,false); BW(a,2,false); SetReg(n, SExt16(Load(a,2,false))); adv(); WbStall(B(m)); SetWb(n);`

### MOVB_L0 — `mov.b @(R0,Rm),Rn` — 0000nnnnmmmm1100 — sh2.cpp:2751-2761
1. `DECODE_NM`. Delay_: yes.
2. `address = R[rm] + R[0]`; `cycles = Access8R(address) + WB(rm, 0)`; `R[rn] = sext8(MemReadByte)`; AdvancePC; `m_wbReg = rn`.
3. Cycles: `Access8R(addr) + WB(rm,R0)`. No constant.
4. m_wbReg `rn`. 5. No bus-wait check.
6. IR: `SyncCycles; a=Add(R(m),R(0)); AddAccessCycles(a,1,false); WbStall(B(m)|B(0)); SetReg(n, SExt8(Load(a,1,false))); adv(); SetWb(n);`

### MOVW_L0 — `mov.w @(R0,Rm),Rn` — 0000nnnnmmmm1101 — sh2.cpp:2764-2777
1. `DECODE_NM`. Delay_: yes.
2. `address = R[rm]+R[0]`; `cycles = Access16R`; if !wait(addr,2,false): `R[rn]=sext16(load16)`; AdvancePC; `cycles += WB(rm,0)`; `m_wbReg=rn`.
3. `Access16R + WB(rm,R0)` (after wait check). 4. `rn`; unchanged on wait. 5. yes, 2, read.
6. IR: `SyncCycles; a=Add(R(m),R(0)); AddAccessCycles(a,2,false); BW(a,2,false); SetReg(n,SExt16(Load(a,2,false))); adv(); WbStall(B(m)|B(0)); SetWb(n);`

### MOVL_L0 — `mov.l @(R0,Rm),Rn` — 0000nnnnmmmm1110 — sh2.cpp:2780-2793
Same as MOVW_L0 with size 4 and no extension.
IR: `SyncCycles; a=Add(R(m),R(0)); AddAccessCycles(a,4,false); BW(a,4,false); SetReg(n,Load(a,4,false)); adv(); WbStall(B(m)|B(0)); SetWb(n);`

### MOVB_L4 — `mov.b @(disp,Rm),R0` — 10000100mmmmdddd — sh2.cpp:2796-2805
1. `DECODE_MD(0)`: `rm = bits[7:4]`, `disp = bits[3:0]` (unsigned, ×1). Delay_: yes.
2. `address = R[rm] + disp`; `cycles = Access8R(address)`; `R[0] = sext8(load8)`; AdvancePC; `m_wbReg = 0`.
3. Cycles: `Access8R(addr)` **only — no WritebackCycles at all** (quirk; MOVW_L4 has one).
4. m_wbReg 0. 5. No bus-wait.
6. IR: `SyncCycles; a=Add(R(m),C(instr&0xF)); AddAccessCycles(a,1,false); SetReg(0,SExt8(Load(a,1,false))); adv(); SetWb(0);`

### MOVW_L4 — `mov.w @(disp,Rm),R0` — 10000101mmmmdddd — sh2.cpp:2808-2820
1. `DECODE_MD(1)`: `rm = bits[7:4]`, `disp = bits[3:0] << 1`. Delay_: yes.
2. `address = R[rm]+disp`; **`cycles = Access16R(address) + WB(rm)`**; if !wait(addr,2,false): `R[0]=sext16(load16)`; AdvancePC; **`cycles += WB(rm)`** (again); `m_wbReg = 0`.
3. Cycles: success = `Access16R + 2*WB(rm)`; wait = `Access16R + WB(rm)`. **Quirk: WritebackCycles(rm) counted twice.** Both evaluations see the same `m_wbReg`.
4. m_wbReg 0; unchanged on wait. 5. Bus-wait yes, 2, read.
6. IR: `SyncCycles; a=Add(R(m),C((instr&0xF)<<1)); AddAccessCycles(a,2,false); WbStall(B(m)); BW(a,2,false); SetReg(0,SExt16(Load(a,2,false))); adv(); WbStall(B(m)); SetWb(0);`

### MOVL_L4 — `mov.l @(disp,Rm),Rn` — 0101nnnnmmmmdddd — sh2.cpp:2823-2836
1. `DECODE_NMD(2)`: `rn=bits[11:8]`, `rm=bits[7:4]`, `disp = bits[3:0] << 2`. Delay_: yes.
2. `address=R[rm]+disp`; `cycles=Access32R`; if !wait(addr,4,false): `R[rn]=load32`; AdvancePC; `cycles += WB(rm)`; `m_wbReg=rn`.
3. `Access32R + WB(rm)` (after check, once). 4. `rn`. 5. yes, 4, read.
6. IR: `SyncCycles; a=Add(R(m),C((instr&0xF)<<2)); AddAccessCycles(a,4,false); BW(a,4,false); SetReg(n,Load(a,4,false)); adv(); WbStall(B(m)); SetWb(n);`

### MOVB_LG — `mov.b @(disp,GBR),R0` — 11000100dddddddd — sh2.cpp:2839-2848
1. `DECODE_D_U(0)`: `disp = bits[7:0]` unsigned. Delay_: yes.
2. `address = GBR + disp`; `cycles = Access8R`; `R[0]=sext8(load8)`; AdvancePC; `m_wbReg=0`.
3. `Access8R` only (no WB; GBR has no writeback tracking). 4. 0. 5. No bus-wait.
6. IR: `SyncCycles; a=Add(GetGBR(),C(instr&0xFF)); AddAccessCycles(a,1,false); SetReg(0,SExt8(Load(a,1,false))); adv(); SetWb(0);`

### MOVW_LG — `mov.w @(disp,GBR),R0` — 11000101dddddddd — sh2.cpp:2851-2862
1. `DECODE_D_U(1)`: `disp = bits[7:0] << 1`. Delay_: yes.
2. `address=GBR+disp`; `cycles=Access16R`; if !wait(addr,2,false): `R[0]=sext16(load16)`; AdvancePC; `m_wbReg=0`.
3. `Access16R` only, no WB anywhere. 4. 0; unchanged on wait. 5. yes, 2, read.
6. IR: `SyncCycles; a=Add(GetGBR(),C((instr&0xFF)<<1)); AddAccessCycles(a,2,false); BW(a,2,false); SetReg(0,SExt16(Load(a,2,false))); adv(); SetWb(0);`

### MOVL_LG — `mov.l @(disp,GBR),R0` — 11000110dddddddd — sh2.cpp:2865-2876
As MOVW_LG, `disp = bits[7:0] << 2`, size 4, no extension, no WB.
IR: `SyncCycles; a=Add(GetGBR(),C((instr&0xFF)<<2)); AddAccessCycles(a,4,false); BW(a,4,false); SetReg(0,Load(a,4,false)); adv(); SetWb(0);`

### MOVB_P — `mov.b @Rm+,Rn` — 0110nnnnmmmm0100 — sh2.cpp:2927-2941
1. `DECODE_NM`. Delay_: yes.
2. `address=R[rm]`; `cycles = Access8R + WB(rm)`; `R[rn]=sext8(load8)`; **`if (rn != rm) R[rm] += 1`**; AdvancePC; `m_wbReg = rn`.
3. `Access8R(R[rm]) + WB(rm)`. 4. `rn`. 5. No bus-wait.
6. IR: `SyncCycles; a=R(m); AddAccessCycles(a,1,false); WbStall(B(m)); v=SExt8(Load(a,1,false)); SetReg(n,v); if(n!=m) SetReg(m,Add(a,C(1))); adv(); SetWb(n);`
   (`a` is the pre-load R[rm]; when n≠m R[rm] isn't modified by the load, so `a+1` is exact.)

### MOVW_P — `mov.w @Rm+,Rn` — 0110nnnnmmmm0101 — sh2.cpp:2944-2961
1. `DECODE_NM`. Delay_: yes.
2. `address=R[rm]`; `cycles=Access16R`; if !wait(addr,2,false): `R[rn]=sext16(load16)`; `if(rn!=rm) R[rm]+=2`; AdvancePC; `cycles += WB(rm)`; `m_wbReg=rn`.
3. `Access16R + WB(rm)` (after check). 4. `rn`; unchanged on wait. 5. yes, 2, read.
6. IR: `SyncCycles; a=R(m); AddAccessCycles(a,2,false); BW(a,2,false); SetReg(n,SExt16(Load(a,2,false))); if(n!=m) SetReg(m,Add(a,C(2))); adv(); WbStall(B(m)); SetWb(n);`

### MOVL_P — `mov.l @Rm+,Rn` — 0110nnnnmmmm0110 — sh2.cpp:2964-2981
As MOVW_P with size 4, increment 4, no extension.
IR: `SyncCycles; a=R(m); AddAccessCycles(a,4,false); BW(a,4,false); SetReg(n,Load(a,4,false)); if(n!=m) SetReg(m,Add(a,C(4))); adv(); WbStall(B(m)); SetWb(n);`

### MOVW_I — `mov.w @(disp,PC),Rn` — 1001nnnndddddddd — sh2.cpp:3164-3175
1. `DECODE_ND8(1)`: `rn=bits[11:8]`, `disp = bits[7:0] << 1` (unsigned). Delay_: yes, **slot-dependent PC base**.
2. `pc = delaySlot ? m_delaySlotTarget - 2 : PC`; `address = pc + disp + 4` (**no `& ~3`**); `cycles = Access16R(address)`; `R[rn] = sext16(MemReadWord<emulateCache, instrFetch=true>(address))`; AdvancePC; `m_wbReg = rn`.
3. `Access16R(addr)` only; no WB, no constant. 4. `rn`. 5. No bus-wait.
6. IR (non-slot): `SyncCycles; a=C(pc + ((instr&0xFF)<<1) + 4); AddAccessCycles(a,2,false); SetReg(n,SExt16(Load(a,2,true))); SetWb(n);`
   IR (slot, constant target T): `a = C(T - 2 + disp + 4)`; (slot, dynamic target): `a = Add(GetDelayTarget(), C(disp + 2))`; then same, with `adv()` after `SetReg`.

### MOVL_I in a delay slot — `mov.l @(disp,PC),Rn` — 1101nnnndddddddd — sh2.cpp:3178-3189
1. `DECODE_ND8(2)`: `disp = bits[7:0] << 2`.
2. `pc = m_delaySlotTarget - 2`; `address = (pc & ~3u) + disp + 4`; `cycles = Access32R(address)`; `R[rn] = MemReadLong<emulateCache, instrFetch=true>(address)`; AdvancePC (slot: PC=target, refill if target&2, pending recompute); `m_wbReg = rn`.
3. `Access32R` only. 4. `rn`. 5. No bus-wait.
6. IR (constant target T): `SyncCycles; a=C(((T-2)&~3u) + disp + 4); AddAccessCycles(a,4,false); SetReg(n,Load(a,4,true)); adv(); SetWb(n);`
   (dynamic target): `a = Add(And(Sub(GetDelayTarget(),C(2)),C(~3u)), C(disp+4))`.
   Note: existing non-slot MOVL_I lowering omits `adv()` — correct only because it's non-slot; slot variant must call `adv()` after the load.

### MOVA — `mova @(disp,PC),R0` — 11000111dddddddd — sh2.cpp:3192-3202
1. `DECODE_D_U(2)`: `disp = bits[7:0] << 2`. Delay_: yes, slot-dependent PC base.
2. `pc = delaySlot ? m_delaySlotTarget - 2 : PC`; `R[0] = (pc & ~3u) + disp + 4`; AdvancePC; `cycles = WB(0) + 1`; `m_wbReg = None`.
3. `WB(R0) + 1`. 4. None. 5. No memory access.
6. IR: non-slot `SetReg(0, C((pc&~3u)+disp+4))`; slot constant T `SetReg(0, C(((T-2)&~3u)+disp+4))`; slot dynamic `SetReg(0, Add(And(Sub(GetDelayTarget(),C(2)),C(~3u)),C(disp+4)))`; then ALU template with mask `B(0)`.

## 3. Data movement — stores

### MOVW_S — `mov.w Rm,@Rn` — 0010nnnnmmmm0001 — sh2.cpp:2996-3008
1. `DECODE_NM`. Delay_: yes.
2. `address=R[rn]`; `cycles=Access16W`; if !wait(addr,2,true): `MemWriteWord(address, R[rm])` (low 16 bits); AdvancePC; `cycles += WB(rm,rn)`; `m_wbReg=None`.
3. `Access16W + WB(rm,rn)` (after check). 4. None; unchanged on wait. 5. yes, 2, write.
6. IR: `SyncCycles; a=R(n); AddAccessCycles(a,2,true); BW(a,2,true); Store(a,2,R(m)); adv(); WbStall(B(m)|B(n)); SetWb(kWbNone);`

### MOVB_M — `mov.b Rm,@-Rn` — 0010nnnnmmmm0100 — sh2.cpp:2879-2890
1. `DECODE_NM`. Delay_: yes.
2. `address = R[rn] - 1`; `cycles = Access8W(address) + WB(rm,rn)`; `MemWriteByte(address, R[rm])` (R[rm] read *before* R[rn] update, so rn==rm stores the original value); `R[rn] = address`; AdvancePC; `m_wbReg=None`.
3. `Access8W(R[rn]-1) + WB(rm,rn)`. 4. None. 5. No bus-wait.
6. IR: `SyncCycles; a=Sub(R(n),C(1)); AddAccessCycles(a,1,true); WbStall(B(m)|B(n)); Store(a,1,R(m)); SetReg(n,a); adv(); SetWb(kWbNone);`

### MOVW_M — `mov.w Rm,@-Rn` — 0010nnnnmmmm0101 — sh2.cpp:2893-2907
1. `DECODE_NM`. Delay_: yes.
2. `address=R[rn]-2`; `cycles=Access16W(address)`; if !wait(addr,2,true): write16(address,R[rm]); `R[rn]=address`; AdvancePC; `cycles += WB(rm,rn)`; `m_wbReg=None`.
3. `Access16W + WB(rm,rn)` after check. 4. None; on wait R[rn] NOT decremented. 5. yes, 2, write.
6. IR: `SyncCycles; a=Sub(R(n),C(2)); AddAccessCycles(a,2,true); BW(a,2,true); Store(a,2,R(m)); SetReg(n,a); adv(); WbStall(B(m)|B(n)); SetWb(kWbNone);`

### MOVL_M — `mov.l Rm,@-Rn` — 0010nnnnmmmm0110 — sh2.cpp:2910-2924
As MOVW_M with 4. IR: `SyncCycles; a=Sub(R(n),C(4)); AddAccessCycles(a,4,true); BW(a,4,true); Store(a,4,R(m)); SetReg(n,a); adv(); WbStall(B(m)|B(n)); SetWb(kWbNone);`

### MOVB_S0 — `mov.b Rm,@(R0,Rn)` — 0000nnnnmmmm0100 — sh2.cpp:3026-3035
1. `DECODE_NM`. Delay_: yes.
2. `address=R[rn]+R[0]`; `cycles = Access8W + WB(rn, 0)` (**rm not in the set**); write8(address,R[rm]); AdvancePC; None.
3. `Access8W + WB(rn,R0)`. 4. None. 5. No bus-wait.
6. IR: `SyncCycles; a=Add(R(n),R(0)); AddAccessCycles(a,1,true); WbStall(B(n)|B(0)); Store(a,1,R(m)); adv(); SetWb(kWbNone);`

### MOVW_S0 — `mov.w Rm,@(R0,Rn)` — 0000nnnnmmmm0101 — sh2.cpp:3038-3050
`address=R[rn]+R[0]`; `Access16W`; wait(addr,2,true); write16; AdvancePC; `+= WB(rn,0)` after check; None.
IR: `SyncCycles; a=Add(R(n),R(0)); AddAccessCycles(a,2,true); BW(a,2,true); Store(a,2,R(m)); adv(); WbStall(B(n)|B(0)); SetWb(kWbNone);`

### MOVL_S0 — `mov.l Rm,@(R0,Rn)` — 0000nnnnmmmm0110 — sh2.cpp:3053-3065
Same with 4. IR: `SyncCycles; a=Add(R(n),R(0)); AddAccessCycles(a,4,true); BW(a,4,true); Store(a,4,R(m)); adv(); WbStall(B(n)|B(0)); SetWb(kWbNone);`

### MOVB_S4 — `mov.b R0,@(disp,Rn)` — 10000000nnnndddd — sh2.cpp:3068-3077
1. `DECODE_ND4(0)`: **`rn = bits[7:4]`**, `disp = bits[3:0]`. Delay_: yes.
2. `address=R[rn]+disp`; `cycles=Access8W + WB(rn,0)`; write8(address,R[0]); AdvancePC; None.
3. `Access8W + WB(rn,R0)`. 5. No bus-wait.
6. IR: `n4=(instr>>4)&0xF; SyncCycles; a=Add(R(n4),C(instr&0xF)); AddAccessCycles(a,1,true); WbStall(B(n4)|B(0)); Store(a,1,R(0)); adv(); SetWb(kWbNone);`

### MOVW_S4 — `mov.w R0,@(disp,Rn)` — 10000001nnnndddd — sh2.cpp:3080-3092
`DECODE_ND4(1)`: `rn=bits[7:4]`, `disp=bits[3:0]<<1`. `Access16W`; wait(addr,2,true); write16(R[0]); AdvancePC; `+= WB(rn,0)` after check; None.
IR: `SyncCycles; a=Add(R(n4),C((instr&0xF)<<1)); AddAccessCycles(a,2,true); BW(a,2,true); Store(a,2,R(0)); adv(); WbStall(B(n4)|B(0)); SetWb(kWbNone);`

### MOVL_S4 — `mov.l Rm,@(disp,Rn)` — 0001nnnnmmmmdddd — sh2.cpp:3095-3107
`DECODE_NMD(2)`: `rn=bits[11:8]`, `rm=bits[7:4]`, `disp=bits[3:0]<<2`. `Access32W`; wait(addr,4,true); write32(R[rm]); AdvancePC; `+= WB(rm,rn)` after check; None.
IR: `SyncCycles; a=Add(R(n),C((instr&0xF)<<2)); AddAccessCycles(a,4,true); BW(a,4,true); Store(a,4,R(m)); adv(); WbStall(B(m)|B(n)); SetWb(kWbNone);`

### MOVB_SG — `mov.b R0,@(disp,GBR)` — 11000000dddddddd — sh2.cpp:3110-3119
`DECODE_D_U(0)`. `address=GBR+disp`; `cycles = Access8W + WB(0)`; write8(R[0]); AdvancePC; None. No bus-wait.
IR: `SyncCycles; a=Add(GetGBR(),C(instr&0xFF)); AddAccessCycles(a,1,true); WbStall(B(0)); Store(a,1,R(0)); adv(); SetWb(kWbNone);`

### MOVW_SG — `mov.w R0,@(disp,GBR)` — 11000001dddddddd — sh2.cpp:3122-3134
`disp<<1`; `Access16W`; wait(addr,2,true); write16(R[0]); AdvancePC; `+= WB(0)` after check; None.
IR: `SyncCycles; a=Add(GetGBR(),C((instr&0xFF)<<1)); AddAccessCycles(a,2,true); BW(a,2,true); Store(a,2,R(0)); adv(); WbStall(B(0)); SetWb(kWbNone);`

### MOVL_SG — `mov.l R0,@(disp,GBR)` — 11000010dddddddd — sh2.cpp:3137-3149
Same with `disp<<2`, size 4.
IR: `SyncCycles; a=Add(GetGBR(),C((instr&0xFF)<<2)); AddAccessCycles(a,4,true); BW(a,4,true); Store(a,4,R(0)); adv(); WbStall(B(0)); SetWb(kWbNone);`

## 4. Register-only ops (all: Delay_ yes, no memory, no bus-wait, m_wbReg = None at end, cycles = WB(set)+1 unless noted, AdvancePC before WB evaluation)

| Opcode | Encoding / decode | sh2.cpp | Semantics (handler order) | Cycles | IR (then ALU template with mask) |
|---|---|---|---|---|---|
| MOVT | 0000nnnn00101001, N | 3205-3214 | `R[rn] = SR.T` | WB(rn)+1 | `SetReg(n, GetT())`; mask B(n) |
| CLRT | 0000000000001000 | 3217-3223 | `SR.T = 0` | **1** (no WB) | `SetT(C(0)); adv(); AddCycles(1); SetWb(kWbNone)` |
| SETT | 0000000000011000 | 3226-3232 | `SR.T = 1` | **1** | `SetT(C(1)); adv(); AddCycles(1); SetWb(kWbNone)` |
| EXTSB | 0110nnnnmmmm1110, NM | 3235-3244 | `R[rn] = sext8(R[rm])` | WB(rm,rn)+1 | `SetReg(n,SExt8(R(m)))`; B(m)\|B(n) |
| EXTSW | 0110nnnnmmmm1111 | 3247-3256 | `R[rn] = sext16(R[rm])` | WB(rm,rn)+1 | `SetReg(n,SExt16(R(m)))` |
| EXTUB | 0110nnnnmmmm1100 | 3259-3268 | `R[rn] = R[rm] & 0xFF` | WB(rm,rn)+1 | `SetReg(n,And(R(m),C(0xFF)))` |
| EXTUW | 0110nnnnmmmm1101 | 3271-3280 | `R[rn] = R[rm] & 0xFFFF` | WB(rm,rn)+1 | `SetReg(n,And(R(m),C(0xFFFF)))` |
| SWAPB | 0110nnnnmmmm1000 | 3283-3294 | `tmp0=R[rm]&0xFFFF0000; tmp1=(R[rm]&0xFF)<<8; R[rn]=((R[rm]>>8)&0xFF)\|tmp1\|tmp0` | WB(rm,rn)+1 | `x=R(m); SetReg(n, Or(Or(And(Shr(x,8),C(0xFF)), Shl(And(x,C(0xFF)),8)), And(x,C(0xFFFF0000))))` |
| SWAPW | 0110nnnnmmmm1001 | 3297-3307 | `R[rn] = (R[rm]<<16) \| (R[rm]>>16)` | WB(rm,rn)+1 | `x=R(m); SetReg(n, Or(Shl(x,16),Shr(x,16)))` |
| XTRCT | 0010nnnnmmmm1101 | 3310-3319 | `R[rn] = (R[rn]>>16) \| (R[rm]<<16)` | WB(rm,rn)+1 | `SetReg(n, Or(Shr(R(n),16), Shl(R(m),16)))` |
| ADDC | 0011nnnnmmmm1110 | 3682-3695 | `tmp1=R[rn]+R[rm]; tmp0=R[rn]; R[rn]=tmp1+T; T=(tmp0>tmp1)\|\|(tmp1>R[rn])` (unsigned) | WB(rm,rn)+1 | `a=R(n); t1=Add(a,R(m)); r=Add(t1,GetT()); SetReg(n,r); SetT(Or(CmpGtU(a,t1),CmpGtU(t1,r)))` |
| ADDV | 0011nnnnmmmm1111 | 3698-3716 | `dst=int(R[rn])<0; src=int(R[rm])<0; R[rn]+=R[rm]; ans=(int(R[rn])<0)^dst; T=(src==dst)&ans` | WB(rm,rn)+1 | `a=R(n); b=R(m); r=Add(a,b); SetReg(n,r); d=Shr(a,31); s=Shr(b,31); SetT(And(CmpEq(s,d), Xor(Shr(r,31),d)))` |
| AND_R | 0010nnnnmmmm1001 | 3719-3728 | `R[rn] &= R[rm]` | WB(rm,rn)+1 | `SetReg(n,And(R(n),R(m)))` |
| AND_I | 11001001iiiiiiii, I_U(0) | 3731-3739 | `R[0] &= imm` (imm zero-ext 8-bit) | WB(0)+1 | `SetReg(0,And(R(0),C(instr&0xFF)))`; B(0) |
| NEG | 0110nnnnmmmm1011 | 3757-3766 | `R[rn] = -R[rm]` (mod 2^32) | WB(rm,rn)+1 | `SetReg(n,Sub(C(0),R(m)))` |
| NEGC | 0110nnnnmmmm1010 | 3769-3780 | `tmp=-R[rm]; R[rn]=tmp-T; T=(0<tmp)\|\|(tmp<R[rn])` (unsigned) | WB(rm,rn)+1 | `tmp=Sub(C(0),R(m)); r=Sub(tmp,GetT()); SetReg(n,r); SetT(Or(CmpGtU(tmp,C(0)),CmpGtU(r,tmp)))` (T read before SetReg — fine, T is SR) |
| NOT | 0110nnnnmmmm0111 | 3783-3792 | `R[rn] = ~R[rm]` | WB(rm,rn)+1 | `SetReg(n,Not(R(m)))` |
| OR_R | 0010nnnnmmmm1011 | 3795-3804 | `R[rn] \|= R[rm]` | WB(rm,rn)+1 | `SetReg(n,Or(R(n),R(m)))` |
| OR_I | 11001011iiiiiiii | 3807-3815 | `R[0] \|= imm` (zero-ext) | WB(0)+1 | `SetReg(0,Or(R(0),C(instr&0xFF)))`; B(0) |
| ROTCL | 0100nnnn00100100, N | 3833-3844 | `tmp=R[rn]>>31; R[rn]=(R[rn]<<1)\|T; T=tmp` | WB(rn)+1 | `x=R(n); SetReg(n,Or(Shl(x,1),GetT())); SetT(Shr(x,31))` (GetT before SetT) |
| ROTCR | 0100nnnn00100101 | 3847-3858 | `tmp=R[rn]&1; R[rn]=(R[rn]>>1)\|(T<<31); T=tmp` | WB(rn)+1 | `x=R(n); SetReg(n,Or(Shr(x,1),Shl(GetT(),31))); SetT(And(x,C(1)))` |
| ROTL | 0100nnnn00000100 | 3861-3871 | `T=R[rn]>>31; R[rn]=(R[rn]<<1)\|T` | WB(rn)+1 | `x=R(n); SetT(Shr(x,31)); SetReg(n,Or(Shl(x,1),Shr(x,31)))` |
| ROTR | 0100nnnn00000101 | 3874-3884 | `T=R[rn]&1; R[rn]=(R[rn]>>1)\|(T<<31)` | WB(rn)+1 | `x=R(n); SetT(And(x,C(1))); SetReg(n,Or(Shr(x,1),Shl(x,31)))` |
| SHAL | 0100nnnn00100000 | 3887-3897 | `T=R[rn]>>31; R[rn]<<=1` | WB(rn)+1 | `x=R(n); SetT(Shr(x,31)); SetReg(n,Shl(x,1))` |
| SHAR | 0100nnnn00100001 | 3900-3910 | `T=R[rn]&1; R[rn]=int(R[rn])>>1` | WB(rn)+1 | `x=R(n); SetT(And(x,C(1))); SetReg(n,Sar(x,1))` |
| SHLL | 0100nnnn00000000 | 3913-3923 | identical to SHAL | WB(rn)+1 | same as SHAL |
| SHLL2 | 0100nnnn00001000 | 3926-3935 | `R[rn]<<=2` (T unchanged) | WB(rn)+1 | `SetReg(n,Shl(R(n),2))` |
| SHLL8 | 0100nnnn00011000 | 3938-3947 | `R[rn]<<=8` | WB(rn)+1 | `SetReg(n,Shl(R(n),8))` |
| SHLL16 | 0100nnnn00101000 | 3950-3959 | `R[rn]<<=16` | WB(rn)+1 | `SetReg(n,Shl(R(n),16))` |
| SHLR | 0100nnnn00000001 | 3962-3972 | `T=R[rn]&1; R[rn]>>=1` (logical) | WB(rn)+1 | `x=R(n); SetT(And(x,C(1))); SetReg(n,Shr(x,1))` |
| SHLR2 | 0100nnnn00001001 | 3975-3984 | `R[rn]>>=2` | WB(rn)+1 | `SetReg(n,Shr(R(n),2))` |
| SHLR8 | 0100nnnn00011001 | 3987-3996 | `R[rn]>>=8` | WB(rn)+1 | `SetReg(n,Shr(R(n),8))` |
| SHLR16 | 0100nnnn00101001 | 3999-4008 | `R[rn]>>=16` | WB(rn)+1 | `SetReg(n,Shr(R(n),16))` |
| SUB | 0011nnnnmmmm1000 | 4011-4021 | `R[rn] -= R[rm]` | WB(rm,rn)+1 | `SetReg(n,Sub(R(n),R(m)))` |
| SUBC | 0011nnnnmmmm1010 | 4024-4038 | `tmp1=R[rn]-R[rm]; tmp0=R[rn]; R[rn]=tmp1-T; T=(tmp0<tmp1)\|\|(tmp1<R[rn])` | WB(rm,rn)+1 | `a=R(n); t1=Sub(a,R(m)); r=Sub(t1,GetT()); SetReg(n,r); SetT(Or(CmpGtU(t1,a),CmpGtU(r,t1)))` |
| SUBV | 0011nnnnmmmm1011 | 4041-4059 | `dst=int(R[rn])<0; src=int(R[rm])<0; R[rn]-=R[rm]; ans=(int(R[rn])<0)^dst; T=(src!=dst)&ans` | WB(rm,rn)+1 | `a=R(n); b=R(m); r=Sub(a,b); SetReg(n,r); d=Shr(a,31); s=Shr(b,31); SetT(And(Xor(s,d), Xor(Shr(r,31),d)))` |
| XOR_R | 0010nnnnmmmm1010 | 4062-4071 | `R[rn] ^= R[rm]` | WB(rm,rn)+1 | `SetReg(n,Xor(R(n),R(m)))` |
| XOR_I | 11001010iiiiiiii | 4074-4082 | `R[0] ^= imm` (zero-ext) | WB(0)+1 | `SetReg(0,Xor(R(0),C(instr&0xFF)))`; B(0) |
| CMP_EQ_I | 10001000iiiiiiii, **I_S(0)** | 4316-4324 | `T = (int32(R[0]) == imm)` where imm = sext8 (comparison in uint32 after usual conversions ⇒ `R0 == sext8(i)`) | WB(0)+1 | `SetT(CmpEq(R(0),C(SImm8(instr))))`; B(0) |
| CMP_GE | 0011nnnnmmmm0011 | 4338-4346 | `T = int(R[rn]) >= int(R[rm])` | WB(rm,rn)+1 | `SetT(CmpGeS(R(n),R(m)))` |
| CMP_GT | 0011nnnnmmmm0111 | 4349-4357 | `T = int(R[rn]) > int(R[rm])` | WB(rm,rn)+1 | `SetT(CmpGtS(R(n),R(m)))` |
| CMP_HI | 0011nnnnmmmm0110 | 4360-4368 | `T = R[rn] > R[rm]` (unsigned) | WB(rm,rn)+1 | `SetT(CmpGtU(R(n),R(m)))` |
| CMP_HS | 0011nnnnmmmm0010 | 4371-4379 | `T = R[rn] >= R[rm]` (unsigned) | WB(rm,rn)+1 | `SetT(CmpGeU(R(n),R(m)))` |
| CMP_PL | 0100nnnn00010101, N | 4382-4390 | `T = int(R[rn]) > 0` | WB(rn)+1 | `SetT(CmpGtS(R(n),C(0)))`; B(n) |
| CMP_PZ | 0100nnnn00010001 | 4393-4401 | `T = int(R[rn]) >= 0` | WB(rn)+1 | `SetT(CmpGeS(R(n),C(0)))` (or `SetT(CmpEq(Shr(x,31),C(0)))`) |
| CMP_STR | 0010nnnnmmmm1100 | 4404-4417 | `tmp=R[rm]^R[rn]; T = !(hh && hl && lh && ll)` (bytes of tmp; T=1 iff any byte is 0) | WB(rm,rn)+1 | `SetT(CmpStr(R(n),R(m)))`, or composed: `t=Xor(R(m),R(n)); z(k)=CmpEq(And(Shr(t,k),C(0xFF)),C(0))` (k=24,16,8; k=0 → `CmpEq(And(t,C(0xFF)),C(0))`); `SetT(Or(Or(z24,z16),Or(z8,z0)))` |
| TST_R | 0010nnnnmmmm1000 | 4437-4445 | `T = (R[rn] & R[rm]) == 0` | WB(rm,rn)+1 | `SetT(CmpEq(And(R(n),R(m)),C(0)))` |
| TST_I | 11001000iiiiiiii, I_U(0) | 4448-4456 | `T = (R[0] & imm) == 0` (zero-ext) | WB(0)+1 | `SetT(CmpEq(And(R(0),C(instr&0xFF)),C(0)))`; B(0) |
| CLRMAC | 0000000000101000 | 4113-4119 | `MAC.u64 = 0` | **1** (no WB) | `SetMACH(C(0)); SetMACL(C(0)); adv(); AddCycles(1); SetWb(kWbNone)` |

Notes: SHAL and SHLL are byte-identical handlers. ADDC/SUBC/NEGC/ADDV/SUBV T formulas above are exact transcriptions (not "equivalent" rewrites), except `tmp0<tmp1` → `CmpGtU(tmp1,tmp0)` and `0<tmp` → `CmpGtU(tmp,0)` which are identities.

## 5. @(R0,GBR) read-modify-write / test (Delay_ yes, **no bus-wait check**, m_wbReg = None)

### AND_M — `and.b #imm,@(R0,GBR)` — 11001101iiiiiiii — sh2.cpp:3742-3754
1. `DECODE_I_U(0)` (zero-ext 8-bit).
2. `address = GBR + R[0]`; `cycles = AccessCycles<uint8,read>(address) + AccessCycles<uint8,write>(address) + WB(0) + 1`; `tmp = MemReadByte(address); tmp &= imm; MemWriteByte(address, tmp)`; AdvancePC; None.
3. `Access8R + Access8W + WB(R0) + 1`. (Does **not** use AccessCyclesRMWByte.)
6. IR: `SyncCycles; a=Add(GetGBR(),R(0)); AddAccessCycles(a,1,false); AddAccessCycles(a,1,true); WbStall(B(0)); AddCycles(1); v=Load(a,1,false); Store(a,1,And(v,C(imm))); adv(); SetWb(kWbNone);`

### OR_M — `or.b #imm,@(R0,GBR)` — 11001111iiiiiiii — sh2.cpp:3818-3830
Same as AND_M but the **cycle terms use `uint16`**: `AccessCycles<uint16,read>(address) + AccessCycles<uint16,write>(address) + WB(0) + 1` (quirk/bug in handler; the actual accesses are byte).
IR: `SyncCycles; a=Add(GetGBR(),R(0)); AddAccessCycles(a,2,false); AddAccessCycles(a,2,true); WbStall(B(0)); AddCycles(1); v=Load(a,1,false); Store(a,1,Or(v,C(imm))); adv(); SetWb(kWbNone);`

### XOR_M — `xor.b #imm,@(R0,GBR)` — 11001110iiiiiiii — sh2.cpp:4085-4097
Same but cycle terms use **`uint32`**: `AccessCycles<uint32,read> + AccessCycles<uint32,write> + WB(0) + 1` (quirk).
IR: `... AddAccessCycles(a,4,false); AddAccessCycles(a,4,true); WbStall(B(0)); AddCycles(1); v=Load(a,1,false); Store(a,1,Xor(v,C(imm))); adv(); SetWb(kWbNone);`

### TST_M — `tst.b #imm,@(R0,GBR)` — 11001100iiiiiiii — sh2.cpp:4459-4469
`address=GBR+R[0]`; `cycles = Access8R(address) + WB(0) + 2`; `tmp=MemReadByte(address)`; `T = (tmp & imm) == 0`; AdvancePC; None. No write.
IR: `SyncCycles; a=Add(GetGBR(),R(0)); AddAccessCycles(a,1,false); WbStall(B(0)); AddCycles(2); SetT(CmpEq(And(Load(a,1,false),C(imm)),C(0))); adv(); SetWb(kWbNone);`
(Load returns zero-extended byte, `And` with zero-extended imm matches `uint8 & uint32`.)

## 6. System-register transfers (Delay_ yes; m_wbReg = None unless noted; no memory)

All of these **clear `m_intrFlags.allow`** (before AdvancePC).

| Opcode | Encoding | sh2.cpp | Steps | Cycles | m_wbReg | IR |
|---|---|---|---|---|---|---|
| LDC_GBR_R | 0100mmmm00011110, **M (rm=bits[11:8])** | 3322-3331 | `GBR=R[rm]; allow=false; AdvancePC` | WB(rm)+1 | None | `SetGBR(R(m8)); ClearIntrAllow; adv(); WbStall(B(m8)); AddCycles(1); SetWb(kWbNone)` |
| LDC_VBR_R | 0100mmmm00101110 | 3349-3358 | `VBR=R[rm]; allow=false; AdvancePC` | WB(rm)+1 | None | `SetVBR(R(m8)); ClearIntrAllow; …` |
| LDS_PR_R | 0100mmmm00101010 | 3385-3394 | `PR=R[rm]; allow=false; AdvancePC` | **WB(rm, PR)**+1 | None | `SetPR(R(m8)); ClearIntrAllow; adv(); WbStall(B(m8)\|PRB); AddCycles(1); SetWb(kWbNone)` |
| STC_GBR_R | 0000nnnn00010010, N | 3397-3407 | `R[rn]=GBR; allow=false; AdvancePC` | WB(rn)+1 | None | `SetReg(n,GetGBR()); ClearIntrAllow; adv(); WbStall(B(n)); AddCycles(1); SetWb(kWbNone)` |
| STC_VBR_R | 0000nnnn00100010 | 3423-3433 | `R[rn]=VBR; …` | WB(rn)+1 | None | `SetReg(n,GetVBR()); …` |
| STC_SR_R | 0000nnnn00000010 | 3410-3420 | `R[rn]=SR.u32` (full); … | WB(rn)+1 | None | `SetReg(n,GetSR()); …` |
| STS_MACH_R | 0000nnnn00001010 | 3436-3445 | `R[rn]=MAC.H; allow=false; AdvancePC; m_wbReg=rn` | **constant 1, no WB stall** | **rn** | `SetReg(n,GetMACH()); ClearIntrAllow; adv(); AddCycles(1); SetWb(n)` |
| STS_MACL_R | 0000nnnn00011010 | 3448-3457 | `R[rn]=MAC.L; …; m_wbReg=rn` | **1** | **rn** | `SetReg(n,GetMACL()); ClearIntrAllow; adv(); AddCycles(1); SetWb(n)` |
| STS_PR_R | 0000nnnn00101010 | 3460-3470 | `R[rn]=PR; allow=false; AdvancePC` | **WB(rn, PR)**+1 | None | `SetReg(n,GetPR()); ClearIntrAllow; adv(); WbStall(B(n)\|PRB); AddCycles(1); SetWb(kWbNone)` |

Not in the requested list but same family (for the intrAllow decision):
- LDC_SR_R 0100mmmm00001110 (3334-3346): `SR.u32 = R[rm] & 0x3F3; m_intrFlags = {pending = !delaySlot && INTC.pending.level > SR.ILevel, allow = false}; AdvancePC; cycles WB(rm)+1; None`. In a slot `pending=false` then `AdvancePC<true>` recomputes `pending = level > ILevel` ⇒ same final state. IR: `SetSR(R(m8), delaySlot); adv(); WbStall(B(m8)); AddCycles(1); SetWb(kWbNone)` — and because pending may become true, nothing else is needed: the next `CheckBoundary` reads live flags (but allow=false blocks it for one instruction).
- LDS_MACH_R / LDS_MACL_R (3361-3382): `MAC.H/L = R[rm]; allow=false; AdvancePC; WB(rm)+1; None`.
- All memory forms LDC.L/LDS.L/STC.L/STS.L (3473-3653) also clear allow; LDC.L SR also recomputes pending; LDS.L PR sets m_wbReg = PR. (Out of scope; they have **no** bus-wait checks.)

### intrAllow handling the JIT must reproduce
Interpreter: allow-clearing instruction X at pc ⇒ at the start of the next instruction Y, `pending && allow` is false ⇒ Y executes, `allow = true` is set before Y runs ⇒ at Y+1 interrupts are accepted normally.
JIT today: `intrAllow = true` only at block entry (executor.cpp:51); `CheckBoundary` tests `pending && allow`.
Required lowering: after X emit `ClearIntrAllow`; for the next instruction Y in the same block, emit `CheckBoundary(Y)` as usual (it won't fire for interrupts, may still fire for the cycle budget) and **then `SetIntrAllow`** before lowering Y. If X is the last instruction (block exit or X in a delay slot), leave allow=false: the executor's pre-entry check (executor.cpp:34) then behaves exactly as InterpretNext's, and executor.cpp:51 sets allow=true at the next block entry. A simple rule: emit `SetIntrAllow` after every `CheckBoundary` that immediately follows an allow-clearing instruction. A JIT-side alternative (fewer ops): end the block right after any allow-clearing instruction.

## 7. Delayed branches (no Delay_ variant — `IllegalSlot` in a slot; add to `IsDelayedBranch`)

`pc` = address of the branch. In all of them `SetupDelaySlot` clears `pending`, then `PC += 2` (the block's normal slot handling covers this).

### BSR — `bsr label` — 1011dddddddddddd — sh2.cpp:4559-4570
1. `DECODE_D12(1)`: `disp = sext12 << 1` (= `Disp12x2`).
2. `PR = PC + 4`; `target = PC + disp + 4`; SetupDelaySlot(target); `PC += 2`; `cycles = WB(PR) + 2`; None.
3. `WB(PR) + 2`. 4. None.
6. IR: `SetPR(C(pc+4)); SetupDelaySlot(C(pc+Disp12x2(instr)+4)); WbStall(PRB); AddCycles(2); SetWb(kWbNone);`
   Note: the old `m_wbReg` is still evaluated *after* PR is written (WB only compares indices), and `WbStall` must precede `SetWb`.

### BRAF — `braf Rm` — 0000mmmm00100011, M (rm=bits[11:8]) — sh2.cpp:4546-4556
`target = PC + R[rm] + 4`; SetupDelaySlot(target); `PC += 2`; `cycles = WB(rm) + 2`; None.
IR: `SetupDelaySlot(Add(R(m8),C(pc+4))); WbStall(B(m8)); AddCycles(2); SetWb(kWbNone);` (dynamic target)

### BSRF — `bsrf Rm` — 0000mmmm00000011 — sh2.cpp:4573-4584
`PR = PC + 4`; `target = PC + R[rm] + 4`; SetupDelaySlot; `PC += 2`; `cycles = WB(rm, PR) + 2`; None.
IR: `SetPR(C(pc+4)); SetupDelaySlot(Add(R(m8),C(pc+4))); WbStall(B(m8)|PRB); AddCycles(2); SetWb(kWbNone);`

### JSR — `jsr @Rm` — 0100mmmm00001011 — sh2.cpp:4600-4611
`PR = PC + 4`; `target = R[rm]`; SetupDelaySlot(target); `PC += 2`; `cycles = WB(rm, PR) + 2`; None.
IR: `SetPR(C(pc+4)); SetupDelaySlot(R(m8)); WbStall(B(m8)|PRB); AddCycles(2); SetWb(kWbNone);`

Delay-slot interaction: the slot instruction may read PR (e.g. `sts pr` / `rts`-like sequences) — PR is already updated at that point in the interpreter, so `SetPR` must be emitted in the branch part, before the slot. For BRAF/BSRF/JSR (and existing JMP/RTS) the target is dynamic ⇒ a slot MOVA/MOVW_I/MOVL_I needs `GetDelayTarget` (or the target ValueId).

## 8. Quirk list (for test design)

1. MOVW_L4 adds `WritebackCycles(rm)` twice (before the wait check and again on success) — sh2.cpp:2812, 2816.
2. MOVB_L4 and MOVB_LG have no writeback stall; MOVW_LG / MOVL_LG have none either (no GBR tracking). MOVB_L4's word sibling does have WB(rm).
3. OR_M charges `AccessCycles<uint16>` read+write and XOR_M charges `AccessCycles<uint32>` read+write, though both access a byte; AND_M uses `uint8`. None use `AccessCyclesRMWByte` (only TAS does). None of the four @(R0,GBR) ops checks bus wait.
4. TST_M: `Access8R + WB(0) + 2`; AND/OR/XOR_M: two access terms `+ WB(0) + 1`.
5. Store `@(R0,Rn)` and `R0,@(disp,Rn)` forms stall on `(rn, R0)` — not on rm; MOVL_S4 stalls on `(rm, rn)`. Load `@(R0,Rm)` stalls on `(rm, R0)`.
6. MOVB_S4/MOVW_S4 take Rn from bits[7:4]; MOVB_L4/MOVW_L4 take Rm from bits[7:4]; LDC/LDS/BRAF/BSRF/JSR take Rm from bits[11:8].
7. STS MACH/MACL: fixed 1 cycle, **no** WB stall, and set `m_wbReg = rn` (load-like). STS PR / LDS PR: `WB(reg, PR) + 1`, m_wbReg None.
8. CLRT/SETT/CLRMAC: fixed 1 cycle, no WB stall.
9. MOVW_I uses `pc + disp + 4` (no alignment); MOVL_I/MOVA use `(pc & ~3) + disp + 4`; in a slot `pc = m_delaySlotTarget - 2`. MOVW_I/MOVL_I loads are `instrFetch=true` reads; neither has a WB stall or bus-wait check.
10. MOVB_P/W_P/L_P: post-increment suppressed when `rn == rm` (compile-time decision). MOVB_M with rn==rm stores the pre-decrement value.
11. Bus-wait opcodes in this list (word/long only; never byte): MOVW_L, MOVW_L0, MOVL_L0, MOVW_L4, MOVL_L4, MOVW_LG, MOVL_LG, MOVW_M, MOVL_M, MOVW_P, MOVL_P, MOVW_S, MOVW_S0, MOVL_S0, MOVW_S4, MOVL_S4, MOVW_SG, MOVL_SG. On wait: return only the cycles summed before the check (Access, plus WB(rm) for MOVW_L4), nothing else changes.
12. `SH2JitContext` lacks MAC and an INTC/pending hook; CLRMAC/STS MACx/LDS MACx and LDC SR need an interface addition in the core fork (sh2_jit_iface.hpp + SH2::InitJitContext at sh2.cpp:4694).
13. Interrupt flags: LDC (GBR/VBR/SR), LDS (MACH/MACL/PR), STC (GBR/VBR/SR), STS (MACH/MACL/PR) — register and memory forms — clear `allow`; LDC SR / LDC.L SR additionally recompute `pending = !delaySlot && INTC.pending.level > SR.ILevel`. BSR/BRAF/BSRF/JSR clear `pending` via SetupDelaySlot. No other opcode in this list touches m_intrFlags.

## 9. Remaining instructions (milestone 2A)

Source: `sh2.cpp` as of 2026-10-01 (handler line ranges include the leading `// mnemonic` comment). Since §1 was written, `SH2JitContext` gained `MACL`, `MACH` (pointers into `RegMAC`: L = low word, H = high word of `MAC.u64`) and `setSR` (`JitSetSR`, sh2.cpp:4789) — `GetMACH/GetMACL/SetMACH/SetMACL/SetSR` are implementable today.

General facts for this section:
- **All 23 opcodes are registered with `setOpcode`** (sh2_decode.cpp:205, 225, 229, 246, 253-254, 263-264, 272, 279, 284-319) ⇒ all have a `Delay_` variant; none has delaySlot-dependent semantics except LDC.L SR's `pending` expression (net effect identical, as for LDC SR). Note the decoder name `OpcodeType::MUL` dispatches to handler `SH2::MULL` (sh2.cpp:2404, 2560).
- **No multiplier latency / busy state is modelled.** There is no "mult unit busy" counter anywhere in sh2.cpp/sh2.hpp; MUL/DMUL use a fixed `+3` (`// TODO: 2 to 4`), MULS/MULU `+1`, MAC `+1`; STS MACH/MACL Rn are a fixed 1 cycle with no stall and STS.L MACx have no MAC-related term. The JIT must *not* add any.
- **None of these opcodes checks bus wait** (no `IsBusWait` call in any handler below) ⇒ no `ExitIfBusWait`.
- Every memory access happens **before** `AdvancePC`. All handlers compute every cycle term with the pre-instruction `m_wbReg`; the ones that evaluate `WritebackCycles` after `AdvancePC` are still equivalent because `AdvancePC` never touches `m_wbReg` — only `WbStall` before `SetWb` matters.
- Misaligned addresses: `MemRead`/`MemWrite` silently align down (`address &= ~(size-1)`, sh2.cpp:689-700, no address error); `AccessCycles` uses the raw address; post-increments/pre-decrements use the raw (unaligned) register value. The JIT's `Load/Store` go through the same `MemRead/MemWrite`, so passing the raw address is exact.
- `m8` = bits[11:8] (`DECODE_M`), `n` = bits[11:8], `m` = bits[7:4].
- SR layout (sh2_regs.hpp:16-25): T bit0, S bit1, ILevel bits 4-7, Q bit8, M bit9.

### 9.1 Proposed new IR ops

| Op | Operands / dst | Semantics (exact) | x64 implementation |
|---|---|---|---|
| `Mul` | dst; a, b | `uint32(a * b)` (low 32 bits; identical for signed/unsigned) | `imul r32, r32` |
| `MulHiS` | dst; a, b | `uint32(uint64(sint64(sint32(a)) * sint64(sint32(b))) >> 32)` | `imul r/m32` (one-operand) → EDX, or `movsxd`+`imul r64`+`shr 32` |
| `MulHiU` | dst; a, b | `uint32((uint64(a) * uint64(b)) >> 32)` | `mul r/m32` → EDX |
| `SetSRBits` | a; imm = mask | `*ctx.SR = (*ctx.SR & ~mask) \| (a & mask)`. Verifier: `mask & ~0x303u == 0` (only T/S/Q/M — never ILevel, so no interrupt recompute and no `setSR` callback needed). Used with mask `0x301` (M\|Q\|T). | plain RMW on `ctx.SR` |
| `Div1` | dst; a = Rn, b = Rm; flag = (n == m) | `dst = Div1Step(a, b, flag, *ctx.SR)`; reads SR.T/Q/M, writes SR.Q and SR.T only (see §9.4 for the helper). | `call Div1Step` (pure helper + SR pointer) |
| `MacW` | a = op1, b = op2 (both already sign-extended 16→32); no dst | `mac = MacWStep(mac, SR.S, sint32(a), sint32(b))` where `mac = (uint64(*ctx.MACH) << 32) \| *ctx.MACL`; writes back both halves. Reads `*ctx.SR` bit1. | `call MacWStep` |
| `MacL` | a = op1, b = op2 (raw 32-bit longs); no dst | `mac = MacLStep(mac, SR.S, sint32(a), sint32(b))`; same MAC/SR access as `MacW`. | `call MacLStep` |
| `AddAccessCyclesRMWByte` | a = address | `cycles += ctx.accessCyclesRMWByte(sh2, a)` = `SH2::AccessCyclesRMWByte<false>(a)` | callback, like `AddAccessCycles` |

All new ops keep single assignment: each defines at most one `ValueId`; their side effects go to context state (SR / MAC / cycle counter), exactly like the existing `SetT`, `SetMACL`, `AddAccessCycles`. They are ordered side effects — the optimiser must not reorder `Div1`/`SetSRBits`/`MacW`/`MacL` across `GetT`/`SetT`/`GetSR`/`SetSR`/`Get/SetMACx` or other `Div1`/`Mac*`.

**Helpers** (put in a header shared by the IR interpreter, the x64 backend and the unit tests; the interpreter handlers are the reference to diff against):

```cpp
// DIV1 step. rmIsRn: the instruction has n == m, so Rm is read AFTER Rn was shifted.
// Returns the new Rn; updates only Q (bit 8) and T (bit 0) of sr.
uint32 Div1Step(uint32 rn, uint32 rm, bool rmIsRn, uint32 &sr);
// MAC.W accumulate. op1 = sext16(@Rm), op2 = sext16(@Rn). Returns the new MAC.u64.
uint64 MacWStep(uint64 mac, bool s, sint32 op1, sint32 op2);
// MAC.L accumulate. op1 = @Rm, op2 = @Rn. Returns the new MAC.u64.
uint64 MacLStep(uint64 mac, bool s, sint32 op1, sint32 op2);
```

**Interface addition** (core fork, `sh2_jit_iface.hpp` + `InitJitContext`): `uint64 (*accessCyclesRMWByte)(void *sh2, uint32 address)` → `AccessCyclesRMWByte<false>(address)`. It cannot be composed from `accessCycles`: for partition 000 `AccessCycles<…,emulateCache=false>` returns 1, but the RMW helper returns the *bus* byte-read cycles − 1, and the IR has no select op to branch on the partition. (A composition via `accessCycles((a & 0x1FFFFFFF) | 0x20000000, 1, false) - 1` would need a runtime partition test; not worth it.)

Rejected alternative: a single `DMul(a,b,signed)` op writing MAC directly — works, but `Mul`/`MulHiS`/`MulHiU` are reusable, verifier-trivial and map to one x64 instruction each.

### 9.2 Multiplies (register-only; Delay_ yes; no memory; m_wbReg = None; AdvancePC before the WB evaluation; no intr-flag effect)

| Opcode | Encoding / decode | sh2.cpp | Semantics (handler order) | Cycles | IR |
|---|---|---|---|---|---|
| MUL (`mul.l Rm,Rn`) | 0000nnnnmmmm0111, NM | 4192-4201 (`MULL`) | `MAC.L = R[rm] * R[rn]` (uint32 wrap; MAC.H unchanged) | `WB(rm,rn) + 3` | `SetMACL(Mul(R(m),R(n))); adv(); WbStall(B(m)\|B(n)); AddCycles(3); SetWb(kWbNone)` |
| MULS (`muls.w`) | 0010nnnnmmmm1111 | 4203-4212 | `MAC.L = bit::sign_extend<16>(R[rm]) * bit::sign_extend<16>(R[rn]);` (sint32 × sint32 of 16-bit values, max \|2^30\| — no overflow; MAC.H unchanged) | `WB(rm,rn) + 1` | `SetMACL(Mul(SExt16(R(m)),SExt16(R(n))))`; ALU template, mask B(m)\|B(n) |
| MULU (`mulu.w`) | 0010nnnnmmmm1110 | 4214-4224 | `auto cast = [](uint32 val) { return static_cast<uint32>(static_cast<uint16>(val)); }; MAC.L = cast(R[rm]) * cast(R[rn]);` (uint32 product ≤ 0xFFFE0001; MAC.H unchanged) | `WB(rm,rn) + 1` | `SetMACL(Mul(And(R(m),C(0xFFFF)),And(R(n),C(0xFFFF))))`; ALU template |
| DMULS (`dmuls.l`) | 0011nnnnmmmm1101 | 4226-4236 | `auto cast = [](uint32 val) { return static_cast<sint64>(static_cast<sint32>(val)); }; MAC.u64 = cast(R[rm]) * cast(R[rn]);` | `WB(rm,rn) + 3` | `x=R(m); y=R(n); SetMACL(Mul(x,y)); SetMACH(MulHiS(x,y)); adv(); WbStall(B(m)\|B(n)); AddCycles(3); SetWb(kWbNone)` |
| DMULU (`dmulu.l`) | 0011nnnnmmmm0101 | 4238-4247 | `MAC.u64 = static_cast<uint64>(R[rm]) * static_cast<uint64>(R[rn]);` | `WB(rm,rn) + 3` | as DMULS with `MulHiU` |

(The `+3` constants carry the interpreter's `// TODO: 2 to 4, but how is that decided?` — reproduce them as-is.)

### 9.3 DIV0S / DIV0U (Delay_ yes; no memory; m_wbReg = None; no intr-flag effect)

**DIV0S** — `div0s Rm,Rn` — 0010nnnnmmmm0111, NM — sh2.cpp:4249-4260
```cpp
SR.M = static_cast<sint32>(R[rm]) < 0;
SR.Q = static_cast<sint32>(R[rn]) < 0;
SR.T = SR.M != SR.Q;
AdvancePC<...>();
const uint64 cycles = WritebackCycles(rm, rn) + 1;
m_wbReg = kWBRegNone;
```
Cycles `WB(rm,rn) + 1`. IR: `mm=Shr(R(m),31); qq=Shr(R(n),31); SetSRBits(Or(Or(Shl(mm,9),Shl(qq,8)),Xor(mm,qq)), 0x301)`; ALU template mask B(m)|B(n).
(Composable without `SetSRBits` only via `GetSR` + `SetSR`, but `SetSR` goes through `JitSetSR`, which clears `allow` and recomputes `pending` — wrong for DIV0S. Hence `SetSRBits`.)

**DIV0U** — `div0u` — 0000000000011001 — sh2.cpp:4262-4271
`SR.M = 0; SR.Q = 0; SR.T = 0; AdvancePC; m_wbReg = None; return 1;` — **fixed 1 cycle, no WB stall** (like CLRT/CLRMAC).
IR: `SetSRBits(C(0), 0x301); adv(); AddCycles(1); SetWb(kWbNone)`.

### 9.4 DIV1 — `div1 Rm,Rn` — 0011nnnnmmmm0100, NM — sh2.cpp:4273-4313
1. Delay_: yes, no slot-specific behaviour.
2. Handler body (verbatim, tracer call omitted):
```cpp
const bool oldQ = SR.Q;
const bool M = SR.M;
bool Q = oldQ;
Q = static_cast<sint32>(R[rn]) < 0;
R[rn] = (R[rn] << 1u) | SR.T;

const uint32 prevVal = R[rn];
if (oldQ == M) {
    R[rn] -= R[rm];
} else {
    R[rn] += R[rm];
}

if (oldQ) {
    if (M) {
        Q ^= R[rn] <= prevVal;
    } else {
        Q ^= R[rn] < prevVal;
    }
} else {
    if (M) {
        Q ^= R[rn] >= prevVal;
    } else {
        Q ^= R[rn] > prevVal;
    }
}

SR.T = Q == M;
SR.Q = Q;
AdvancePC<...>();
const uint64 cycles = WritebackCycles(rm, rn) + 1;
m_wbReg = kWBRegNone;
```
   **`R[rm]` is read after `R[rn]` was shifted** ⇒ with n == m the subtrahend/addend is the shifted value (`R[rn]` becomes `0` when oldQ == M, `2*prevVal` otherwise). All compares are unsigned.
3. Cycles: `WB(rm,rn) + 1`. m_wbReg None. No memory, no bus wait, no intr effect.
4. Helper (exact transcription):
```cpp
uint32 Div1Step(uint32 rn, uint32 rm, bool rmIsRn, uint32 &sr) {
    const bool oldQ = (sr >> 8) & 1, M = (sr >> 9) & 1, T = sr & 1;
    bool Q = static_cast<sint32>(rn) < 0;
    rn = (rn << 1) | T;
    const uint32 prevVal = rn;
    const uint32 src = rmIsRn ? rn : rm;
    rn = (oldQ == M) ? rn - src : rn + src;
    if (oldQ) Q ^= M ? (rn <= prevVal) : (rn < prevVal);
    else      Q ^= M ? (rn >= prevVal) : (rn > prevVal);
    sr = (sr & ~0x101u) | (uint32(Q) << 8) | uint32(Q == M);
    return rn;
}
```
5. IR: `SetReg(n, Div1(R(n), R(m), /*flag*/ n==m))`; ALU template mask B(m)|B(n). (When n == m the frontend may pass the same `ValueId` twice; the helper ignores `b` when the flag is set.)

### 9.5 MAC.W / MAC.L (Delay_ yes; no bus-wait check; m_wbReg = None; no intr effect)

**MACW** — `mac.w @Rm+,@Rn+` — 0100nnnnmmmm1111, NM — sh2.cpp:4121-4156 (tracer calls omitted):
```cpp
const uint32 address2 = R[rn];
uint64 cycles = AccessCycles<uint16, false, emulateCache>(address2);
const sint32 op2 = static_cast<sint16>(MemReadWord<emulateCache>(address2));
R[rn] += 2;

const uint32 address1 = R[rm];
cycles += AccessCycles<uint16, false, emulateCache>(address1);
const sint32 op1 = static_cast<sint16>(MemReadWord<emulateCache>(address1));
R[rm] += 2;

const sint32 mul = op1 * op2;
if (SR.S) {
    const sint64 result = static_cast<sint64>(static_cast<sint32>(MAC.L)) + mul;
    const sint32 saturatedResult = std::clamp<sint64>(result, -0x80000000LL, 0x7FFFFFFFLL);
    if (result == saturatedResult) {
        MAC.L = result;
    } else {
        MAC.L = saturatedResult;
        MAC.H |= 1;
    }
} else {
    MAC.u64 += mul;
}

AdvancePC<...>();
cycles += WritebackCycles(rm, rn);
m_wbReg = kWBRegNone;
return cycles + 1; // TODO: where does the + 1 come from?
```
- Read order: **@Rn first** (op2), Rn += 2, **then** Rm is read (op1). With **n == m**: op2 = @R, op1 = @(R+2), R ends at R+4.
- S=1: only MAC.L is accumulated as 32-bit signed; MAC.H is **not** sign-updated — it is left as is, and only bit 0 is OR-ed in on overflow (both directions). `mul` ∈ [−2^30+2^15, 2^30] never overflows.
- S=0: full 64-bit add of the sign-extended product (`sint32` → `uint64` conversion sign-extends).
- Cycles: `Access16R(R[rn]) + Access16R(R[rn]+2 if n==m else R[rm]) + WB(rm,rn) + 1`.
- `MacWStep(mac, s, op1, op2)`: `mul = op1*op2; if (!s) return mac + uint64(sint64(mul)); r = sint64(sint32(uint32(mac))) + mul; if (r in [INT32_MIN, INT32_MAX]) return (mac & ~0xFFFFFFFF) | uint32(r); return ((mac | (1ull << 32)) & ~0xFFFFFFFFull) | uint32(clamp(r))`.
- IR:
  ```
  SyncCycles
  a2 = R(n); AddAccessCycles(a2,2,false); v2 = SExt16(Load(a2,2,false)); SetReg(n, Add(a2,C(2)))
  a1 = (n==m) ? Add(a2,C(2)) : R(m)
  AddAccessCycles(a1,2,false); v1 = SExt16(Load(a1,2,false)); SetReg(m, Add(a1,C(2)))
  MacW(v1, v2)
  adv(); WbStall(B(m)|B(n)); AddCycles(1); SetWb(kWbNone)
  ```
  One `SyncCycles` before the first access covers both reads (the interpreter commits cycles only on return). The n==m address is chosen at compile time so correctness does not depend on whether `GetReg` after `SetReg` is forwarded.

**MACL** — `mac.l @Rm+,@Rn+` — 0000nnnnmmmm1111, NM — sh2.cpp:4158-4190:
```cpp
const uint32 address2 = R[rn];
uint64 cycles = AccessCycles<uint32, false, emulateCache>(address2);
const sint64 op2 = static_cast<sint64>(static_cast<sint32>(MemReadLong<emulateCache>(address2)));
R[rn] += 4;

const uint32 address1 = R[rm];
cycles += AccessCycles<uint32, false, emulateCache>(address1);
const sint64 op1 = static_cast<sint64>(static_cast<sint32>(MemReadLong<emulateCache>(address1)));
R[rm] += 4;

const sint64 mul = op1 * op2;
sint64 result = mul + MAC.u64;
if (SR.S && result > 0x00007FFFFFFFFFFFull && result < 0xFFFF800000000000ull) {
    if (static_cast<sint32>(op1 ^ op2) < 0) {
        result = 0xFFFF800000000000ull;
    } else {
        result = 0x00007FFFFFFFFFFFull;
    }
}
MAC.u64 = result;

AdvancePC<...>();
cycles += WritebackCycles(rm, rn);
m_wbReg = kWBRegNone;
return cycles + 1; // TODO: where does the + 1 come from?
```
- Same read/increment order as MAC.W with 4 (n == m: op2 = @R, op1 = @(R+4), R ends at R+8).
- Saturation test is done on `uint64(result)` (comparison with `ull` constants converts the signed value): saturate iff the 64-bit sum is outside the signed 48-bit range. **Direction comes from the sign of the product (`op1 ^ op2`, bit 31), not from the sum** — e.g. a zero product with a negative operand saturates to the negative bound if MAC was already out of range. The saturated values are full 64-bit (MACH = `0xFFFF8000` / `0x00007FFF`). No saturation flag bit is set. With S=0 the sum is a plain 64-bit wrap.
- Cycles: `Access32R(R[rn]) + Access32R(R[rn]+4 if n==m else R[rm]) + WB(rm,rn) + 1`.
- `MacLStep(mac, s, op1, op2)`: `mul = sint64(op1)*sint64(op2); r = uint64(mul) + mac; if (s && r > 0x00007FFFFFFFFFFF && r < 0xFFFF800000000000) r = (sint32(op1 ^ op2) < 0) ? 0xFFFF800000000000 : 0x00007FFFFFFFFFFF; return r;`
- IR: as MAC.W with size 4, increment 4, no `SExt16`, and `MacL(v1, v2)`.

### 9.6 TAS — `tas.b @Rn` — 0100nnnn00011011, N — sh2.cpp:4419-4434
1. Delay_: yes.
2. Body:
```cpp
const uint32 address = R[rn];
const uint64 cycles = AccessCyclesRMWByte<emulateCache>(address) + WritebackCycles(rn) + 4;
// TODO: enable bus lock on this read
const uint8 tmp = MemReadByte<false>(address);
SR.T = tmp == 0;
// TODO: disable bus lock on this write
MemWriteByte<debug, emulateCache>(address, tmp | 0x80);
AdvancePC<...>();
m_wbReg = kWBRegNone;
return cycles;
```
3. **No bus lock** (TODO only), no peek, no bus-wait check. Read is `MemReadByte<emulateCache=false>` — always cache-bypassing; the JIT's `read` callback already uses emulateCache=false, so `Load(a,1,false)` is exact. Write is a normal byte write. T = (old byte == 0). Rn unchanged.
4. Cycles: `AccessCyclesRMWByte(R[rn]) + WB(rn) + 4`, where (sh2.cpp:1042-1056) `AccessCyclesRMWByte(a)` = partitions 000/001/101: `m_bus.GetAccessCycles<uint8,false>(a) - 1` (**bus cycles even for the cached partition**, minus 1); others: `Access8R(a) + Access8W(a)` (010/011/100/110 → 2, 111 → 8).
5. m_wbReg None. No intr effect.
6. IR: `SyncCycles; a=R(n); AddAccessCyclesRMWByte(a); WbStall(B(n)); AddCycles(4); v=Load(a,1,false); SetT(CmpEq(v,C(0))); Store(a,1,Or(v,C(0x80))); adv(); SetWb(kWbNone);`
   (`SetT` before `Store` mirrors the handler; it is unobservable either way.)

### 9.7 LDC.L / LDS.L `@Rm+` (Delay_ yes; no bus-wait check; all clear `allow`; m_wbReg None except LDS.L PR)

Common shape: `address = R[rm]` (`DECODE_M`, rm = bits[11:8]); `cycles = Access32R(address) + WB(rm) [+ 2]`; `<sysreg> = MemReadLong(address)`; `R[rm] += 4`; `m_intrFlags.allow = false`; AdvancePC; `m_wbReg = …`.
Common IR prefix: `SyncCycles; a=R(m8); AddAccessCycles(a,4,false); WbStall(B(m8));` suffix: `SetReg(m8, Add(a,C(4))); ClearIntrAllow; adv(); SetWb(<wb>)`. Add each to `ClearsIntrAllow`.

| Opcode | Encoding | sh2.cpp | Load target / special | Cycles | m_wbReg | IR middle |
|---|---|---|---|---|---|---|
| LDC_GBR_M (`ldc.l @Rm+,GBR`) | 0100mmmm00010111 | 3472-3485 | `GBR = load32` | `Access32R + WB(rm) + 2` | None | `AddCycles(2); SetGBR(Load(a,4,false))` |
| LDC_SR_M (`ldc.l @Rm+,SR`) | 0100mmmm00000111 | 3487-3503 | `SR.u32 = load32 & 0x3F3; m_intrFlags = {pending = !delaySlot && INTC.pending.level > SR.ILevel, allow = false}` — then `R[rm] += 4`, AdvancePC | `Access32R + WB(rm) + 2` | None | `AddCycles(2); SetSR(Load(a,4,false), delaySlot)` (SetSR already clears allow; the extra `ClearIntrAllow` is harmless) |
| LDC_VBR_M | 0100mmmm00100111 | 3505-3518 | `VBR = load32` | `Access32R + WB(rm) + 2` | None | `AddCycles(2); SetVBR(Load(a,4,false))` |
| LDS_MACH_M | 0100mmmm00000110 | 3520-3533 | `MAC.H = load32` | `Access32R + WB(rm)` (**no constant**) | None | `SetMACH(Load(a,4,false))` |
| LDS_MACL_M | 0100mmmm00010110 | 3535-3548 | `MAC.L = load32` | `Access32R + WB(rm)` | None | `SetMACL(Load(a,4,false))` |
| LDS_PR_M | 0100mmmm00100110 | 3550-3563 | `PR = load32` | `Access32R + WB(rm)` (no PR term) | **PR (0x10)** | `SetPR(Load(a,4,false))`; suffix `SetWb(kWBRegPR = 0x10)` |

LDC.L SR interrupt detail: in a slot `pending` is set false by the handler and then recomputed by `AdvancePC<true>` (= `EndDelaySlot`) as `INTC.pending.level > SR.ILevel` with the new SR ⇒ same final state as non-slot. `SetSR` must precede `adv()`. `pending` may become true, but `allow = false` blocks acceptance before the next instruction (§6 rule).

### 9.8 STC.L / STS.L `@-Rn` (Delay_ yes; no bus-wait check; all clear `allow`; m_wbReg None)

Common shape (`DECODE_N`): **`R[rn] -= 4` first**; `address = R[rn]`; `cycles = Access32W(address) + WB(rn[, PR]) [+ 2]`; `MemWriteLong(address, <sysreg>)`; `allow = false`; AdvancePC; `m_wbReg = None`.
Common IR: `SyncCycles; a=Sub(R(n),C(4)); SetReg(n,a); AddAccessCycles(a,4,true); WbStall(<mask>); [AddCycles(2);] Store(a,4,<value>); ClearIntrAllow; adv(); SetWb(kWbNone)`. Add each to `ClearsIntrAllow`.

| Opcode | Encoding | sh2.cpp | Stored value | Cycles | WbStall mask | value |
|---|---|---|---|---|---|---|
| STC_SR_M (`stc.l SR,@-Rn`) | 0100nnnn00000011 | 3580-3593 | `SR.u32` (full register incl. Q/M/S/I/T, upper bits as stored — always 0 given the 0x3F3 masks) | `Access32W + WB(rn) + 2` | B(n) | `GetSR()` |
| STC_GBR_M | 0100nnnn00010011 | 3565-3578 | `GBR` | `Access32W + WB(rn) + 2` | B(n) | `GetGBR()` |
| STC_VBR_M | 0100nnnn00100011 | 3595-3608 | `VBR` | `Access32W + WB(rn) + 2` | B(n) | `GetVBR()` |
| STS_MACH_M | 0100nnnn00000010 | 3610-3623 | `MAC.H` | `Access32W + WB(rn)` (**no constant**) | B(n) | `GetMACH()` |
| STS_MACL_M | 0100nnnn00010010 | 3625-3638 | `MAC.L` | `Access32W + WB(rn)` | B(n) | `GetMACL()` |
| STS_PR_M | 0100nnnn00100010 | 3640-3653 | `PR` | `Access32W + WB(rn, PR)` (no constant) | B(n)\|PRB | `GetPR()` |

Since there is no bus-wait exit, emitting `SetReg(n,a)` before the store is exact (unlike MOVL_M, where Rn is only decremented on success).

### 9.x Quirks and test notes

1. **MAC.W/MAC.L with n == m**: op2 = @R, op1 = @(R+2/4), R += 4/8. Also n ≠ m with overlapping addresses; both registers post-incremented from their *own* pre-values; misaligned addresses (read aligned down, increment from raw value, `AccessCycles` on the raw address).
2. **MAC.W S=1**: boundary sums exactly `0x7FFFFFFF`/`-0x80000000` (no saturation, MACH unchanged), one past (saturate, MACH |= 1), MACH with bit0 already set, negative MACH left untouched (not sign-extended), op values `0x8000×0x8000`. S=0: 64-bit wrap and carry into MACH, negative product borrow from MACH.
3. **MAC.L S=1**: sums at ±2^47 boundaries (`0x00007FFFFFFFFFFF` no-sat, `+1` sat; `0xFFFF800000000000` no-sat, `-1` sat); saturation direction from product sign even when the sum lies the other way (MAC preloaded out of 48-bit range via LDS, product 0 with one negative operand); max product `0x80000000×0x80000000`; S=0 full 64-bit wrap.
4. **DIV1**: full `div0u` + 32×(`rotcl`/`div1`) unsigned and `div0s` + 32×`div1` signed sequences vs interpreter (random dividends/divisors incl. 0, 1, `0x80000000`, `0xFFFFFFFF`); all 8 (oldQ, M, T) combinations at single-step level; **n == m** (Rm read after shift); DIV0S for each sign combination; DIV0U fixed 1 cycle (no WB stall even after a load into any register).
5. **Multiply**: MULS with `0x8000`, `0xFFFF` and dirty upper halves; MULU `0xFFFF×0xFFFF`; DMULS mixed signs / `0x80000000²`; DMULU `0xFFFFFFFF²`; MUL/MULS/MULU leave MAC.H unchanged; cycles `+3` (MUL/DMULx) vs `+1` (MULS/MULU) vs `+1` + accesses (MAC); no extra cycles for STS MACx right after a multiply.
6. **TAS**: byte 0 → T=1, byte 0x80 → T=0 and stays 0x80, byte 0x7F → 0xFF; cycles per partition (000 uses bus cycles − 1, *not* 1; 001/101 bus − 1; 111 → 8; 010/011/100/110 → 2) + WB(rn) + 4; Rn unchanged; in a delay slot.
7. **LDC.L SR**: in a delay slot (pending recomputed by EndDelaySlot), with a pending interrupt at a level that the new mask unblocks (must be taken *one instruction later*, after the allow-cleared instruction), masks `& 0x3F3` (load `0xFFFFFFFF`), rm == R15 stack pop; LDC.L GBR/VBR/SR `+2`, LDS.L MACH/MACL/PR no constant.
8. **LDS.L PR** sets `m_wbReg = PR` ⇒ following RTS/STS PR/BSR/JSR/STS.L PR pay the WB(PR) stall; LDS.L MACx/LDC.L set None.
9. **STC.L SR** stores the full SR (set S, Q, M, ILevel, T = all ones pattern `0x3F3`); STC.L / STS.L with rn == R15 (pre-decrement before the store; WB(rn) uses the pre-instruction m_wbReg, e.g. after `mov.l @r15+,r15`); STS.L PR stall mask (rn, PR) after LDS.L PR.
10. **Interrupt allow**: every LDC.L/LDS.L/STC.L/STS.L clears allow (an interrupt pending before it must not be taken before the next instruction, but must be taken before the one after); none of MUL/MAC/DIV/TAS touches intr flags.
11. **No bus-wait** in any §9 opcode, including the 32-bit LDC.L/STC.L/MAC.L accesses — a pending bus wait must *not* produce an exit (differential test with a wait-asserting region).
12. All §9 opcodes in a delay slot (every one has a `Delay_` variant), particularly MAC.x/TAS/LDC.L SR whose accesses must precede `EndDelaySlot`'s refill.

**State not reachable via `SH2JitContext`**: only `AccessCyclesRMWByte` (TAS) — add `accessCyclesRMWByte`. SR.S/Q/M are reachable through `ctx.SR`, MAC through `ctx.MACL/MACH`, INTC level through `setSR`. The TAS bus lock is not modelled by the interpreter, so nothing is needed for it.
