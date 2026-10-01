#include <brimir/jit/interp_backend.hpp>

#include <vector>

namespace brimir::jit {

ExitInfo RunBlock(const Block &block, ymir::sh2::SH2JitContext &ctx, const bool *abortRequested) {
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
        case Op::Load:
            v[in.dst] = ctx.read(ctx.sh2, v[in.a], in.size, in.flag);
            if (abortNow()) {
                info.aborted = true;
                return info;
            }
            break;
        case Op::Store:
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
        case Op::Refill:
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
