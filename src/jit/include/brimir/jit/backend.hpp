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

// Generated code for one block. It is only valid with the SH2JitContext of the CPU it was compiled
// for: guest state is addressed at offsets taken from that context. Each executor owns its own
// block cache (and backend), so code never runs against another CPU.
struct NativeCode {
    const void *entry = nullptr; // nullptr: run the block with RunBlock
    // The code checks at entry that its guest code is unchanged (reporting ExitInfo::stale), so the
    // block cache does not check it, and it may be published to the backend's link table.
    bool selfValidating = false;
};

class INativeBackend {
public:
    virtual ~INativeBackend() = default;
    virtual BackendKind Kind() const = 0;
    // Compiles a verified block (guestInstrCount > 0) for the CPU whose state ctx points to.
    // Returns false, leaving out.entry == nullptr, if this backend cannot compile it.
    virtual bool Compile(const Block &block, const ymir::sh2::SH2JitContext &ctx, NativeCode &out) = 0;
    // Same contract as RunBlock, for one block. An exception thrown by a context callback is
    // rethrown here after the generated code has returned. `ctx` must be the context `code` was
    // compiled with (same CPU; see NativeCode).
    //
    // With allowChain, a block that leaves through a static Exit, a taken ExitIf or ExitDynamic
    // does what Executor::Run does between steps instead of returning: it stores
    // *ctx.cyclesExecuted = entry + cycles so far, stops if they reached `target`, and otherwise
    // enters the block published for the new PC, if any. That block first makes Executor::Step's
    // checks (pending interrupt, PC & 2 fetch buffer) and returns if one fails; the executor then
    // continues with a normal step. The result covers the whole chain (see ExitInfo).
    virtual ExitInfo Run(const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target = kNoCycleTarget,
                         const bool *abortRequested = nullptr, bool allowChain = false) = 0;
    // Link table: makes `code` (selfValidating, compiled for pc) the block chained to at pc, or
    // removes the block published for pc. Publish without selfValidating is ignored.
    virtual void Publish(uint32_t pc, const NativeCode &code) = 0;
    virtual void Unpublish(uint32_t pc) = 0;
    // Frees all generated code and empties the link table. Never called while generated code runs.
    virtual void Reset() = 0;
    virtual size_t CodeBytes() const = 0; // bytes of generated code currently held
};

// nullptr for BackendKind::Ir or when the kind is unavailable in this build.
std::unique_ptr<INativeBackend> MakeNativeBackend(BackendKind kind);

} // namespace brimir::jit
