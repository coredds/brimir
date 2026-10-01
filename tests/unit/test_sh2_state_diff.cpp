// Brimir - SH-2 state diff and test rig bus log tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <brimir/lockstep.hpp>

#include <memory>

using brimir::DiffSH2State;
using brimir::SH2DiffScope;

TEST_CASE("DiffSH2State reports the first differing field", "[jit][diff]") {
    auto rig = std::make_unique<sh2test::Rig>();
    const auto a = rig->State();
    auto b = a;
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::CpuAndPeripherals).empty());

    b.R[5] ^= 1;
    const std::string diff = DiffSH2State(a, b, SH2DiffScope::Cpu, "master ");
    REQUIRE(diff.find("master R5 differs") != std::string::npos);
}

TEST_CASE("DiffSH2State compares peripherals only in the peripheral scope", "[jit][diff]") {
    auto rig = std::make_unique<sh2test::Rig>();
    const auto a = rig->State();
    auto b = a;
    b.frt.FRC ^= 1;
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::Cpu).empty());
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::CpuAndPeripherals).find("frt.FRC") != std::string::npos);

    b = a;
    b.wdt.WTCNT ^= 1;
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::CpuAndPeripherals).find("wdt.WTCNT") != std::string::npos);
    b = a;
    b.intc.pendingLevel ^= 1;
    REQUIRE(DiffSH2State(a, b, SH2DiffScope::Cpu).empty());
    REQUIRE_FALSE(DiffSH2State(a, b, SH2DiffScope::CpuAndPeripherals).empty());
}

TEST_CASE("DiffSH2State compares the cache arrays in the CPU scope", "[jit][diff]") {
    auto rig = std::make_unique<sh2test::Rig>();
    const auto a = rig->State();
    auto b = a;
    b.cache.entries[3].lines[1][7] ^= 0x5A;
    CHECK(DiffSH2State(a, b, SH2DiffScope::Cpu).find("cache.entries[3].lines[1][7] differs") !=
          std::string::npos);

    b = a;
    b.cache.entries[9].tags[2] ^= 0x10;
    CHECK(DiffSH2State(a, b, SH2DiffScope::Cpu).find("cache.entries[9].tags[2] differs") != std::string::npos);

    b = a;
    b.cache.lru[3] ^= 1;
    CHECK(DiffSH2State(a, b, SH2DiffScope::Cpu).find("cache.lru[3] differs") != std::string::npos);
}

TEST_CASE("Test rig logs MMIO accesses and DiffRigs compares the logs", "[jit][diff]") {
    auto a = std::make_unique<sh2test::Rig>();
    auto b = std::make_unique<sh2test::Rig>();
    auto &ctx = a->sh2->GetJitContext();
    ctx.write(ctx.sh2, 0x22000010, 4, 0xCAFEF00D);
    ctx.read(ctx.sh2, 0x22000010, 2, false);
    ctx.busWait(ctx.sh2, 0x22000010, 4, false);
    REQUIRE(a->mmio.log.size() == 3);
    CHECK(a->mmio.log[0].kind == 'W');
    CHECK(a->mmio.log[0].size == 4);
    CHECK(a->mmio.log[0].value == 0xCAFEF00Du);
    CHECK(a->mmio.log[1].kind == 'R');
    CHECK(a->mmio.log[1].value == 0xCAFEu);
    CHECK(a->mmio.log[2].kind == 'B');

    // Same final MMIO contents, different access sequence: still a difference.
    auto &ctxB = b->sh2->GetJitContext();
    ctxB.write(ctxB.sh2, 0x22000010, 4, 0xCAFEF00D);
    REQUIRE(sh2test::DiffRigs(*a, *b).find("MMIO log") != std::string::npos);
}
