// Brimir - SH-2 JIT IR tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/jit/ir.hpp>

#include <string>
#include <utility>

using namespace brimir::jit;

TEST_CASE("IR builder assigns sequential values and records operands", "[jit][ir]") {
    Block block;
    block.guestInstrCount = 1;
    Builder b(block);
    const ValueId r1 = b.GetReg(1);
    const ValueId k = b.Const(5);
    const ValueId sum = b.Add(r1, k);
    b.SetReg(1, sum);
    b.AddCycles(1);
    b.Exit(0x06000002, 1);

    REQUIRE(r1 == 0);
    REQUIRE(k == 1);
    REQUIRE(sum == 2);
    REQUIRE(block.numValues == 3);
    REQUIRE(block.code.size() == 6);
    REQUIRE(block.code[2].op == Op::Add);
    REQUIRE(block.code[2].a == r1);
    REQUIRE(block.code[2].b == k);
    REQUIRE(block.code[3].imm == 1);
    REQUIRE(block.code[5].op == Op::Exit);
    REQUIRE(block.code[5].imm == 0x06000002u);
    REQUIRE(block.code[5].retired == 1);
    REQUIRE(VerifyBlock(block).empty());
}

TEST_CASE("IR verifier accepts an empty fallback block", "[jit][ir]") {
    Block block;
    REQUIRE(VerifyBlock(block).empty());
    block.code.push_back(Inst{});
    REQUIRE_FALSE(VerifyBlock(block).empty());
}

TEST_CASE("IR verifier rejects malformed blocks", "[jit][ir]") {
    SECTION("missing exit") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.AddCycles(1);
        REQUIRE(VerifyBlock(block).find("does not end with an exit") != std::string::npos);
    }
    SECTION("exit before the end") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.Exit(0, 0);
        b.AddCycles(1);
        b.Exit(0, 1);
        REQUIRE(VerifyBlock(block).find("exit before end") != std::string::npos);
    }
    SECTION("use of an undefined value") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetReg(0, 7);
        b.Exit(0, 1);
        block.numValues = 8;
        REQUIRE(VerifyBlock(block).find("undefined value") != std::string::npos);
    }
    SECTION("bad access size") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        const ValueId addr = b.Const(0x06000000);
        b.Load(addr, 3, false);
        b.Exit(0, 1);
        REQUIRE(VerifyBlock(block).find("invalid access size") != std::string::npos);
    }
    SECTION("register index out of range") {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.GetReg(16);
        b.Exit(0, 1);
        REQUIRE(VerifyBlock(block).find("register index") != std::string::npos);
    }
}

TEST_CASE("IR names the logic, shift, compare and system-register ops", "[jit][ir]") {
    const std::pair<Op, const char *> names[] = {
        {Op::And, "And"},
        {Op::Or, "Or"},
        {Op::Xor, "Xor"},
        {Op::Not, "Not"},
        {Op::Shl, "Shl"},
        {Op::Shr, "Shr"},
        {Op::Sar, "Sar"},
        {Op::CmpGtU, "CmpGtU"},
        {Op::CmpGeU, "CmpGeU"},
        {Op::CmpGtS, "CmpGtS"},
        {Op::CmpGeS, "CmpGeS"},
        {Op::GetGBR, "GetGBR"},
        {Op::SetGBR, "SetGBR"},
        {Op::GetVBR, "GetVBR"},
        {Op::SetVBR, "SetVBR"},
        {Op::SetPR, "SetPR"},
        {Op::GetSR, "GetSR"},
        {Op::SetSR, "SetSR"},
        {Op::GetMACH, "GetMACH"},
        {Op::GetMACL, "GetMACL"},
        {Op::SetMACH, "SetMACH"},
        {Op::SetMACL, "SetMACL"},
        {Op::ClearIntrAllow, "ClearIntrAllow"},
        {Op::SetIntrAllow, "SetIntrAllow"},
        {Op::GetDelayTarget, "GetDelayTarget"},
        {Op::Load, "Load"},
    };
    for (const auto &[op, name] : names) {
        CHECK(std::string(OpName(op)) == name);
    }
}

TEST_CASE("IR verifier rejects shift amounts outside 1..31", "[jit][ir]") {
    for (const uint32_t amount : {0u, 32u}) {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetReg(1, b.Shl(b.GetReg(1), amount));
        b.Exit(0, 1);
        CHECK(VerifyBlock(block).find("shift amount") != std::string::npos);
    }
    Block block;
    block.guestInstrCount = 1;
    Builder b(block);
    const ValueId r = b.GetReg(1);
    b.SetReg(1, b.Shl(r, 1));
    b.SetReg(2, b.Shr(r, 31));
    b.SetReg(3, b.Sar(r, 16));
    b.Exit(0, 1);
    CHECK(VerifyBlock(block).empty());
}

TEST_CASE("IR printer lists every instruction", "[jit][ir]") {
    Block block;
    block.startPC = 0x06001000;
    block.guestInstrCount = 1;
    Builder b(block);
    const ValueId addr = b.GetReg(4);
    const ValueId value = b.Load(addr, 4, false);
    b.SetReg(2, value);
    b.Exit(0x06001002, 1);

    const std::string text = PrintBlock(block);
    REQUIRE(text.find("block @06001000") != std::string::npos);
    REQUIRE(text.find("v1 = Load.32 v0") != std::string::npos);
    REQUIRE(text.find("SetReg v1") != std::string::npos);
    REQUIRE(text.find("Exit") != std::string::npos);
    REQUIRE(std::string(OpName(Op::ExitIfBusWait)) == "ExitIfBusWait");
    REQUIRE(std::string(OpName(Op::SyncCycles)) == "SyncCycles");
    REQUIRE(std::string(OpName(Op::CheckBoundary)) == "CheckBoundary");
}
