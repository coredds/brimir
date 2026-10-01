#include "x64_backend.hpp"

#include <stdexcept>

namespace brimir::jit {

X64Backend::X64Backend()
    : m_runtime(std::make_unique<asmjit::JitRuntime>()) {}

X64Backend::~X64Backend() = default;

bool X64Backend::Compile(const Block &block, const ymir::sh2::SH2JitContext &ctx, NativeCode &out) {
    (void)block;
    (void)ctx;
    out.entry = nullptr;
    return false;
}

ExitInfo X64Backend::Run(const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target,
                         const bool *abortRequested) {
    (void)code;
    (void)ctx;
    (void)target;
    (void)abortRequested;
    // Compile never produces code yet, so the executor never gets here.
    throw std::logic_error("X64Backend::Run called without generated code");
}

void X64Backend::Reset() {
    // Destroying the runtime releases every block it generated.
    m_runtime.reset();
    m_runtime = std::make_unique<asmjit::JitRuntime>();
    m_codeBytes = 0;
}

} // namespace brimir::jit
