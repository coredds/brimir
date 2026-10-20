// Brimir - SH-2 JIT integration with CoreWrapper
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/compile_worker.hpp>
#include <brimir/jit/executor.hpp>

#include "jit_test_backend.hpp"

#include <vector>

using brimir::CoreWrapper;

TEST_CASE("SH-2 JIT is off by default and toggles executors", "[jit][integration]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);
    REQUIRE_FALSE(core.IsSH2JitEnabled());
    REQUIRE(saturn->masterSH2.GetJitExecutor() == nullptr);

    core.SetSH2JitBackend(sh2test::TestBackend());
    core.SetSH2JitEnabled(true);
    REQUIRE(core.IsSH2JitEnabled());
    REQUIRE(saturn->masterSH2.GetJitExecutor() != nullptr);
    REQUIRE(saturn->slaveSH2.GetJitExecutor() != nullptr);
    REQUIRE(saturn->masterSH2.GetJitExecutor() != saturn->slaveSH2.GetJitExecutor());

    core.SetSH2JitEnabled(false);
    REQUIRE(saturn->masterSH2.GetJitExecutor() == nullptr);
    REQUIRE(saturn->slaveSH2.GetJitExecutor() == nullptr);
}

TEST_CASE("SH-2 JIT enabled before Initialize is applied", "[jit][integration]") {
    CoreWrapper core;
    core.SetSH2JitBackend(sh2test::TestBackend());
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    REQUIRE(core.GetSaturn()->masterSH2.GetJitExecutor() != nullptr);
}

TEST_CASE("Frames run with the SH-2 JIT enabled", "[jit][integration]") {
    // Uses the built-in null IPL program (no BIOS loaded).
    CoreWrapper core;
    core.SetSH2JitBackend(sh2test::TestBackend());
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    for (int i = 0; i < 30; ++i) {
        core.RunFrame();
    }
    REQUIRE(core.GetLastError().empty());
    const auto* master = core.GetSH2JitExecutor(true);
    REQUIRE(master != nullptr);
    REQUIRE(master->GetStats().blocksRun + master->GetStats().interpreted > 0);
}

TEST_CASE("Save state round trip with the SH-2 JIT keeps the block caches", "[jit][integration]") {
    CoreWrapper core;
    core.SetSH2JitBackend(sh2test::TestBackend());
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    for (int i = 0; i < 10; ++i) {
        core.RunFrame();
    }
    std::vector<uint8_t> state(core.GetStateSize());
    REQUIRE(core.SaveState(state.data(), state.size()));
    const auto* master = core.GetSH2JitExecutor(true);
    const auto* slave = core.GetSH2JitExecutor(false);
    const size_t masterSize = master->Cache().Size();
    const size_t slaveSize = slave->Cache().Size();
    const uint64_t masterFlushes = master->Cache().FlushesRequested();
    const uint64_t slaveFlushes = slave->Cache().FlushesRequested();
    REQUIRE(masterSize > 0);
    REQUIRE(core.LoadState(state.data(), state.size()));
    REQUIRE(master->Cache().Size() == masterSize);
    REQUIRE(slave->Cache().Size() == slaveSize);
    REQUIRE(master->Cache().FlushesRequested() == masterFlushes);
    REQUIRE(slave->Cache().FlushesRequested() == slaveFlushes);
    core.RunFrame();
    REQUIRE(core.GetLastError().empty());
}

namespace {

// The worker is created exactly when the option is on, the CPU has the threads and the backend is x64.
bool WorkerExpected(const CoreWrapper &core, bool option) {
    return option && brimir::jit::CompileThreadSupported() &&
           core.GetSH2JitBackend() == brimir::jit::BackendKind::X64;
}

// A 512 KiB IPL whose master SH-2 loops through two small blocks forever (the built-in null IPL
// runs a handful of blocks and sleeps, so nothing gets hot enough to compile).
std::vector<uint8_t> HotLoopIpl() {
    std::vector<uint8_t> ipl(512 * 1024, 0);
    const auto put16 = [&ipl](size_t at, uint16_t v) {
        ipl[at] = static_cast<uint8_t>(v >> 8);
        ipl[at + 1] = static_cast<uint8_t>(v);
    };
    const auto put32 = [&](size_t at, uint32_t v) {
        put16(at, static_cast<uint16_t>(v >> 16));
        put16(at + 2, static_cast<uint16_t>(v));
    };
    put32(0, 0x00000100); // reset PC
    put32(4, 0x06100000); // reset SP
    put16(0x100, 0x7001); // L1: add #1, r0
    put16(0x102, 0xA000); //     bra L2
    put16(0x104, 0x0009); //     nop
    put16(0x106, 0x7001); // L2: add #1, r0
    put16(0x108, 0xAFFA); //     bra L1
    put16(0x10A, 0x0009); //     nop
    return ipl;
}

void RunFrames(CoreWrapper &core, int frames) {
    for (int i = 0; i < frames; ++i) {
        core.RunFrame();
    }
    REQUIRE(core.GetLastError().empty());
}

} // namespace

TEST_CASE("Frames run with the JIT compile thread", "[jit][integration][worker]") {
    CoreWrapper core;
    core.SetSH2JitCompileThread(true);
    if (brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        core.SetSH2JitBackend(brimir::jit::BackendKind::X64);
    }
    core.SetSH2JitEnabled(true);
    REQUIRE(core.Initialize());
    REQUIRE(core.LoadIPL(HotLoopIpl()));
    core.Reset(); // the master SH-2 takes its reset vector from the new IPL
    RunFrames(core, 60);

    const bool expected = WorkerExpected(core, true);
    REQUIRE((core.GetSH2JitCompileWorker() != nullptr) == expected);
    const auto *master = core.GetSH2JitExecutor(true);
    REQUIRE(master != nullptr);
    if (expected) {
        // The worker needs a moment on a loaded machine: give it a bounded number of extra frames.
        for (int extra = 0; extra < 600 && master->Cache().AsyncCompiles() == 0; ++extra) {
            RunFrames(core, 1);
        }
        REQUIRE(master->Cache().AsyncCompiles() > 0);
    } else {
        REQUIRE(master->Cache().AsyncCompiles() == 0);
    }

    // Toggling the option and the backend between frames recreates the executors (and the worker).
    core.SetSH2JitCompileThread(false);
    REQUIRE(core.GetSH2JitCompileWorker() == nullptr);
    RunFrames(core, 5);
    core.SetSH2JitCompileThread(true);
    REQUIRE((core.GetSH2JitCompileWorker() != nullptr) == WorkerExpected(core, true));
    RunFrames(core, 20);
    core.SetSH2JitBackend(brimir::jit::BackendKind::Ir);
    REQUIRE(core.GetSH2JitCompileWorker() == nullptr);
    RunFrames(core, 5);
    if (brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
        core.SetSH2JitBackend(brimir::jit::BackendKind::X64);
        REQUIRE((core.GetSH2JitCompileWorker() != nullptr) == WorkerExpected(core, true));
        RunFrames(core, 20);
    }
    core.SetSH2JitEnabled(false);
    RunFrames(core, 5);
    core.SetSH2JitEnabled(true);
    REQUIRE((core.GetSH2JitCompileWorker() != nullptr) == WorkerExpected(core, true));
    RunFrames(core, 5);
}

TEST_CASE("Core unload with compiles in flight", "[jit][integration][worker]") {
    for (int round = 0; round < 20; ++round) {
        CoreWrapper core;
        core.SetSH2JitCompileThread(true);
        if (brimir::jit::IsBackendAvailable(brimir::jit::BackendKind::X64)) {
            core.SetSH2JitBackend(brimir::jit::BackendKind::X64);
        }
        core.SetSH2JitEnabled(true);
        REQUIRE(core.Initialize());
        REQUIRE(core.LoadIPL(HotLoopIpl()));
        core.Reset(); // the master SH-2 takes its reset vector from the new IPL
        RunFrames(core, 10);
        // `core` is destroyed here with its worker possibly mid-compile.
    }
}
