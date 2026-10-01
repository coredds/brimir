#include "x64_backend.hpp"
#include "x64_emitter.hpp"
#include "x64_factory.hpp"

#include <asmjit/x86.h>

#include <exception>

namespace brimir::jit {

namespace {

// Turns asmjit errors into a failed Compile (the block then runs on RunBlock).
class ErrorRecorder final : public asmjit::ErrorHandler {
public:
    void handle_error(asmjit::Error err, const char *message, asmjit::BaseEmitter *origin) override {
        (void)message;
        (void)origin;
        if (error == asmjit::Error::kOk) {
            error = err;
        }
    }

    asmjit::Error error = asmjit::Error::kOk;
};

const bool kNoAbort = false;

} // namespace

X64Backend::X64Backend()
    : m_runtime(std::make_unique<asmjit::JitRuntime>()) {}

X64Backend::~X64Backend() = default;

bool X64Backend::Compile(const Block &block, const ymir::sh2::SH2JitContext &ctx, NativeCode &out) {
    out.entry = nullptr;
    if (!CanEmitBlock(block)) {
        return false; // cheap rejection before any asmjit work
    }

    asmjit::CodeHolder code;
    if (code.init(m_runtime->environment(), m_runtime->cpu_features()) != asmjit::Error::kOk) {
        return false;
    }
    ErrorRecorder errors;
    code.set_error_handler(&errors);
    asmjit::x86::Compiler cc(&code);
    if (!EmitBlock(cc, block, ctx)) {
        return false;
    }
    if (cc.finalize() != asmjit::Error::kOk || errors.error != asmjit::Error::kOk) {
        return false;
    }

    X64BlockFn fn = nullptr;
    if (m_runtime->add(&fn, &code) != asmjit::Error::kOk || fn == nullptr) {
        return false;
    }
    m_codeBytes += code.code_size();
    out.entry = reinterpret_cast<const void *>(fn);
    return true;
}

ExitInfo X64Backend::Run(const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target,
                         const bool *abortRequested) {
    X64Frame frame{};
    frame.ctx = &ctx;
    frame.entryCycles = *ctx.cyclesExecuted;
    frame.limit = target >= frame.entryCycles ? target - frame.entryCycles : 0;
    frame.abortRequested = abortRequested != nullptr ? abortRequested : &kNoAbort;

    const auto fn = reinterpret_cast<X64BlockFn>(const_cast<void *>(code.entry));
    fn(&frame);

    if (frame.error) {
        std::rethrow_exception(frame.error);
    }
    return frame.out;
}

void X64Backend::Reset() {
    // Destroying the runtime releases every block it generated.
    m_runtime.reset();
    m_runtime = std::make_unique<asmjit::JitRuntime>();
    m_codeBytes = 0;
}

std::unique_ptr<INativeBackend> MakeX64Backend() {
    return std::make_unique<X64Backend>();
}

} // namespace brimir::jit
