#pragma once
// Native code backends for the SH-2 JIT (design/sh2-jit-m2.md section 4). A block a native backend
// cannot compile runs on the IR interpreter (RunBlock), which is also the reference semantics.
#include <brimir/jit/interp_backend.hpp> // ExitInfo, kNoCycleTarget
#include <brimir/jit/ir.hpp>
#include <ymir/hw/sh2/sh2_jit_iface.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace brimir::jit {

enum class BackendKind : uint8_t { Ir, X64 };

bool IsBackendAvailable(BackendKind kind); // Ir: always; X64: only in x86-64 builds
BackendKind DefaultBackend();              // X64 when available, else Ir
const char *BackendName(BackendKind kind); // "ir", "x64"
bool ParseBackend(std::string_view name, BackendKind &out);

struct NativeCode {
    const void *entry = nullptr; // nullptr: run the block with RunBlock
};

class INativeBackend {
public:
    virtual ~INativeBackend() = default;
    virtual BackendKind Kind() const = 0;
    // Compiles a verified block (guestInstrCount > 0) for the CPU whose state ctx points to.
    // Returns false, leaving out.entry == nullptr, if this backend cannot compile it.
    virtual bool Compile(const Block &block, const ymir::sh2::SH2JitContext &ctx, NativeCode &out) = 0;
    // Same contract as RunBlock. An exception thrown by a context callback is rethrown here after
    // the generated code has returned.
    virtual ExitInfo Run(const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target = kNoCycleTarget,
                         const bool *abortRequested = nullptr) = 0;
    // Frees all generated code. Never called while generated code runs.
    virtual void Reset() = 0;
    virtual size_t CodeBytes() const = 0; // bytes of generated code currently held
};

// nullptr for BackendKind::Ir or when the kind is unavailable in this build.
std::unique_ptr<INativeBackend> MakeNativeBackend(BackendKind kind);

} // namespace brimir::jit
