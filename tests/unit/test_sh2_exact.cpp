// Brimir - SH-2 interpreter cycle-exact regression tests (performance round, Task 2)
// Licensed under GPL-3.0
//
// Each test runs a small program on the isolated SH-2 rig the way Saturn::Run drives the master SH-2: scheduler steps
// of varying length, each split into SH2::Advance calls of at most `step` cycles, with the global cycle counter
// advanced by the executed cycles. After every scheduler step the full SH-2 state (CPU, BSC, DMAC, WDT, DIVU, FRT,
// INTC, cache, standby) and the cycle counter are folded into a digest; at the end the program's log area and the
// MMIO access log are folded in too. The expected digests were recorded on the interpreter before the Task 2
// speed-ups (40aa9d1): any change in what the SH-2 does, or when (one cycle earlier or later), changes them.
//
// Covered mechanisms: FRT/WDT reads and writes after long busy and SLEEP idle stretches, FRT compare/overflow and
// WDT interval interrupts, interrupts raised by INTC (IPR) and SR writes, DMAC transfers as channels and DMAOR are
// toggled (with the transfer-end interrupt), and instruction fetch across a 64 KiB bus page boundary, from a handler
// page and from a page remapped while code runs from it (both fetch-buffer alignments), misaligned loads and stores,
// and stores to a read-only array page (Task 2b). Every program runs with the
// plain interpreter and with SH-2 cache emulation (Advance<false, true>, cache enabled), and with two scheduler
// patterns.

#include "catch_amalgamated.hpp"
#include "sh2_test_rig.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using sh2test::Rig;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Encoders and a tiny assembler with labels

uint16_t Nm(uint16_t base, uint32_t n, uint32_t m) {
    return static_cast<uint16_t>(base | (n << 8) | (m << 4));
}
uint16_t NImm(uint16_t base, uint32_t n, uint32_t imm) {
    return static_cast<uint16_t>(base | (n << 8) | (imm & 0xFF));
}
constexpr uint16_t kNop = 0x0009;
constexpr uint16_t kRte = 0x002B;
constexpr uint16_t kSleepOp = 0x001B;
uint16_t AddI(uint32_t n, uint32_t imm) { return NImm(0x7000, n, imm); }
uint16_t MovI(uint32_t n, uint32_t imm) { return NImm(0xE000, n, imm); }
uint16_t MovR(uint32_t n, uint32_t m) { return Nm(0x6003, n, m); }
uint16_t MovBL(uint32_t n, uint32_t m) { return Nm(0x6000, n, m); } // mov.b @Rm,Rn
uint16_t MovWL(uint32_t n, uint32_t m) { return Nm(0x6001, n, m); } // mov.w @Rm,Rn
uint16_t MovLL(uint32_t n, uint32_t m) { return Nm(0x6002, n, m); } // mov.l @Rm,Rn
uint16_t MovLP(uint32_t n, uint32_t m) { return Nm(0x6006, n, m); } // mov.l @Rm+,Rn
uint16_t MovBS(uint32_t n, uint32_t m) { return Nm(0x2000, n, m); } // mov.b Rm,@Rn
uint16_t MovWS(uint32_t n, uint32_t m) { return Nm(0x2001, n, m); } // mov.w Rm,@Rn
uint16_t MovLS(uint32_t n, uint32_t m) { return Nm(0x2002, n, m); } // mov.l Rm,@Rn
uint16_t Tst(uint32_t n, uint32_t m) { return Nm(0x2008, n, m); }
uint16_t Shlr8(uint32_t n) { return static_cast<uint16_t>(0x4019 | (n << 8)); }
uint16_t Dt(uint32_t n) { return static_cast<uint16_t>(0x4010 | (n << 8)); }
uint16_t LdcSr(uint32_t m) { return static_cast<uint16_t>(0x400E | (m << 8)); }
uint16_t Jmp(uint32_t m) { return static_cast<uint16_t>(0x402B | (m << 8)); }

struct Asm {
    std::vector<uint16_t> words;
    std::map<std::string, size_t> labels;
    struct Fixup {
        size_t index;
        std::string label;
        bool bra; // 12-bit displacement (BRA) vs 8-bit (BT/BF)
    };
    std::vector<Fixup> fixups;

    Asm &operator<<(uint16_t w) {
        words.push_back(w);
        return *this;
    }
    void Label(const std::string &name) { labels[name] = words.size(); }
    void Bt(const std::string &l) { Branch(0x8900, l, false); }
    void Bf(const std::string &l) { Branch(0x8B00, l, false); }
    void Bra(const std::string &l) { Branch(0xA000, l, true); }
    // mov.l Rm,@R14 ; add #4,R14
    void Log(uint32_t m) {
        words.push_back(MovLS(14, m));
        words.push_back(AddI(14, 4));
    }
    std::vector<uint16_t> Finish() {
        for (const Fixup &f : fixups) {
            const int disp = static_cast<int>(labels.at(f.label)) - static_cast<int>(f.index) - 2;
            words[f.index] |= static_cast<uint16_t>(disp & (f.bra ? 0xFFF : 0xFF));
        }
        return words;
    }

private:
    void Branch(uint16_t op, const std::string &l, bool bra) {
        fixups.push_back({words.size(), l, bra});
        words.push_back(op);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Digest

struct Digest {
    uint64_t h = 0xCBF29CE484222325ull;
    void Byte(uint8_t b) {
        h ^= b;
        h *= 0x100000001B3ull;
    }
    void Add(uint64_t v, int bytes = 8) {
        for (int i = 0; i < bytes; ++i) {
            Byte(static_cast<uint8_t>(v >> (8 * i)));
        }
    }
};

void AddState(Digest &d, const ymir::savestate::SH2SaveState &s) {
    for (uint32_t r : s.R) {
        d.Add(r, 4);
    }
    for (uint32_t v : {s.PC, s.PR, s.MACL, s.MACH, s.SR, s.GBR, s.VBR, s.delaySlotTarget, s.fetchedOpcodes}) {
        d.Add(v, 4);
    }
    d.Add(s.delaySlot, 1);
    d.Add(s.intrAllow, 1);
    d.Add(s.wbReg, 1);
    for (uint16_t v : {s.bsc.BCR1, s.bsc.BCR2, s.bsc.WCR, s.bsc.MCR, s.bsc.RTCSR, s.bsc.RTCNT, s.bsc.RTCOR}) {
        d.Add(v, 2);
    }
    d.Add(s.dmac.DMAOR, 4);
    for (const auto &ch : s.dmac.channels) {
        for (uint32_t v : {ch.SAR, ch.DAR, ch.TCR, ch.CHCR}) {
            d.Add(v, 4);
        }
        d.Add(ch.DRCR, 1);
    }
    d.Add(s.wdt.WTCSR, 1);
    d.Add(s.wdt.WTCNT, 1);
    d.Add(s.wdt.RSTCSR, 1);
    d.Add(s.wdt.cycleCount);
    d.Add(s.wdt.WTCSR_mask, 1);
    d.Add(s.wdt.busValue, 1);
    for (uint32_t v : {s.divu.DVSR, s.divu.DVDNT, s.divu.DVCR, s.divu.DVDNTH, s.divu.DVDNTL, s.divu.DVDNTUH,
                       s.divu.DVDNTUL}) {
        d.Add(v, 4);
    }
    d.Add(s.divu.VCRDIV, 2);
    d.Add(s.frt.TIER, 1);
    d.Add(s.frt.FTCSR, 1);
    d.Add(s.frt.FRC, 2);
    d.Add(s.frt.OCRA, 2);
    d.Add(s.frt.OCRB, 2);
    d.Add(s.frt.TCR, 1);
    d.Add(s.frt.TOCR, 1);
    d.Add(s.frt.ICR, 2);
    d.Add(s.frt.TEMP, 1);
    d.Add(s.frt.cycleCount);
    d.Add(s.frt.FTCSR_mask, 1);
    d.Add(s.intc.ICR, 2);
    for (uint8_t v : s.intc.levels) {
        d.Add(v, 1);
    }
    for (uint8_t v : s.intc.vectors) {
        d.Add(v, 1);
    }
    d.Add(s.intc.pendingSource, 1);
    d.Add(s.intc.pendingLevel, 1);
    d.Add(s.intc.NMI, 1);
    d.Add(s.intc.extVec, 1);
    d.Add(s.cache.CCR, 1);
    for (const auto &e : s.cache.entries) {
        for (uint32_t t : e.tags) {
            d.Add(t, 4);
        }
        for (const auto &line : e.lines) {
            for (uint8_t b : line) {
                d.Add(b, 1);
            }
        }
    }
    for (uint8_t v : s.cache.lru) {
        d.Add(v, 1);
    }
    d.Add(s.SBYCR, 1);
    d.Add(s.sleep, 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// Driver: the master SH-2 under Saturn::Run

constexpr uint32_t kCode = 0x06001000;
constexpr uint32_t kVbr = 0x06008000;
constexpr uint32_t kStack = 0x0600F000;
constexpr uint32_t kLog = 0x06060000;
constexpr uint32_t kLogSize = 0x8000;
constexpr uint32_t kTable = 0x06070000;

struct Pattern {
    const char *name;
    uint32_t step;     // Saturn::Run's kSH2SyncMaxStep (32), or another split
    uint32_t maxChunk; // scheduler steps are 1..maxChunk cycles (deterministic LCG)
};
constexpr Pattern kPatterns[] = {{"step32", 32, 400}, {"step7", 7, 61}};

struct Driver {
    std::unique_ptr<Rig> rig = std::make_unique<Rig>();
    uint64_t now = 0;
    bool cache;
    Digest digest;
    uint32_t lcg = 12345;

    explicit Driver(bool emulateCache)
        : cache(emulateCache) {
        rig->sh2->BindGlobalCycleCounter(now);
        if (cache) {
            OnChip(0xFFFFFE92, 1, 0x01); // CCR: cache enable
        }
    }

    void OnChip(uint32_t address, uint32_t size, uint32_t value) {
        auto &ctx = rig->sh2->GetJitContext();
        ctx.write(ctx.sh2, address, size, value);
    }

    uint64_t Advance(uint64_t target, uint64_t spill) {
        return cache ? rig->sh2->Advance<false, true>(target, spill) : rig->sh2->Advance<false, false>(target, spill);
    }

    // One Saturn::Run: Advance in steps of at most `step` cycles until `cycles`; the scheduler then advances by the
    // executed cycles.
    void RunStep(uint64_t cycles, uint32_t step) {
        uint64_t exec = 0;
        do {
            const uint64_t target = std::min<uint64_t>(exec + step, cycles);
            exec = Advance(target, exec);
        } while (exec < cycles);
        now += exec;
        digest.Add(now);
        AddState(digest, rig->State());
    }

    void Run(uint64_t total, const Pattern &p) {
        while (now < total) {
            lcg = lcg * 1103515245u + 12345u;
            RunStep(1 + (lcg >> 8) % p.maxChunk, p.step);
        }
    }

    uint64_t Finish() {
        for (uint32_t a = kLog; a < kLog + kLogSize; a += 4) {
            digest.Add(rig->Read32(a), 4);
        }
        digest.Add(rig->mmio.log.size());
        for (const auto &e : rig->mmio.log) {
            digest.Add(static_cast<uint8_t>(e.kind), 1);
            digest.Add(e.size, 1);
            digest.Add(e.address, 4);
            digest.Add(e.value, 4);
        }
        return digest.h;
    }

    uint32_t LogWords() const {
        uint32_t n = 0;
        for (uint32_t a = kLog; a < kLog + kLogSize; a += 4) {
            if (rig->Read32(a) != 0) {
                n = (a - kLog) / 4 + 1;
            }
        }
        return n;
    }
};

std::string Hex64(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(v));
    return buf;
}

// Expected digests: [program][cache][pattern].
struct Expected {
    uint64_t v[2][2];
};

void CheckDigest(const char *program, bool cache, const Pattern &p, uint64_t actual, uint64_t expected) {
    INFO(program << (cache ? " cache" : " plain") << " " << p.name << ": digest " << Hex64(actual) << ", expected "
                 << Hex64(expected));
    CHECK(actual == expected);
}

// ---------------------------------------------------------------------------------------------------------------------
// Programs

// FRT reads and writes after busy stretches of different lengths and clock selects. Table entries (count, TCR, FRC):
// write TCR, write FRC (FRCH then FRCL), DT/BF loop `count` times, read FRCH, FRCL and FTCSR, log them.
void SetupFrtIdle(Driver &d) {
    Asm a;
    a.Label("top");
    a << MovLP(5, 9) << Tst(5, 5);
    a.Bt("end");
    a << MovLP(3, 9) << MovBS(6, 3); // TCR
    a << MovLP(3, 9) << MovR(4, 3) << Shlr8(4) << MovBS(1, 4) << MovBS(2, 3); // FRCH (TEMP), FRCL (write)
    a.Label("loop");
    a << Dt(5);
    a.Bf("loop");
    a << MovBL(3, 1) << MovBL(4, 2) << MovBL(8, 7);
    a.Log(3);
    a.Log(4);
    a.Log(8);
    a << MovBS(7, 0); // FTCSR write 0 after the read: clears the flags read as set
    a.Bra("top");
    a << kNop;
    a.Label("end");
    a << kSleepOp;
    d.rig->WriteCode(kCode, a.Finish());

    const uint32_t table[][3] = {{1, 0, 0},      {2, 0, 0x0010},  {3, 0, 0xFFF0},  {7, 1, 0x1234},  {8, 1, 0},
                                 {9, 2, 0xFFFF}, {31, 2, 0x8000}, {32, 0, 0xFFFE}, {33, 3, 0x0100}, {100, 0, 0},
                                 {1000, 1, 7},   {9000, 0, 0xF000}, {3, 2, 0x00FF}, {40000, 0, 0x0000}, {5, 0, 1}};
    uint32_t addr = kTable;
    for (const auto &e : table) {
        for (uint32_t v : e) {
            d.rig->Write32(addr, v);
            addr += 4;
        }
    }
    d.rig->Write32(addr, 0);

    auto s = d.rig->BaseState(kCode);
    s.R[0] = 0;
    s.R[1] = 0xFFFFFE12; // FRCH
    s.R[2] = 0xFFFFFE13; // FRCL
    s.R[6] = 0xFFFFFE16; // TCR
    s.R[7] = 0xFFFFFE11; // FTCSR
    s.R[9] = kTable;
    s.R[14] = kLog;
    s.R[15] = kStack;
    d.rig->Load(s);
}

// FRT compare-match A (counter clear) and overflow interrupts, the WDT interval timer interrupt, SLEEP idle between
// interrupts, and interrupts raised by an INTC write (IPRB, while the compare flag is already set) and by LDC SR.
void SetupTimerInterrupts(Driver &d) {
    constexpr uint32_t kFrtHandler = 0x06009000;
    constexpr uint32_t kWdtHandler = 0x06009200;
    constexpr uint32_t kOviHandler = 0x06009400;
    d.rig->Write32(kVbr + 0x60 * 4, kFrtHandler);
    d.rig->Write32(kVbr + 0x61 * 4, kOviHandler);
    d.rig->Write32(kVbr + 0x62 * 4, kWdtHandler);

    // Main: busy loop (the compare flag gets set while the FRT interrupt level is 0), IPRB = 5, LDC SR unmask, then
    // SLEEP / count wake-ups forever.
    Asm m;
    m.Label("busy");
    m << Dt(5);
    m.Bf("busy");
    m << MovBS(12, 11) << AddI(0, 1) << LdcSr(13) << AddI(0, 1) << AddI(0, 1);
    m.Label("idle");
    m << kSleepOp << AddI(0, 1);
    m.Bra("idle");
    m << kNop;
    d.rig->WriteCode(kCode, m.Finish());

    // FRT OCI: log R0, FRCH, FRCL, FTCSR; clear OCFA (keep CCLRA)
    Asm f;
    f.Log(0);
    f << MovBL(3, 1) << MovBL(4, 2);
    f.Log(3);
    f.Log(4);
    f << MovBL(3, 7);
    f.Log(3);
    f << MovI(3, 1) << MovBS(7, 3) << kRte << kNop;
    d.rig->WriteCode(kFrtHandler, f.Finish());

    // WDT ITI: log R0 | 0x10000, WTCNT, WTCSR; clear OVF with an A5xx word write
    Asm w;
    w << MovR(3, 0) << AddI(3, 0x7F);
    w.Log(3);
    w << MovBL(3, 6);
    w.Log(3);
    w << MovBL(3, 10);
    w.Log(3);
    w << MovWS(10, 8) << kRte << kNop;
    d.rig->WriteCode(kWdtHandler, w.Finish());

    // FRT OVI: log FTCSR; clear (write 0x01 keeps CCLRA)
    Asm o;
    o << MovBL(3, 7);
    o.Log(3);
    o << MovI(3, 1) << MovBS(7, 3) << kRte << kNop;
    d.rig->WriteCode(kOviHandler, o.Finish());

    d.OnChip(0xFFFFFE67, 1, 0x60); // VCRC: FRT OCI vector
    d.OnChip(0xFFFFFE68, 1, 0x61); // VCRD: FRT OVI vector
    d.OnChip(0xFFFFFEE4, 1, 0x62); // VCRWDT: WDT ITI vector
    d.OnChip(0xFFFFFEE3, 1, 0x40); // IPRA low byte: WDT level 4
    d.OnChip(0xFFFFFE16, 1, 0x00); // TCR: phi/8
    d.OnChip(0xFFFFFE14, 1, 0x01); // OCRA H (TEMP)
    d.OnChip(0xFFFFFE15, 1, 0x2C); // OCRA L: 0x012C = 300
    d.OnChip(0xFFFFFE11, 1, 0x01); // FTCSR: CCLRA
    d.OnChip(0xFFFFFE10, 1, 0x0B); // TIER: OCIAE | OVIE
    d.OnChip(0xFFFFFE80, 2, 0xA521); // WTCSR: interval timer, TME, phi/64

    auto s = d.rig->BaseState(kCode);
    s.VBR = kVbr;
    s.R[0] = 0;
    s.R[1] = 0xFFFFFE12;
    s.R[2] = 0xFFFFFE13;
    s.R[8] = 0xA521; // WTCSR write: OVF clear, interval timer, TME, phi/64
    s.R[5] = 700; // busy loop iterations: past the first compare match (2400 cycles)
    s.R[6] = 0xFFFFFE81; // WTCNT
    s.R[7] = 0xFFFFFE11;
    s.R[10] = 0xFFFFFE80; // WTCSR
    s.R[11] = 0x05;
    s.R[12] = 0xFFFFFE60; // IPRB high byte: FRT level
    s.R[13] = 0x10; // LDC SR: mask 1 (blocks the IRL, which RecalcInterrupts raises at level 1)
    s.R[14] = kLog;
    s.R[15] = kStack;
    d.rig->Load(s);
}

// DMAC channel 0 as DE, DMAOR.DME and the transfer-end interrupt are toggled. Table entries (SAR, DAR, TCR, CHCR,
// order): order 0 writes CHCR (DE) before DMAOR.DME, order 1 enables DMAOR first and sets DE after a few
// instructions. Each pass logs CHCR/TCR before and after the enable, then clears CHCR (after reading TE) and DMAOR.
void SetupDma(Driver &d) {
    constexpr uint32_t kDmaHandler = 0x06009600;
    d.rig->Write32(kVbr + 0x48 * 4, kDmaHandler);
    Asm h;
    h << MovR(3, 0) << AddI(3, 0x40);
    h.Log(3);
    h << MovLL(3, 4); // CHCR read in the handler (TE read as set)
    h.Log(3);
    h << MovI(3, 0) << MovLS(4, 3) << kRte << kNop; // CHCR = 0: clears TE and IE, lowers the interrupt
    d.rig->WriteCode(kDmaHandler, h.Finish());

    Asm a;
    a.Label("top");
    a << MovLP(3, 9) << Tst(3, 3);
    a.Bt("end");
    a << MovLS(1, 3);                // SAR0
    a << MovLP(3, 9) << MovLS(2, 3); // DAR0
    a << MovLP(3, 9) << MovLS(6, 3); // TCR0
    a << MovLP(5, 9);                // CHCR value
    a << MovLP(3, 9) << Tst(3, 3);   // order
    a.Bf("dmaorFirst");
    a << MovLS(4, 5) << AddI(0, 1) << MovLL(3, 4);
    a.Log(3);
    a << MovI(3, 1) << MovLS(7, 3); // DMAOR.DME
    a.Bra("after");
    a << AddI(0, 1);
    a.Label("dmaorFirst");
    a << MovI(3, 1) << MovLS(7, 3) << AddI(0, 1) << AddI(0, 1) << MovLL(3, 4);
    a.Log(3);
    a << MovLS(4, 5); // CHCR with DE
    a.Label("after");
    a << AddI(0, 1) << AddI(0, 1) << MovLL(3, 4);
    a.Log(3);
    a << MovLL(3, 6);
    a.Log(3);
    a << MovLL(3, 4) << MovI(3, 0) << MovLS(4, 3) << MovLS(7, 3); // read TE, CHCR = 0, DMAOR = 0
    a.Bra("top");
    a << AddI(0, 1);
    a.Label("end");
    a << kSleepOp;
    d.rig->WriteCode(kCode, a.Finish());

    // Source data
    for (uint32_t i = 0; i < 0x400; i += 4) {
        d.rig->Write32(0x06040000 + i, 0xA5000000u | (i * 0x01010101u));
    }
    // CHCR: DM inc 0x4000, SM inc 0x1000, TS long 0x0800 / word 0x0400 / byte 0, AR 0x0200, IE 0x0004, DE 0x0001
    const uint32_t table[][5] = {
        {0x06040000, 0x06050000, 4, 0x5A01, 0},   {0x06040010, 0x06050100, 16, 0x5A05, 1},
        {0x06040020, 0x26050200, 3, 0x5601, 0},   {0x06040040, 0x06050300, 9, 0x5205, 1},
        {0x26040000, 0x06050400, 1, 0x5A05, 0},   {0x06040000, 0x06050500, 64, 0x5A01, 1},
        {0x06040100, 0x22000040, 8, 0x5A05, 0},   {0x22000000, 0x06050600, 4, 0x5A01, 1},
        {0x06040000, 0x06050700, 2, 0x5A00, 0}, // DE clear: no transfer
    };
    uint32_t addr = kTable;
    for (const auto &e : table) {
        for (uint32_t v : e) {
            d.rig->Write32(addr, v);
            addr += 4;
        }
    }
    d.rig->Write32(addr, 0);

    d.OnChip(0xFFFFFFA0, 4, 0x48);  // VCRDMA0
    d.OnChip(0xFFFFFEE2, 1, 0x06);  // IPRA: DMAC level 6
    auto s = d.rig->BaseState(kCode);
    s.SR = 0x10; // mask 1: blocks the IRL (RecalcInterrupts raises it at level 1), lets the DMAC level 6 through
    s.VBR = kVbr;
    s.R[0] = 0;
    s.R[1] = 0xFFFFFF80; // SAR0
    s.R[2] = 0xFFFFFF84; // DAR0
    s.R[4] = 0xFFFFFF8C; // CHCR0
    s.R[6] = 0xFFFFFF88; // TCR0
    s.R[7] = 0xFFFFFFB0; // DMAOR
    s.R[9] = kTable;
    s.R[14] = kLog;
    s.R[15] = kStack;
    d.rig->Load(s);
}

// Instruction fetch across a 64 KiB bus page boundary, from MMIO handler pages, from a page remapped by an MMIO
// write before the jump into it, and from the page currently executing (remapped by a store in it, at both fetch
// buffer alignments).
std::array<uint8_t, 0x10000> g_altA;
std::array<uint8_t, 0x10000> g_altB;
std::array<uint8_t, 0x10000> g_altC;

void WriteWords(std::array<uint8_t, 0x10000> &page, uint32_t offset, const std::vector<uint16_t> &words) {
    for (size_t i = 0; i < words.size(); ++i) {
        page[(offset + i * 2) & 0xFFFF] = static_cast<uint8_t>(words[i] >> 8);
        page[(offset + i * 2 + 1) & 0xFFFF] = static_cast<uint8_t>(words[i]);
    }
}

void OnRemapWrite(Rig &rig, uint32_t address) {
    switch (address & 0xFF) {
    case 0x00: rig.bus.MapArray(0x6040000, 0x604FFFF, g_altA, true); break;
    case 0x10: rig.bus.MapArray(0x6050000, 0x605FFFF, g_altB, true); break;
    case 0x20: rig.bus.MapArray(0x6050000, 0x605FFFF, g_altC, true); break;
    default: break;
    }
}

void SetupFetch(Driver &d) {
    // Back in main after each excursion: log R0 then continue at the next stage (R10 = return address chain).
    Asm a;
    a << AddI(0, 1) << MovLS(12, 0) << Jmp(11) << kNop; // remap page 0x0604xxxx, jump into it
    a.Label("back1");
    a.Log(0);
    a << MovLP(10, 9) << MovLP(11, 9) << Jmp(11) << kNop; // straddling code (cached area)
    a.Label("back2");
    a.Log(0);
    a << MovLP(10, 9) << MovLP(11, 9) << Jmp(11) << kNop; // straddling code (cache-through area)
    a.Label("back3");
    a.Log(0);
    a << MovLP(10, 9) << MovLP(11, 9) << Jmp(11) << kNop; // code in MMIO
    a.Label("back4");
    a.Log(0);
    a << MovLP(10, 9) << MovLP(11, 9) << AddI(12, 0x10) << Jmp(11) << kNop; // self-remap, even store
    a.Label("back5");
    a.Log(0);
    a << MovLP(10, 9) << MovLP(11, 9) << AddI(12, 0x10) << Jmp(11) << kNop; // self-remap, odd store
    a.Label("back6");
    a.Log(0);
    a << kSleepOp;
    const auto main = a.Finish();
    d.rig->WriteCode(kCode, main);
    auto at = [&](const char *label) { return kCode + static_cast<uint32_t>(a.labels.at(label)) * 2; };

    const std::vector<uint16_t> excursion = {AddI(0, 2), AddI(0, 2), Jmp(10), kNop};
    // 0x06040000: original RAM vs the remapped page
    d.rig->WriteCode(0x06040000, excursion);
    g_altA.fill(0);
    WriteWords(g_altA, 0, {AddI(0, 16), AddI(0, 16), MovLL(3, 13), Jmp(10), AddI(0, 3)});
    // Straddling 0x06020000 / 0x26020000 (same RAM): 12 adds with a load in the middle, then return
    const std::vector<uint16_t> straddle = {AddI(0, 3), AddI(0, 3), AddI(0, 3), AddI(0, 3), MovLL(3, 13),
                                            AddI(0, 3), AddI(0, 3), AddI(0, 3), Jmp(10),    AddI(0, 5)};
    d.rig->WriteCode(0x0601FFF6, straddle);
    // MMIO code at 0x22000100
    for (size_t i = 0; i < excursion.size(); ++i) {
        d.rig->mmio.data[0x100 + i * 2] = static_cast<uint8_t>(excursion[i] >> 8);
        d.rig->mmio.data[0x100 + i * 2 + 1] = static_cast<uint8_t>(excursion[i]);
    }
    // Self-remap at 0x06050000: the store remaps this page; RAM holds the "old" stream, g_altB/g_altC the new one.
    // even: store at 0x06050002 (PC & 2 == 2: the next instruction is fetched after the remap)
    // odd:  store at 0x06050100 (PC & 2 == 0: the next instruction is already in the fetch buffer)
    const std::vector<uint16_t> oldStream = {AddI(0, 1), MovLS(12, 0), AddI(0, 1), AddI(0, 1),
                                             AddI(0, 1), AddI(0, 1),   Jmp(10),    kNop};
    std::vector<uint16_t> newStream = oldStream;
    for (size_t i = 2; i < 6; ++i) {
        newStream[i] = AddI(0, 0x20);
    }
    d.rig->WriteCode(0x06050000, oldStream);
    d.rig->WriteCode(0x060500FE, oldStream);
    g_altB.fill(0);
    g_altC.fill(0);
    // g_altB (mapped by the first store) still holds the old stream at 0xFE for the second excursion
    WriteWords(g_altB, 0x0000, newStream);
    WriteWords(g_altB, 0x00FE, oldStream);
    WriteWords(g_altC, 0x0000, newStream);
    WriteWords(g_altC, 0x00FE, newStream);

    const uint32_t chain[] = {at("back2"), 0x0601FFF6, at("back3"), 0x2601FFF6, at("back4"), 0x22000100,
                              at("back5"), 0x06050000, at("back6"), 0x060500FE};
    uint32_t addr = kTable;
    for (uint32_t v : chain) {
        d.rig->Write32(addr, v);
        addr += 4;
    }
    d.rig->Write32(0x06045000, 0x13572468);
    g_altA[0x5000] = 0x24;
    g_altA[0x5001] = 0x68;
    g_altA[0x5002] = 0x13;
    g_altA[0x5003] = 0x57;
    d.rig->mmio.onWrite = OnRemapWrite;

    auto s = d.rig->BaseState(kCode);
    s.R[0] = 0;
    s.R[9] = kTable;
    s.R[10] = at("back1");
    s.R[11] = 0x06040000;
    s.R[12] = 0x22000000; // MMIO: the write remaps by address
    s.R[13] = 0x06045000;
    s.R[14] = kLog;
    s.R[15] = kStack;
    d.rig->Load(s);
}

// Misaligned word and longword loads and stores (aligned down by the SH-2 emulation) in cached RAM, cache-through RAM
// and MMIO, plus aligned ones around them; every value read is logged.
void SetupMisaligned(Driver &d) {
    Asm a;
    a.Label("start");
    a << MovWL(3, 1);
    a.Log(3);
    a << MovLL(3, 2);
    a.Log(3);
    a << MovLL(3, 4);
    a.Log(3);
    a << MovWL(3, 8);
    a.Log(3);
    a << MovLL(3, 9);
    a.Log(3);
    a << MovWS(6, 5) << MovLS(7, 5) << MovWS(10, 5) << MovLS(11, 5); // misaligned stores (log area, MMIO)
    a << MovWL(3, 6);
    a.Log(3);
    a << MovLL(3, 7);
    a.Log(3);
    a << MovLL(3, 11);
    a.Log(3);
    a << AddI(1, 1) << AddI(2, 1) << AddI(4, 1) << AddI(8, 1) << AddI(9, 1) << AddI(6, 1) << AddI(7, 1)
      << AddI(10, 1) << AddI(11, 1);
    a << Dt(12);
    a.Bf("again");
    a << kSleepOp;
    a.Label("again");
    a.Bra("start");
    a << kNop;
    d.rig->WriteCode(kCode, a.Finish());
    for (uint32_t i = 0; i < 0x40; i += 4) {
        d.rig->Write32(0x06040000 + i, 0x01020304u * (i / 4 + 1));
        d.rig->mmio.data[0x80 + i] = static_cast<uint8_t>(0x80 + i);
    }

    auto s = d.rig->BaseState(kCode);
    s.R[1] = 0x06040001;  // word, odd
    s.R[2] = 0x06040006;  // long, +2
    s.R[4] = 0x26040013;  // long, +3, cache-through
    s.R[5] = 0xA1B2C3D4;  // stored value
    s.R[6] = kLog + 0x7001;  // word store, odd
    s.R[7] = kLog + 0x7012;  // long store, +2
    s.R[8] = 0x22000081;  // MMIO word, odd
    s.R[9] = 0x22000086;  // MMIO long, +2
    s.R[10] = 0x22000091; // MMIO word store, odd
    s.R[11] = 0x220000A2; // MMIO long store, +2
    s.R[12] = 5;          // passes
    s.R[14] = kLog;
    s.R[15] = kStack;
    d.rig->Load(s);
}

// Byte, word and longword stores to a read-only array page (cached and cache-through addresses: the store is dropped,
// the SH-2 fast path must leave it to Bus::Write), each read back and logged, as are the RAM loads around them.
std::array<uint8_t, 0x10000> g_rom;

void SetupReadOnly(Driver &d) {
    Asm a;
    a.Label("start");
    a << MovBS(1, 5) << MovWS(2, 5) << MovLS(4, 5); // cached
    a << MovBS(6, 5) << MovWS(7, 5) << MovLS(8, 5); // cache-through
    a << MovBL(3, 1);
    a.Log(3);
    a << MovWL(3, 2);
    a.Log(3);
    a << MovLL(3, 4);
    a.Log(3);
    a << MovBL(3, 6);
    a.Log(3);
    a << MovWL(3, 7);
    a.Log(3);
    a << MovLL(3, 8);
    a.Log(3);
    a << MovLL(3, 9); // RAM next to the ROM page
    a.Log(3);
    a << AddI(5, 0x11) << AddI(1, 4) << AddI(2, 4) << AddI(4, 4) << AddI(6, 4) << AddI(7, 4) << AddI(8, 4)
      << AddI(9, 4);
    a << Dt(12);
    a.Bf("again");
    a << kSleepOp;
    a.Label("again");
    a.Bra("start");
    a << kNop;
    d.rig->WriteCode(kCode, a.Finish());
    for (uint32_t i = 0; i < g_rom.size(); ++i) {
        g_rom[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    d.rig->bus.MapArray(0x6030000, 0x603FFFF, g_rom, false);
    for (uint32_t i = 0; i < 0x40; i += 4) {
        d.rig->Write32(0x06040000 + i, 0x0A0B0C0Du * (i / 4 + 1));
    }

    auto s = d.rig->BaseState(kCode);
    s.R[1] = 0x06030011; // byte
    s.R[2] = 0x06030022; // word
    s.R[4] = 0x06030034; // long
    s.R[6] = 0x26030041; // byte, cache-through
    s.R[7] = 0x26030052; // word, cache-through
    s.R[8] = 0x26030064; // long, cache-through
    s.R[5] = 0xA1B2C3D4; // stored value
    s.R[9] = 0x06040000; // RAM
    s.R[12] = 5;         // passes
    s.R[14] = kLog;
    s.R[15] = kStack;
    d.rig->Load(s);
}

struct Program {
    const char *name;
    void (*setup)(Driver &);
    uint64_t cycles;
    Expected expected;
};

// Expected digests recorded at 40aa9d1 (before the Task 2 changes; "readonly-page" on 95f6efc, whose core is
// 40aa9d1's). Do not update them for a speed-up: a different digest means the change is not cycle-exact.
const Program kPrograms[] = {
    {"frt-idle", SetupFrtIdle, 700000, {{{0xDA7F4ACE0713814Eull, 0xB579D6F80430E3D4ull}, {0x45F718F711F56E54ull, 0x2FA6236A7F57507Cull}}}},
    {"timer-interrupts", SetupTimerInterrupts, 300000, {{{0x0E70E0CBB714810Dull, 0xE49CF04E795E7066ull}, {0x1A38E3A987323463ull, 0xED6B154AFDFD3FD3ull}}}},
    {"dma", SetupDma, 20000, {{{0x40B754E186D7DAACull, 0x1170965F5963CA21ull}, {0x758635C11D6CC52Cull, 0x312D5A4D2BB0AC78ull}}}},
    {"fetch", SetupFetch, 3000, {{{0x5311A29B64C7C105ull, 0x3BF075019663476Aull}, {0xEEA8BA4CAB32E32Full, 0xF0E6F8187E88D362ull}}}},
    {"misaligned", SetupMisaligned, 3000, {{{0xBBF0A6397D8D95BCull, 0xC046BAEDBAEBD7CAull}, {0xCB31DD192074EFC2ull, 0xB83D3AFB886E2C5Bull}}}},
    {"readonly-page", SetupReadOnly, 3000, {{{0x830594F280F3CC61ull, 0xFEFA6178AE2B78C0ull}, {0x0235F723D7772B11ull, 0x45FEB51EF901E22Dull}}}},
};

} // namespace

TEST_CASE("SH-2 interpreter runs are cycle-exact against the recorded digests", "[sh2][exact]") {
    for (const Program &prog : kPrograms) {
        for (int cache = 0; cache < 2; ++cache) {
            for (int pi = 0; pi < 2; ++pi) {
                const Pattern &p = kPatterns[pi];
                Driver d{cache != 0};
                prog.setup(d);
                d.Run(prog.cycles, p);
                const uint32_t logWords = d.LogWords();
                const uint64_t digest = d.Finish();
                INFO(prog.name << " log words " << logWords);
                CHECK(logWords > 4); // the program ran and logged
                CheckDigest(prog.name, cache != 0, p, digest, prog.expected.v[cache][pi]);
            }
        }
    }
}

// Prints a program's log (plain interpreter, Saturn step pattern) for diagnosis: brimir_tests "[.sh2dump]"
TEST_CASE("SH-2 exact program logs", "[.sh2dump]") {
    for (const Program &prog : kPrograms) {
        Driver d{false};
        prog.setup(d);
        d.Run(prog.cycles, kPatterns[0]);
        const uint32_t n = std::min<uint32_t>(d.LogWords(), 96);
        std::string line = std::string(prog.name) + ":";
        const uint32_t total = d.LogWords();
        for (uint32_t i = 0; i < n; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), " %X", d.rig->Read32(kLog + i * 4));
            line += buf;
        }
        line += " ... ";
        for (uint32_t i = total > 48 ? total - 48 : 0; i < total; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), " %X", d.rig->Read32(kLog + i * 4));
            line += buf;
        }
        const auto s = d.rig->State();
        char tail[96];
        std::snprintf(tail, sizeof(tail), " | PC %08X SR %X sleep %d R0 %X now %llu", s.PC, s.SR, s.sleep, s.R[0],
                      static_cast<unsigned long long>(d.now));
        WARN(line + tail);
    }
}

// The same programs must give the same results whatever the order the tests run in: running a program twice in a row
// on fresh rigs gives the same digest (no state leaks between rigs).
TEST_CASE("SH-2 exact runs are reproducible", "[sh2][exact]") {
    for (const Program &prog : kPrograms) {
        Driver a{false};
        prog.setup(a);
        a.Run(prog.cycles, kPatterns[0]);
        Driver b{false};
        prog.setup(b);
        b.Run(prog.cycles, kPatterns[0]);
        INFO(prog.name);
        CHECK(a.Finish() == b.Finish());
    }
}
