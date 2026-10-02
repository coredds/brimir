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

TEST_CASE("IR verifier checks guest code, the tail word and known refills", "[jit][ir]") {
    constexpr uint32_t kStart = 0x06001000;
    // Two instructions plus the tail word, a known refill of the first pair and one of the second.
    const auto make = [&] {
        Block block;
        block.startPC = kStart;
        block.guestInstrCount = 3;
        block.hasTailWord = true;
        block.fetchFromArrays = true;
        block.guestOpcodes = {0x1111, 0x2222, 0x3333, 0x4444};
        Builder b(block);
        b.KnownRefill(kStart, 0x11112222);
        b.KnownRefill(kStart + 4, 0x33334444);
        b.Exit(kStart + 6, 3);
        return block;
    };
    REQUIRE(VerifyBlock(make()).empty());
    CHECK(make().code[0].flag);
    CHECK(make().code[1].imm2 == 0x33334444u);

    SECTION("word count") {
        Block block = make();
        block.hasTailWord = false;
        CHECK(VerifyBlock(block).find("guestOpcodes does not match") != std::string::npos);
    }
    SECTION("tail word without code") {
        Block block = make();
        block.guestOpcodes.clear();
        block.code.erase(block.code.begin(), block.code.begin() + 2);
        CHECK(VerifyBlock(block).find("tail word without guestOpcodes") != std::string::npos);
    }
    SECTION("array fetches without code") {
        Block block = make();
        block.guestOpcodes.clear();
        block.hasTailWord = false;
        block.code.erase(block.code.begin(), block.code.begin() + 2);
        CHECK(VerifyBlock(block).find("fetchFromArrays without guestOpcodes") != std::string::npos);
    }
    SECTION("known refill value") {
        Block block = make();
        block.code[1].imm2 = 0x33334445;
        CHECK(VerifyBlock(block).find("known refill value differs") != std::string::npos);
    }
    SECTION("known refill past the code") {
        Block block = make();
        block.code[1].imm = kStart + 8;
        CHECK(VerifyBlock(block).find("known refill outside guestOpcodes") != std::string::npos);
    }
    SECTION("known refill before the code") {
        Block block = make();
        block.code[0].imm = kStart - 4;
        CHECK(VerifyBlock(block).find("known refill outside guestOpcodes") != std::string::npos);
    }
    SECTION("known refill at an unaligned address") {
        Block block = make();
        block.code[0].imm = kStart + 2;
        block.code[0].imm2 = 0x22223333;
        CHECK(VerifyBlock(block).find("known refill outside guestOpcodes") != std::string::npos);
    }
    SECTION("an unknown refill may read anything") {
        Block block = make();
        block.code[1].flag = false;
        block.code[1].imm = 0x22000000;
        CHECK(VerifyBlock(block).empty());
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

TEST_CASE("IR names the multiply, SR-bit, DIV1, MAC and RMW-cycle ops", "[jit][ir]") {
    const std::pair<Op, const char *> names[] = {
        {Op::Mul, "Mul"},
        {Op::MulHiS, "MulHiS"},
        {Op::MulHiU, "MulHiU"},
        {Op::SetSRBits, "SetSRBits"},
        {Op::Div1, "Div1"},
        {Op::MacW, "MacW"},
        {Op::MacL, "MacL"},
        {Op::AddAccessCyclesRMWByte, "AddAccessCyclesRMWByte"},
    };
    for (const auto &[op, name] : names) {
        CHECK(std::string(OpName(op)) == name);
    }
    CHECK(std::string(OpName(Op::Load)) == "Load");
    CHECK(std::string(OpName(Op::ExitDynamic)) == "ExitDynamic");
}

TEST_CASE("IR builder records the new ops' operands", "[jit][ir]") {
    Block block;
    block.guestInstrCount = 1;
    Builder b(block);
    const ValueId x = b.GetReg(1);
    const ValueId y = b.GetReg(2);
    const ValueId mul = b.Mul(x, y);
    const ValueId hiS = b.MulHiS(x, y);
    const ValueId hiU = b.MulHiU(x, y);
    const ValueId div = b.Div1(x, y, true);
    b.SetSRBits(mul, 0x301);
    b.MacW(hiS, hiU);
    b.MacL(div, x);
    b.AddAccessCyclesRMWByte(y);
    b.Exit(0, 1);
    REQUIRE(VerifyBlock(block).empty());
    REQUIRE(block.code[5].op == Op::Div1);
    REQUIRE(block.code[5].flag);
    REQUIRE(block.code[5].dst == div);
    REQUIRE(block.code[6].op == Op::SetSRBits);
    REQUIRE(block.code[6].a == mul);
    REQUIRE(block.code[6].imm == 0x301u);
    REQUIRE(block.code[7].a == hiS);
    REQUIRE(block.code[7].b == hiU);
    REQUIRE(block.code[9].op == Op::AddAccessCyclesRMWByte);
    REQUIRE(block.code[9].a == y);
}

TEST_CASE("IR verifier restricts SetSRBits to T, S, Q and M", "[jit][ir]") {
    for (const uint32_t mask : {0x10u, 0xF0u, 0x400u, 0x80000000u}) {
        Block block;
        block.guestInstrCount = 1;
        Builder b(block);
        b.SetSRBits(b.Const(0), mask);
        b.Exit(0, 1);
        CHECK(VerifyBlock(block).find("SetSRBits mask") != std::string::npos);
    }
    Block block;
    block.guestInstrCount = 1;
    Builder b(block);
    b.SetSRBits(b.Const(0), 0x303);
    b.Exit(0, 1);
    CHECK(VerifyBlock(block).empty());
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
