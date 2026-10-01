#pragma once

// x86-64 native backend for the SH-2 JIT (design/sh2-jit-m2.md section 4), built on asmjit.
// Only files under src/jit/src/x64/ include asmjit.

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
    // Compiles blocks whose ops x64_emitter lowers (no calls out of generated code yet); others
    // return false and run on RunBlock.
    bool Compile(const Block &block, const ymir::sh2::SH2JitContext &ctx, NativeCode &out) override;
    ExitInfo Run(const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target = kNoCycleTarget,
                 const bool *abortRequested = nullptr) override;
    void Reset() override;
    size_t CodeBytes() const override {
        return m_codeBytes;
    }

private:
    std::unique_ptr<asmjit::JitRuntime> m_runtime; // owns all generated code
    size_t m_codeBytes = 0;
};

} // namespace brimir::jit
