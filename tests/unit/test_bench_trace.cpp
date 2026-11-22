// brimir_bench's --trace-msh2 and --trace-io (tools/bench_trace.hpp, header-only): the line formats the MAME
// comparison relies on, the traced address windows, and the lines a real master SH-2 writes for a store to an SCU
// register and a load from the SMPC.
#include "catch_amalgamated.hpp"

#include "../../tools/bench_trace.hpp"

#include <ymir/sys/saturn.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

using namespace brimir_tools;

namespace {

std::string IOLine(int frame, uint32_t pc, bool write, unsigned size, uint32_t address, uint32_t value) {
    char buf[48];
    const int n = FormatIOLine(buf, sizeof(buf), frame, pc, write, size, address, value);
    return std::string(buf, static_cast<size_t>(n));
}

std::string ReadAll(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("Bench trace: I/O and PC line formats", "[stv][bench-trace]") {
    CHECK(IOLine(0, 0x2000'021A, true, 4, 0x05FE'00C4, 1) == "0 2000021A W32 05FE00C4 00000001\n");
    CHECK(IOLine(12, 0x0000'4670, false, 1, 0x0010'0063, 0x1FF) == "12 00004670 R8 00100063 FF\n");
    CHECK(IOLine(1199, 0x0601'52BC, false, 2, 0x0580'0008, 0xFFFF'ABCD) == "1199 060152BC R16 05800008 ABCD\n");

    char buf[24];
    const int n = FormatPCLine(buf, 42, 0x2000'0202);
    CHECK(std::string(buf, static_cast<size_t>(n)) == "42 20000202\n");
}

TEST_CASE("Bench trace: traced address windows", "[stv][bench-trace]") {
    constexpr uint32_t kCartEnd = 0x0340'0000; // a 20 MiB image
    CHECK(IsTracedIOAddress(0x0010'0063, kCartEnd));
    CHECK_FALSE(IsTracedIOAddress(0x0010'0080, kCartEnd));
    CHECK(IsTracedIOAddress(0x0040'0001, kCartEnd));
    CHECK(IsTracedIOAddress(0x0040'007F, kCartEnd));
    CHECK_FALSE(IsTracedIOAddress(0x0200'0000, kCartEnd));
    CHECK_FALSE(IsTracedIOAddress(0x033F'FFFF, kCartEnd));
    CHECK(IsTracedIOAddress(0x0340'0000, kCartEnd));
    CHECK(IsTracedIOAddress(0x04FF'FFF0, kCartEnd));
    CHECK(IsTracedIOAddress(0x057F'FFFC, kCartEnd));
    CHECK(IsTracedIOAddress(0x0589'0008, kCartEnd));
    CHECK_FALSE(IsTracedIOAddress(0x0590'0000, kCartEnd));
    CHECK(IsTracedIOAddress(0x05FE'00CC, kCartEnd));
    CHECK_FALSE(IsTracedIOAddress(0x05FE'00D0, kCartEnd));
    CHECK_FALSE(IsTracedIOAddress(0x0600'0000, kCartEnd));

    uint32_t phys = 0;
    CHECK(PhysicalAddress(0x2010'0063, phys));
    CHECK(phys == 0x0010'0063);
    CHECK(PhysicalAddress(0x05FE'00A0, phys));
    CHECK(phys == 0x05FE'00A0);
    CHECK_FALSE(PhysicalAddress(0xFFFF'FE10, phys)); // on-chip registers
    CHECK_FALSE(PhysicalAddress(0x4000'0000, phys)); // associative purge
}

TEST_CASE("Bench trace: data registers of loads and stores", "[stv][bench-trace]") {
    CHECK(IOLoadStoreRegister(0x2122).reg == 2);  // mov.l r2,@r1
    CHECK(IOLoadStoreRegister(0x2126).reg == 2);  // mov.l r2,@-r1
    CHECK(IOLoadStoreRegister(0x0125).reg == 2);  // mov.w r2,@(r0,r1)
    CHECK(IOLoadStoreRegister(0x1123).reg == 2);  // mov.l r2,@(12,r1)
    CHECK(IOLoadStoreRegister(0x8013).reg == 0);  // mov.b r0,@(3,r1)
    CHECK(IOLoadStoreRegister(0xC213).reg == 0);  // mov.l r0,@(0x4C,gbr)
    CHECK(IOLoadStoreRegister(0x6430).reg == 4);  // mov.b @r3,r4
    CHECK(IOLoadStoreRegister(0x6436).reg == 4);  // mov.l @r3+,r4
    CHECK(IOLoadStoreRegister(0x043D).reg == 4);  // mov.w @(r0,r3),r4
    CHECK(IOLoadStoreRegister(0x5431).reg == 4);  // mov.l @(4,r3),r4
    CHECK(IOLoadStoreRegister(0x8531).reg == 0);  // mov.w @(2,r3),r0
    CHECK(IOLoadStoreRegister(0xC613).reg == 0);  // mov.l @(0x4C,gbr),r0
    CHECK(IOLoadStoreRegister(0xCD01).reg == -1); // and.b #1,@(r0,gbr): peeked
    CHECK(IOLoadStoreRegister(0x411B).reg == -1); // tas.b @r1: peeked
    CHECK(IOLoadStoreRegister(0x4122).reg == -1); // sts.l pr,@-r1: peeked
    CHECK(IOLoadStoreRegister(0x0009).reg == -1); // nop
}

TEST_CASE("Bench trace: master SH-2 store to an SCU register and load from the SMPC", "[stv][bench-trace]") {
    auto saturn = std::make_unique<ymir::Saturn>();
    constexpr uint32_t kBase = 0x0600'4000;
    const uint16_t program[] = {
        0xD103, // mov.l @(lit1,pc),r1   r1 = 0x25FE00B0 (SCU A-bus set register, cache-through mirror)
        0xD204, // mov.l @(lit2,pc),r2   r2 = 0x12345678
        0xD304, // mov.l @(lit3,pc),r3   r3 = 0x20100063 (SMPC SF, cache-through mirror)
        0x2122, // mov.l r2,@r1
        0x6430, // mov.b @r3,r4
        0x0009, // nop
        0xAFFE, // bra $
        0x0009, // nop
    };
    for (size_t i = 0; i < std::size(program); ++i) {
        saturn->mainBus.Write<uint16_t>(kBase + static_cast<uint32_t>(i * 2), program[i]);
    }
    saturn->mainBus.Write<uint32_t>(kBase + 0x10, 0x25FE'00B0);
    saturn->mainBus.Write<uint32_t>(kBase + 0x14, 0x1234'5678);
    saturn->mainBus.Write<uint32_t>(kBase + 0x18, 0x2010'0063);
    saturn->masterSH2.GetProbe().PC() = kBase;

    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "brimir_test_bench_trace";
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    const auto pcPath = dir / "msh2.tr";
    const auto ioPath = dir / "io.tr";
    std::FILE *pcFile = std::fopen(pcPath.string().c_str(), "wb");
    std::FILE *ioFile = std::fopen(ioPath.string().c_str(), "wb");
    REQUIRE(pcFile != nullptr);
    REQUIRE(ioFile != nullptr);

    {
        SH2TraceWriter tracer(saturn->masterSH2, pcFile, ioFile, 0x0300'0000);
        tracer.SetFrame(7);
        saturn->EnableDebugTracing(true);
        saturn->masterSH2.UseTracer(&tracer);
        for (int i = 0; i < 6; ++i) { // up to the nop: the load's value is resolved when the nop starts
            saturn->StepMasterSH2();
        }
        tracer.Flush();
        saturn->masterSH2.UseTracer(nullptr);
        CHECK(tracer.IOLines() == 2);
    }
    std::fclose(pcFile);
    std::fclose(ioFile);

    CHECK(ReadAll(pcPath) == "7 06004000\n7 06004002\n7 06004004\n7 06004006\n7 06004008\n7 0600400A\n");
    const uint8_t sf = saturn->mainBus.Peek<uint8_t>(0x0010'0063);
    CHECK(ReadAll(ioPath) == "7 06004006 W32 05FE00B0 12345678\n" + IOLine(7, 0x0600'4008, false, 1, 0x0010'0063, sf));
    CHECK(saturn->masterSH2.GetProbe().R()[4] == static_cast<uint32_t>(static_cast<int8_t>(sf)));

    std::filesystem::remove_all(dir, ec);
}
