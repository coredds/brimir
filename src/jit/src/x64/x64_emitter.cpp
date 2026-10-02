#include "x64_emitter.hpp"

#include <brimir/jit/bus_fast_path.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
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
    // Optional: without them, refills and delay-slot ops call their trampolines.
    int32_t fetchedOpcodes, delaySlot, intcPendingLevel;
    bool hasFetchedOpcodes; // fetchedOpcodes is valid
    bool hasDelaySlot;      // delaySlot, intcPendingLevel and fetchedOpcodes are valid
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
    const bool required =
        ctx.R != nullptr && OffsetFromR(ctx, ctx.PC, o.PC) && OffsetFromR(ctx, ctx.PR, o.PR) &&
        OffsetFromR(ctx, ctx.GBR, o.GBR) && OffsetFromR(ctx, ctx.VBR, o.VBR) && OffsetFromR(ctx, ctx.SR, o.SR) &&
        OffsetFromR(ctx, ctx.MACL, o.MACL) && OffsetFromR(ctx, ctx.MACH, o.MACH) &&
        OffsetFromR(ctx, ctx.delaySlotTarget, o.delaySlotTarget) && OffsetFromR(ctx, ctx.wbReg, o.wbReg) &&
        OffsetFromR(ctx, ctx.intrPending, o.intrPending) && OffsetFromR(ctx, ctx.intrAllow, o.intrAllow) &&
        OffsetFromR(ctx, ctx.cyclesExecuted, o.cyclesExecuted);
    if (!required) {
        return false;
    }
    o.hasFetchedOpcodes = OffsetFromR(ctx, ctx.fetchedOpcodes, o.fetchedOpcodes);
    o.hasDelaySlot = o.hasFetchedOpcodes && OffsetFromR(ctx, ctx.delaySlot, o.delaySlot) &&
                     OffsetFromR(ctx, ctx.intcPendingLevel, o.intcPendingLevel);
    return true;
}

constexpr int32_t kFrameCtx = static_cast<int32_t>(offsetof(X64Frame, ctx));
constexpr int32_t kFrameLimit = static_cast<int32_t>(offsetof(X64Frame, limit));
constexpr int32_t kFrameEntryCycles = static_cast<int32_t>(offsetof(X64Frame, entryCycles));
constexpr int32_t kOutCycles = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, cycles));
constexpr int32_t kOutRetired = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, retired));
constexpr int32_t kOutBoundary = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, boundary));
constexpr int32_t kOutBusWait = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, busWait));
constexpr int32_t kOutAborted = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, aborted));
constexpr int32_t kOutStale = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, stale));
constexpr int32_t kOutBlocksRun = static_cast<int32_t>(offsetof(X64Frame, out) + offsetof(ExitInfo, blocksRun));
constexpr int32_t kFrameStop = static_cast<int32_t>(offsetof(X64Frame, stop));
constexpr int32_t kFrameCodeDirty = static_cast<int32_t>(offsetof(X64Frame, codeDirty));
constexpr int32_t kFrameAllowChain = static_cast<int32_t>(offsetof(X64Frame, allowChain));
constexpr int32_t kFrameChained = static_cast<int32_t>(offsetof(X64Frame, chained));
constexpr int32_t kFrameAbortRequested = static_cast<int32_t>(offsetof(X64Frame, abortRequested));
static_assert(sizeof(ExitInfo::blocksRun) == 4);
constexpr int32_t kLinkPc = static_cast<int32_t>(offsetof(X64LinkSlot, pc));
constexpr int32_t kLinkEntry = static_cast<int32_t>(offsetof(X64LinkSlot, entry));
constexpr int32_t kCtxR = static_cast<int32_t>(offsetof(ymir::sh2::SH2JitContext, R));

// 32-bit immediates for 32-bit operations, as asmjit expects them (sign-extended form).
constexpr int32_t Imm32(uint32_t value) {
    return static_cast<int32_t>(value);
}

// True if generated code can walk the bus page table (bus_fast_path.hpp): a table is present and
// every offset fits a 32-bit displacement. Otherwise every access calls its trampoline.
bool CanInlineBus(const ymir::sh2::SH2JitBusLayout &bus) {
    // A field is read at [entry + offset] with offset as a signed 32-bit displacement. The 8-byte
    // margin keeps the whole field (at most 8 bytes: a uint64 cycle count or the array pointer)
    // inside the int32 displacement range.
    constexpr uint32_t kMaxDisp = static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) - 8;
    if (bus.pages == nullptr || bus.pageStride == 0 || bus.pageShift == 0 || bus.pageShift >= 32 ||
        bus.arrayOffset > kMaxDisp || bus.arrayWritableOffset > kMaxDisp) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (bus.readCyclesOffset[i] > kMaxDisp || bus.writeCyclesOffset[i] > kMaxDisp) {
            return false;
        }
    }
    return true;
}

// Partition bit sets for `bt mask, address >> 29`.
constexpr uint32_t kBusPartitions = (1u << 0b000) | (1u << 0b001) | (1u << 0b101); // MemRead/MemWrite to the bus
constexpr uint32_t kBusCyclePartitions = (1u << 0b001) | (1u << 0b101);            // AccessCycles from the bus

uint32_t SizeIndex(uint32_t size) {
    return size == 1 ? 0 : size == 2 ? 1 : 2;
}

class Emitter {
public:
    Emitter(x86::Compiler &cc, const Block &block, const StateOffsets &off, const ymir::sh2::SH2JitBusLayout &bus,
            const X64LinkSlot *links)
        : m_cc(cc)
        , m_block(block)
        , m_off(off)
        , m_bus(bus)
        , m_inlineBus(CanInlineBus(bus))
        , m_links(links)
        , m_values(block.numValues) {
        // The block's code in host memory, if it is all on array pages now.
        const bool onArrays =
            m_inlineBus && !block.guestOpcodes.empty() &&
            FindCodeHostRanges(bus, block.startPC, static_cast<uint32_t>(block.guestOpcodes.size()), m_ranges);
        // Self-validation compares those bytes at entry; the chained-entry gates need the
        // delay-slot flag and the fetch buffer (hasDelaySlot), and chaining needs the link table.
        m_selfValidating = onArrays && off.hasDelaySlot && links != nullptr;
        // Known refills need the inline bus (for the entry check and the store classification),
        // the fetch buffer's offset, and code on array pages now as when the front end checked.
        m_known = onArrays && off.hasFetchedOpcodes && block.fetchFromArrays;
        if (m_known) {
            for (size_t i = 0; i < block.code.size(); ++i) {
                if (block.code[i].op == Op::Refill && block.code[i].flag) {
                    m_lastKnownRefill = static_cast<ptrdiff_t>(i);
                }
            }
            m_known = m_lastKnownRefill >= 0;
        }
    }

    bool SelfValidating() const {
        return m_selfValidating;
    }

    void Emit() {
        FuncNode *func = m_cc.add_func(FuncSignature::build<const void *, X64Frame *>());
        m_frame = m_cc.new_gp_ptr("frame");
        func->set_arg(0, m_frame);
        m_retNull = m_cc.new_label();
        m_regs = m_cc.new_gp_ptr("regs");
        m_cc.mov(m_regs, x86::qword_ptr(m_frame, kFrameCtx));
        m_cc.mov(m_regs, x86::qword_ptr(m_regs, kCtxR));

        // Prologue (x64_emitter.hpp): validation, chained-entry gates, block count.
        if (m_selfValidating || m_known) {
            m_stale = m_cc.new_label();
            EmitPageCheck();
            if (m_selfValidating) {
                EmitCodeCheck();
            }
        }
        if (m_selfValidating) {
            EmitChainGates();
        }
        m_cc.add(x86::dword_ptr(m_frame, kOutBlocksRun), 1);
        if (m_known) {
            m_cc.mov(x86::byte_ptr(m_frame, kFrameCodeDirty), 0); // per block: a chain shares the frame
        }
        // Cycles count from the start of the chain (0 for its first block).
        m_cycles = m_cc.new_gp64("cycles");
        m_cc.mov(m_cycles, x86::qword_ptr(m_frame, kOutCycles));

        for (size_t i = 0; i < m_block.code.size(); ++i) {
            // Data accesses before the last known refill classify themselves for codeDirty.
            m_trackDirty = m_known && static_cast<ptrdiff_t>(i) < m_lastKnownRefill;
            Lower(m_block.code[i]);
        }

        // Out-of-line boundary exits, after the block's final exit.
        for (const BoundaryStub &stub : m_stubs) {
            m_cc.bind(stub.label);
            WriteExit(true, stub.pc, stub.retired, m_cycles, true);
            m_cc.jmp(m_retNull);
        }
        // Stale entry: out.stale only (nothing else was written).
        if (m_selfValidating || m_known) {
            m_cc.bind(m_stale);
            m_cc.mov(x86::byte_ptr(m_frame, kOutStale), 1);
            m_cc.jmp(m_retNull);
        }
        // Chained entry with a pending delay slot (never happens; see TrChainedInDelaySlot).
        if (m_selfValidating) {
            m_cc.bind(m_delaySlotStub);
            Call(&TrChainedInDelaySlot, {});
            m_cc.jmp(m_retNull);
        }
        // The shared abort exit taken when a trampoline sets frame->stop.
        if (m_abortUsed) {
            m_cc.bind(m_abort);
            AbortExit(m_cycles);
        }
        // Every non-chaining exit: return nullptr to X64Backend::Run.
        m_cc.bind(m_retNull);
        x86::Gp none = m_cc.new_gp_ptr();
        m_cc.xor_(none, none);
        m_cc.ret(none);
        m_cc.end_func();
    }

private:
    // ---- Guest register cache (design/sh2-x64-performance.md, 2C item 4) ----
    //
    // R0-R15 and SR live in virtual registers inside the block. Lowering is a single pass in op
    // order, so the cache state is known at compile time at every op. Its rules:
    //   - GetReg/GetSR load a slot on first use; SetReg/SetT/SetSRBits replace its virtual register
    //     (IR values are SSA: a slot's register is never written in place) and mark it dirty.
    //   - Before every trampoline call the dirty slots are stored (callbacks may read them: SR.ILevel
    //     through interrupt recomputation, and the abort and exception paths must leave RunBlock's
    //     state). On the op's main path the store makes the slot clean; on a conditional path (an
    //     inline access's slow path) the slot stays dirty, so both paths agree at the join and a
    //     later flush only stores the same value again. Div1/MacW/MacL read SR only (x64_backend.cpp):
    //     only SR is stored before them.
    //   - Every exit stores the dirty slots: the final exits on the main path; a taken ExitIf on its
    //     own path, leaving the fall-through cache unchanged. CheckBoundary stores them on the main
    //     path before its test (write-back once per guest instruction), so its out-of-line stub has
    //     nothing to store. Keeping slots dirty across checks instead, with each stub storing the
    //     dirty set of its check, made code 11% larger and compiles slower for no measurable gain
    //     (design/sh2-x64-performance.md, 2C progress, Task 4). The abort exit needs nothing: it
    //     is only reached right after a call, before which everything was stored.
    //   - Callbacks never write R0-R15 (x64_emitter.hpp, "Register cache"). SR is reloaded after
    //     SetSR and Div1, whose helpers write it, and everything after EndDelaySlot.
    static constexpr uint32_t kSlotSR = 16;
    static constexpr uint32_t kSlots = 17;
    struct CachedSlot {
        x86::Gp value;
        bool valid = false;
        bool dirty = false;
    };
    struct DirtyStore {
        int32_t offset;
        x86::Gp value;
    };

    int32_t SlotOffset(uint32_t slot) const {
        return slot == kSlotSR ? m_off.SR : static_cast<int32_t>(slot * 4);
    }

    const x86::Gp &CachedGet(uint32_t slot) {
        CachedSlot &s = m_cache[slot];
        if (!s.valid) {
            s.value = m_cc.new_gp32();
            m_cc.mov(s.value, State32(SlotOffset(slot)));
            s.valid = true;
            s.dirty = false;
        }
        return s.value;
    }

    void CachedSet(uint32_t slot, const x86::Gp &value) {
        CachedSlot &s = m_cache[slot];
        s.value = value;
        s.valid = true;
        s.dirty = true;
    }

    // The dirty slots now, for a store on another path (a taken ExitIf).
    std::vector<DirtyStore> DirtySet() const {
        std::vector<DirtyStore> set;
        for (uint32_t slot = 0; slot < kSlots; ++slot) {
            if (m_cache[slot].dirty) {
                set.push_back({SlotOffset(slot), m_cache[slot].value});
            }
        }
        return set;
    }

    void StoreDirty(const std::vector<DirtyStore> &set) {
        for (const DirtyStore &d : set) {
            m_cc.mov(State32(d.offset), d.value);
        }
    }

    // Stores the dirty slots (SR only if srOnly). mainPath: the store is on the op's main path, so
    // the slots become clean; otherwise they stay dirty (see the rules above).
    void FlushRegs(bool mainPath, bool srOnly = false) {
        for (uint32_t slot = srOnly ? kSlotSR : 0; slot < kSlots; ++slot) {
            CachedSlot &s = m_cache[slot];
            if (s.dirty) {
                m_cc.mov(State32(SlotOffset(slot)), s.value);
                if (mainPath) {
                    s.dirty = false;
                }
            }
        }
    }

    // Drops clean slots (reloaded on next use); the caller has stored them first.
    void InvalidateRegs(bool srOnly = false) {
        for (uint32_t slot = srOnly ? kSlotSR : 0; slot < kSlots; ++slot) {
            assert(!m_cache[slot].dirty && "invalidating a dirty cached register");
            m_cache[slot].valid = false;
        }
    }

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

    // Writes PC (if setPC), out.retired, out.boundary (if boundary) and out.cycles. The caller then
    // returns (jmp m_retNull) or chains (ChainExit).
    void WriteExit(bool setPC, uint32_t pc, uint8_t retired, const x86::Gp &cycles, bool boundary) {
        if (setPC) {
            m_cc.mov(State32(m_off.PC), Imm32(pc));
        }
        m_cc.mov(x86::byte_ptr(m_frame, kOutRetired), retired);
        if (boundary) {
            m_cc.mov(x86::byte_ptr(m_frame, kOutBoundary), 1);
        }
        m_cc.mov(x86::qword_ptr(m_frame, kOutCycles), cycles);
    }

    // After WriteExit of a chainable exit: what Executor::Run does before its next Step, then the
    // link-table lookup for PC (`pc` when constPC, else read back from the state).
    void ChainExit(bool constPC, uint32_t pc, const x86::Gp &cycles) {
        if (m_links == nullptr) {
            m_cc.jmp(m_retNull);
            return;
        }
        m_cc.cmp(x86::byte_ptr(m_frame, kFrameAllowChain), 0);
        m_cc.je(m_retNull);
        // A flush requested by a callback that does not stop the block (setSR, endDelaySlot,
        // accessCycles, busWait: RunBlock runs on after them too) must end the chain here, with
        // this block's exit complete, as RunBlock would return; the executor then flushes. A
        // chained block would otherwise run with the request pending and abort at its first
        // memory callback, partway through.
        x86::Gp abortFlag = m_cc.new_gp_ptr();
        m_cc.mov(abortFlag, x86::qword_ptr(m_frame, kFrameAbortRequested));
        m_cc.cmp(x86::byte_ptr(abortFlag), 0);
        m_cc.jne(m_retNull);
        // *cyclesExecuted = entryCycles + cycles (Executor::Run, before each Step).
        x86::Gp now = m_cc.new_gp64();
        m_cc.mov(now, x86::qword_ptr(m_frame, kFrameEntryCycles));
        m_cc.add(now, cycles);
        m_cc.mov(x86::qword_ptr(m_regs, m_off.cyclesExecuted), now);
        // Run's loop ends once the target is reached.
        m_cc.cmp(cycles, x86::qword_ptr(m_frame, kFrameLimit));
        m_cc.jae(m_retNull);
        x86::Gp slot = m_cc.new_gp64();
        if (constPC) {
            m_cc.mov(slot, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&m_links[LinkIndex(pc)])));
            m_cc.cmp(x86::dword_ptr(slot, kLinkPc), Imm32(pc));
        } else {
            x86::Gp next = m_cc.new_gp32();
            x86::Gp index = m_cc.new_gp64();
            m_cc.mov(next, State32(m_off.PC));
            m_cc.mov(index.r32(), next); // zero-extends
            m_cc.shr(index.r32(), 1);
            m_cc.and_(index.r32(), Imm32(kLinkSlots - 1));
            m_cc.shl(index, 4); // sizeof(X64LinkSlot)
            m_cc.mov(slot, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_links)));
            m_cc.add(slot, index);
            m_cc.cmp(x86::dword_ptr(slot, kLinkPc), next);
        }
        m_cc.jne(m_retNull);
        x86::Gp entry = m_cc.new_gp_ptr();
        m_cc.mov(entry, x86::qword_ptr(slot, kLinkEntry));
        m_cc.mov(x86::byte_ptr(m_frame, kFrameChained), 1);
        m_cc.ret(entry);
    }

    // RunBlock's abort exit: out.aborted = true, out.cycles; PC and out.retired untouched.
    void AbortExit(const x86::Gp &cycles) {
        m_cc.mov(x86::byte_ptr(m_frame, kOutAborted), 1);
        m_cc.mov(x86::qword_ptr(m_frame, kOutCycles), cycles);
        m_cc.jmp(m_retNull);
    }

    // Calls a trampoline: frame, then `args` (registers or immediates), result into `ret` if given.
    // An asmjit failure is reported through the error handler (Compile then fails).
    template <typename Ret, typename... Args>
    void Call(Ret (*fn)(X64Frame *, Args...) noexcept, std::initializer_list<Operand> args,
              const x86::Gp *ret = nullptr) {
        static_assert(sizeof...(Args) < 4, "trampolines take at most 4 arguments");
        assert(args.size() == sizeof...(Args) && "argument count must match the trampoline signature");
        // Register arguments and the result go through short-lived copies. Argument and result
        // registers are fixed-register uses; on a long-lived value (a cached guest register, a
        // load result kept in the cache) they steer asmjit's bin packing to that one register, and
        // when it is taken the value is left unassigned and goes through its stack slot at every use.
        std::array<Operand, 4> copies{};
        size_t count = 0;
        for (const Operand &arg : args) {
            if (arg.is_reg()) {
                x86::Gp copy = m_cc.new_similar_reg(arg.as<x86::Gp>());
                m_cc.mov(copy, arg.as<x86::Gp>());
                copies[count++] = copy;
            } else {
                copies[count++] = arg;
            }
        }
        InvokeNode *node = nullptr;
        m_cc.invoke(Out(node), reinterpret_cast<uint64_t>(fn), FuncSignature::build<Ret, X64Frame *, Args...>());
        if (node == nullptr) {
            return;
        }
        node->set_arg(0, m_frame); // the function's argument: its hint is the same register
        for (size_t i = 0; i < count; ++i) {
            if (copies[i].is_reg()) {
                node->set_arg(i + 1, copies[i].as<Reg>());
            } else {
                node->set_arg(i + 1, copies[i].as<Imm>());
            }
        }
        if (ret != nullptr) {
            x86::Gp result = m_cc.new_similar_reg(*ret);
            node->set_ret(0, result);
            m_cc.mov(*ret, result);
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

    // ---- Inline bus fast path (bus_fast_path.hpp is the C++ reference) ----

    // Jumps to `miss` unless the partition (address >> 29) is in `partitions` (bit set).
    void JumpUnlessPartition(const x86::Gp &address, uint32_t partitions, const Label &miss) {
        x86::Gp part = m_cc.new_gp32();
        x86::Gp mask = m_cc.new_gp32();
        m_cc.mov(part, address);
        m_cc.shr(part, 29);
        m_cc.mov(mask, Imm32(partitions));
        m_cc.bt(mask, part);
        m_cc.jnc(miss);
    }

    // Host pointer to the page entry of `address`: pages + ((address & addressMask) >> pageShift) * stride.
    x86::Gp PageEntry(const x86::Gp &address) {
        x86::Gp index = m_cc.new_gp64();
        m_cc.mov(index.r32(), address); // zero-extends
        m_cc.and_(index.r32(), Imm32(m_bus.addressMask));
        m_cc.shr(index.r32(), m_bus.pageShift);
        if ((m_bus.pageStride & (m_bus.pageStride - 1)) == 0) {
            uint32_t shift = 0;
            while ((1u << shift) != m_bus.pageStride) {
                ++shift;
            }
            if (shift != 0) {
                m_cc.shl(index, shift);
            }
        } else {
            m_cc.imul(index, index, Imm32(m_bus.pageStride));
        }
        x86::Gp entry = m_cc.new_gp64();
        m_cc.mov(entry, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_bus.pages)));
        m_cc.add(entry, index);
        return entry;
    }

    // Loads the page's array pointer into a new register and jumps to `miss` if it is null.
    x86::Gp PageArrayOrMiss(const x86::Gp &entry, const Label &miss) {
        x86::Gp array = m_cc.new_gp64();
        m_cc.mov(array, x86::qword_ptr(entry, static_cast<int32_t>(m_bus.arrayOffset)));
        m_cc.test(array, array);
        m_cc.jz(miss);
        return array;
    }

    // Offset of the size-aligned address inside its page.
    x86::Gp PageOffset(const x86::Gp &address, uint32_t size) {
        const uint32_t pageMask = (1u << m_bus.pageShift) - 1;
        x86::Gp offset = m_cc.new_gp64();
        m_cc.mov(offset.r32(), address); // zero-extends
        m_cc.and_(offset.r32(), Imm32(pageMask & ~(size - 1)));
        return offset;
    }

    void LowerLoad(const Inst &in) {
        const x86::Gp d = Def(in.dst);
        const Label slow = m_cc.new_label();
        const Label done = m_cc.new_label();
        if (m_inlineBus) {
            const x86::Gp &address = Use(in.a);
            JumpUnlessPartition(address, kBusPartitions, slow);
            const x86::Gp entry = PageEntry(address);
            const x86::Gp array = PageArrayOrMiss(entry, slow);
            const x86::Gp offset = PageOffset(address, in.size);
            switch (in.size) {
            case 1: m_cc.movzx(d, x86::byte_ptr(array, offset)); break;
            case 2:
                m_cc.movzx(d, x86::word_ptr(array, offset));
                m_cc.rol(d.r16(), 8); // big-endian; bits 31-16 stay zero
                break;
            default:
                m_cc.mov(d, x86::dword_ptr(array, offset));
                m_cc.bswap(d);
                break;
            }
            m_cc.jmp(done);
        }
        m_cc.bind(slow);
        FlushRegs(!m_inlineBus);
        Call(&TrRead, {Use(in.a), U32(in.size), U32(in.flag ? 1 : 0)}, &d);
        CheckStop();
        if (m_trackDirty) {
            // A handler read outside the on-chip registers (partition 0b111) may write memory.
            const Label onChip = m_cc.new_label();
            x86::Gp part = m_cc.new_gp32();
            m_cc.mov(part, Use(in.a));
            m_cc.shr(part, 29);
            m_cc.cmp(part, 0b111);
            m_cc.je(onChip);
            SetCodeDirty();
            m_cc.bind(onChip);
        }
        m_cc.bind(done);
    }

    void LowerStore(const Inst &in) {
        const Label slow = m_cc.new_label();
        const Label done = m_cc.new_label();
        if (m_inlineBus) {
            const x86::Gp &address = Use(in.a);
            JumpUnlessPartition(address, kBusPartitions, slow);
            const x86::Gp entry = PageEntry(address);
            const x86::Gp array = PageArrayOrMiss(entry, slow);
            // Bus::Write: an array page that is not writable drops the write.
            m_cc.cmp(x86::byte_ptr(entry, static_cast<int32_t>(m_bus.arrayWritableOffset)), 0);
            m_cc.je(done);
            const x86::Gp offset = PageOffset(address, in.size);
            if (m_trackDirty) {
                x86::Gp host = m_cc.new_gp64();
                m_cc.lea(host, x86::ptr(array, offset));
                MarkDirtyIfInCode(host, in.size);
            }
            switch (in.size) {
            case 1: m_cc.mov(x86::byte_ptr(array, offset), Use(in.b).r8()); break;
            case 2: {
                x86::Gp t = m_cc.new_gp32();
                m_cc.mov(t, Use(in.b));
                m_cc.rol(t.r16(), 8);
                m_cc.mov(x86::word_ptr(array, offset), t.r16());
                break;
            }
            default: {
                x86::Gp t = m_cc.new_gp32();
                m_cc.mov(t, Use(in.b));
                m_cc.bswap(t);
                m_cc.mov(x86::dword_ptr(array, offset), t);
                break;
            }
            }
            m_cc.jmp(done);
        }
        m_cc.bind(slow);
        FlushRegs(!m_inlineBus);
        Call(&TrWrite, {Use(in.a), U32(in.size), Use(in.b)});
        CheckStop();
        if (m_trackDirty) {
            SetCodeDirty(); // a handler write may write anything
        }
        m_cc.bind(done);
    }

    // ---- Fetch buffer and delay slots ----

    void SetCodeDirty() {
        m_cc.mov(x86::byte_ptr(m_frame, kFrameCodeDirty), 1);
        m_mayBeDirty = true;
    }

    // codeDirty = 1 if [host, host + size) overlaps the block's code (m_ranges, compile-time host
    // pointers kept valid by the entry check).
    void MarkDirtyIfInCode(const x86::Gp &host, uint32_t size) {
        x86::Gp bound = m_cc.new_gp64();
        x86::Gp end = m_cc.new_gp64();
        m_cc.lea(end, x86::ptr(host, static_cast<int32_t>(size)));
        for (uint32_t r = 0; r < m_ranges.count; ++r) {
            const Label outside = m_cc.new_label();
            m_cc.mov(bound, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_ranges.hi[r])));
            m_cc.cmp(host, bound);
            m_cc.jae(outside);
            m_cc.mov(bound, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_ranges.lo[r])));
            m_cc.cmp(end, bound);
            m_cc.jbe(outside);
            m_cc.mov(x86::byte_ptr(m_frame, kFrameCodeDirty), 1);
            m_cc.bind(outside);
        }
        m_mayBeDirty = true;
    }

    // Entry check of a self-validating block or one with known refills: every code page still has
    // its compile-time array (so m_ranges still points at the block's code).
    void EmitPageCheck() {
        std::vector<const uint8_t *> entries;
        for (size_t i = 0; i < m_block.guestOpcodes.size(); ++i) {
            const uint8_t *entry = brimir::jit::PageEntry(m_bus, m_block.startPC + static_cast<uint32_t>(i * 2));
            if (std::find(entries.begin(), entries.end(), entry) != entries.end()) {
                continue;
            }
            entries.push_back(entry);
            const uint8_t *array = nullptr;
            std::memcpy(&array, entry + m_bus.arrayOffset, sizeof(array));
            x86::Gp e = m_cc.new_gp64();
            x86::Gp expected = m_cc.new_gp64();
            m_cc.mov(e, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry)));
            m_cc.mov(expected, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(array)));
            m_cc.cmp(x86::qword_ptr(e, static_cast<int32_t>(m_bus.arrayOffset)), expected);
            m_cc.jne(m_stale);
        }
    }

    // Self-validation, after EmitPageCheck: the code bytes in host memory (big-endian words, as on
    // the bus) still equal guestOpcodes. With the page check this is BlockCache::IsCurrent, which
    // reads the same arrays through FastPeek16. Compared 8 bytes at a time, then 4 and 2.
    void EmitCodeCheck() {
        std::vector<uint8_t> bytes;
        bytes.reserve(m_block.guestOpcodes.size() * 2);
        for (const uint16_t op : m_block.guestOpcodes) {
            bytes.push_back(static_cast<uint8_t>(op >> 8));
            bytes.push_back(static_cast<uint8_t>(op));
        }
        size_t pos = 0; // FindCodeHostRanges fills the ranges in word order
        for (uint32_t r = 0; r < m_ranges.count; ++r) {
            const auto length = static_cast<size_t>(m_ranges.hi[r] - m_ranges.lo[r]);
            x86::Gp base = m_cc.new_gp64();
            m_cc.mov(base, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_ranges.lo[r])));
            size_t o = 0;
            while (length - o >= 8) {
                uint64_t v = 0;
                std::memcpy(&v, bytes.data() + pos + o, 8);
                x86::Gp expected = m_cc.new_gp64();
                m_cc.mov(expected, v);
                m_cc.cmp(x86::qword_ptr(base, static_cast<int32_t>(o)), expected);
                m_cc.jne(m_stale);
                o += 8;
            }
            if (length - o >= 4) {
                uint32_t v = 0;
                std::memcpy(&v, bytes.data() + pos + o, 4);
                m_cc.cmp(x86::dword_ptr(base, static_cast<int32_t>(o)), Imm32(v));
                m_cc.jne(m_stale);
                o += 4;
            }
            if (length - o >= 2) {
                uint16_t v = 0;
                std::memcpy(&v, bytes.data() + pos + o, 2);
                m_cc.cmp(x86::word_ptr(base, static_cast<int32_t>(o)), Imm(static_cast<int16_t>(v)));
                m_cc.jne(m_stale);
                o += 2;
            }
            assert(o == length);
            pos += length;
        }
        assert(pos == bytes.size());
    }

    // Executor::Step's checks, repeated when this block is entered by chaining (they hold for the
    // executor's own entry, which already made them).
    void EmitChainGates() {
        m_delaySlotStub = m_cc.new_label();
        const Label notChained = m_cc.new_label();
        const Label noInterrupt = m_cc.new_label();
        m_cc.cmp(x86::byte_ptr(m_frame, kFrameChained), 0);
        m_cc.je(notChained);
        // Step: delay slot pending -> interpreter. No chainable exit leaves one pending.
        m_cc.cmp(State8(m_off.delaySlot), 0);
        m_cc.jne(m_delaySlotStub);
        // Step: interrupt pending and allowed -> interpreter (interrupt entry).
        m_cc.cmp(State8(m_off.intrPending), 0);
        m_cc.je(noInterrupt);
        m_cc.cmp(State8(m_off.intrAllow), 0);
        m_cc.jne(m_retNull);
        m_cc.bind(noInterrupt);
        if ((m_block.startPC & 2u) != 0) {
            // Step: at PC & 2 the fetch buffer's low half must be the opcode in memory, which the
            // code check has just shown to be guestOpcodes[0].
            m_cc.cmp(x86::word_ptr(m_regs, m_off.fetchedOpcodes), Imm(static_cast<int16_t>(m_block.guestOpcodes[0])));
            m_cc.jne(m_retNull);
        }
        // RunEntry: *intrAllow = true before the block.
        m_cc.mov(State8(m_off.intrAllow), 1);
        m_cc.bind(notChained);
    }

    void LowerRefill(const Inst &in) {
        if (!in.flag || !m_known) {
            FlushRegs(true);
            Call(&TrRefill, {U32(in.imm)});
            CheckStop();
            return;
        }
        const x86::Mem fetched = State32(m_off.fetchedOpcodes);
        if (!m_mayBeDirty) {
            m_cc.mov(fetched, Imm32(in.imm2)); // no access before this point can set codeDirty
            return;
        }
        const Label slow = m_cc.new_label();
        const Label done = m_cc.new_label();
        m_cc.cmp(x86::byte_ptr(m_frame, kFrameCodeDirty), 0);
        m_cc.jne(slow);
        m_cc.mov(fetched, Imm32(in.imm2));
        m_cc.jmp(done);
        m_cc.bind(slow);
        FlushRegs(false);
        Call(&TrRefill, {U32(in.imm)});
        CheckStop();
        m_cc.bind(done);
    }

    // Inline SH2::JitRefillPipeline for a constant address on an array page (an instruction fetch
    // there has no side effects); jumps to `slow` otherwise (the caller then calls TrRefill).
    void InlineRefillConst(uint32_t address, const Label &slow) {
        if (!m_inlineBus || !m_off.hasFetchedOpcodes || ((kBusPartitions >> (address >> 29)) & 1u) == 0) {
            m_cc.jmp(slow);
            return;
        }
        const uint32_t aligned = address & ~3u;
        const uint8_t *entry = brimir::jit::PageEntry(m_bus, aligned);
        x86::Gp e = m_cc.new_gp64();
        m_cc.mov(e, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry)));
        const x86::Gp array = PageArrayOrMiss(e, slow);
        const uint32_t pageMask = (1u << m_bus.pageShift) - 1;
        x86::Gp v = m_cc.new_gp32();
        m_cc.mov(v, x86::dword_ptr(array, static_cast<int32_t>(aligned & pageMask)));
        m_cc.bswap(v);
        m_cc.mov(State32(m_off.fetchedOpcodes), v);
    }

    void LowerSetupDelaySlot(const Inst &in) {
        if (!m_off.hasDelaySlot) {
            FlushRegs(true);
            Call(&TrSetupDelaySlot, {Use(in.a)});
            CheckStop();
            return;
        }
        // SH2::SetupDelaySlot
        m_cc.mov(State8(m_off.delaySlot), 1);
        m_cc.mov(State32(m_off.delaySlotTarget), Use(in.a));
        m_cc.mov(State8(m_off.intrPending), 0);
    }

    void LowerEndDelaySlot() {
        // Both forms read SR.ILevel from memory (the inline one below, AdvancePC in the callback).
        // The cache is stored and then dropped (the front end ends the block right after it).
        FlushRegs(true);
        LowerEndDelaySlotOp();
        InvalidateRegs();
    }

    void LowerEndDelaySlotOp() {
        if (!m_off.hasDelaySlot) {
            Call(&TrEndDelaySlot, {});
            CheckStop();
            return;
        }
        // SH2::AdvancePC<debug = false, emulateCache = false, delaySlot = true>:
        //   PC = target; if (PC & 2) refill from PC; delaySlot = false;
        //   intrPending = INTC.pending.level > SR.ILevel
        // The refill is inline on array pages; otherwise TrEndDelaySlot does the whole step.
        const Label slow = m_cc.new_label();
        const Label noRefill = m_cc.new_label();
        const Label done = m_cc.new_label();
        x86::Gp target = m_cc.new_gp32();
        m_cc.mov(target, State32(m_off.delaySlotTarget));
        m_cc.test(target, 2);
        m_cc.jz(noRefill);
        if (m_inlineBus) {
            JumpUnlessPartition(target, kBusPartitions, slow);
            const x86::Gp entry = PageEntry(target);
            const x86::Gp array = PageArrayOrMiss(entry, slow);
            const x86::Gp offset = PageOffset(target, 4);
            x86::Gp v = m_cc.new_gp32();
            m_cc.mov(v, x86::dword_ptr(array, offset));
            m_cc.bswap(v);
            m_cc.mov(State32(m_off.fetchedOpcodes), v);
        } else {
            m_cc.jmp(slow);
        }
        m_cc.bind(noRefill);
        m_cc.mov(State32(m_off.PC), target);
        m_cc.mov(State8(m_off.delaySlot), 0);
        x86::Gp level = m_cc.new_gp32();
        x86::Gp ilevel = m_cc.new_gp32();
        x86::Gp pending = m_cc.new_gp32();
        m_cc.movzx(level, State8(m_off.intcPendingLevel));
        m_cc.mov(ilevel, State32(m_off.SR));
        m_cc.shr(ilevel, 4);
        m_cc.and_(ilevel, 0xF);
        m_cc.xor_(pending, pending); // before cmp: xor changes the flags
        m_cc.cmp(level, ilevel);
        m_cc.seta(pending.r8());
        m_cc.mov(State8(m_off.intrPending), pending.r8());
        m_cc.jmp(done);
        m_cc.bind(slow);
        Call(&TrEndDelaySlot, {});
        CheckStop();
        m_cc.bind(done);
    }

    void LowerAccessCycles(const Inst &in) {
        if (!m_inlineBus) {
            const x86::Gp c = m_cc.new_gp64();
            FlushRegs(true);
            Call(&TrAccessCycles, {Use(in.a), U32(in.size), U32(in.flag ? 1 : 0)}, &c);
            CheckStop();
            m_cc.add(m_cycles, c);
            return;
        }
        // SH2::AccessCycles<T, write, emulateCache = false>, no call.
        const x86::Gp &address = Use(in.a);
        const Label notBus = m_cc.new_label();
        const Label io = m_cc.new_label();
        const Label done = m_cc.new_label();
        x86::Gp part = m_cc.new_gp32();
        x86::Gp mask = m_cc.new_gp32();
        m_cc.mov(part, address);
        m_cc.shr(part, 29);
        m_cc.mov(mask, Imm32(kBusCyclePartitions));
        m_cc.bt(mask, part);
        m_cc.jnc(notBus);
        const x86::Gp entry = PageEntry(address);
        const uint32_t field =
            in.flag ? m_bus.writeCyclesOffset[SizeIndex(in.size)] : m_bus.readCyclesOffset[SizeIndex(in.size)];
        m_cc.add(m_cycles, x86::qword_ptr(entry, static_cast<int32_t>(field)));
        m_cc.jmp(done);
        m_cc.bind(notBus);
        m_cc.cmp(part, 0b111);
        m_cc.je(io);
        m_cc.add(m_cycles, 1); // cached area (always a hit), purge, cache arrays
        m_cc.jmp(done);
        m_cc.bind(io);
        m_cc.add(m_cycles, 4); // I/O area
        m_cc.bind(done);
    }

    void LowerExitIfBusWait(const Inst &in) {
        // If the bus is busy: PC = imm, out.retired, out.busWait, out.cycles, return.
        const Label cont = m_cc.new_label();
        if (m_inlineBus) {
            // SH2Bus::IsBusWait is false on array pages (any partition).
            const x86::Gp entry = PageEntry(Use(in.a));
            m_cc.cmp(x86::qword_ptr(entry, static_cast<int32_t>(m_bus.arrayOffset)), 0);
            m_cc.jne(cont);
        }
        const x86::Gp wait = m_cc.new_gp32();
        FlushRegs(!m_inlineBus); // the exit below is only reached from here
        Call(&TrBusWait, {Use(in.a), U32(in.size), U32(in.flag ? 1 : 0)}, &wait);
        CheckStop();
        m_cc.test(wait, wait);
        m_cc.jz(cont);
        m_cc.mov(x86::byte_ptr(m_frame, kOutBusWait), 1);
        WriteExit(true, in.imm, in.retired, m_cycles, false);
        m_cc.jmp(m_retNull);
        m_cc.bind(cont);
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
        // R0-R15 and SR go through the register cache; the IR value is the slot's register.
        case Op::GetReg: m_values[in.dst] = CachedGet(in.imm); break;
        case Op::SetReg: CachedSet(in.imm, Use(in.a)); break;
        case Op::GetPR: m_cc.mov(Def(in.dst), State32(m_off.PR)); break;
        case Op::SetPR: m_cc.mov(State32(m_off.PR), Use(in.a)); break;
        case Op::GetT: {
            const x86::Gp d = Def(in.dst);
            m_cc.mov(d, CachedGet(kSlotSR));
            m_cc.and_(d, 1);
            break;
        }
        case Op::SetT: {
            // SR = (SR & ~1) | (a != 0)
            x86::Gp t = m_cc.new_gp32();
            x86::Gp sr = m_cc.new_gp32();
            m_cc.xor_(t, t);
            m_cc.test(Use(in.a), Use(in.a));
            m_cc.setnz(t.r8());
            m_cc.mov(sr, CachedGet(kSlotSR));
            m_cc.and_(sr, Imm32(~1u));
            m_cc.or_(sr, t);
            CachedSet(kSlotSR, sr);
            break;
        }
        case Op::GetGBR: m_cc.mov(Def(in.dst), State32(m_off.GBR)); break;
        case Op::SetGBR: m_cc.mov(State32(m_off.GBR), Use(in.a)); break;
        case Op::GetVBR: m_cc.mov(Def(in.dst), State32(m_off.VBR)); break;
        case Op::SetVBR: m_cc.mov(State32(m_off.VBR), Use(in.a)); break;
        case Op::GetSR: m_values[in.dst] = CachedGet(kSlotSR); break;
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
            // hi and lo are fixed to EDX/EAX: compute into temporaries (see Call).
            x86::Gp hi = m_cc.new_gp32();
            x86::Gp lo = m_cc.new_gp32();
            m_cc.mov(lo, Use(in.a));
            if (in.op == Op::MulHiS) {
                m_cc.imul(hi, lo, Use(in.b));
            } else {
                m_cc.mul(hi, lo, Use(in.b));
            }
            m_cc.mov(Def(in.dst), hi);
            break;
        }
        case Op::SetSRBits: {
            // SR = (SR & ~mask) | (a & mask)
            x86::Gp t = m_cc.new_gp32();
            x86::Gp sr = m_cc.new_gp32();
            m_cc.mov(t, Use(in.a));
            m_cc.and_(t, Imm32(in.imm));
            m_cc.mov(sr, CachedGet(kSlotSR));
            m_cc.and_(sr, Imm32(~in.imm));
            m_cc.or_(sr, t);
            CachedSet(kSlotSR, sr);
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
            // Stop if cycles >= limit, or an interrupt is pending and allowed. A cycles-only check
            // (ir_opt.hpp) skips the interrupt test; one that relies on inline known refills tests
            // it anyway when this block's known refills call TrRefill (!m_known).
            // Write-back at each guest-instruction boundary, on the main path (cache rules above):
            // the out-of-line stub then needs no stores of its own.
            FlushRegs(true);
            BoundaryStub stub{m_cc.new_label(), in.imm, in.retired};
            m_cc.cmp(m_cycles, x86::qword_ptr(m_frame, kFrameLimit));
            m_cc.jae(stub.label);
            const bool testInterrupt = !in.flag || ((in.imm2 & kCheckNeedsInlineRefills) != 0 && !m_known);
            if (testInterrupt) {
                const Label cont = m_cc.new_label();
                m_cc.cmp(State8(m_off.intrPending), 0);
                m_cc.je(cont);
                m_cc.cmp(State8(m_off.intrAllow), 0);
                m_cc.jne(stub.label);
                m_cc.bind(cont);
            }
            m_stubs.push_back(stub);
            break;
        }
        case Op::ExitIf: {
            // Taken: cycles += imm2, refill from imm (if flag; abort exit on stop), PC = imm, return.
            const Label notTaken = m_cc.new_label();
            m_cc.test(Use(in.a), Use(in.a));
            m_cc.jz(notTaken);
            // Taken path only: the fall-through cache stays as it is.
            StoreDirty(DirtySet());
            x86::Gp taken = m_cc.new_gp64();
            m_cc.mov(taken, m_cycles);
            AddU32(taken, in.imm2);
            if (in.flag) {
                // Inline on an array page; otherwise the trampoline (abort exit on stop).
                const Label slow = m_cc.new_label();
                const Label go = m_cc.new_label();
                InlineRefillConst(in.imm, slow);
                m_cc.jmp(go);
                m_cc.bind(slow);
                Call(&TrRefill, {U32(in.imm)});
                m_cc.cmp(x86::byte_ptr(m_frame, kFrameStop), 0);
                m_cc.je(go);
                AbortExit(taken);
                m_cc.bind(go);
            }
            WriteExit(true, in.imm, in.retired, taken, false);
            ChainExit(true, in.imm, taken);
            m_cc.bind(notTaken);
            break;
        }
        case Op::Exit:
            FlushRegs(true);
            WriteExit(true, in.imm, in.retired, m_cycles, false);
            ChainExit(true, in.imm, m_cycles);
            break;
        case Op::ExitDynamic:
            FlushRegs(true);
            WriteExit(false, 0, in.retired, m_cycles, false);
            ChainExit(false, 0, m_cycles);
            break;

        // Calls out of generated code (trampolines, see x64_emitter.hpp).
        // Load/Store/AddAccessCycles/ExitIfBusWait are inline on array pages (bus fast path), with
        // the trampoline as the fallback inside the same op.
        case Op::Load: LowerLoad(in); break;
        case Op::Store: LowerStore(in); break;
        case Op::Refill: LowerRefill(in); break;
        case Op::AddAccessCycles: LowerAccessCycles(in); break;
        case Op::AddAccessCyclesRMWByte: {
            const x86::Gp c = m_cc.new_gp64();
            FlushRegs(true);
            Call(&TrAccessCyclesRMWByte, {Use(in.a)}, &c);
            CheckStop();
            m_cc.add(m_cycles, c);
            break;
        }
        case Op::ExitIfBusWait: LowerExitIfBusWait(in); break;
        case Op::SetupDelaySlot: LowerSetupDelaySlot(in); break;
        case Op::EndDelaySlot: LowerEndDelaySlot(); break;
        case Op::SetSR:
            FlushRegs(true);
            Call(&TrSetSR, {Use(in.a), U32(in.flag ? 1 : 0)});
            CheckStop();
            InvalidateRegs(true); // the callback wrote SR
            break;
        case Op::Div1: {
            // Div1Step reads and writes SR (M, Q, T) through ctx; it reads nothing else.
            const x86::Gp d = Def(in.dst);
            FlushRegs(true, true);
            Call(&TrDiv1, {Use(in.a), Use(in.b), U32(in.flag ? 1 : 0)}, &d);
            InvalidateRegs(true);
            break;
        }
        // MacStep reads SR.S and MACH/MACL through ctx and writes only MACH/MACL.
        case Op::MacW:
            FlushRegs(true, true);
            Call(&TrMacW, {Use(in.a), Use(in.b)});
            break;
        case Op::MacL:
            FlushRegs(true, true);
            Call(&TrMacL, {Use(in.a), Use(in.b)});
            break;
        }
    }

    x86::Compiler &m_cc;
    const Block &m_block;
    const StateOffsets &m_off;
    const ymir::sh2::SH2JitBusLayout &m_bus;
    bool m_inlineBus; // inline RAM/ROM accesses and access cycles (CanInlineBus)
    const X64LinkSlot *m_links; // the backend's link table (nullptr: never chain)
    std::vector<x86::Gp> m_values; // one 32-bit virtual register per IR value
    std::vector<BoundaryStub> m_stubs;
    std::array<CachedSlot, kSlots> m_cache{}; // R0-R15, SR (register cache rules above)
    x86::Gp m_frame;
    x86::Gp m_regs;   // ctx->R
    x86::Gp m_cycles; // cycles accumulated by this block (ExitInfo::cycles)
    Label m_abort;    // shared abort exit, created by the first CheckStop
    bool m_abortUsed = false;
    bool m_known = false;              // known refills store their value (entry check emitted)
    ptrdiff_t m_lastKnownRefill = -1;  // index of the last known Refill in block.code (m_known)
    CodeHostRanges m_ranges;           // the block's code in host memory (m_selfValidating or m_known)
    bool m_trackDirty = false;         // the op being lowered must classify its accesses for codeDirty
    bool m_mayBeDirty = false;         // an access emitted so far can set codeDirty
    bool m_selfValidating = false;     // prologue validates the code and gates chained entries
    Label m_stale;                     // stale entry exit (m_selfValidating or m_known)
    Label m_delaySlotStub;             // chained entry with a pending delay slot (m_selfValidating)
    Label m_retNull;                   // return nullptr: every exit that does not chain
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

bool EmitBlock(x86::Compiler &cc, const Block &block, const ymir::sh2::SH2JitContext &ctx,
               const X64LinkSlot *links, bool &selfValidating) {
    selfValidating = false;
    StateOffsets off{};
    if (!CanEmitBlock(block) || !ComputeOffsets(ctx, off)) {
        return false;
    }
    Emitter emitter(cc, block, off, ctx.bus, links);
    emitter.Emit();
    selfValidating = emitter.SelfValidating();
    return true;
}

} // namespace brimir::jit
