#include "jit_test_backend.hpp"

#include "catch_amalgamated.hpp"

#include <cstdlib>
#include <string>

namespace sh2test {

using brimir::jit::BackendKind;

BackendKind TestBackend() {
    const char *env = std::getenv("BRIMIR_JIT_BACKEND");
    if (env == nullptr || *env == '\0') {
        return brimir::jit::DefaultBackend();
    }
    BackendKind kind{};
    if (!brimir::jit::ParseBackend(env, kind)) {
        FAIL("BRIMIR_JIT_BACKEND='" << env << "' is not a backend name (expected ir or x64)");
    }
    if (!brimir::jit::IsBackendAvailable(kind)) {
        FAIL("BRIMIR_JIT_BACKEND='" << env << "' names a backend that is not available in this build");
    }
    return kind;
}

std::vector<BackendKind> AvailableBackends() {
    std::vector<BackendKind> kinds;
    for (const BackendKind kind : {BackendKind::Ir, BackendKind::X64}) {
        if (brimir::jit::IsBackendAvailable(kind)) {
            kinds.push_back(kind);
        }
    }
    return kinds;
}

brimir::jit::ExitInfo RunOnBackend(BackendKind kind, const brimir::jit::Block &block, ymir::sh2::SH2JitContext &ctx,
                                   uint64_t target, const bool *abortRequested) {
    if (kind == BackendKind::Ir) {
        return brimir::jit::RunBlock(block, ctx, target, abortRequested);
    }
    const auto backend = brimir::jit::MakeNativeBackend(kind);
    REQUIRE(backend != nullptr);
    brimir::jit::NativeCode code;
    const bool compiled = backend->Compile(block, ctx, code);
    INFO("backend " << brimir::jit::BackendName(kind) << " failed to compile the block at 0x" << std::hex
                    << block.startPC);
    REQUIRE(compiled);
    REQUIRE(code.entry != nullptr);
    return backend->Run(code, ctx, target, abortRequested);
}

void MapFastPathTestPages(Rig &rig, FastRom &rom) {
    for (size_t i = 0; i < rom.size(); ++i) {
        rom[i] = static_cast<uint8_t>(i * 37 + (i >> 8) + 0x5A);
    }
    rig.bus.MapArray(0x4000000, 0x400FFFF, rom, false);
    rig.bus.Unmap(0x5010000, 0x501FFFF);
}

std::vector<uint32_t> FastPathTestAddresses() {
    std::vector<uint32_t> addresses;
    for (uint32_t partition = 0; partition < 8; ++partition) {
        for (const uint32_t offset : {kFastRamOffset, kFastMmioOffset, kFastRomOffset, kFastUnmappedOffset}) {
            for (uint32_t misalign = 0; misalign < 4; ++misalign) {
                addresses.push_back((partition << 29) | (offset + misalign));
            }
        }
    }
    return addresses;
}

} // namespace sh2test
