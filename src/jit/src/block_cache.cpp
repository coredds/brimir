#include <brimir/jit/block_cache.hpp>

#include <brimir/jit/bus_fast_path.hpp>
#include <brimir/jit/frontend.hpp>
#include <brimir/jit/ir_opt.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
    ++m_version;
    const uint32_t pc = it->first;
    std::unique_ptr<CachedBlock> entry = std::move(it->second);
    m_blocks.erase(it);
    RecentSlot &slot = SlotFor(pc);
    if (slot.entry == entry.get()) {
        slot = RecentSlot{};
    }
    ++m_invalidations;
    if (m_maxStaleDampShift > 0) {
        if (uint8_t &stale = m_staleCounts[pc]; stale < UINT8_MAX) {
            ++stale;
        }
    }
    const NativeCode &code = entry->code;
    if (code.selfValidating) {
        m_native->Unpublish(pc);
    }
    if (code.entry == nullptr) {
        if (!entry->block.code.empty()) {
            RetireIr(pc, std::move(entry)); // its IR stays counted
        }
        return;
    }
    assert(entry->block.code.empty() && "a native block's IR is freed when it is installed");
    m_staleNativeBytes += code.bytes;
    if (code.selfValidating && m_maxRetiredCodeBytes > 0) {
        Retire(pc, std::move(entry));
        return;
    }
    m_native->Release(pc, code);
    m_releasedNativeBytes += code.bytes;
}

void BlockCache::Retire(uint32_t pc, std::unique_ptr<CachedBlock> entry) {
    ++m_version;
    m_retiredBytes += entry->code.bytes;
    {
        std::vector<RetiredList::iterator> &atPc = m_retiredByPc[pc];
        atPc.push_back(m_retired.insert(m_retired.end(), RetiredBlock{pc, std::move(entry)}));
        if (atPc.size() > kMaxRetiredPerPc) {
            ++m_retiredEvictions;
            ReleaseRetired(atPc.front()); // atPc keeps kMaxRetiredPerPc blocks
        }
    }
    // atPc is out of scope here on purpose: this loop can erase its map entry (the oldest block may
    // be the last one at this pc), which would leave a reference into m_retiredByPc dangling.
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
        ++m_version;
        std::unique_ptr<CachedBlock> entry = std::move(it->entry);
        m_retiredBytes -= entry->code.bytes;
        RemoveRetired(it);
        ++m_retiredReuses;
        CachedBlock *ref = entry.get();
        [[maybe_unused]] const bool inserted = m_blocks.emplace(pc, std::move(entry)).second;
        assert(inserted && "Reuse is only called on a miss, so pc has no cached block");
        SlotFor(pc) = RecentSlot{pc, ref};
        m_native->Publish(pc, ref->code);
        return ref;
    }
    return nullptr;
}

bool BlockCache::SameBuild(const Block &block, ymir::sh2::SH2JitContext &ctx) {
    bool arrays = ctx.bus.pages != nullptr;
    for (size_t i = 0; i < block.guestOpcodes.size() && arrays; ++i) {
        uint16_t opcode = 0;
        arrays = FastPeek16(ctx.bus, block.startPC + static_cast<uint32_t>(i * 2), opcode);
    }
    return arrays == block.fetchFromArrays && IsCurrent(block, ctx) && BlockEndIsCurrent(ctx, block);
}

void BlockCache::RetireIr(uint32_t pc, std::unique_ptr<CachedBlock> entry) {
    // As a fresh build: no compile pending (the result for the ticket is dropped when it arrives),
    // no code pages; Started sets the runs.
    ++m_version;
    entry->pendingTicket = 0;
    entry->runsUntilNative = 0;
    entry->codePages = {};
    m_retiredIrInsts += entry->block.code.size();
    std::vector<RetiredList::iterator> &atPc = m_retiredIrByPc[pc];
    atPc.push_back(m_retiredIr.insert(m_retiredIr.end(), RetiredBlock{pc, std::move(entry)}));
    if (atPc.size() > kMaxRetiredPerPc) {
        const RetiredList::iterator oldest = atPc.front(); // atPc keeps kMaxRetiredPerPc blocks
        const size_t insts = oldest->entry->block.code.size();
        m_retiredIrInsts -= insts;
        m_totalInsts -= insts;
        RemoveRetiredIr(oldest);
    }
}

void BlockCache::RemoveRetiredIr(RetiredList::iterator it) {
    const auto byPc = m_retiredIrByPc.find(it->pc);
    assert(byPc != m_retiredIrByPc.end());
    std::vector<RetiredList::iterator> &atPc = byPc->second;
    atPc.erase(std::find(atPc.begin(), atPc.end(), it));
    if (atPc.empty()) {
        m_retiredIrByPc.erase(byPc);
    }
    m_retiredIr.erase(it);
}

void BlockCache::EvictLeastRecentlyUsedIr() {
    const auto t0 = Clock::now();
    ++m_version;
    ++m_irEvictions;
    const size_t below = m_maxCachedInsts - m_maxCachedInsts / 8 * (8 - kIrCapTargetEighths);

    // Every IR block: the live IR-only ones and the IR pool's.
    std::vector<IrVictim> &victims = m_irVictims;
    victims.clear();
    for (const auto &[pc, entry] : m_blocks) {
        if (entry->code.entry == nullptr && !entry->block.code.empty()) { // an interpreter fallback frees nothing
            victims.push_back(IrVictim{entry->lastUse, entry->block.code.size(), pc, entry.get(), {}});
        }
    }
    for (auto it = m_retiredIr.begin(); it != m_retiredIr.end(); ++it) {
        victims.push_back(IrVictim{it->entry->lastUse, it->entry->block.code.size(), it->pc, nullptr, it});
    }

    // The fewest least recently used blocks whose IR brings m_totalInsts below `below`: a weighted
    // selection by lastUse with nth_element on halving ranges (linear on average, no full sort).
    // Invariant: [0, lo) are older than [lo, n) and hold `freed` < need instructions; the answer
    // (the number of blocks to free) is in (lo, hi].
    const size_t need = m_totalInsts - below + 1;
    const auto older = [](const IrVictim &a, const IrVictim &b) { return a.lastUse < b.lastUse; };
    size_t count = victims.size(); // all of them if even that is not enough
    {
        size_t total = 0;
        for (const IrVictim &v : victims) {
            total += v.insts;
        }
        if (total >= need) {
            size_t lo = 0;
            size_t hi = victims.size();
            size_t freed = 0;
            while (hi - lo > 1) {
                const size_t mid = lo + (hi - lo) / 2;
                std::nth_element(victims.begin() + lo, victims.begin() + mid, victims.begin() + hi, older);
                size_t left = 0;
                for (size_t i = lo; i < mid; ++i) {
                    left += victims[i].insts;
                }
                if (freed + left >= need) {
                    hi = mid;
                } else {
                    freed += left;
                    lo = mid;
                }
            }
            count = hi;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        const IrVictim &v = victims[i];
        m_totalInsts -= v.insts;
        if (v.live != nullptr) {
            // Never published, so no link slot to clear; a pending compile's result is dropped when
            // it arrives (no entry at pc holds its ticket).
            RecentSlot &slot = SlotFor(v.pc);
            if (slot.entry == v.live) {
                slot = RecentSlot{};
            }
            ++m_irEvictedBlocks;
            m_blocks.erase(v.pc);
        } else {
            m_retiredIrInsts -= v.insts;
            ++m_retiredIrReleases;
            RemoveRetiredIr(v.retired);
        }
    }
    assert((m_totalInsts < below || count == victims.size()) && "the IR cap frees down to 7/8 of the cap");
    m_irCapNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

CachedBlock *BlockCache::ReuseIr(ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
    const auto byPc = m_retiredIrByPc.find(pc);
    if (byPc == m_retiredIrByPc.end()) {
        return nullptr;
    }
    for (auto r = byPc->second.rbegin(); r != byPc->second.rend(); ++r) {
        const RetiredList::iterator it = *r;
        if (!SameBuild(it->entry->block, ctx)) {
            continue;
        }
        ++m_version;
        std::unique_ptr<CachedBlock> entry = std::move(it->entry);
        m_retiredIrInsts -= entry->block.code.size();
        RemoveRetiredIr(it); // invalidates r and byPc; the loop ends here
        ++m_retiredIrReuses;
#ifndef NDEBUG
        {
            Block fresh = BuildBlock(ctx, pc);
            OptimizeBlock(fresh);
            const Block &block = entry->block;
            assert(fresh.guestOpcodes == block.guestOpcodes && fresh.guestInstrCount == block.guestInstrCount &&
                   fresh.hasTailWord == block.hasTailWord && fresh.fetchFromArrays == block.fetchFromArrays &&
                   fresh.numValues == block.numValues && PrintBlock(fresh) == PrintBlock(block) &&
                   "a reused IR block equals a fresh build");
        }
#endif
        CachedBlock *ref = entry.get();
        [[maybe_unused]] const bool inserted = m_blocks.emplace(pc, std::move(entry)).second;
        assert(inserted && "ReuseIr is only called on a miss, so pc has no cached block");
        SlotFor(pc) = RecentSlot{pc, ref};
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
    ++m_version;
    const NativeCode &code = it->entry->code;
    m_native->Release(it->pc, code);
    m_releasedNativeBytes += code.bytes;
    m_retiredBytes -= code.bytes;
    RemoveRetired(it);
}

void BlockCache::ReleaseAllRetired() {
    ++m_version;
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

BlockCache::~BlockCache() {
    if (m_worker != nullptr) {
        // No result may be delivered to a destroyed cache, and no job of it may outlive the backend.
        m_worker->Cancel(this);
    }
}

const CachedBlock &BlockCache::Get(ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
    ++m_lookups;
    // No block runs here, so committing and publishing code is safe.
    // A result exists only for a pending ticket, so the worker is polled only with compiles pending.
    if (HasWaitingResults()) {
        InstallResults();
    }
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
    if (CachedBlock *reused = ReuseIr(ctx, pc); reused != nullptr) {
        return Started(*reused, ctx, pc, native); // its IR is already counted
    }
    if (m_totalInsts >= m_maxCachedInsts) {
        EvictLeastRecentlyUsedIr();
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
    ++m_version;
    CachedBlock *ref = entry.get();
    m_blocks.emplace(pc, std::move(entry));
    SlotFor(pc) = RecentSlot{pc, ref};
    return Started(*ref, ctx, pc, native);
}

const CachedBlock &BlockCache::Started(CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint32_t pc, bool native) {
    entry.lastUse = ++m_useClock;
    if (m_native == nullptr || entry.block.guestInstrCount == 0) {
        return entry; // RunBlock (or the interpreter) only
    }
    if (native) {
        return CompileNative(entry, ctx, pc);
    }
    // This run is the first; the threshold run compiles it. Threshold 1 is native on the first
    // run and returned above, so only thresholds above 1 are damped.
    uint32_t threshold = m_nativeCompileThreshold;
    if (const auto stale = m_staleCounts.find(pc); stale != m_staleCounts.end()) {
        const uint32_t shift = std::min<uint32_t>(stale->second, m_maxStaleDampShift);
        if (shift > 0) {
            threshold <<= shift;
            ++m_dampedBuilds;
        }
    }
    entry.runsUntilNative = threshold - 1;
    return entry;
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
    if (entry.code.selfValidating) {
        entry.codePages = CodePages(entry.block, ctx);
    }
    Installed(entry, pc);
    return entry;
}

void BlockCache::Installed(CachedBlock &entry, uint32_t pc) {
    ++m_version;
    ++m_nativeCompiles;
    m_nativeBytes += entry.code.bytes;
    // The native code never needs the IR again (see CachedBlock): free it and stop counting it.
    m_totalInsts -= entry.block.code.size();
    std::vector<Inst>().swap(entry.block.code);
    if (entry.code.selfValidating) {
        m_native->Publish(pc, entry.code);
    }
}

const CachedBlock &BlockCache::SubmitNative(CachedBlock &entry, ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
    assert(m_native != nullptr && m_worker != nullptr && entry.code.entry == nullptr && !entry.block.code.empty());
    assert(entry.pendingTicket == 0 && "a pending entry has no runs left until native");
    entry.runsUntilNative = 0;
    // The code cap as in CompileNative (code still being finalized is not counted yet).
    if (AtCodeCap()) {
        ReleaseAllRetired();
    }
    if (AtCodeCap()) {
        ++m_flushesCodeCap;
        FlushAll();
        return Build(ctx, pc, true);
    }
    if (m_pendingPcs.size() >= kMaxPendingCompiles) {
        entry.runsUntilNative = m_nativeCompileThreshold; // retry later
        return entry;
    }

    const auto t0 = Clock::now();
    std::unique_ptr<PreparedCompile> prepared = m_native->Prepare(entry.block, ctx);
    m_nativeNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    if (prepared == nullptr) {
        ++m_compileFallbacks; // stays IR-only and runs with RunBlock; never retried
        return entry;
    }
    // The pages the prologue will compare are the ones the emitter saw now; the commit must not
    // recompute them (a page remapped meanwhile makes the block stale at its first entry).
    if (ctx.bus.pages != nullptr) {
        entry.codePages = CodePages(entry.block, ctx);
    }
    const uint64_t ticket = m_nextTicket++;
    entry.pendingTicket = ticket;
    m_pendingPcs.emplace(ticket, pc);
    m_worker->Submit(CompileJob{this, ticket, std::move(prepared)});
    return entry;
}

void BlockCache::InstallResults() {
    m_worker->TakeResults(this, m_results);
    for (CompileResult &result : m_results) {
        const auto pending = m_pendingPcs.find(result.ticket);
        assert(pending != m_pendingPcs.end() && "a flush cancels every pending compile");
        if (pending == m_pendingPcs.end()) {
            ++m_asyncDropped; // an invariant violation: visible in the bench
            continue;
        }
        const uint32_t pc = pending->second;
        m_pendingPcs.erase(pending);
        // Only the entry holding this ticket waits for it: not a block dropped meanwhile, nor one
        // rebuilt at the same PC (it has no ticket or another one).
        CachedBlock *entry = nullptr;
        if (result.ticket >= m_generationStart) {
            if (const auto it = m_blocks.find(pc);
                it != m_blocks.end() && it->second->pendingTicket == result.ticket && it->second->code.entry == nullptr) {
                entry = it->second.get();
            }
        }
        if (entry == nullptr) {
            ++m_asyncDropped;
            continue;
        }
        entry->pendingTicket = 0;
        if (!result.ok) {
            ++m_compileFallbacks; // stays IR-only and runs with RunBlock; never retried
            continue;
        }
        const auto t0 = Clock::now();
        const bool ok = m_native->Commit(*result.prepared, entry->code);
        m_nativeNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        m_peakNativeBytes = std::max(m_peakNativeBytes, m_native->CodeBytes());
        if (!ok) {
            entry->code = NativeCode{};
            ++m_compileFallbacks;
            continue;
        }
        if (!entry->code.selfValidating) {
            entry->codePages = {}; // only self-validating blocks use them, as without a worker
        }
        ++m_asyncCompiles;
        Installed(*entry, pc);
    }
    m_results.clear(); // frees the prepared compiles
}

void BlockCache::Flush() {
    ++m_flushesRequested;
    FlushAll();
}

void BlockCache::FlushAll() {
    ++m_version;
    if (m_worker != nullptr) {
        // A new generation: nothing submitted before can be committed after (Cancel also drops
        // every result and waits for the job in flight, before the backend frees its code).
        m_worker->Cancel(this);
        m_asyncDropped += m_pendingPcs.size();
        m_pendingPcs.clear();
        m_generationStart = m_nextTicket;
    }
    m_recent.fill(RecentSlot{});
    m_blocks.clear();
    m_retired.clear(); // the backend reset frees their code
    m_retiredByPc.clear();
    m_retiredBytes = 0;
    m_retiredIr.clear();
    m_retiredIrByPc.clear();
    m_retiredIrInsts = 0;
    m_staleCounts.clear();
    m_totalInsts = 0;
    if (m_native != nullptr) {
        m_native->Reset();
    }
}

} // namespace brimir::jit
