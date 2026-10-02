#include <brimir/jit/interp_backend.hpp>

#include <brimir/jit/bus_fast_path.hpp>
#include <brimir/jit/sh2_helpers.hpp>

#include <vector>

namespace brimir::jit {

ExitInfo RunBlock(const Block &block, ymir::sh2::SH2JitContext &ctx, uint64_t target, const bool *abortRequested) {
    thread_local std::vector<uint32_t> values;
    if (values.size() < block.numValues) {
        values.resize(block.numValues);
    }
    uint32_t *v = values.data();

    const uint64_t entryCycles = *ctx.cyclesExecuted;
    ExitInfo info;
    // Memory callbacks can reset the CPU (watchdog reset), which requests a cache flush. Stop at
    // once, without touching PC, which the reset already set.
    const auto abortNow = [&] { return abortRequested != nullptr && *abortRequested; };

    // Known refills (header comment): usable while the code is on array pages and not dirty.
    CodeHostRanges codeRanges;
    const bool knownUsable =
        block.fetchFromArrays &&
        FindCodeHostRanges(ctx.bus, block.startPC, static_cast<uint32_t>(block.guestOpcodes.size()), codeRanges);
    bool codeDirty = false;
    // Classifies a data access (before it runs, while the page table is as the access sees it).
    const auto markRead = [&](uint32_t address, uint32_t size) {
        bool writable = false;
        if (knownUsable && FastArrayPointer(ctx.bus, address, size, writable) == nullptr && (address >> 29) != 0b111) {
            codeDirty = true; // handler read outside the on-chip registers
        }
    };
    const auto markWrite = [&](uint32_t address, uint32_t size) {
        if (!knownUsable) {
            return;
        }
        bool writable = false;
        const uint8_t *p = FastArrayPointer(ctx.bus, address, size, writable);
        if (p == nullptr || (writable && codeRanges.Overlaps(p, size))) {
            codeDirty = true; // handler write, or an array store into the block's code
        }
    };

    for (const Inst &in : block.code) {
        switch (in.op) {
        case Op::Const: v[in.dst] = in.imm; break;
        case Op::GetReg: v[in.dst] = ctx.R[in.imm]; break;
        case Op::SetReg: ctx.R[in.imm] = v[in.a]; break;
        case Op::GetPR: v[in.dst] = *ctx.PR; break;
        case Op::GetT: v[in.dst] = *ctx.SR & 1u; break;
        case Op::SetT: *ctx.SR = (*ctx.SR & ~1u) | (v[in.a] != 0 ? 1u : 0u); break;
        case Op::Add: v[in.dst] = v[in.a] + v[in.b]; break;
        case Op::Sub: v[in.dst] = v[in.a] - v[in.b]; break;
        case Op::CmpEq: v[in.dst] = v[in.a] == v[in.b] ? 1u : 0u; break;
        case Op::SExt8:
            v[in.dst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v[in.a] & 0xFFu)));
            break;
        case Op::SExt16:
            v[in.dst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v[in.a] & 0xFFFFu)));
            break;
        case Op::And: v[in.dst] = v[in.a] & v[in.b]; break;
        case Op::Or: v[in.dst] = v[in.a] | v[in.b]; break;
        case Op::Xor: v[in.dst] = v[in.a] ^ v[in.b]; break;
        case Op::Not: v[in.dst] = ~v[in.a]; break;
        case Op::Shl: v[in.dst] = v[in.a] << in.imm; break;
        case Op::Shr: v[in.dst] = v[in.a] >> in.imm; break;
        case Op::Sar: v[in.dst] = static_cast<uint32_t>(static_cast<int32_t>(v[in.a]) >> in.imm); break;
        case Op::CmpGtU: v[in.dst] = v[in.a] > v[in.b] ? 1u : 0u; break;
        case Op::CmpGeU: v[in.dst] = v[in.a] >= v[in.b] ? 1u : 0u; break;
        case Op::CmpGtS: v[in.dst] = static_cast<int32_t>(v[in.a]) > static_cast<int32_t>(v[in.b]) ? 1u : 0u; break;
        case Op::CmpGeS: v[in.dst] = static_cast<int32_t>(v[in.a]) >= static_cast<int32_t>(v[in.b]) ? 1u : 0u; break;
        case Op::GetGBR: v[in.dst] = *ctx.GBR; break;
        case Op::SetGBR: *ctx.GBR = v[in.a]; break;
        case Op::GetVBR: v[in.dst] = *ctx.VBR; break;
        case Op::SetVBR: *ctx.VBR = v[in.a]; break;
        case Op::SetPR: *ctx.PR = v[in.a]; break;
        case Op::GetSR: v[in.dst] = *ctx.SR; break;
        case Op::SetSR: ctx.setSR(ctx.sh2, v[in.a], in.flag); break;
        case Op::GetMACH: v[in.dst] = *ctx.MACH; break;
        case Op::GetMACL: v[in.dst] = *ctx.MACL; break;
        case Op::SetMACH: *ctx.MACH = v[in.a]; break;
        case Op::SetMACL: *ctx.MACL = v[in.a]; break;
        case Op::ClearIntrAllow: *ctx.intrAllow = false; break;
        case Op::SetIntrAllow: *ctx.intrAllow = true; break;
        case Op::GetDelayTarget: v[in.dst] = *ctx.delaySlotTarget; break;
        case Op::Mul: v[in.dst] = v[in.a] * v[in.b]; break;
        case Op::MulHiS:
            v[in.dst] = static_cast<uint32_t>(
                static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v[in.a])) *
                                      static_cast<int64_t>(static_cast<int32_t>(v[in.b]))) >>
                32);
            break;
        case Op::MulHiU:
            v[in.dst] = static_cast<uint32_t>((static_cast<uint64_t>(v[in.a]) * static_cast<uint64_t>(v[in.b])) >> 32);
            break;
        case Op::SetSRBits: *ctx.SR = (*ctx.SR & ~in.imm) | (v[in.a] & in.imm); break;
        case Op::Div1: v[in.dst] = Div1Step(v[in.a], v[in.b], in.flag, *ctx.SR); break;
        case Op::MacW:
        case Op::MacL: {
            const uint64_t mac = (static_cast<uint64_t>(*ctx.MACH) << 32) | *ctx.MACL;
            const bool s = ((*ctx.SR >> 1) & 1u) != 0;
            const int32_t op1 = static_cast<int32_t>(v[in.a]);
            const int32_t op2 = static_cast<int32_t>(v[in.b]);
            const uint64_t result = in.op == Op::MacW ? MacWStep(mac, s, op1, op2) : MacLStep(mac, s, op1, op2);
            *ctx.MACH = static_cast<uint32_t>(result >> 32);
            *ctx.MACL = static_cast<uint32_t>(result);
            break;
        }
        case Op::AddAccessCyclesRMWByte: info.cycles += ctx.accessCyclesRMWByte(ctx.sh2, v[in.a]); break;
        case Op::Load:
            markRead(v[in.a], in.size);
            v[in.dst] = ctx.read(ctx.sh2, v[in.a], in.size, in.flag);
            if (abortNow()) {
                info.aborted = true;
                return info;
            }
            break;
        case Op::Store:
            markWrite(v[in.a], in.size);
            ctx.write(ctx.sh2, v[in.a], in.size, v[in.b]);
            if (abortNow()) {
                info.aborted = true;
                return info;
            }
            break;
        case Op::AddCycles: info.cycles += in.imm; break;
        case Op::AddAccessCycles: info.cycles += ctx.accessCycles(ctx.sh2, v[in.a], in.size, in.flag); break;
        case Op::WbStall: {
            const uint8_t wb = *ctx.wbReg;
            if (wb <= 16 && ((in.imm >> wb) & 1u) != 0) {
                info.cycles += 1;
            }
            break;
        }
        case Op::SetWb: *ctx.wbReg = static_cast<uint8_t>(in.imm); break;
        case Op::SyncCycles: *ctx.cyclesExecuted = entryCycles + info.cycles; break;
        case Op::CheckBoundary:
            // The interpreter's per-instruction checks: Advance's budget (m_cyclesExecuted < target)
            // and InterpretNext's interrupt test (pending && allowed).
            if (entryCycles + info.cycles >= target || (*ctx.intrPending && *ctx.intrAllow)) {
                *ctx.PC = in.imm;
                info.retired = in.retired;
                info.boundary = true;
                return info;
            }
            break;
        case Op::Refill:
            if (in.flag && knownUsable && !codeDirty) {
                // The fetch would read these two words of guestOpcodes, unchanged since the entry
                // check; an array-page fetch has no side effects.
                *ctx.fetchedOpcodes = in.imm2;
                break;
            }
            ctx.refillPipeline(ctx.sh2, in.imm);
            if (abortNow()) {
                info.aborted = true;
                return info;
            }
            break;
        case Op::SetupDelaySlot: ctx.setupDelaySlot(ctx.sh2, v[in.a]); break;
        case Op::EndDelaySlot: ctx.endDelaySlot(ctx.sh2); break;
        case Op::ExitIfBusWait:
            if (ctx.busWait(ctx.sh2, v[in.a], in.size, in.flag)) {
                *ctx.PC = in.imm;
                info.retired = in.retired;
                info.busWait = true;
                return info;
            }
            break;
        case Op::ExitIf:
            if (v[in.a] != 0) {
                info.cycles += in.imm2;
                if (in.flag) {
                    ctx.refillPipeline(ctx.sh2, in.imm);
                    if (abortNow()) {
                        info.aborted = true;
                        return info;
                    }
                }
                *ctx.PC = in.imm;
                info.retired = in.retired;
                return info;
            }
            break;
        case Op::Exit:
            *ctx.PC = in.imm;
            info.retired = in.retired;
            return info;
        case Op::ExitDynamic: info.retired = in.retired; return info;
        }
    }
    return info; // unreachable for verified blocks (they always end with an exit)
}

} // namespace brimir::jit
