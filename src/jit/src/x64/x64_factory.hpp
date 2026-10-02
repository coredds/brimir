#pragma once

// Factory for the x86-64 native backend. Kept free of asmjit so code outside src/jit/src/x64/
// can create the backend without seeing asmjit headers.

#include <brimir/jit/backend.hpp>

#include <memory>

namespace brimir::jit {

std::unique_ptr<INativeBackend> MakeX64Backend();

} // namespace brimir::jit
