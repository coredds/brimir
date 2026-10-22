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
    uint32_t bytes = 0; // size of the generated code (set by Compile; counted in CodeBytes)
};

// A native compile between INativeBackend::Prepare and Commit: everything the emitter read from the
// block and the emulator is already captured in it. It may be destroyed on any thread: with a
// CompileWorker, a job cancelled while in flight is destroyed on the worker thread, and the others
// on the thread calling Cancel, TakeResults or the worker's destructor (see compile_worker.hpp). So
// the destructor, like Finalize, must touch only the object's own state.
class PreparedCompile {
public:
    virtual ~PreparedCompile() = default;
    // Runs the backend's code generation passes. May run on any thread, but never concurrently with
    // another call on the same object, and the object must not be used by anyone else meanwhile: it
    // touches only the object's own state (not the block, the context, guest memory, the bus or
    // the backend). Returns false if code generation failed.
    virtual bool Finalize() noexcept = 0;
};

class INativeBackend {
public:
    virtual ~INativeBackend() = default;
    virtual BackendKind Kind() const = 0;
    // A native compile runs in three phases: Prepare, Finalize (PreparedCompile) and Commit.
    // Prepare and Commit run on the emulation thread, outside any running block.
    //
    // Prepare: translates a verified block (guestInstrCount > 0) for the CPU whose state ctx points
    // to, reading everything it needs from the block and the emulator (including the bus page
    // table at this moment). Returns nullptr if this backend cannot compile the block.
    virtual std::unique_ptr<PreparedCompile> Prepare(const Block &block, const ymir::sh2::SH2JitContext &ctx) = 0;
    // Commit: after a successful Finalize of `prepared` (made by this backend, not committed
    // before), adds the code to the backend and sets out.entry, out.bytes and out.selfValidating.
    // Returns false, leaving out.entry == nullptr, on a runtime error.
    virtual bool Commit(PreparedCompile &prepared, NativeCode &out) = 0;
    // Prepare, Finalize and Commit in one call, on the calling thread. Returns false, leaving
    // out.entry == nullptr, if this backend cannot compile the block.
    bool Compile(const Block &block, const ymir::sh2::SH2JitContext &ctx, NativeCode &out);
    // Same contract as RunBlock, for one block. An exception thrown by a context callback is
    // rethrown here after the generated code has returned. `ctx` must be the context `code` was
    // compiled with (same CPU; see NativeCode). Run is not reentrant and not thread-safe: one Run
    // at a time per backend (the backend holds the exception while the generated code unwinds).
    //
    // With allowChain, a block that leaves through a static Exit, a taken ExitIf or ExitDynamic
    // does what Executor::Run does between steps instead of returning: it stores
    // *ctx.cyclesExecuted = entry + cycles so far, stops if they reached `target`, and otherwise
    // enters the block published for the new PC, if any. That block first makes Executor::Step's
    // checks (pending interrupt, PC & 2 fetch buffer) and returns if one fails; the executor then
    // continues with a normal step. The result covers the whole chain (see ExitInfo). When a chain
    // or block stops (cycle limit or boundary), the entry linked for the new PC is stored in
    // out.next (see ExitInfo::next).
    virtual ExitInfo Run(const NativeCode &code, ymir::sh2::SH2JitContext &ctx, uint64_t target = kNoCycleTarget,
                         const bool *abortRequested = nullptr, bool allowChain = false) = 0;
    // Link table: makes `code` (selfValidating, compiled for pc) the block chained to at pc, or
    // removes the block published for pc. Publish without selfValidating is ignored.
    virtual void Publish(uint32_t pc, const NativeCode &code) = 0;
    virtual void Unpublish(uint32_t pc) = 0;
    // Frees the code of one block compiled for pc and subtracts code.bytes from CodeBytes(). The
    // caller has already unpublished pc (the link table must never point to freed code), and no
    // generated code runs.
    virtual void Release(uint32_t pc, const NativeCode &code) = 0;
    // Frees all generated code and empties the link table. Never called while generated code runs.
    virtual void Reset() = 0;
    virtual size_t CodeBytes() const = 0; // bytes of generated code currently held
};

// nullptr for BackendKind::Ir or when the kind is unavailable in this build.
std::unique_ptr<INativeBackend> MakeNativeBackend(BackendKind kind);

} // namespace brimir::jit
