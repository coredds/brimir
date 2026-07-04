// Brimir ST-V Boot Test
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <ymir/debug/sh2_tracer_base.hpp>
#include <ymir/hw/sh2/sh2_disasm.hpp>
#include <ymir/sys/saturn.hpp>

#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace brimir;

// ---------------------------------------------------------------------------
// Minimal SH-2 disassembly text formatter, built on top of the structured
// decoder in ymir::sh2::Disassemble(). Not exhaustive, but good enough to
// read boot-time control flow (branches, compares, loads/stores) instead of
// hand-decoding raw opcode hex.
// ---------------------------------------------------------------------------

static const char *MnemonicName(ymir::sh2::Mnemonic m) {
    using M = ymir::sh2::Mnemonic;
    switch (m) {
    case M::NOP: return "nop";
    case M::SLEEP: return "sleep";
    case M::MOV: return "mov";
    case M::MOVA: return "mova";
    case M::MOVT: return "movt";
    case M::CLRT: return "clrt";
    case M::SETT: return "sett";
    case M::EXTU: return "extu";
    case M::EXTS: return "exts";
    case M::SWAP: return "swap";
    case M::XTRCT: return "xtrct";
    case M::LDC: return "ldc";
    case M::LDS: return "lds";
    case M::STC: return "stc";
    case M::STS: return "sts";
    case M::ADD: return "add";
    case M::ADDC: return "addc";
    case M::ADDV: return "addv";
    case M::AND: return "and";
    case M::NEG: return "neg";
    case M::NEGC: return "negc";
    case M::NOT: return "not";
    case M::OR: return "or";
    case M::ROTCL: return "rotcl";
    case M::ROTCR: return "rotcr";
    case M::ROTL: return "rotl";
    case M::ROTR: return "rotr";
    case M::SHAL: return "shal";
    case M::SHAR: return "shar";
    case M::SHLL: return "shll";
    case M::SHLL2: return "shll2";
    case M::SHLL8: return "shll8";
    case M::SHLL16: return "shll16";
    case M::SHLR: return "shlr";
    case M::SHLR2: return "shlr2";
    case M::SHLR8: return "shlr8";
    case M::SHLR16: return "shlr16";
    case M::SUB: return "sub";
    case M::SUBC: return "subc";
    case M::SUBV: return "subv";
    case M::XOR: return "xor";
    case M::DT: return "dt";
    case M::CLRMAC: return "clrmac";
    case M::MAC: return "mac";
    case M::MUL: return "mul";
    case M::MULS: return "muls";
    case M::MULU: return "mulu";
    case M::DMULS: return "dmuls";
    case M::DMULU: return "dmulu";
    case M::DIV0S: return "div0s";
    case M::DIV0U: return "div0u";
    case M::DIV1: return "div1";
    case M::CMP_EQ: return "cmp/eq";
    case M::CMP_GE: return "cmp/ge";
    case M::CMP_GT: return "cmp/gt";
    case M::CMP_HI: return "cmp/hi";
    case M::CMP_HS: return "cmp/hs";
    case M::CMP_PL: return "cmp/pl";
    case M::CMP_PZ: return "cmp/pz";
    case M::CMP_STR: return "cmp/str";
    case M::TAS: return "tas";
    case M::TST: return "tst";
    case M::BF: return "bf";
    case M::BFS: return "bf/s";
    case M::BT: return "bt";
    case M::BTS: return "bt/s";
    case M::BRA: return "bra";
    case M::BRAF: return "braf";
    case M::BSR: return "bsr";
    case M::BSRF: return "bsrf";
    case M::JMP: return "jmp";
    case M::JSR: return "jsr";
    case M::TRAPA: return "trapa";
    case M::RTE: return "rte";
    case M::RTS: return "rts";
    default: return "???";
    }
}

static const char *SizeSuffix(ymir::sh2::OperandSize sz) {
    using S = ymir::sh2::OperandSize;
    switch (sz) {
    case S::Byte: return ".b";
    case S::Word: return ".w";
    case S::Long: return ".l";
    default: return "";
    }
}

static std::string FormatOperand(const ymir::sh2::Operand &op, uint32 pc) {
    using T = ymir::sh2::Operand::Type;
    char buf[64];
    switch (op.type) {
    case T::None: return "";
    case T::Imm: std::snprintf(buf, sizeof(buf), "#0x%X", op.immDisp); return buf;
    case T::Rn: std::snprintf(buf, sizeof(buf), "R%u", op.reg); return buf;
    case T::AtRn: std::snprintf(buf, sizeof(buf), "@R%u", op.reg); return buf;
    case T::AtRnPlus: std::snprintf(buf, sizeof(buf), "@R%u+", op.reg); return buf;
    case T::AtMinusRn: std::snprintf(buf, sizeof(buf), "@-R%u", op.reg); return buf;
    case T::AtDispRn: std::snprintf(buf, sizeof(buf), "@(0x%X,R%u)", op.immDisp, op.reg); return buf;
    case T::AtR0Rn: std::snprintf(buf, sizeof(buf), "@(R0,R%u)", op.reg); return buf;
    case T::AtDispGBR: std::snprintf(buf, sizeof(buf), "@(0x%X,GBR)", op.immDisp); return buf;
    case T::AtR0GBR: return "@(R0,GBR)";
    case T::AtRnPC: std::snprintf(buf, sizeof(buf), "@R%u", op.reg); return buf;
    case T::AtDispPC:
        std::snprintf(buf, sizeof(buf), "@(0x%X,PC) [target=0x%08X]", op.immDisp, pc + 4 + op.immDisp);
        return buf;
    case T::AtDispPCWordAlign:
        std::snprintf(buf, sizeof(buf), "@(0x%X,PC&~3) [target=0x%08X]", op.immDisp, (pc + 4 & ~3u) + op.immDisp);
        return buf;
    case T::DispPC:
        std::snprintf(buf, sizeof(buf), "0x%08X", pc + 4 + op.immDisp);
        return buf;
    case T::RnPC: std::snprintf(buf, sizeof(buf), "R%u+PC", op.reg); return buf;
    case T::SR: return "SR";
    case T::GBR: return "GBR";
    case T::VBR: return "VBR";
    case T::MACH: return "MACH";
    case T::MACL: return "MACL";
    case T::PR: return "PR";
    default: return "?";
    }
}

static std::string DisasmText(uint32 pc, uint16 opcode) {
    const auto &instr = ymir::sh2::Disassemble(opcode);
    std::string s = MnemonicName(instr.mnemonic);
    s += SizeSuffix(instr.opSize);
    std::string a = FormatOperand(instr.op1, pc);
    std::string b = FormatOperand(instr.op2, pc);
    if (!a.empty()) {
        s += " ";
        s += a;
    }
    if (!b.empty()) {
        s += ", ";
        s += b;
    }
    return s;
}

// ---------------------------------------------------------------------------
// DiagTracer: keeps a ring buffer of the last executed instructions (with
// disassembly) plus a live call stack (via Call/Return hooks), so that when
// boot gets stuck we can print exactly how execution arrived there instead
// of guessing from a raw hex dump.
// ---------------------------------------------------------------------------

class DiagTracer : public ymir::debug::ISH2Tracer {
public:
    struct Entry {
        uint32 pc;
        uint16 opcode;
        bool delaySlot;
        uint32 r0, r1, r2, r3, r4, r5, r6;
    };

    explicit DiagTracer(ymir::Saturn *saturn, size_t ringSize = 4000)
        : m_saturn(saturn), m_ringSize(ringSize) {}

    void ExecuteInstruction(uint32 pc, uint16 opcode, bool delaySlot) override {
        if (!m_saturn) return;
        ymir::sh2::SH2::Probe p(m_saturn->masterSH2);
        const auto &r = p.R();
        m_ring.push_back(Entry{pc, opcode, delaySlot, r[0], r[1], r[2], r[3], r[4], r[5], r[6]});
        if (m_ring.size() > m_ringSize) m_ring.pop_front();

        // Track progress of the copy-loop countdown register (R4) at pc=0x060152BC
        // to distinguish "one large transfer still running" from "restarting from
        // scratch every frame" (the latter means something upstream never advances).
        ++m_totalInstrs;
        if (pc == 0x060152BCu) {
            ++m_loopHits;
            if (r[4] > m_lastR4 && m_restarts < 100) {
                printf("LOOPRESTART #%llu at instr=%llu: R4 %08X -> %08X (min seen %08X)\n",
                       (unsigned long long)m_restarts, (unsigned long long)m_totalInstrs, m_lastR4, r[4], m_minR4);
                ++m_restarts;
            }
            if (r[4] < m_minR4) m_minR4 = r[4];
            m_lastR4 = r[4];
        }

        // If the previous instruction was a load from IOGA port 2 (coin/start/
        // test/service), the loaded value is now visible in the destination
        // register (this call is "before" the *current* instruction, i.e.
        // "after" the previous one). Print it here before it's overwritten.
        if (m_pendingIOGAWatchReg >= 0) {
            printf("  -> loaded value = 0x%02X (r%d) [frame=%d]\n", r[m_pendingIOGAWatchReg] & 0xFFu,
                   m_pendingIOGAWatchReg, m_frame);
            m_pendingIOGAWatchReg = -1;
        }

        // Detect any memory access whose effective address lands in the IOGA
        // register window (physical 0x00400000-0x0040007F once the SH-2 cache
        // area bits are masked off), to see whether coin/start/test/service
        // (port index 2) is ever actually read on this code path.
        const auto &instr = ymir::sh2::Disassemble(opcode);
        const ymir::sh2::Operand *ops[2] = {&instr.op1, &instr.op2};
        for (int i = 0; i < 2; i++) {
            using T = ymir::sh2::Operand::Type;
            const auto *op = ops[i];
            if (op->type != T::AtRn && op->type != T::AtRnPlus && op->type != T::AtMinusRn &&
                op->type != T::AtDispRn && op->type != T::AtR0Rn) {
                continue;
            }
            uint32 addr = r[op->reg];
            if (op->type == T::AtDispRn) addr += static_cast<uint32>(op->immDisp);
            if (op->type == T::AtR0Rn) addr += r[0];
            const uint32 physAddr = addr & 0x1FFFFFFFu;
            if (physAddr >= 0x00400000u && physAddr <= 0x0040007Fu) {
                const uint32 iogaAddr = (physAddr >> 1) & 0x3Fu;
                const bool logThis = (iogaAddr == 2) ? (m_iogaPort2Accesses < 600) : (m_iogaAccessLogs < 200);
                if (logThis) {
                    printf("IOGAACCESS frame=%d pc=%08X %-28s addr=%08X(phys=%08X port=%u) %s r0=%08X r2=%08X r3=%08X\n",
                           m_frame, pc, DisasmText(pc, opcode).c_str(), addr, physAddr, iogaAddr,
                           op->read ? "READ" : "WRITE", r[0], r[2], r[3]);
                    ++m_iogaAccessLogs;
                    if (op->read) {
                        const auto *other = ops[1 - i];
                        if (other->type == T::Rn && other->write) {
                            m_pendingIOGAWatchReg = other->reg;
                        }
                    }
                }
                ++m_iogaAccessTotal;
                if (iogaAddr == 2) ++m_iogaPort2Accesses;
            }
        }
    }

    void ReportIOGAStats() const {
        printf("--- IOGA access stats: total=%llu port2(coin/start/test/service)=%llu loggedFirst=%llu ---\n",
               (unsigned long long)m_iogaAccessTotal, (unsigned long long)m_iogaPort2Accesses,
               (unsigned long long)m_iogaAccessLogs);
    }

    void ReportLoopStats() const {
        printf("--- Copy-loop (pc=0x060152BC) stats: hits=%llu restarts=%llu minR4=%08X lastR4=%08X totalInstrs=%llu ---\n",
               (unsigned long long)m_loopHits, (unsigned long long)m_restarts, m_minR4, m_lastR4,
               (unsigned long long)m_totalInstrs);
    }

    void SetFrame(int frame) { m_frame = frame; }

    void Call(uint32 target) override {
        m_callStack.push_back(target);
    }

    void Return(uint32 target) override {
        if (!m_callStack.empty()) m_callStack.pop_back();
    }

    void DumpRing(size_t lastN) const {
        printf("--- Last %zu executed instructions (disassembly) ---\n", (std::min)(lastN, m_ring.size()));
        size_t start = m_ring.size() > lastN ? m_ring.size() - lastN : 0;
        for (size_t i = start; i < m_ring.size(); i++) {
            const auto &e = m_ring[i];
            printf("  pc=%08X op=%04X%s  %-28s r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X\n",
                   e.pc, e.opcode, e.delaySlot ? " ds" : "   ", DisasmText(e.pc, e.opcode).c_str(), e.r0, e.r1, e.r2,
                   e.r3, e.r4, e.r5, e.r6);
        }
    }

    void DumpCallStack() const {
        printf("--- Call stack (depth=%zu) ---\n", m_callStack.size());
        for (size_t i = 0; i < m_callStack.size(); i++) {
            printf("  [%zu] 0x%08X\n", i, m_callStack[i]);
        }
    }

private:
    ymir::Saturn *m_saturn = nullptr;
    size_t m_ringSize;
    std::deque<Entry> m_ring;
    std::vector<uint32> m_callStack;
    unsigned long long m_totalInstrs = 0;
    unsigned long long m_loopHits = 0;
    unsigned long long m_restarts = 0;
    uint32 m_minR4 = 0xFFFFFFFFu;
    uint32 m_lastR4 = 0;
    unsigned long long m_iogaAccessTotal = 0;
    unsigned long long m_iogaPort2Accesses = 0;
    unsigned long long m_iogaAccessLogs = 0;
    int m_pendingIOGAWatchReg = -1;
    int m_frame = 0;
};

static void DumpCartCS0(ymir::Saturn* saturn) {
    printf("\n--- Cartridge CS0 at 0x02000000 ---\n");
    for (uint32 off = 0; off < 0x100; off += 16) {
        uint32 addr = 0x02000000u + off;
        printf("  +%04X: %04X %04X %04X %04X %04X %04X %04X %04X\n", off,
               saturn->mainBus.Peek<uint16>(addr), saturn->mainBus.Peek<uint16>(addr+2),
               saturn->mainBus.Peek<uint16>(addr+4), saturn->mainBus.Peek<uint16>(addr+6),
               saturn->mainBus.Peek<uint16>(addr+8), saturn->mainBus.Peek<uint16>(addr+10),
               saturn->mainBus.Peek<uint16>(addr+12), saturn->mainBus.Peek<uint16>(addr+14));
    }
}

static void DumpBus(ymir::Saturn* saturn, uint32 addr, uint32 count) {
    printf("--- Bus peek at 0x%08X ---\n", addr);
    for (uint32 off = 0; off < count; off += 16) {
        uint32 a = addr + off;
        printf("  +%04X: %04X %04X %04X %04X %04X %04X %04X %04X\n", off,
               saturn->mainBus.Peek<uint16>(a), saturn->mainBus.Peek<uint16>(a+2),
               saturn->mainBus.Peek<uint16>(a+4), saturn->mainBus.Peek<uint16>(a+6),
               saturn->mainBus.Peek<uint16>(a+8), saturn->mainBus.Peek<uint16>(a+10),
               saturn->mainBus.Peek<uint16>(a+12), saturn->mainBus.Peek<uint16>(a+14));
    }
}

static void DumpDisasm(ymir::Saturn* saturn, uint32 addr, uint32 count) {
    printf("--- Disassembly at 0x%08X ---\n", addr);
    for (uint32 off = 0; off < count; off += 2) {
        uint32 a = addr + off;
        uint16 opcode = saturn->mainBus.Peek<uint16>(a);
        printf("  %08X: %04X  %s\n", a, opcode, DisasmText(a, opcode).c_str());
    }
}

static void CompareBusReads(ymir::Saturn* saturn, uint32 a, uint32 b, const char* label) {
    printf("--- Mirror compare: %s ---\n", label);
    printf("  A=0x%08X  B=0x%08X\n", a, b);
    printf("  byte: A=%02X B=%02X  word: A=%04X B=%04X  long: A=%08X B=%08X\n",
           saturn->mainBus.Peek<uint8>(a), saturn->mainBus.Peek<uint8>(b),
           saturn->mainBus.Peek<uint16>(a), saturn->mainBus.Peek<uint16>(b),
           saturn->mainBus.Peek<uint32>(a), saturn->mainBus.Peek<uint32>(b));
}

static void CompareSH2Reads(ymir::Saturn* saturn, uint32 a, uint32 b, const char* label) {
    ymir::sh2::SH2::Probe probe(saturn->masterSH2);
    printf("--- Master SH-2 MemPeek compare: %s ---\n", label);
    printf("  A=0x%08X  B=0x%08X\n", a, b);
    printf("  byte: A=%02X B=%02X  word: A=%04X B=%04X  long: A=%08X B=%08X\n",
           probe.MemPeekByte(a, false), probe.MemPeekByte(b, false),
           probe.MemPeekWord(a, false), probe.MemPeekWord(b, false),
           probe.MemPeekLong(a, false), probe.MemPeekLong(b, false));
}

static void SearchLong(ymir::Saturn* saturn, uint32 start, uint32 end, uint32 needle) {
    printf("--- Searching 0x%08X..0x%08X for 0x%08X ---\n", start, end, needle);
    int hits = 0;
    for (uint32 addr = start; addr + 4 <= end; addr += 2) {
        if (saturn->mainBus.Peek<uint32>(addr) == needle) {
            printf("  hit @ 0x%08X\n", addr);
            if (++hits >= 40) break;
        }
    }
    if (hits == 0) printf("  no hits\n");
}

TEST_CASE("ST-V Baku Baku boots past CD block area after CKCHG352 NMI", "[stv][integration]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    const char* biosDir = "F:/OneDrive/Roms/BIOS";
    const char* romPath = "C:/Users/david/Downloads/bakubaku.zip";

    bool loaded = core.LoadSTVGame(romPath, biosDir);
    if (!loaded) {
        const auto& err = core.GetLastError();
        WARN("LoadSTVGame failed: " << (err.empty() ? "(no message)" : err));
    }
    REQUIRE(loaded);

    ymir::Saturn* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    saturn->EnableDebugTracing(true);
    DiagTracer diagTracer(saturn);
    saturn->masterSH2.UseTracer(&diagTracer);

    // Dump before running any frames
    printf("\n=== Before frame 1 ===\n");
    DumpCartCS0(saturn);
    DumpBus(saturn, 0x00000000u, 0x40);  // BIOS vectors
    DumpBus(saturn, 0x06000000u, 0x40);  // CD block area
    DumpBus(saturn, 0x00000F40u, 0x20);  // Cartridge header area (EEPROM init reads here)
    printf("--- Cartridge at 0x02020000 (program code area) ---\n");
    DumpBus(saturn, 0x02020000u, 0x40);
    printf("--- Cartridge at 0x02200000 (word ROM, first 16LE file) ---\n");
    DumpBus(saturn, 0x02200000u, 0x60);
    printf("--- Cartridge at 0x02400000 (4MB offset, word ROM mpr17970.2) ---\n");
    DumpBus(saturn, 0x02400000u, 0x60);
    printf("--- Cartridge at 0x02000200 (reset vector target) ---\n");
    DumpBus(saturn, 0x02000200u, 0x40);

    bool pcLeftCDBlock = false;
    uint32 lastMPC = 0;
    uint32 lastSPC = 0;

    // The BIOS doesn't reach its attract/idle loop until ~frame 90 (confirmed by
    // prior tracing), so coin/start signals must be applied well after that.
    const int kMaxFrames = 600;
    for (int frame = 1; frame <= kMaxFrames; frame++) {
        diagTracer.SetFrame(frame);
        // Hypothesis: the BIOS is correctly idling on a "insert coin / push start"
        // screen, and nothing in the harness (or, previously, the libretro core)
        // ever presses Start. Simulate a coin insert + a held Start press well
        // after the idle loop is reached, to see if boot proceeds into cartridge
        // code once those signals are provided.
        if (frame == 150) {
            printf("--- Inserting coin at frame 150 ---\n");
            core.InsertCoin();
        }
        if (frame == 160) {
            printf("--- Holding P1 Start from frame 160 onward ---\n");
        }
        core.SetSTVStart(0, frame >= 160);

        core.RunFrame();

        {
            ymir::sh2::SH2::Probe mProbe(saturn->masterSH2);
            ymir::sh2::SH2::Probe sProbe(saturn->slaveSH2);
            lastMPC = mProbe.PC();
            lastSPC = sProbe.PC();
        }

        bool inCDBlock = (lastMPC >= 0x06000000u && lastMPC < 0x06100000u);
        if (!inCDBlock && lastMPC > 0x00010000u) {
            pcLeftCDBlock = true;
            printf("Frame %d: Master PC left CD block! mPC=0x%08X sPC=0x%08X\n", frame, lastMPC, lastSPC);
            break;
        }

        if (frame == 60) {
            printf("--- CD block area at frame 60 ---\n");
            DumpBus(saturn, 0x06000000u, 0x80);
            DumpBus(saturn, 0x06002000u, 0x80);
            DumpBus(saturn, 0x06015000u, 0x80);
        }
        if (frame < 10 || frame % 30 == 0) {
            printf("Frame %d: mPC=0x%08X sPC=0x%08X\n", frame, lastMPC, lastSPC);
        }
    }

    printf("Final state: mPC=0x%08X sPC=0x%08X\n", lastMPC, lastSPC);

    if (!pcLeftCDBlock) {
        // Diagnostic: dump the loop code, registers and likely-polled hardware
        printf("\n=== STUCK LOOP DIAGNOSTICS at mPC=0x%08X ===\n", lastMPC);
        printf("--- Code at mPC-0x40 .. mPC+0x40 ---\n");
        DumpBus(saturn, (lastMPC & ~15u) - 0x40, 0xC0);
        {
            ymir::sh2::SH2::Probe mProbe(saturn->masterSH2);
            auto &R = mProbe.R();
            printf("--- Master SH-2 registers ---\n");
            for (int i = 0; i < 16; i++) {
                printf("  R%02d=0x%08X%s", i, R[i], (i % 4 == 3) ? "\n" : "  ");
            }
            printf("  PC=0x%08X  PR=0x%08X\n", mProbe.PC(), mProbe.PR());
        }
        printf("--- CD block regs at 0x05800000 (CR1/CR2/CR3/CR4/HIRQ) ---\n");
        DumpBus(saturn, 0x05800000u, 0x20);
        printf("--- SCU regs at 0x25FE0000 (IMS=0xA0, IST=0xA4) ---\n");
        DumpBus(saturn, 0x25FE00A0u, 0x10);
        printf("--- VDP2 regs at 0x25F80000 (TVMR, status, vertical timing) ---\n");
        DumpBus(saturn, 0x25F80000u, 0x80);
        printf("--- BIOS input/state buffer near 0x06002860 ---\n");
        DumpBus(saturn, 0x06002860u, 0x40);
        SearchLong(saturn, 0x06000000u, 0x06100000u, 0x06002CC8u);
        printf("--- Code near suspected service/game decision at 0x06024800 ---\n");
        DumpBus(saturn, 0x06024800u, 0x180);
        CompareBusReads(saturn, 0x25E634F8u, 0x05E634F8u, "VDP2 VRAM mirror at R6");
        DumpBus(saturn, 0x25E634C0u, 0x80);
        DumpBus(saturn, 0x05E634C0u, 0x80);
        CompareBusReads(saturn, 0x20400007u, 0x02400007u, "cartridge CS0 mirror at R3");
        CompareSH2Reads(saturn, 0x20400007u, 0x02400007u, "cartridge CS0 mirror at R3");
        CompareBusReads(saturn, 0x00400007u, 0x02400007u, "physical 0x00400000 cartridge mirror");
        CompareSH2Reads(saturn, 0x00400007u, 0x02400007u, "physical 0x00400000 cartridge mirror");
        DumpBus(saturn, 0x20400000u, 0x40);
        DumpBus(saturn, 0x02400000u, 0x40);
        {
            ymir::sh2::SH2::Probe mProbe(saturn->masterSH2);
            auto sr = mProbe.SR();
            auto &intc = mProbe.INTC();
            printf("--- Master SH-2 interrupt state ---\n");
            printf("  SR.u32=0x%08X  ILevel=%u  T=%u  VBR=0x%08X\n", sr.u32, (unsigned)sr.ILevel, (unsigned)sr.T, mProbe.VBR());
            printf("  INTC pending: source=%u level=%u  NMI=%d\n",
                   (unsigned)intc.pending.source, (unsigned)intc.pending.level, (int)intc.NMI);
            printf("  IRL level=%u vector=0x%02X\n",
                   (unsigned)intc.GetLevel(ymir::sh2::InterruptSource::IRL),
                   (unsigned)intc.GetVector(ymir::sh2::InterruptSource::IRL));
        }
        printf("--- SMPC OREG (INTBACK result) ---\n");
        DumpBus(saturn, 0x00010020u, 0x40);
        diagTracer.DumpCallStack();
        diagTracer.ReportLoopStats();
        diagTracer.ReportIOGAStats();
        printf("--- Cached active-high input state (0x06002860 area; +0x0C=port2/coin-start-test-service) ---\n");
        DumpBus(saturn, 0x06002860u, 0x20);
        printf("--- Source buffer for the redraw memcpy (likely char/tile data) ---\n");
        DumpBus(saturn, 0x060FB3E0u, 0x100);
        printf("--- Input-poll routine around 0x060020B8 (reads IOGA port 2) ---\n");
        DumpDisasm(saturn, 0x06002090u, 0x60);
        printf("--- Literal pool for input-poll routine (hw addrs / dest ptrs) ---\n");
        DumpBus(saturn, 0x06002120u, 0x50);
        printf("--- Outer call chain: 0x06015278 (contains the memcpy loop) ---\n");
        DumpDisasm(saturn, 0x06015250u, 0x90);
        printf("--- Outer call chain: 0x0601547E ---\n");
        DumpDisasm(saturn, 0x06015460u, 0x60);
        printf("--- Outer call chain: 0x06013078 ---\n");
        DumpDisasm(saturn, 0x06013050u, 0x60);
        printf("--- Outer call chain: 0x0601258C ---\n");
        DumpDisasm(saturn, 0x06012560u, 0x60);
        diagTracer.DumpRing(400);
        WARN("Master PC still in CD block area after " << kMaxFrames << " frames.");
    }

    CHECK(pcLeftCDBlock);
}
