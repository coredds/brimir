#pragma once

// Factory for the x86-64 native backend. Kept free of asmjit so code outside src/jit/src/x64/
// can create the backend without seeing asmjit headers.

#include <brimir/jit/backend.hpp>

#include <memory>

namespace brimir::jit {

std::unique_ptr<INativeBackend> MakeX64Backend();

// INativeBackend::Run on a backend made by MakeX64Backend, without the virtual call (the executor's
// per-chain entry).
ExitInfo RunX64(INativeBackend &backend, const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target,
                const bool *abortRequested, bool allowChain);

} // namespace brimir::jit
