#pragma once

// SH-2 JIT executor: dispatches compiled blocks and falls back to the interpreter
// (see design/sh2-jit.md section 4.3).

#include <brimir/jit/block_cache.hpp>
#include <brimir/jit/frontend.hpp>
#include <brimir/jit/interp_backend.hpp>

#include <ymir/core/types.hpp>
#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>

namespace brimir::jit {

class Executor final : public ymir::sh2::ISH2Executor {
public:
    struct Stats {
        uint64_t blocksRun = 0;
        uint64_t interpreted = 0;
    };

    uint64 Run(ymir::sh2::SH2JitContext &ctx, uint64 executed, uint64 target) override;
    void Flush() override;

    // Runs one compiled block, or one interpreter instruction when no block applies
    // (pending interrupt, delay slot, or unsupported first instruction; reported as retired = 1).
    ExitInfo Step(ymir::sh2::SH2JitContext &ctx);

    const BlockCache &Cache() const {
        return m_cache;
    }
    const Stats &GetStats() const {
        return m_stats;
    }

private:
    BlockCache m_cache;
    Stats m_stats;
};

} // namespace brimir::jit
