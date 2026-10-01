// Brimir - whole-system lockstep: a JIT core and an interpreter core must stay identical
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/core_wrapper.hpp>
#include <brimir/jit/executor.hpp>
#include <brimir/lockstep.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using brimir::CoreWrapper;

namespace {

std::unique_ptr<CoreWrapper> MakeLockstepCore(bool jit) {
    auto core = std::make_unique<CoreWrapper>();
    core->SetSH2JitEnabled(jit);
    REQUIRE(core->Initialize());
    brimir::PrepareLockstepCore(*core);
    return core;
}

// SH-2 encoders (copied from test_jit_diff.cpp)
uint16_t Nm(uint16_t base, uint32_t n, uint32_t m) {
    return static_cast<uint16_t>(base | (n << 8) | (m << 4));
}
uint16_t NImm(uint16_t base, uint32_t n, uint32_t imm) {
    return static_cast<uint16_t>(base | (n << 8) | (imm & 0xFF));
}
constexpr uint16_t kNop = 0x0009;
constexpr uint16_t kRte = 0x002B;
uint16_t MovI(uint32_t n, uint32_t imm) { return NImm(0xE000, n, imm); }
uint16_t MovBL(uint32_t n, uint32_t m) { return Nm(0x6000, n, m); }
uint16_t MovLL(uint32_t n, uint32_t m) { return Nm(0x6002, n, m); }
uint16_t MovBS(uint32_t n, uint32_t m) { return Nm(0x2000, n, m); }
uint16_t MovLS(uint32_t n, uint32_t m) { return Nm(0x2002, n, m); }
uint16_t MovLI(uint32_t n, uint32_t disp) { return NImm(0xD000, n, disp); }
uint16_t Add(uint32_t n, uint32_t m) { return Nm(0x300C, n, m); }
uint16_t AddI(uint32_t n, uint32_t imm) { return NImm(0x7000, n, imm); }
uint16_t Dt(uint32_t n) { return static_cast<uint16_t>(0x4010 | (n << 8)); }
uint16_t Bf(uint32_t d) { return static_cast<uint16_t>(0x8B00 | (d & 0xFF)); }
uint16_t Bra(uint32_t d) { return static_cast<uint16_t>(0xA000 | (d & 0xFFF)); }

constexpr uint32_t kMainLoop = 0x06004000;
constexpr uint32_t kHandler = 0x06005000;
constexpr uint32_t kVbr = 0x06008000;
constexpr uint32_t kFrtOciVector = 0x60;
constexpr uint32_t kStack = 0x0600F000;
constexpr uint32_t kSrcTable = 0x26010000; // cache-through alias of WRAM-H 0x06010000
constexpr uint32_t kDstArea = 0x26020000;  // cache-through alias of WRAM-H 0x06020000
constexpr uint32_t kLoopCount = 64;

// WRAM-H is stored big-endian (the bus maps it with util::ReadBE/WriteBE).
void PokeWramHigh16(ymir::Saturn &saturn, uint32_t address, uint16_t value) {
    const uint32_t off = address & 0xFFFFF;
    saturn.mem.WRAMHigh[off + 0] = static_cast<uint8_t>(value >> 8);
    saturn.mem.WRAMHigh[off + 1] = static_cast<uint8_t>(value);
}
void PokeWramHigh32(ymir::Saturn &saturn, uint32_t address, uint32_t value) {
    PokeWramHigh16(saturn, address, static_cast<uint16_t>(value >> 16));
    PokeWramHigh16(saturn, address + 2, static_cast<uint16_t>(value));
}
void PokeCode(ymir::Saturn &saturn, uint32_t address, const std::vector<uint16_t> &words) {
    for (uint16_t w : words) {
        PokeWramHigh16(saturn, address, w);
        address += 2;
    }
}

// Installs a master SH-2 workload that mixes compiled code, WRAM traffic and FRT compare-match A
// interrupts (taken mid-loop, serviced by an interpreted RTE). Called right after Initialize +
// PrepareLockstepCore, identically on both cores.
void InstallFrtWorkload(CoreWrapper &core) {
    ymir::Saturn &saturn = *core.GetSaturn();
    auto &sh2 = saturn.masterSH2;

    // Main loop. mov.l @(disp,PC),Rn loads from ((PC & ~3) + 4 + disp * 4).
    // Branch targets are (PC + 4 + disp * 2).
    //
    // The loop also reads FRCH. The FRT is otherwise advanced only at the start of each
    // SH2::Advance slice, so the compare-match interrupt would always be pending at block entry;
    // an FRCH read runs AdvanceFRT<false> inside the compiled block, so the interrupt becomes
    // pending mid-block and the block's interrupt boundary checks are exercised.
    PokeCode(saturn, kMainLoop,
             {
                 MovLI(8, 8),    // 4000: (4000+4)+8*4  = 4024 -> R8 = kSrcTable
                 MovLI(9, 9),    // 4002: (4000+4)+9*4  = 4028 -> R9 = kDstArea
                 MovLI(3, 9),    // 4004: (4004+4)+9*4  = 402C -> R3 = 64
                 MovLI(11, 10),  // 4006: (4004+4)+10*4 = 4030 -> R11 = 0xFFFFFE12 (FRCH)
                                 // loop:
                 MovLL(1, 8),    // 4008: mov.l @R8,R1
                 Add(2, 1),      // 400A: add R1,R2
                 MovLS(9, 2),    // 400C: mov.l R2,@R9
                 MovBL(0, 11),   // 400E: mov.b @R11,R0 (FRCH read: AdvanceFRT mid-block)
                 AddI(8, 4),     // 4010: add #4,R8
                 AddI(9, 4),     // 4012: add #4,R9
                 Dt(3),          // 4014: dt R3
                 Bf(0xF7),       // 4016: bf loop: 4016+4+(-9)*2 = 4008
                 MovLI(3, 4),    // 4018: (4018+4)+4*4 = 402C -> R3 = 64
                 MovLI(8, 2),    // 401A: (4018+4)+2*4 = 4024 -> R8 = kSrcTable
                 MovLI(9, 2),    // 401C: (401C+4)+2*4 = 4028 -> R9 = kDstArea
                 Bra(0xFF3),     // 401E: bra loop: 401E+4+(-13)*2 = 4008
                 kNop,           // 4020: delay slot
                 kNop,           // 4022: padding (literal pool must be longword-aligned)
             });
    PokeWramHigh32(saturn, kMainLoop + 0x24, kSrcTable);
    PokeWramHigh32(saturn, kMainLoop + 0x28, kDstArea);
    PokeWramHigh32(saturn, kMainLoop + 0x2C, kLoopCount);
    PokeWramHigh32(saturn, kMainLoop + 0x30, 0xFFFFFE12);

    // FRT OCI handler. Reading FTCSR arms the flag clear (FTCSR.mask); writing 0x01 then clears
    // OCFA while keeping CCLRA = 1 (WriteFTCSR<false> in sh2_frt.hpp).
    PokeCode(saturn, kHandler,
             {
                 AddI(10, 1),    // 5000: add #1,R10
                 MovLI(4, 3),    // 5002: (5000+4)+3*4 = 5010 -> R4 = 0xFFFFFE11 (FTCSR)
                 MovBL(5, 4),    // 5004: mov.b @R4,R5 (read FTCSR)
                 MovI(5, 1),     // 5006: mov #1,R5 (OCFA = 0, CCLRA = 1)
                 MovBS(4, 5),    // 5008: mov.b R5,@R4
                 kRte,           // 500A: rte
                 kNop,           // 500C: delay slot
                 kNop,           // 500E: padding
             });
    PokeWramHigh32(saturn, kHandler + 0x10, 0xFFFFFE11);

    // Vector table
    PokeWramHigh32(saturn, kVbr + kFrtOciVector * 4, kHandler);

    // Source table: nonzero data, so the running sum stored to the destination area is nonzero.
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        PokeWramHigh32(saturn, kSrcTable + i * 4, 0x9E3779B9u * (i + 1));
    }

    // On-chip FRT and INTC setup (byte writes; on-chip regs are 0xFFFFFE00 + (address & 0x1FF),
    // see SH2::OnChipRegWriteByte in sh2.cpp).
    auto &ctx = sh2.GetJitContext();
    ctx.write(ctx.sh2, 0xFFFFFE17, 1, 0xE0); // TOCR: OCRS = 0, OCR writes go to OCRA
    ctx.write(ctx.sh2, 0xFFFFFE14, 1, 0x02); // OCRH -> TEMP
    ctx.write(ctx.sh2, 0xFFFFFE15, 1, 0x00); // OCRL: OCRA = 0x0200 (512 ticks * 8 = 4096 cycles)
    ctx.write(ctx.sh2, 0xFFFFFE16, 1, 0x00); // TCR: CKS = 0, internal clock / 8
    ctx.write(ctx.sh2, 0xFFFFFE12, 1, 0x00); // FRCH -> TEMP
    ctx.write(ctx.sh2, 0xFFFFFE13, 1, 0x00); // FRCL: FRC = 0
    ctx.write(ctx.sh2, 0xFFFFFE11, 1, 0x01); // FTCSR: CCLRA = 1 (clear FRC on compare match A)
    ctx.write(ctx.sh2, 0xFFFFFE67, 1, kFrtOciVector); // VCRC low byte: FRT OCI vector
    ctx.write(ctx.sh2, 0xFFFFFE60, 1, 0x0A);          // IPRB high byte, bits 3-0: FRT level 10
    ctx.write(ctx.sh2, 0xFFFFFE10, 1, 0x09);          // TIER: OCIAE = 1 (bit 0 reads as one)

    // CPU state, with the fetch buffer refilled to match memory at the new PC.
    ymir::savestate::SH2SaveState state{};
    sh2.SaveState(state);
    state.PC = kMainLoop;
    // Interrupt mask 1, not 0: the master's IRL source sits at level 1 / vector 0x40 after reset
    // (INTC levels[IRL] = 1 with the null IPL, SCU IMS = 0xBFFF), and RecalcInterrupts (run by the
    // handler's FTCSR write) always re-raises IRL at that level. With mask 0 the CPU takes that
    // spurious IRL after an RTE and jumps through an empty vector. Mask 1 blocks only it; the
    // level-10 FRT interrupt still gets through.
    state.SR = 0x10;
    state.VBR = kVbr;
    state.R[2] = 0;
    state.R[3] = 0;
    state.R[10] = 0;
    state.R[15] = kStack;
    state.delaySlot = false;
    state.delaySlotTarget = 0;
    state.intrAllow = true;
    state.wbReg = 0xFF; // no pending load write-back
    state.sleep = false;
    state.forceFetchOpcodes = false;
    // The two halfwords at PC, as the interpreter's fetch buffer holds them (big-endian WRAM).
    const auto &wram = saturn.mem.WRAMHigh;
    const uint32_t pcOff = kMainLoop & 0xFFFFF;
    state.fetchedOpcodes = (uint32_t{wram[pcOff]} << 24) | (uint32_t{wram[pcOff + 1]} << 16) |
                           (uint32_t{wram[pcOff + 2]} << 8) | wram[pcOff + 3];
    sh2.LoadState(state);
}

} // namespace

// Control: proves the emulator itself is deterministic, so any JIT lockstep failure is the JIT's.
TEST_CASE("Lockstep control: two interpreter cores stay identical", "[lockstep]") {
    // Uses the built-in null IPL program (no BIOS loaded).
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    const auto result = brimir::RunLockstep(*a, *b, 120);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());
    REQUIRE(result.framesRun == 120);
}

TEST_CASE("Lockstep: JIT core matches interpreter core", "[lockstep][jit]") {
    auto jit = MakeLockstepCore(true);
    auto ref = MakeLockstepCore(false);
    const auto result = brimir::RunLockstep(*jit, *ref, 300);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());
    const auto &stats = jit->GetSH2JitExecutor(true)->GetStats();
    // The null IPL runs only a few instructions before SLEEP, so this is a smoke test; still
    // require that at least one compiled block ran.
    REQUIRE(stats.blocksRun > 0);
}

TEST_CASE("CompareCores detects a difference", "[lockstep]") {
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    a->RunFrame();
    b->RunFrame();
    REQUIRE(brimir::CompareCores(*a, *b).empty());
    b->GetSaturn()->mem.WRAMHigh[0x123] ^= 0xFF;
    REQUIRE(brimir::CompareCores(*a, *b).find("WRAMHigh") != std::string::npos);
    b->GetSaturn()->mem.WRAMHigh[0x123] ^= 0xFF;
    REQUIRE(brimir::CompareCores(*a, *b).empty());

    // Slave SH-2 register.
    auto &slave = b->GetSaturn()->slaveSH2;
    ymir::savestate::SH2SaveState original{};
    slave.SaveState(original);
    auto modified = original;
    modified.R[3] ^= 1;
    slave.LoadState(modified);
    {
        const std::string diff = brimir::CompareCores(*a, *b);
        INFO(diff);
        REQUIRE(diff.find("slave SH-2 R3") != std::string::npos);
    }
    slave.LoadState(original);
    REQUIRE(brimir::CompareCores(*a, *b).empty());

    // Framebuffer pixel (after the frame; the frontend-visible output buffer).
    REQUIRE(b->GetFramebuffer() != nullptr);
    REQUIRE(b->GetFramebufferWidth() > 0);
    REQUIRE(b->GetFramebufferHeight() > 0);
    auto *pixels = static_cast<uint8_t *>(const_cast<void *>(b->GetFramebuffer()));
    pixels[0] ^= 0xFF;
    {
        const std::string diff = brimir::CompareCores(*a, *b);
        INFO(diff);
        REQUIRE(diff.find("frame row") != std::string::npos);
    }
}

TEST_CASE("RunLockstep detects an audio difference", "[lockstep][jit]") {
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    REQUIRE(brimir::RunLockstep(*a, *b, 2).divergence.empty());

    // Only audio differs: both cores run the same frames, but b's output of one frame is drained
    // before the lockstep step, so b then yields fewer samples than a.
    a->RunFrame();
    b->RunFrame();
    std::vector<int16_t> scratch(4096 * 2);
    const size_t drained = b->GetAudioSamples(scratch.data(), 4096);
    REQUIRE(drained > 0); // the frame produced audio, so the drain makes a difference
    REQUIRE(brimir::CompareCores(*a, *b).empty());

    const auto result = brimir::RunLockstep(*a, *b, 1);
    INFO(result.divergence);
    REQUIRE(result.divergence.find("audio sample count differs") != std::string::npos);
}

TEST_CASE("CompareCores detects differences outside the SH-2s and WRAM", "[lockstep]") {
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    a->RunFrame();
    b->RunFrame();
    REQUIRE(brimir::CompareCores(*a, *b).empty());

    // VDP VRAM is not visible to the SH-2 diff or WRAM comparison. VDP::Probe has no VDP2 VRAM
    // writer, so this pokes one byte of VDP1 VRAM, which is equally VDP-only state.
    auto &probe = b->GetSaturn()->VDP.GetProbe();
    probe.VDP1WriteVRAM<uint8_t>(0x100, 0x5A);
    const std::string diff = brimir::CompareCores(*a, *b);
    INFO(diff);
    REQUIRE(diff.find("vdp state differs") != std::string::npos);
}

TEST_CASE("Lockstep: interrupt-driven synthetic workload, JIT vs interpreter", "[lockstep][jit]") {
    auto jit = MakeLockstepCore(true);
    auto ref = MakeLockstepCore(false);
    InstallFrtWorkload(*jit);
    InstallFrtWorkload(*ref);

    const auto result = brimir::RunLockstep(*jit, *ref, 240);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());

    // The workload really ran: interrupts were taken and compiled code ran. A passing run measured
    // R10 = 26239 FRT interrupts (about 109 per frame) and blocksRun = 9652633; the thresholds
    // are about 50% of those.
    auto &probe = ref->GetSaturn()->masterSH2.GetProbe();
    CHECK(probe.R(10) > 13000u);                         // FRT interrupts serviced
    CHECK(ref->GetSaturn()->mem.WRAMHigh[0x20003] != 0); // destination area written
    const auto &stats = jit->GetSH2JitExecutor(true)->GetStats();
    CHECK(stats.blocksRun > 4800000u);
}

TEST_CASE("Lockstep control: synthetic workload on two interpreter cores", "[lockstep]") {
    auto a = MakeLockstepCore(false);
    auto b = MakeLockstepCore(false);
    InstallFrtWorkload(*a);
    InstallFrtWorkload(*b);
    const auto result = brimir::RunLockstep(*a, *b, 240);
    INFO("frame " << result.framesRun << ": " << result.divergence);
    REQUIRE(result.divergence.empty());
}
