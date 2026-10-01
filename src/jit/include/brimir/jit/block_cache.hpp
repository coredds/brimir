#pragma once

// Per-CPU cache of compiled blocks keyed by the full guest PC (see design/sh2-jit.md section 6).

#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace brimir::jit {

// Total IR instructions kept before the cache is flushed (~20 MB of IR).
constexpr size_t kMaxCachedInsts = size_t{1} << 20;

class BlockCache {
public:
    // Returns the block for pc, compiling it on a miss or when its guest code changed.
    const Block &Get(ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    void Flush();

    size_t Size() const {
        return m_blocks.size();
    }
    uint64_t Compiles() const {
        return m_compiles;
    }
    uint64_t Invalidations() const {
        return m_invalidations;
    }

private:
    static bool IsCurrent(const Block &block, ymir::sh2::SH2JitContext &ctx);

    std::unordered_map<uint32_t, std::unique_ptr<Block>> m_blocks;
    size_t m_totalInsts = 0;
    uint64_t m_compiles = 0;
    uint64_t m_invalidations = 0;
};

} // namespace brimir::jit
