#include <brimir/jit/ir_opt.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace brimir::jit {

// Audits behind the rules (ir_opt.hpp), against src/core/src/ymir/hw/sh2/sh2.cpp:
//
// m_wbReg writers. Besides the instruction handlers (which compiled code replaces by SetWb) only
// SH2::Reset (m_wbReg = kWBRegNone) and LoadState write it. The callbacks a block can run:
//   - setupDelaySlot = SH2::SetupDelaySlot: delay-slot flag, target, intrPending only;
//   - endDelaySlot = SH2::AdvancePC<..., delaySlot = true>: PC, refill, delay-slot flag, intrPending;
//   - setSR = SH2::JitSetSR: SR and the interrupt flags;
//   - read/write/refillPipeline/accessCycles/accessCyclesRMWByte/busWait: MemRead/MemWrite,
//     AccessCycles, AccessCyclesRMWByte, SH2Bus::IsBusWait. They never write m_wbReg themselves; an
//     on-chip register access can reach SH2::Reset through AdvanceWDT (watchdog reset), and Reset
//     flushes the executor, which aborts the block right after that read or write (RunBlock and
//     the x64 trampolines stop on the abort flag), so no later op of the block runs.
//   - interpretOne is never called inside a block.
// So no op but SetWb changes the value while the block runs.
//
// *intrPending / *intrAllow writers. intrPending: RecalcInterrupts (from Raise/LowerInterrupt,
// reached by on-chip register accesses, timers synced on access, and bus devices through
// SetExternalInterrupt), SH2::SetupDelaySlot (false), AdvancePC in a delay slot, JitSetSR,
// interrupt entry and Reset. intrAllow: the instruction handlers (ClearIntrAllow / SetIntrAllow),
// JitSetSR (false), InterpretNext (not in a block) and Reset. The x64 backend's inline
// SetupDelaySlot/EndDelaySlot store the same values. Div1/MacW/MacL (inline Div1, TrMacW/TrMacL in
// x64) are pure helpers on their operands, SR and MAC.

namespace {

enum class IntrEffect {
    None,         // cannot change the interrupt state
    Unknown,      // may change it
    AllowFalse,   // intrAllow = false (ClearIntrAllow)
    PendingFalse, // intrPending = false (SetupDelaySlot)
    AllowTrue,    // intrAllow = true, intrPending unchanged (SetIntrAllow)
    InlineRefill, // a known refill both backends store inline unless the code left its arrays
};

IntrEffect IntrEffectOf(const Block &block, const Inst &in, bool dataAccessSeen) {
    switch (in.op) {
    case Op::ClearIntrAllow: return IntrEffect::AllowFalse;
    case Op::SetIntrAllow: return IntrEffect::AllowTrue;
    case Op::SetupDelaySlot: return IntrEffect::PendingFalse;
    case Op::Refill:
        // Known refills fall back to the callback when the code left its array pages (RunBlock's
        // knownUsable, the x64 entry check) or after a data access set codeDirty; only Load and
        // Store set codeDirty.
        return in.flag && block.fetchFromArrays && !dataAccessSeen ? IntrEffect::InlineRefill : IntrEffect::Unknown;
    case Op::Load:
    case Op::Store:
    case Op::AddAccessCycles:
    case Op::AddAccessCyclesRMWByte:
    case Op::ExitIfBusWait:
    case Op::SetSR:
    case Op::EndDelaySlot: return IntrEffect::Unknown;
    default: return IntrEffect::None;
    }
}

// Ops that may call out of compiled code (in either backend), plus checks, syncs and exits:
// cycles never move across them.
bool IsCycleBarrier(Op op) {
    switch (op) {
    case Op::CheckBoundary:
    case Op::SyncCycles:
    case Op::ExitIf:
    case Op::ExitIfBusWait:
    case Op::Exit:
    case Op::ExitDynamic:
    case Op::Load:
    case Op::Store:
    case Op::Refill:
    case Op::AddAccessCycles:
    case Op::AddAccessCyclesRMWByte:
    case Op::SetSR:
    case Op::SetupDelaySlot:
    case Op::EndDelaySlot:
    case Op::Div1:
    case Op::MacW:
    case Op::MacL: return true;
    default: return false;
    }
}

bool AddsCycles(Op op) {
    return op == Op::AddCycles || op == Op::AddAccessCycles || op == Op::WbStall || op == Op::AddAccessCyclesRMWByte;
}

// Rule 1.
void FoldWriteBack(std::vector<Inst> &code) {
    std::vector<Inst> out;
    out.reserve(code.size());
    bool known = false;
    uint32_t wb = 0;
    for (const Inst &in : code) {
        if (in.op == Op::SetWb) {
            known = true;
            wb = in.imm;
        } else if (in.op == Op::WbStall && known) {
            if (wb <= 16 && ((in.imm >> wb) & 1u) != 0) {
                Inst add;
                add.op = Op::AddCycles;
                add.imm = 1;
                out.push_back(add);
            }
            continue;
        }
        out.push_back(in);
    }
    code = std::move(out);
}

// Rule 3.
void MergeCycles(std::vector<Inst> &code) {
    std::vector<Inst> out;
    out.reserve(code.size());
    ptrdiff_t open = -1; // index in `out` of the AddCycles later ones merge into
    for (const Inst &in : code) {
        if (in.op == Op::AddCycles) {
            if (in.imm == 0) {
                continue;
            }
            if (open >= 0 && out[open].imm <= UINT32_MAX - in.imm) {
                out[open].imm += in.imm;
                continue;
            }
            open = static_cast<ptrdiff_t>(out.size());
        } else if (IsCycleBarrier(in.op)) {
            open = -1;
        }
        out.push_back(in);
    }
    code = std::move(out);
}

// Rule 4.
void DropRedundantSyncs(std::vector<Inst> &code) {
    std::vector<Inst> out;
    out.reserve(code.size());
    bool synced = true; // at entry *cyclesExecuted == entryCycles
    for (const Inst &in : code) {
        if (in.op == Op::SyncCycles) {
            if (synced) {
                continue;
            }
            synced = true;
        } else if (AddsCycles(in.op)) {
            synced = false;
        }
        out.push_back(in);
    }
    code = std::move(out);
}

// Rule 2. Facts about the interrupt test, each with "relies on inline known refills".
void ElideInterruptTests(const Block &block, std::vector<Inst> &code) {
    struct Fact {
        bool known = false;
        bool needsInline = false;
    };
    Fact passed;       // (pending && allow) was false at a passed check
    Fact allowFalse;   // intrAllow is false
    Fact pendingFalse; // intrPending is false
    bool dataAccessSeen = false;
    for (Inst &in : code) {
        if (in.op == Op::CheckBoundary) {
            bool elide = false;
            bool needsInline = true;
            for (const Fact *fact : {&passed, &allowFalse, &pendingFalse}) {
                if (fact->known) {
                    elide = true;
                    needsInline = needsInline && fact->needsInline;
                }
            }
            in.flag = elide;
            in.imm2 = elide && needsInline ? kCheckNeedsInlineRefills : 0u;
            // Past this check the test is false; a cycles-only check that relies on inline
            // refills makes this fact rely on them too.
            passed = {true, elide && needsInline};
            continue;
        }
        switch (IntrEffectOf(block, in, dataAccessSeen)) {
        case IntrEffect::None: break;
        case IntrEffect::Unknown: passed = allowFalse = pendingFalse = {}; break;
        case IntrEffect::AllowFalse: allowFalse = {true, false}; break; // pending unchanged
        case IntrEffect::PendingFalse: pendingFalse = {true, false}; break; // allow unchanged
        case IntrEffect::AllowTrue: passed = allowFalse = {}; break;        // pending unchanged
        case IntrEffect::InlineRefill:
            // No change unless the refill calls back, which the backends rule out at run time for
            // the checks that rely on it.
            for (Fact *fact : {&passed, &allowFalse, &pendingFalse}) {
                fact->needsInline = fact->needsInline || fact->known;
            }
            break;
        }
        if (in.op == Op::Load || in.op == Op::Store) {
            dataAccessSeen = true;
        }
    }
}

// Rule 5: ops allowed between the first and the last check of a guarded run.
bool AllowedInRun(const Block &block, const Inst &in, bool dataAccessSeen) {
    switch (in.op) {
    case Op::SyncCycles:
    case Op::ExitIf:
    case Op::Exit:
    case Op::ExitDynamic:
    case Op::GuardRun: return false;
    default: break;
    }
    const IntrEffect effect = IntrEffectOf(block, in, dataAccessSeen);
    return effect == IntrEffect::None || effect == IntrEffect::InlineRefill;
}

// Rule 5.
void GuardRuns(const Block &block, std::vector<Inst> &code) {
    struct Run {
        size_t first = 0;     // index in code of the first check
        uint32_t checks = 0;  // 0: no open run
        uint64_t cycles = 0;  // from the first check to the last one so far
        uint64_t pending = 0; // since the last check
        bool needsInline = false;
    };
    std::vector<std::pair<size_t, Inst>> guards; // inserted before code[first]
    Run run;
    const auto close = [&] {
        if (run.checks >= 2) {
            Inst guard;
            guard.op = Op::GuardRun;
            guard.imm = static_cast<uint32_t>(run.cycles);
            guard.imm2 = run.checks;
            guard.flag = run.needsInline;
            guards.emplace_back(run.first, guard);
        }
        run = {};
    };
    bool dataAccessSeen = false;
    for (size_t i = 0; i < code.size(); ++i) {
        const Inst &in = code[i];
        if (in.op == Op::CheckBoundary) {
            if (!in.flag) {
                close();
                continue;
            }
            const bool needsInline = (in.imm2 & kCheckNeedsInlineRefills) != 0;
            if (run.checks > 0) {
                const uint64_t cycles = run.cycles + run.pending;
                if (run.checks < kMaxGuardedChecks && cycles <= UINT32_MAX) {
                    ++run.checks;
                    run.cycles = cycles;
                    run.pending = 0;
                    run.needsInline = run.needsInline || needsInline;
                    continue;
                }
                close();
            }
            run.first = i;
            run.checks = 1;
            run.needsInline = needsInline;
            continue;
        }
        if (run.checks > 0) {
            if (in.op == Op::AddCycles) {
                run.pending += in.imm;
            } else if (in.op == Op::WbStall) {
                run.pending += 1; // at most one cycle
            } else if (!AllowedInRun(block, in, dataAccessSeen)) {
                close();
            }
        }
        if (in.op == Op::Load || in.op == Op::Store) {
            dataAccessSeen = true;
        }
    }
    close();
    if (guards.empty()) {
        return;
    }
    std::vector<Inst> out;
    out.reserve(code.size() + guards.size());
    size_t next = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        if (next < guards.size() && guards[next].first == i) {
            out.push_back(guards[next].second);
            ++next;
        }
        out.push_back(code[i]);
    }
    code = std::move(out);
}

} // namespace

void OptimizeBlock(Block &block) {
    if (block.guestInstrCount == 0) {
        return;
    }
    FoldWriteBack(block.code);
    MergeCycles(block.code);
    DropRedundantSyncs(block.code);
    ElideInterruptTests(block, block.code);
    GuardRuns(block, block.code);
}

} // namespace brimir::jit
