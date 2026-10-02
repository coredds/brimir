#include <brimir/jit/block_cache.hpp>

#include <brimir/jit/bus_fast_path.hpp>
#include <brimir/jit/frontend.hpp>
#include <brimir/jit/ir_opt.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace brimir::jit {

bool BlockCache::IsCurrent(const Block &block, ymir::sh2::SH2JitContext &ctx) {
    for (size_t i = 0; i < block.guestOpcodes.size(); ++i) {
        const uint32_t address = block.startPC + static_cast<uint32_t>(i * 2);
        if (PeekOpcode(ctx, address) != block.guestOpcodes[i]) {
            return false;
        }
    }
    return true;
}

bool BlockCache::Validate(const CachedBlock &entry, uint32_t pc, ymir::sh2::SH2JitContext &ctx) {
    if (entry.code.selfValidating) {
        // Its prologue checks the guest code. Re-publishing restores a link slot that another PC
        // took over (the table is direct-mapped).
        m_native->Publish(pc, entry.code);
        return true;
    }
    return IsCurrent(entry.block, ctx);
}

void BlockCache::Invalidate(BlockMap::iterator it) {
    // Its native code (if any) stays allocated until the next Flush.
    RecentSlot &slot = SlotFor(it->first);
    if (slot.entry == it->second.get()) {
        slot = RecentSlot{};
    }
    if (it->second->code.selfValidating) {
        m_native->Unpublish(it->first);
    }
    ++m_invalidations;
    m_totalInsts -= it->second->block.code.size();
    m_blocks.erase(it);
}

void BlockCache::Invalidate(uint32_t pc) {
    if (auto it = m_blocks.find(pc); it != m_blocks.end()) {
        Invalidate(it);
    }
}

const CachedBlock &BlockCache::Get(ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
    RecentSlot &slot = SlotFor(pc);
    if (slot.entry != nullptr && slot.pc == pc && Validate(*slot.entry, pc, ctx)) {
        return Hit(*slot.entry, ctx, pc);
    }
    if (auto it = m_blocks.find(pc); it != m_blocks.end()) {
        if (slot.entry != it->second.get() && Validate(*it->second, pc, ctx)) {
            slot = RecentSlot{pc, it->second.get()};
            return Hit(*it->second, ctx, pc);
        }
        Invalidate(it);
    }
    return Build(ctx, pc, false);
}

const CachedBlock &BlockCache::Build(ymir::sh2::SH2JitContext &ctx, uint32_t pc, bool compileNow) {
    const bool native = m_native != nullptr && (compileNow || m_nativeCompileThreshold <= 1);
    if (m_totalInsts >= m_maxCachedInsts) {
        EvictIrOnly();
    }
    if (native && m_native->CodeBytes() >= m_maxNativeCodeBytes && m_native->CodeBytes() > 0) {
        // Checked before building when this block is compiled at once, so it is built only once.
        ++m_flushesCodeCap;
        FlushAll();
    }

    const auto t0 = Clock::now();
    auto entry = std::make_unique<CachedBlock>();
    entry->block = BuildBlock(ctx, pc);
    Block &block = entry->block;
    OptimizeBlock(block); // both backends run the optimized block; guestOpcodes are unchanged
    if (!VerifyBlock(block).empty()) {
        // Never run a malformed block: fall back to the interpreter for this PC.
        assert(false && "front end produced an invalid block");
        const uint16_t opcode = ctx.peekInstruction(ctx.sh2, pc);
        block = Block{};
        block.startPC = pc;
        block.guestOpcodes.push_back(opcode);
    }
    m_buildNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    ++m_compiles;
    m_totalInsts += block.code.size();
    CachedBlock *ref = entry.get();
    m_blocks.emplace(pc, std::move(entry));
    SlotFor(pc) = RecentSlot{pc, ref};
    if (m_native == nullptr || ref->block.guestInstrCount == 0) {
        return *ref; // RunBlock (or the interpreter) only
    }
    if (native) {
        return CompileNative(*ref, ctx, pc);
    }
    // This run is the first; the threshold run compiles it.
    ref->runsUntilNative = m_nativeCompileThreshold - 1;
    return *ref;
}

const CachedBlock &BlockCache::CompileNative(CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
    assert(m_native != nullptr && entry.code.entry == nullptr && !entry.block.code.empty());
    entry.runsUntilNative = 0;
    const size_t bytesBefore = m_native->CodeBytes();
    if (bytesBefore >= m_maxNativeCodeBytes && bytesBefore > 0) {
        // Drops entry too; the block is rebuilt from the current guest code and compiled at once
        // (the code cache is empty now, so this does not flush again).
        ++m_flushesCodeCap;
        FlushAll();
        return Build(ctx, pc, true);
    }

    const auto t0 = Clock::now();
    const bool ok = m_native->Compile(entry.block, ctx, entry.code);
    m_nativeNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    if (!ok) {
        entry.code = NativeCode{}; // stays IR-only and runs with RunBlock; never retried
        ++m_compileFallbacks;
        return entry;
    }
    ++m_nativeCompiles;
    m_nativeBytes += m_native->CodeBytes() - bytesBefore;
    // The native code never needs the IR again (see CachedBlock): free it and stop counting it.
    m_totalInsts -= entry.block.code.size();
    std::vector<Inst>().swap(entry.block.code);
    if (entry.code.selfValidating) {
        m_native->Publish(pc, entry.code);
    }
    return entry;
}

void BlockCache::EvictIrOnly() {
    ++m_irEvictions;
    for (auto it = m_blocks.begin(); it != m_blocks.end();) {
        CachedBlock *entry = it->second.get();
        if (entry->code.entry != nullptr) {
            ++it; // native: keeps its code, link slot and recent slot
            continue;
        }
        // IR-only: never published, so no link slot to clear.
        RecentSlot &slot = SlotFor(it->first);
        if (slot.entry == entry) {
            slot = RecentSlot{};
        }
        m_totalInsts -= entry->block.code.size();
        ++m_irEvictedBlocks;
        it = m_blocks.erase(it);
    }
    assert(m_totalInsts == 0 && "every counted IR instruction belongs to an IR-only block");
}

void BlockCache::Flush() {
    ++m_flushesRequested;
    FlushAll();
}

void BlockCache::FlushAll() {
    m_recent.fill(RecentSlot{});
    m_blocks.clear();
    m_totalInsts = 0;
    if (m_native != nullptr) {
        m_native->Reset();
    }
}

} // namespace brimir::jit
