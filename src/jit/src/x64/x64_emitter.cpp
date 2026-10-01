#include "x64_emitter.hpp"

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <vector>

namespace brimir::jit {

namespace {

using namespace asmjit;

// Offsets of the guest state fields from ctx.R. Generated code loads R once and addresses every
// field as [R + offset].
struct StateOffsets {
    int32_t PC, PR, GBR, VBR, SR, MACL, MACH, delaySlotTarget, wbReg, intrPending, intrAllow, cyclesExecuted;
};

bool OffsetFromR(const ymir::sh2::SH2JitContext &ctx, const void *field, int32_t &out) {
    if (field == nullptr) {
        return false;
    }
    const auto base = static_cast<int64_t>(reinterpret_cast<uintptr_t>(ctx.R));
    const auto addr = static_cast<int64_t>(reinterpret_cast<uintptr_t>(field));
    const int64_t off = addr - base;
    if (off < std::numeric_limits<int32_t>::min() || off > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    out = static_cast<int32_t>(off);
    return true;
}

bool ComputeOffsets(const ymir::sh2::SH2JitContext &ctx, StateOffsets &o) {
    return ctx.R != nullptr && OffsetFromR(ctx, ctx.PC, o.PC) && OffsetFromR(ctx, ctx.PR, o.PR) &&
           OffsetFromR(ctx, ctx.GBR, o.GBR) && OffsetFromR(ctx, ctx.VBR, o.VBR) && OffsetFromR(ctx, ctx.SR, o.SR) &&
           OffsetFromR(ctx, ctx.MACL, o.MACL) && OffsetFromR(ctx, ctx.MACH, o.MACH) &&
           OffsetFromR(ctx, ctx.delaySlotTarget, o.delaySlotTarget) && OffsetFromR(ctx, ctx.wbReg, o.wbReg) &&
           OffsetFromR(ctx, ctx.intrPending, o.intrPending) && OffsetFromR(ctx, ctx.intrAllow, o.intrAllow) &&
           OffsetFromR(ctx, ctx.cyclesExecuted, o.cyclesExecuted);
}

constexpr int32_t kFrameCtx = static_cast<int32_t>(offsetof(X64Frame, ctx));
constexpr int32_t kFrameLimit = static_cast<int32_t>(offsetof(X64Frame, limit));
constexpr int32_t kFrameEntryCycles = static_cast<int32_t>(offsetof(X64Frame, entryCycles));
constexpr int32_t kOutCycles = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, cycles));
constexpr int32_t kOutRetired = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, retired));
constexpr int32_t kOutBoundary = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, boundary));
constexpr int32_t kOutBusWait = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, busWait));
constexpr int32_t kOutAborted = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, aborted));
constexpr int32_t kFrameStop = static_cast<int32_t>(offsetof(X64Frame, stop));
constexpr int32_t kCtxR = static_cast<int32_t>(offsetof(ymir::sh2::SH2JitContext, R));

// 32-bit immediates for 32-bit operations, as asmjit expects them (sign-extended form).
constexpr int32_t Imm32(uint32_t value) {
    return static_cast<int32_t>(value);
}

class Emitter {
public:
    Emitter(x86::Compiler &cc, const Block &block, const StateOffsets &off)
        : m_cc(cc)
        , m_block(block)
        , m_off(off)
        , m_values(block.numValues) {}

    void Emit() {
        FuncNode *func = m_cc.add_func(FuncSignature::build<void, X64Frame *>());
        m_frame = m_cc.new_gp_ptr("frame");
        func->set_arg(0, m_frame);
        m_regs = m_cc.new_gp_ptr("regs");
        m_cc.mov(m_regs, x86::qword_ptr(m_frame, kFrameCtx));
        m_cc.mov(m_regs, x86::qword_ptr(m_regs, kCtxR));
        m_cycles = m_cc.new_gp64("cycles");
        m_cc.xor_(m_cycles, m_cycles);

        for (const Inst &in : m_block.code) {
            Lower(in);
        }

        // Out-of-line boundary exits, after the block's final exit.
        for (const BoundaryStub &stub : m_stubs) {
            m_cc.bind(stub.label);
            WriteExit(true, stub.pc, stub.retired, m_cycles, true);
        }
        // The shared abort exit taken when a trampoline sets frame->stop.
        if (m_abortUsed) {
            m_cc.bind(m_abort);
            AbortExit(m_cycles);
        }
        m_cc.end_func();
    }

private:
    struct BoundaryStub {
        Label label;
        uint32_t pc;
        uint8_t retired;
    };

    x86::Mem State32(int32_t off) const {
        return x86::dword_ptr(m_regs, off);
    }
    x86::Mem State8(int32_t off) const {
        return x86::byte_ptr(m_regs, off);
    }

    x86::Gp Def(ValueId id) {
        m_values[id] = m_cc.new_gp32();
        return m_values[id];
    }
    const x86::Gp &Use(ValueId id) const {
        return m_values[id];
    }

    // reg += imm (64-bit). add r64, imm32 sign-extends, so larger values go through a register.
    void AddU32(const x86::Gp &reg, uint32_t imm) {
        if (imm == 0) {
            return;
        }
        if (imm <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
            m_cc.add(reg, Imm32(imm));
        } else {
            x86::Gp t = m_cc.new_gp64();
            m_cc.mov(t, static_cast<uint64_t>(imm));
            m_cc.add(reg, t);
        }
    }

    // Writes PC (if setPC), out.retired, out.boundary (if boundary) and out.cycles, then returns.
    void WriteExit(bool setPC, uint32_t pc, uint8_t retired, const x86::Gp &cycles, bool boundary) {
        if (setPC) {
            m_cc.mov(State32(m_off.PC), Imm32(pc));
        }
        m_cc.mov(x86::byte_ptr(m_frame, kOutRetired), retired);
        if (boundary) {
            m_cc.mov(x86::byte_ptr(m_frame, kOutBoundary), 1);
        }
        m_cc.mov(x86::qword_ptr(m_frame, kOutCycles), cycles);
        m_cc.ret();
    }

    // RunBlock's abort exit: out.aborted = true, out.cycles; PC and out.retired untouched.
    void AbortExit(const x86::Gp &cycles) {
        m_cc.mov(x86::byte_ptr(m_frame, kOutAborted), 1);
        m_cc.mov(x86::qword_ptr(m_frame, kOutCycles), cycles);
        m_cc.ret();
    }

    // Calls a trampoline: frame, then `args` (registers or immediates), result into `ret` if given.
    // An asmjit failure is reported through the error handler (Compile then fails).
    template <typename Ret, typename... Args>
    void Call(Ret (*fn)(X64Frame *, Args...) noexcept, std::initializer_list<Operand> args,
              const x86::Gp *ret = nullptr) {
        static_assert(sizeof...(Args) < 4, "trampolines take at most 4 arguments");
        InvokeNode *node = nullptr;
        m_cc.invoke(Out(node), reinterpret_cast<uint64_t>(fn), FuncSignature::build<Ret, X64Frame *, Args...>());
        if (node == nullptr) {
            return;
        }
        node->set_arg(0, m_frame);
        size_t index = 1;
        for (const Operand &arg : args) {
            if (arg.is_reg()) {
                node->set_arg(index, arg.as<Reg>());
            } else {
                node->set_arg(index, arg.as<Imm>());
            }
            ++index;
        }
        if (ret != nullptr) {
            node->set_ret(0, *ret);
        }
    }

    // Leaves through the shared abort exit if the last trampoline set frame->stop.
    void CheckStop() {
        if (!m_abortUsed) {
            m_abort = m_cc.new_label();
            m_abortUsed = true;
        }
        m_cc.cmp(x86::byte_ptr(m_frame, kFrameStop), 0);
        m_cc.jne(m_abort);
    }

    static Imm U32(uint32_t value) {
        return Imm(static_cast<int32_t>(value));
    }

    void Binary(const Inst &in, InstId id) {
        const x86::Gp d = Def(in.dst);
        m_cc.mov(d, Use(in.a));
        m_cc.emit(id, d, Use(in.b));
    }

    void Compare(const Inst &in, x86::CondCode cond) {
        const x86::Gp d = Def(in.dst);
        m_cc.xor_(d, d); // before cmp: xor changes the flags
        m_cc.cmp(Use(in.a), Use(in.b));
        m_cc.set(cond, d.r8());
    }

    void Lower(const Inst &in) {
        switch (in.op) {
        case Op::Const: m_cc.mov(Def(in.dst), Imm32(in.imm)); break;
        case Op::GetReg: m_cc.mov(Def(in.dst), State32(static_cast<int32_t>(in.imm * 4))); break;
        case Op::SetReg: m_cc.mov(State32(static_cast<int32_t>(in.imm * 4)), Use(in.a)); break;
        case Op::GetPR: m_cc.mov(Def(in.dst), State32(m_off.PR)); break;
        case Op::SetPR: m_cc.mov(State32(m_off.PR), Use(in.a)); break;
        case Op::GetT: {
            const x86::Gp d = Def(in.dst);
            m_cc.mov(d, State32(m_off.SR));
            m_cc.and_(d, 1);
            break;
        }
        case Op::SetT: {
            // SR = (SR & ~1) | (a != 0)
            x86::Gp t = m_cc.new_gp32();
            m_cc.xor_(t, t);
            m_cc.test(Use(in.a), Use(in.a));
            m_cc.setnz(t.r8());
            m_cc.and_(State32(m_off.SR), Imm32(~1u));
            m_cc.or_(State32(m_off.SR), t);
            break;
        }
        case Op::GetGBR: m_cc.mov(Def(in.dst), State32(m_off.GBR)); break;
        case Op::SetGBR: m_cc.mov(State32(m_off.GBR), Use(in.a)); break;
        case Op::GetVBR: m_cc.mov(Def(in.dst), State32(m_off.VBR)); break;
        case Op::SetVBR: m_cc.mov(State32(m_off.VBR), Use(in.a)); break;
        case Op::GetSR: m_cc.mov(Def(in.dst), State32(m_off.SR)); break;
        case Op::GetMACH: m_cc.mov(Def(in.dst), State32(m_off.MACH)); break;
        case Op::GetMACL: m_cc.mov(Def(in.dst), State32(m_off.MACL)); break;
        case Op::SetMACH: m_cc.mov(State32(m_off.MACH), Use(in.a)); break;
        case Op::SetMACL: m_cc.mov(State32(m_off.MACL), Use(in.a)); break;
        case Op::ClearIntrAllow: m_cc.mov(State8(m_off.intrAllow), 0); break;
        case Op::SetIntrAllow: m_cc.mov(State8(m_off.intrAllow), 1); break;
        case Op::GetDelayTarget: m_cc.mov(Def(in.dst), State32(m_off.delaySlotTarget)); break;
        case Op::Add: Binary(in, x86::Inst::kIdAdd); break;
        case Op::Sub: Binary(in, x86::Inst::kIdSub); break;
        case Op::And: Binary(in, x86::Inst::kIdAnd); break;
        case Op::Or: Binary(in, x86::Inst::kIdOr); break;
        case Op::Xor: Binary(in, x86::Inst::kIdXor); break;
        case Op::Mul: Binary(in, x86::Inst::kIdImul); break; // low 32 bits are the same signed or not
        case Op::Not: {
            const x86::Gp d = Def(in.dst);
            m_cc.mov(d, Use(in.a));
            m_cc.not_(d);
            break;
        }
        case Op::Shl:
        case Op::Shr:
        case Op::Sar: {
            const x86::Gp d = Def(in.dst);
            m_cc.mov(d, Use(in.a));
            const InstId id = in.op == Op::Shl   ? x86::Inst::kIdShl
                              : in.op == Op::Shr ? x86::Inst::kIdShr
                                                 : x86::Inst::kIdSar;
            m_cc.emit(id, d, Imm(in.imm)); // 1..31 (VerifyBlock)
            break;
        }
        case Op::SExt8: m_cc.movsx(Def(in.dst), Use(in.a).r8()); break;
        case Op::SExt16: m_cc.movsx(Def(in.dst), Use(in.a).r16()); break;
        case Op::CmpEq: Compare(in, x86::CondCode::kEqual); break;
        case Op::CmpGtU: Compare(in, x86::CondCode::kUnsignedGT); break;
        case Op::CmpGeU: Compare(in, x86::CondCode::kUnsignedGE); break;
        case Op::CmpGtS: Compare(in, x86::CondCode::kSignedGT); break;
        case Op::CmpGeS: Compare(in, x86::CondCode::kSignedGE); break;
        case Op::MulHiS:
        case Op::MulHiU: {
            // One-operand (i)mul: EDX:EAX = EAX * b, the full 64-bit product of the 32-bit operands.
            const x86::Gp hi = Def(in.dst);
            x86::Gp lo = m_cc.new_gp32();
            m_cc.mov(lo, Use(in.a));
            if (in.op == Op::MulHiS) {
                m_cc.imul(hi, lo, Use(in.b));
            } else {
                m_cc.mul(hi, lo, Use(in.b));
            }
            break;
        }
        case Op::SetSRBits: {
            // SR = (SR & ~mask) | (a & mask)
            x86::Gp t = m_cc.new_gp32();
            m_cc.mov(t, Use(in.a));
            m_cc.and_(t, Imm32(in.imm));
            m_cc.and_(State32(m_off.SR), Imm32(~in.imm));
            m_cc.or_(State32(m_off.SR), t);
            break;
        }
        case Op::AddCycles: AddU32(m_cycles, in.imm); break;
        case Op::WbStall: {
            // wb = *wbReg; if (wb <= 16 && ((imm >> wb) & 1)) cycles += 1
            const uint32_t mask = in.imm & 0x1FFFFu; // bits a wb <= 16 can select
            if (mask == 0) {
                break;
            }
            const Label skip = m_cc.new_label();
            x86::Gp wb = m_cc.new_gp32();
            x86::Gp bits = m_cc.new_gp32();
            m_cc.movzx(wb, State8(m_off.wbReg));
            m_cc.cmp(wb, 16);
            m_cc.ja(skip);
            m_cc.mov(bits, Imm32(mask));
            m_cc.bt(bits, wb);
            m_cc.jnc(skip);
            m_cc.add(m_cycles, 1);
            m_cc.bind(skip);
            break;
        }
        case Op::SetWb: m_cc.mov(State8(m_off.wbReg), static_cast<uint8_t>(in.imm)); break;
        case Op::SyncCycles: {
            // *cyclesExecuted = entryCycles + cycles
            x86::Gp t = m_cc.new_gp64();
            m_cc.mov(t, x86::qword_ptr(m_frame, kFrameEntryCycles));
            m_cc.add(t, m_cycles);
            m_cc.mov(x86::qword_ptr(m_regs, m_off.cyclesExecuted), t);
            break;
        }
        case Op::CheckBoundary: {
            // Stop if cycles >= limit, or an interrupt is pending and allowed.
            BoundaryStub stub{m_cc.new_label(), in.imm, in.retired};
            const Label cont = m_cc.new_label();
            m_cc.cmp(m_cycles, x86::qword_ptr(m_frame, kFrameLimit));
            m_cc.jae(stub.label);
            m_cc.cmp(State8(m_off.intrPending), 0);
            m_cc.je(cont);
            m_cc.cmp(State8(m_off.intrAllow), 0);
            m_cc.jne(stub.label);
            m_cc.bind(cont);
            m_stubs.push_back(stub);
            break;
        }
        case Op::ExitIf: {
            // Taken: cycles += imm2, refill from imm (if flag; abort exit on stop), PC = imm, return.
            const Label notTaken = m_cc.new_label();
            m_cc.test(Use(in.a), Use(in.a));
            m_cc.jz(notTaken);
            x86::Gp taken = m_cc.new_gp64();
            m_cc.mov(taken, m_cycles);
            AddU32(taken, in.imm2);
            if (in.flag) {
                Call(&TrRefill, {U32(in.imm)});
                const Label go = m_cc.new_label();
                m_cc.cmp(x86::byte_ptr(m_frame, kFrameStop), 0);
                m_cc.je(go);
                AbortExit(taken);
                m_cc.bind(go);
            }
            WriteExit(true, in.imm, in.retired, taken, false);
            m_cc.bind(notTaken);
            break;
        }
        case Op::Exit: WriteExit(true, in.imm, in.retired, m_cycles, false); break;
        case Op::ExitDynamic: WriteExit(false, 0, in.retired, m_cycles, false); break;

        // Calls out of generated code (trampolines, see x64_emitter.hpp).
        case Op::Load: {
            const x86::Gp d = Def(in.dst);
            Call(&TrRead, {Use(in.a), U32(in.size), U32(in.flag ? 1 : 0)}, &d);
            CheckStop();
            break;
        }
        case Op::Store:
            Call(&TrWrite, {Use(in.a), U32(in.size), Use(in.b)});
            CheckStop();
            break;
        case Op::Refill:
            Call(&TrRefill, {U32(in.imm)});
            CheckStop();
            break;
        case Op::AddAccessCycles: {
            const x86::Gp c = m_cc.new_gp64();
            Call(&TrAccessCycles, {Use(in.a), U32(in.size), U32(in.flag ? 1 : 0)}, &c);
            CheckStop();
            m_cc.add(m_cycles, c);
            break;
        }
        case Op::AddAccessCyclesRMWByte: {
            const x86::Gp c = m_cc.new_gp64();
            Call(&TrAccessCyclesRMWByte, {Use(in.a)}, &c);
            CheckStop();
            m_cc.add(m_cycles, c);
            break;
        }
        case Op::ExitIfBusWait: {
            // If the bus is busy: PC = imm, out.retired, out.busWait, out.cycles, return.
            const x86::Gp wait = m_cc.new_gp32();
            Call(&TrBusWait, {Use(in.a), U32(in.size), U32(in.flag ? 1 : 0)}, &wait);
            CheckStop();
            const Label cont = m_cc.new_label();
            m_cc.test(wait, wait);
            m_cc.jz(cont);
            m_cc.mov(x86::byte_ptr(m_frame, kOutBusWait), 1);
            WriteExit(true, in.imm, in.retired, m_cycles, false);
            m_cc.bind(cont);
            break;
        }
        case Op::SetupDelaySlot:
            Call(&TrSetupDelaySlot, {Use(in.a)});
            CheckStop();
            break;
        case Op::EndDelaySlot:
            Call(&TrEndDelaySlot, {});
            CheckStop();
            break;
        case Op::SetSR:
            Call(&TrSetSR, {Use(in.a), U32(in.flag ? 1 : 0)});
            CheckStop();
            break;
        case Op::Div1: {
            const x86::Gp d = Def(in.dst);
            Call(&TrDiv1, {Use(in.a), Use(in.b), U32(in.flag ? 1 : 0)}, &d);
            break;
        }
        case Op::MacW: Call(&TrMacW, {Use(in.a), Use(in.b)}); break;
        case Op::MacL: Call(&TrMacL, {Use(in.a), Use(in.b)}); break;
        }
    }

    x86::Compiler &m_cc;
    const Block &m_block;
    const StateOffsets &m_off;
    std::vector<x86::Gp> m_values; // one 32-bit virtual register per IR value
    std::vector<BoundaryStub> m_stubs;
    x86::Gp m_frame;
    x86::Gp m_regs;   // ctx->R
    x86::Gp m_cycles; // cycles accumulated by this block (ExitInfo::cycles)
    Label m_abort;    // shared abort exit, created by the first CheckStop
    bool m_abortUsed = false;
};

} // namespace

bool CanEmitBlock(const Block &block) {
    for (const Inst &in : block.code) {
        switch (in.op) {
        case Op::Const:
        case Op::GetReg:
        case Op::SetReg:
        case Op::GetPR:
        case Op::SetPR:
        case Op::GetT:
        case Op::SetT:
        case Op::GetGBR:
        case Op::SetGBR:
        case Op::GetVBR:
        case Op::SetVBR:
        case Op::GetSR:
        case Op::GetMACH:
        case Op::GetMACL:
        case Op::SetMACH:
        case Op::SetMACL:
        case Op::ClearIntrAllow:
        case Op::SetIntrAllow:
        case Op::GetDelayTarget:
        case Op::Add:
        case Op::Sub:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Not:
        case Op::Shl:
        case Op::Shr:
        case Op::Sar:
        case Op::SExt8:
        case Op::SExt16:
        case Op::CmpEq:
        case Op::CmpGtU:
        case Op::CmpGeU:
        case Op::CmpGtS:
        case Op::CmpGeS:
        case Op::Mul:
        case Op::MulHiS:
        case Op::MulHiU:
        case Op::SetSRBits:
        case Op::AddCycles:
        case Op::WbStall:
        case Op::SetWb:
        case Op::SyncCycles:
        case Op::CheckBoundary:
        case Op::Exit:
        case Op::ExitDynamic:
        case Op::ExitIf:
        case Op::SetSR:
        case Op::Div1:
        case Op::MacW:
        case Op::MacL:
        case Op::AddAccessCyclesRMWByte:
        case Op::Load:
        case Op::Store:
        case Op::AddAccessCycles:
        case Op::Refill:
        case Op::SetupDelaySlot:
        case Op::EndDelaySlot:
        case Op::ExitIfBusWait: break;
        default: return false; // not a valid op
        }
    }
    return true;
}

bool EmitBlock(x86::Compiler &cc, const Block &block, const ymir::sh2::SH2JitContext &ctx) {
    StateOffsets off{};
    if (!CanEmitBlock(block) || !ComputeOffsets(ctx, off)) {
        return false;
    }
    Emitter(cc, block, off).Emit();
    return true;
}

} // namespace brimir::jit
