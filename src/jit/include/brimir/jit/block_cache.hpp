#pragma once

// Per-CPU cache of compiled blocks keyed by the full guest PC (see design/sh2-jit.md section 6).

#include <brimir/jit/backend.hpp>
#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace brimir::jit {

// Total IR instructions kept before the cache is flushed (~20 MB of IR).
constexpr size_t kMaxCachedInsts = size_t{1} << 20;

// A cached block: its IR (always) and its native code (entry == nullptr: run it with RunBlock).
struct CachedBlock {
    Block block;
    NativeCode code;
};

class BlockCache {
public:
    // native: the backend that compiles each new block, or nullptr to run every block with RunBlock.
    // Not owned; it must outlive the cache.
    explicit BlockCache(INativeBackend *native = nullptr)
        : m_native(native) {}

    // Returns the block for pc, compiling it on a miss or when its guest code changed.
    const CachedBlock &Get(ymir::sh2::SH2JitContext &ctx, uint32_t pc);

    // Drops every block and, with a native backend, all of its generated code.
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
    // Blocks the native backend could not compile (they run with RunBlock).
    uint64_t CompileFallbacks() const {
        return m_compileFallbacks;
    }

private:
    static bool IsCurrent(const Block &block, ymir::sh2::SH2JitContext &ctx);

    INativeBackend *m_native;
    std::unordered_map<uint32_t, std::unique_ptr<CachedBlock>> m_blocks;
    size_t m_totalInsts = 0;
    uint64_t m_compiles = 0;
    uint64_t m_invalidations = 0;
    uint64_t m_compileFallbacks = 0;
};

} // namespace brimir::jit
