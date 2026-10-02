#include <brimir/jit/block_cache.hpp>

#include <brimir/jit/bus_fast_path.hpp>
#include <brimir/jit/frontend.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

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
        return *slot.entry;
    }
    if (auto it = m_blocks.find(pc); it != m_blocks.end()) {
        if (slot.entry != it->second.get() && Validate(*it->second, pc, ctx)) {
            slot = RecentSlot{pc, it->second.get()};
            return *it->second;
        }
        Invalidate(it);
    }

    if (m_totalInsts >= kMaxCachedInsts || (m_native != nullptr && m_native->CodeBytes() >= m_maxNativeCodeBytes)) {
        Flush();
    }

    auto entry = std::make_unique<CachedBlock>();
    entry->block = BuildBlock(ctx, pc);
    Block &block = entry->block;
    if (!VerifyBlock(block).empty()) {
        // Never run a malformed block: fall back to the interpreter for this PC.
        assert(false && "front end produced an invalid block");
        const uint16_t opcode = ctx.peekInstruction(ctx.sh2, pc);
        block = Block{};
        block.startPC = pc;
        block.guestOpcodes.push_back(opcode);
    }
    if (m_native != nullptr && block.guestInstrCount > 0 && !m_native->Compile(block, ctx, entry->code)) {
        entry->code = NativeCode{}; // runs with RunBlock
        ++m_compileFallbacks;
    }
    ++m_compiles;
    m_totalInsts += block.code.size();
    CachedBlock *ref = entry.get();
    m_blocks.emplace(pc, std::move(entry));
    SlotFor(pc) = RecentSlot{pc, ref};
    if (ref->code.selfValidating) {
        m_native->Publish(pc, ref->code);
    }
    return *ref;
}

void BlockCache::Flush() {
    m_recent.fill(RecentSlot{});
    m_blocks.clear();
    m_totalInsts = 0;
    if (m_native != nullptr) {
        m_native->Reset();
    }
}

} // namespace brimir::jit
