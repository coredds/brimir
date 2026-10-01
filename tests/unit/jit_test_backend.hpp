#pragma once

// Backend selection for the SH-2 JIT tests. The environment variable BRIMIR_JIT_BACKEND (ir|x64)
// picks the backend the executor-level tests run on; unset, they use DefaultBackend().

#include <brimir/jit/backend.hpp>
#include <brimir/jit/interp_backend.hpp>
#include <brimir/jit/ir.hpp>

#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstdint>
#include <vector>

namespace sh2test {

// BRIMIR_JIT_BACKEND if set (fails the test if it names an unknown or unavailable backend),
// else DefaultBackend().
brimir::jit::BackendKind TestBackend();

// Every backend available in this build (Ir first).
std::vector<brimir::jit::BackendKind> AvailableBackends();

// Runs one verified block on `kind`: RunBlock for Ir, otherwise REQUIREs that the native backend
// compiles it and runs the generated code. The backend lives for the duration of the call.
brimir::jit::ExitInfo RunOnBackend(brimir::jit::BackendKind kind, const brimir::jit::Block &block,
                                   ymir::sh2::SH2JitContext &ctx,
                                   uint64_t target = brimir::jit::kNoCycleTarget,
                                   const bool *abortRequested = nullptr);

} // namespace sh2test
