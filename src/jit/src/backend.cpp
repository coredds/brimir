#include <brimir/jit/backend.hpp>

#if BRIMIR_JIT_HAS_X64
#include "x64/x64_factory.hpp"
#endif

namespace brimir::jit {

bool INativeBackend::Compile(const Block &block, const ymir::sh2::SH2JitContext &ctx, NativeCode &out) {
    out.entry = nullptr;
    out.bytes = 0;
    const std::unique_ptr<PreparedCompile> prepared = Prepare(block, ctx);
    if (prepared == nullptr || !prepared->Finalize()) {
        return false;
    }
    return Commit(*prepared, out);
}

bool IsBackendAvailable(BackendKind kind) {
    switch (kind) {
    case BackendKind::Ir:
        return true;
    case BackendKind::X64:
        return BRIMIR_JIT_HAS_X64 != 0;
    }
    return false;
}

BackendKind DefaultBackend() {
    return IsBackendAvailable(BackendKind::X64) ? BackendKind::X64 : BackendKind::Ir;
}

const char *BackendName(BackendKind kind) {
    switch (kind) {
    case BackendKind::Ir:
        return "ir";
    case BackendKind::X64:
        return "x64";
    }
    return "unknown";
}

bool ParseBackend(std::string_view name, BackendKind &out) {
    for (const BackendKind kind : {BackendKind::Ir, BackendKind::X64}) {
        if (name == BackendName(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

std::unique_ptr<INativeBackend> MakeNativeBackend(BackendKind kind) {
#if BRIMIR_JIT_HAS_X64
    if (kind == BackendKind::X64) {
        return MakeX64Backend();
    }
#endif
    (void)kind;
    return nullptr;
}

} // namespace brimir::jit
