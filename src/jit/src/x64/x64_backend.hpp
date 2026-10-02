#pragma once

// x86-64 native backend for the SH-2 JIT (design/sh2-jit-m2.md section 4), built on asmjit.
// Only files under src/jit/src/x64/ include asmjit.

#include "x64_emitter.hpp"

#include <brimir/jit/backend.hpp>

#include <asmjit/core.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace brimir::jit {

class X64Backend final : public INativeBackend {
public:
    X64Backend();
    ~X64Backend() override;

    X64Backend(const X64Backend &) = delete;
    X64Backend &operator=(const X64Backend &) = delete;

    BackendKind Kind() const override {
        return BackendKind::X64;
    }
    // Compiles any verified block; fails (the block then runs on RunBlock) only if a state field's
    // offset from ctx.R does not fit in 32 bits, a state pointer is null, or asmjit fails.
    bool Compile(const Block &block, const ymir::sh2::SH2JitContext &ctx, NativeCode &out) override;
    // Runs `code` and, with allowChain, the blocks it chains to (a plain loop over the returned
    // entries: no host frames between blocks).
    ExitInfo Run(const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target = kNoCycleTarget,
                 const bool *abortRequested = nullptr, bool allowChain = false) override;
    void Publish(uint32_t pc, const NativeCode &code) override;
    void Unpublish(uint32_t pc) override;
    void Reset() override;
    size_t CodeBytes() const override {
        return m_codeBytes;
    }

private:
    std::unique_ptr<asmjit::JitRuntime> m_runtime; // owns all generated code
    // Link table (x64_emitter.hpp); its address is embedded in generated code, so it never moves.
    std::unique_ptr<X64LinkSlot[]> m_links;
    size_t m_codeBytes = 0;
};

} // namespace brimir::jit
