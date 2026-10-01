#include <brimir/jit/block_cache.hpp>

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
        if (ctx.peekInstruction(ctx.sh2, address) != block.guestOpcodes[i]) {
            return false;
        }
    }
    return true;
}

const CachedBlock &BlockCache::Get(ymir::sh2::SH2JitContext &ctx, uint32_t pc) {
    if (auto it = m_blocks.find(pc); it != m_blocks.end()) {
        if (IsCurrent(it->second->block, ctx)) {
            return *it->second;
        }
        // Its native code (if any) stays allocated until the next Flush.
        ++m_invalidations;
        m_totalInsts -= it->second->block.code.size();
        m_blocks.erase(it);
    }

    if (m_totalInsts >= kMaxCachedInsts) {
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
    const CachedBlock &ref = *entry;
    m_blocks.emplace(pc, std::move(entry));
    return ref;
}

void BlockCache::Flush() {
    m_blocks.clear();
    m_totalInsts = 0;
    if (m_native != nullptr) {
        m_native->Reset();
    }
}

} // namespace brimir::jit
