#include "x64_backend.hpp"
#include "x64_emitter.hpp"
#include "x64_factory.hpp"

#include <brimir/jit/sh2_helpers.hpp>

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

// Stores the exception being handled and stops the block.
void Fail(X64Frame *f) noexcept {
    f->error = std::current_exception();
    f->stop = 1;
}

// After a memory callback or refill: stop if the callback requested an abort (RunBlock's abortNow).
void CheckAbort(X64Frame *f) noexcept {
    if (*f->abortRequested) {
        f->stop = 1;
    }
}

} // namespace

// Trampolines (contract in x64_emitter.hpp). Callbacks are read from ctx at call time.

uint32_t TrRead(X64Frame *f, uint32_t address, uint32_t size, uint32_t instrFetch) noexcept {
    try {
        auto &ctx = *f->ctx;
        const uint32_t value = ctx.read(ctx.sh2, address, size, instrFetch != 0);
        CheckAbort(f);
        return value;
    } catch (...) {
        Fail(f);
        return 0;
    }
}

void TrWrite(X64Frame *f, uint32_t address, uint32_t size, uint32_t value) noexcept {
    try {
        auto &ctx = *f->ctx;
        ctx.write(ctx.sh2, address, size, value);
        CheckAbort(f);
    } catch (...) {
        Fail(f);
    }
}

void TrRefill(X64Frame *f, uint32_t address) noexcept {
    try {
        auto &ctx = *f->ctx;
        ctx.refillPipeline(ctx.sh2, address);
        CheckAbort(f);
    } catch (...) {
        Fail(f);
    }
}

uint64_t TrAccessCycles(X64Frame *f, uint32_t address, uint32_t size, uint32_t write) noexcept {
    try {
        auto &ctx = *f->ctx;
        return ctx.accessCycles(ctx.sh2, address, size, write != 0);
    } catch (...) {
        Fail(f);
        return 0;
    }
}

uint64_t TrAccessCyclesRMWByte(X64Frame *f, uint32_t address) noexcept {
    try {
        auto &ctx = *f->ctx;
        return ctx.accessCyclesRMWByte(ctx.sh2, address);
    } catch (...) {
        Fail(f);
        return 0;
    }
}

uint32_t TrBusWait(X64Frame *f, uint32_t address, uint32_t size, uint32_t write) noexcept {
    try {
        auto &ctx = *f->ctx;
        return ctx.busWait(ctx.sh2, address, size, write != 0) ? 1u : 0u;
    } catch (...) {
        Fail(f);
        return 0;
    }
}

void TrSetupDelaySlot(X64Frame *f, uint32_t target) noexcept {
    try {
        auto &ctx = *f->ctx;
        ctx.setupDelaySlot(ctx.sh2, target);
    } catch (...) {
        Fail(f);
    }
}

void TrEndDelaySlot(X64Frame *f) noexcept {
    try {
        auto &ctx = *f->ctx;
        ctx.endDelaySlot(ctx.sh2);
    } catch (...) {
        Fail(f);
    }
}

void TrSetSR(X64Frame *f, uint32_t value, uint32_t delaySlot) noexcept {
    try {
        auto &ctx = *f->ctx;
        ctx.setSR(ctx.sh2, value, delaySlot != 0);
    } catch (...) {
        Fail(f);
    }
}

uint32_t TrDiv1(X64Frame *f, uint32_t rn, uint32_t rm, uint32_t rmIsRn) noexcept {
    return Div1Step(rn, rm, rmIsRn != 0, *f->ctx->SR);
}

namespace {

// RunBlock's MacW/MacL case.
template <bool kLong>
void MacStep(X64Frame *f, uint32_t a, uint32_t b) noexcept {
    auto &ctx = *f->ctx;
    const uint64_t mac = (static_cast<uint64_t>(*ctx.MACH) << 32) | *ctx.MACL;
    const bool s = ((*ctx.SR >> 1) & 1u) != 0;
    const auto op1 = static_cast<int32_t>(a);
    const auto op2 = static_cast<int32_t>(b);
    const uint64_t result = kLong ? MacLStep(mac, s, op1, op2) : MacWStep(mac, s, op1, op2);
    *ctx.MACH = static_cast<uint32_t>(result >> 32);
    *ctx.MACL = static_cast<uint32_t>(result);
}

} // namespace

void TrMacW(X64Frame *f, uint32_t op1, uint32_t op2) noexcept {
    MacStep<false>(f, op1, op2);
}

void TrMacL(X64Frame *f, uint32_t op1, uint32_t op2) noexcept {
    MacStep<true>(f, op1, op2);
}

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
