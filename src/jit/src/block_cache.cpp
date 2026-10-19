#include <brimir/jit/block_cache.hpp>

#include <brimir/jit/bus_fast_path.hpp>
#include <brimir/jit/frontend.hpp>
#include <brimir/jit/ir_opt.hpp>

#include <algorithm>
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

std::array<const uint8_t *, 2> BlockCache::CodePages(const Block &block, const ymir::sh2::SH2JitContext &ctx) {
    assert(ctx.bus.pages != nullptr && !block.guestOpcodes.empty());
    const uint32_t last = block.startPC + static_cast<uint32_t>((block.guestOpcodes.size() - 1) * 2);
#ifndef NDEBUG
    // The prologue checks every page of the code; a block spans at most these two.
    for (size_t i = 0; i < block.guestOpcodes.size(); ++i) {
        const uint8_t *entry = PageEntry(ctx.bus, block.startPC + static_cast<uint32_t>(i * 2));
        assert(entry == PageEntry(ctx.bus, block.startPC) || entry == PageEntry(ctx.bus, last));
    }
#endif
    return {PageArrayAt(ctx.bus, block.startPC), PageArrayAt(ctx.bus, last)};
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
    // Order matters: no link slot or recent slot may point to the code once it is freed or retired.
    const uint32_t pc = it->first;
    std::unique_ptr<CachedBlock> entry = std::move(it->second);
    m_blocks.erase(it);
    RecentSlot &slot = SlotFor(pc);
    if (slot.entry == entry.get()) {
        slot = RecentSlot{};
    }
    ++m_invalidations;
    m_totalInsts -= entry->block.code.size();
    const NativeCode &code = entry->code;
    if (code.selfValidating) {
        m_native->Unpublish(pc);
    }
    if (code.entry == nullptr) {
        return;
    }
    m_staleNativeBytes += code.bytes;
    if (code.selfValidating && m_maxRetiredCodeBytes > 0) {
        Retire(pc, std::move(entry));
        return;
    }
    m_native->Release(pc, code);
    m_releasedNativeBytes += code.bytes;
}

void BlockCache::Retire(uint32_t pc, std::unique_ptr<CachedBlock> entry) {
    m_retiredBytes += entry->code.bytes;
    std::vector<RetiredList::iterator> &atPc = m_retiredByPc[pc];
    atPc.push_back(m_retired.insert(m_retired.end(), RetiredBlock{pc, std::move(entry)}));
    if (atPc.size() > kMaxRetiredPerPc) {
        ++m_retiredEvictions;
        ReleaseRetired(atPc.front()); // atPc keeps kMaxRetiredPerPc blocks
    }
    while (m_retiredBytes > m_maxRetiredCodeBytes) {
        ++m_retiredEvictions;
        ReleaseRetired(m_retired.begin());
    }
}

CachedBlock *BlockCache::Reuse(ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
    const auto byPc = m_retiredByPc.find(pc);
    if (byPc == m_retiredByPc.end()) {
        return nullptr;
    }
    // Same opcodes on the same arrays: the prologue would pass, exactly as for a fresh compile.
    for (auto r = byPc->second.rbegin(); r != byPc->second.rend(); ++r) {
        const RetiredList::iterator it = *r;
        const CachedBlock &candidate = *it->entry;
        if (candidate.codePages != CodePages(candidate.block, ctx) || !IsCurrent(candidate.block, ctx)) {
            continue;
        }
        std::unique_ptr<CachedBlock> entry = std::move(it->entry);
        m_retiredBytes -= entry->code.bytes;
        RemoveRetired(it);
        ++m_retiredReuses;
        CachedBlock *ref = entry.get();
        m_blocks.emplace(pc, std::move(entry));
        SlotFor(pc) = RecentSlot{pc, ref};
        m_native->Publish(pc, ref->code);
        return ref;
    }
    return nullptr;
}

void BlockCache::RemoveRetired(RetiredList::iterator it) {
    const auto byPc = m_retiredByPc.find(it->pc);
    assert(byPc != m_retiredByPc.end());
    std::vector<RetiredList::iterator> &atPc = byPc->second;
    atPc.erase(std::find(atPc.begin(), atPc.end(), it));
    if (atPc.empty()) {
        m_retiredByPc.erase(byPc);
    }
    m_retired.erase(it);
}

void BlockCache::ReleaseRetired(RetiredList::iterator it) {
    const NativeCode &code = it->entry->code;
    m_native->Release(it->pc, code);
    m_releasedNativeBytes += code.bytes;
    m_retiredBytes -= code.bytes;
    RemoveRetired(it);
}

void BlockCache::ReleaseAllRetired() {
    for (RetiredBlock &retired : m_retired) {
        m_native->Release(retired.pc, retired.entry->code);
        m_releasedNativeBytes += retired.entry->code.bytes;
    }
    m_retired.clear();
    m_retiredByPc.clear();
    m_retiredBytes = 0;
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
    if (CachedBlock *reused = Reuse(ctx, pc); reused != nullptr) {
        return *reused;
    }
    const bool native = m_native != nullptr && (compileNow || m_nativeCompileThreshold <= 1);
    if (m_totalInsts >= m_maxCachedInsts) {
        EvictIrOnly();
    }
    if (native && AtCodeCap()) {
        // Checked before building when this block is compiled at once, so it is built only once.
        ReleaseAllRetired();
        if (AtCodeCap()) {
            ++m_flushesCodeCap;
            FlushAll();
        }
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
    if (AtCodeCap()) {
        // Retired code goes first; entry is in the map, never in the pool, so it stays valid.
        ReleaseAllRetired();
    }
    if (AtCodeCap()) {
        // Drops entry too; the block is rebuilt from the current guest code and compiled at once
        // (the code cache is empty now, so this does not flush again).
        ++m_flushesCodeCap;
        FlushAll();
        return Build(ctx, pc, true);
    }

    const auto t0 = Clock::now();
    const bool ok = m_native->Compile(entry.block, ctx, entry.code);
    m_nativeNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    m_peakNativeBytes = std::max(m_peakNativeBytes, m_native->CodeBytes());
    if (!ok) {
        entry.code = NativeCode{}; // stays IR-only and runs with RunBlock; never retried
        ++m_compileFallbacks;
        return entry;
    }
    ++m_nativeCompiles;
    m_nativeBytes += entry.code.bytes;
    // The native code never needs the IR again (see CachedBlock): free it and stop counting it.
    m_totalInsts -= entry.block.code.size();
    std::vector<Inst>().swap(entry.block.code);
    if (entry.code.selfValidating) {
        entry.codePages = CodePages(entry.block, ctx);
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
    m_retired.clear(); // the backend reset frees their code
    m_retiredByPc.clear();
    m_retiredBytes = 0;
    m_totalInsts = 0;
    if (m_native != nullptr) {
        m_native->Reset();
    }
}

} // namespace brimir::jit
