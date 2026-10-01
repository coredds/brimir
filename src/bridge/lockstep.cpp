// Brimir - state comparison for SH-2 JIT validation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "brimir/lockstep.hpp"
#include "brimir/core_wrapper.hpp"

#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/saturn.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

namespace brimir {

namespace {

std::string Describe(const char *label, const char *field, uint64_t a, uint64_t b) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s%s differs: a=0x%llX b=0x%llX", label, field,
                  static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
}

std::string StateDiffers(const char *subsystem, size_t offset) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s state differs (byte offset %zu)", subsystem, offset);
    return buf;
}

template <typename T>
void ZeroBytes(T &obj) {
    static_assert(std::is_trivially_copyable_v<T>, "only trivially copyable state can be cleared bytewise");
    std::memset(static_cast<void *>(&obj), 0, sizeof(T));
}

template <typename T>
size_t OffsetIn(const T &base, const void *member) {
    return static_cast<size_t>(static_cast<const char *>(member) - reinterpret_cast<const char *>(&base));
}

// Returns the offset of the first differing byte of a member, or SIZE_MAX if equal.
template <typename T>
size_t FirstDiffByte(const T &a, const T &b) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto *pa = reinterpret_cast<const unsigned char *>(&a);
    const auto *pb = reinterpret_cast<const unsigned char *>(&b);
    if (std::memcmp(pa, pb, sizeof(T)) == 0) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i < sizeof(T); ++i) {
        if (pa[i] != pb[i]) {
            return i;
        }
    }
    return SIZE_MAX;
}

// Clears every byte of the state that Saturn::SaveState writes into, padding included, so the
// bytewise comparison sees only data. SCUSaveState and SMPCSaveState hold std::vectors and cannot be
// cleared bytewise; their trivially copyable members are cleared and the rest is compared by value.
void ClearSaveState(ymir::savestate::SaveState &s) {
    ZeroBytes(s.scheduler);
    ZeroBytes(s.system);
    ZeroBytes(s.msh2);
    ZeroBytes(s.ssh2);
    ZeroBytes(s.scu.dma);
    ZeroBytes(s.scu.dsp);
    ZeroBytes(s.vdp);
    ZeroBytes(s.scsp);
    ZeroBytes(s.cdblock);
    ZeroBytes(s.sh1);
    ZeroBytes(s.ygr);
    ZeroBytes(s.cddrive);
    ZeroBytes(s.cdblockDRAM);
    s.cdblockLLE = false;
    s.msh2SpilloverCycles = 0;
    s.ssh2SpilloverCycles = 0;
    s.sh1SpilloverCycles = 0;
    s.sh1FracCycles = 0;
}

template <typename T>
std::string DiffBytes(const char *subsystem, const T &a, const T &b) {
    if (const size_t off = FirstDiffByte(a, b); off != SIZE_MAX) {
        return StateDiffers(subsystem, off);
    }
    return {};
}

// SCUSaveState is not trivially copyable (cartData), so it is compared member by member. The
// reported offset is the member's offset within SCUSaveState (plus the byte within it for arrays).
std::string DiffSCU(const ymir::savestate::SCUSaveState &a, const ymir::savestate::SCUSaveState &b) {
    if (const size_t off = FirstDiffByte(a.dma, b.dma); off != SIZE_MAX) {
        return StateDiffers("scu", OffsetIn(a, &a.dma) + off);
    }
    if (const size_t off = FirstDiffByte(a.dsp, b.dsp); off != SIZE_MAX) {
        return StateDiffers("scu", OffsetIn(a, &a.dsp) + off);
    }
#define BRIMIR_DIFF_MEMBER(field)                                                                            \
    if (a.field != b.field) {                                                                                \
        return StateDiffers("scu", OffsetIn(a, &a.field));                                                   \
    }
    BRIMIR_DIFF_MEMBER(cartType)
    BRIMIR_DIFF_MEMBER(cartData)
    BRIMIR_DIFF_MEMBER(intrMask)
    BRIMIR_DIFF_MEMBER(intrStatus)
    BRIMIR_DIFF_MEMBER(abusIntrsPendingAck)
    BRIMIR_DIFF_MEMBER(pendingIntrLevel)
    BRIMIR_DIFF_MEMBER(pendingIntrIndex)
    BRIMIR_DIFF_MEMBER(timer0Counter)
    BRIMIR_DIFF_MEMBER(timer0Compare)
    BRIMIR_DIFF_MEMBER(timer1Reload)
    BRIMIR_DIFF_MEMBER(timer1Mode)
    BRIMIR_DIFF_MEMBER(timerEnable)
    BRIMIR_DIFF_MEMBER(wramSizeSelect)
#undef BRIMIR_DIFF_MEMBER
    return {};
}

// SMPCSaveState is not trivially copyable (intback.report), so it is compared member by member.
std::string DiffSMPC(const ymir::savestate::SMPCSaveState &a, const ymir::savestate::SMPCSaveState &b) {
#define BRIMIR_DIFF_MEMBER(field)                                                                            \
    if (a.field != b.field) {                                                                                \
        return StateDiffers("smpc", OffsetIn(a, &a.field));                                                  \
    }
    BRIMIR_DIFF_MEMBER(IREG)
    BRIMIR_DIFF_MEMBER(OREG)
    BRIMIR_DIFF_MEMBER(COMREG)
    BRIMIR_DIFF_MEMBER(SR)
    BRIMIR_DIFF_MEMBER(SF)
    BRIMIR_DIFF_MEMBER(PDR1)
    BRIMIR_DIFF_MEMBER(PDR2)
    BRIMIR_DIFF_MEMBER(DDR1)
    BRIMIR_DIFF_MEMBER(DDR2)
    BRIMIR_DIFF_MEMBER(IOSEL)
    BRIMIR_DIFF_MEMBER(EXLE)
    BRIMIR_DIFF_MEMBER(intback.getPeripheralData)
    BRIMIR_DIFF_MEMBER(intback.optimize)
    BRIMIR_DIFF_MEMBER(intback.port1mode)
    BRIMIR_DIFF_MEMBER(intback.port2mode)
    BRIMIR_DIFF_MEMBER(intback.report)
    BRIMIR_DIFF_MEMBER(intback.reportOffset)
    BRIMIR_DIFF_MEMBER(intback.inProgress)
    BRIMIR_DIFF_MEMBER(busValue)
    BRIMIR_DIFF_MEMBER(resetDisable)
    BRIMIR_DIFF_MEMBER(commandEventState)
    BRIMIR_DIFF_MEMBER(rtcTimestamp)
    BRIMIR_DIFF_MEMBER(rtcSysClockCount)
#undef BRIMIR_DIFF_MEMBER
    return {};
}

// Compares the save state of every subsystem other than the SH-2s (compared field by field by
// CompareCores) and the disc hash (identical by construction). Returns the first difference.
std::string DiffSaturnState(const ymir::Saturn &sa, const ymir::Saturn &sb) {
    // Several MB each; kept per thread so lockstep runs do not allocate every frame.
    static thread_local std::unique_ptr<ymir::savestate::SaveState> xa = std::make_unique<ymir::savestate::SaveState>();
    static thread_local std::unique_ptr<ymir::savestate::SaveState> xb = std::make_unique<ymir::savestate::SaveState>();
    auto &a = *xa;
    auto &b = *xb;
    ClearSaveState(a);
    ClearSaveState(b);
    sa.SaveState(a);
    sb.SaveState(b);

    if (std::string d = DiffBytes("scheduler", a.scheduler, b.scheduler); !d.empty()) {
        return d;
    }
    if (std::string d = DiffBytes("system", a.system, b.system); !d.empty()) {
        return d;
    }
    if (std::string d = DiffSCU(a.scu, b.scu); !d.empty()) {
        return d;
    }
    if (std::string d = DiffSMPC(a.smpc, b.smpc); !d.empty()) {
        return d;
    }
    if (std::string d = DiffBytes("vdp", a.vdp, b.vdp); !d.empty()) {
        return d;
    }
    if (std::string d = DiffBytes("scsp", a.scsp, b.scsp); !d.empty()) {
        return d;
    }
    if (a.cdblockLLE != b.cdblockLLE) {
        return Describe("", "cdblockLLE", a.cdblockLLE, b.cdblockLLE);
    }
    if (!a.cdblockLLE) {
        if (std::string d = DiffBytes("cdblock", a.cdblock, b.cdblock); !d.empty()) {
            return d;
        }
    } else {
        if (std::string d = DiffBytes("sh1", a.sh1, b.sh1); !d.empty()) {
            return d;
        }
        if (std::string d = DiffBytes("ygr", a.ygr, b.ygr); !d.empty()) {
            return d;
        }
        if (std::string d = DiffBytes("cddrive", a.cddrive, b.cddrive); !d.empty()) {
            return d;
        }
        if (std::string d = DiffBytes("cdblockDRAM", a.cdblockDRAM, b.cdblockDRAM); !d.empty()) {
            return d;
        }
    }
    if (a.msh2SpilloverCycles != b.msh2SpilloverCycles) {
        return Describe("", "msh2SpilloverCycles", a.msh2SpilloverCycles, b.msh2SpilloverCycles);
    }
    if (a.ssh2SpilloverCycles != b.ssh2SpilloverCycles) {
        return Describe("", "ssh2SpilloverCycles", a.ssh2SpilloverCycles, b.ssh2SpilloverCycles);
    }
    if (a.sh1SpilloverCycles != b.sh1SpilloverCycles) {
        return Describe("", "sh1SpilloverCycles", a.sh1SpilloverCycles, b.sh1SpilloverCycles);
    }
    if (a.sh1FracCycles != b.sh1FracCycles) {
        return Describe("", "sh1FracCycles", a.sh1FracCycles, b.sh1FracCycles);
    }
    return {};
}

} // namespace

std::string DiffSH2State(const ymir::savestate::SH2SaveState &a, const ymir::savestate::SH2SaveState &b,
                         SH2DiffScope scope, const char *label) {
#define BRIMIR_DIFF(field)                                                                                   \
    if (a.field != b.field) {                                                                                \
        return Describe(label, #field, static_cast<uint64_t>(a.field), static_cast<uint64_t>(b.field));       \
    }

    char name[32];
    for (int i = 0; i < 16; ++i) {
        if (a.R[i] != b.R[i]) {
            std::snprintf(name, sizeof(name), "R%d", i);
            return Describe(label, name, a.R[i], b.R[i]);
        }
    }
    BRIMIR_DIFF(PC)
    BRIMIR_DIFF(PR)
    BRIMIR_DIFF(MACL)
    BRIMIR_DIFF(MACH)
    BRIMIR_DIFF(SR)
    BRIMIR_DIFF(GBR)
    BRIMIR_DIFF(VBR)
    BRIMIR_DIFF(delaySlot)
    BRIMIR_DIFF(delaySlotTarget)
    BRIMIR_DIFF(intrAllow)
    BRIMIR_DIFF(fetchedOpcodes)
    BRIMIR_DIFF(wbReg)
    BRIMIR_DIFF(sleep)
    BRIMIR_DIFF(SBYCR)
    BRIMIR_DIFF(bsc.BCR1)
    BRIMIR_DIFF(bsc.BCR2)
    BRIMIR_DIFF(bsc.WCR)
    BRIMIR_DIFF(bsc.MCR)
    BRIMIR_DIFF(bsc.RTCSR)
    BRIMIR_DIFF(bsc.RTCNT)
    BRIMIR_DIFF(bsc.RTCOR)
    BRIMIR_DIFF(divu.DVSR)
    BRIMIR_DIFF(divu.DVDNT)
    BRIMIR_DIFF(divu.DVCR)
    BRIMIR_DIFF(divu.VCRDIV)
    BRIMIR_DIFF(divu.DVDNTH)
    BRIMIR_DIFF(divu.DVDNTL)
    BRIMIR_DIFF(divu.DVDNTUH)
    BRIMIR_DIFF(divu.DVDNTUL)
    BRIMIR_DIFF(intc.ICR)
    BRIMIR_DIFF(intc.NMI)
    BRIMIR_DIFF(intc.extVec)
    for (int i = 0; i < 16; ++i) {
        if (a.intc.levels[i] != b.intc.levels[i]) {
            std::snprintf(name, sizeof(name), "intc.levels[%d]", i);
            return Describe(label, name, a.intc.levels[i], b.intc.levels[i]);
        }
        if (a.intc.vectors[i] != b.intc.vectors[i]) {
            std::snprintf(name, sizeof(name), "intc.vectors[%d]", i);
            return Describe(label, name, a.intc.vectors[i], b.intc.vectors[i]);
        }
    }
    BRIMIR_DIFF(cache.CCR)
    // Compiled code can write the cache address/data arrays through MemWrite even with cache
    // emulation off, so the arrays are part of the CPU state. memcmp is the fast path; the loops
    // below only run to name the first difference.
    for (int i = 0; i < 64; ++i) {
        const auto &ea = a.cache.entries[i];
        const auto &eb = b.cache.entries[i];
        if (std::memcmp(ea.tags.data(), eb.tags.data(), sizeof(ea.tags)) != 0) {
            for (int w = 0; w < 4; ++w) {
                if (ea.tags[w] != eb.tags[w]) {
                    std::snprintf(name, sizeof(name), "cache.entries[%d].tags[%d]", i, w);
                    return Describe(label, name, ea.tags[w], eb.tags[w]);
                }
            }
        }
        if (std::memcmp(ea.lines.data(), eb.lines.data(), sizeof(ea.lines)) != 0) {
            for (int w = 0; w < 4; ++w) {
                for (int k = 0; k < 16; ++k) {
                    if (ea.lines[w][k] != eb.lines[w][k]) {
                        std::snprintf(name, sizeof(name), "cache.entries[%d].lines[%d][%d]", i, w, k);
                        return Describe(label, name, ea.lines[w][k], eb.lines[w][k]);
                    }
                }
            }
        }
    }
    if (std::memcmp(a.cache.lru.data(), b.cache.lru.data(), sizeof(a.cache.lru)) != 0) {
        for (int i = 0; i < 64; ++i) {
            if (a.cache.lru[i] != b.cache.lru[i]) {
                std::snprintf(name, sizeof(name), "cache.lru[%d]", i);
                return Describe(label, name, a.cache.lru[i], b.cache.lru[i]);
            }
        }
    }

    if (scope == SH2DiffScope::CpuAndPeripherals) {
        BRIMIR_DIFF(intc.pendingSource)
        BRIMIR_DIFF(intc.pendingLevel)
        BRIMIR_DIFF(frt.TIER)
        BRIMIR_DIFF(frt.FTCSR)
        BRIMIR_DIFF(frt.FRC)
        BRIMIR_DIFF(frt.OCRA)
        BRIMIR_DIFF(frt.OCRB)
        BRIMIR_DIFF(frt.TCR)
        BRIMIR_DIFF(frt.TOCR)
        BRIMIR_DIFF(frt.ICR)
        BRIMIR_DIFF(frt.TEMP)
        BRIMIR_DIFF(frt.cycleCount)
        BRIMIR_DIFF(frt.FTCSR_mask)
        BRIMIR_DIFF(wdt.WTCSR)
        BRIMIR_DIFF(wdt.WTCNT)
        BRIMIR_DIFF(wdt.RSTCSR)
        BRIMIR_DIFF(wdt.cycleCount)
        BRIMIR_DIFF(wdt.WTCSR_mask)
        BRIMIR_DIFF(wdt.busValue)
        BRIMIR_DIFF(dmac.DMAOR)
        for (int ch = 0; ch < 2; ++ch) {
            const auto &ca = a.dmac.channels[ch];
            const auto &cb = b.dmac.channels[ch];
            const struct {
                const char *field;
                uint64_t x, y;
            } fields[] = {{"SAR", ca.SAR, cb.SAR},
                          {"DAR", ca.DAR, cb.DAR},
                          {"TCR", ca.TCR, cb.TCR},
                          {"CHCR", ca.CHCR, cb.CHCR},
                          {"DRCR", ca.DRCR, cb.DRCR}};
            for (const auto &f : fields) {
                if (f.x != f.y) {
                    std::snprintf(name, sizeof(name), "dmac.channels[%d].%s", ch, f.field);
                    return Describe(label, name, f.x, f.y);
                }
            }
        }
    }
#undef BRIMIR_DIFF
    return {};
}

void PrepareLockstepCore(CoreWrapper &core) {
    core.SetThreadedVDP1(false);
    core.SetThreadedVDP2(false);
    // The default RTC mode reads the host clock, so two cores run one after the other can see
    // different seconds (the BIOS diverges within a few seconds of emulated time). The virtual RTC
    // advances with emulated time only.
    if (ymir::Saturn *saturn = core.GetSaturn(); saturn != nullptr) {
        saturn->configuration.rtc.mode = ymir::core::config::rtc::Mode::Virtual;
        // Keep the SCSP on the emulation thread so audio timing cannot depend on host thread
        // scheduling (the option is currently unimplemented upstream; pinned for when it lands).
        saturn->configuration.audio.threadedSCSP = false;
    }
}

std::string CompareCores(CoreWrapper &a, CoreWrapper &b) {
    ymir::Saturn *sa = a.GetSaturn();
    ymir::Saturn *sb = b.GetSaturn();
    if (sa == nullptr || sb == nullptr) {
        return "core not initialized";
    }

    ymir::savestate::SH2SaveState x{};
    ymir::savestate::SH2SaveState y{};
    sa->masterSH2.SaveState(x);
    sb->masterSH2.SaveState(y);
    if (std::string diff = DiffSH2State(x, y, SH2DiffScope::CpuAndPeripherals, "master SH-2 "); !diff.empty()) {
        return diff;
    }
    sa->slaveSH2.SaveState(x);
    sb->slaveSH2.SaveState(y);
    if (std::string diff = DiffSH2State(x, y, SH2DiffScope::CpuAndPeripherals, "slave SH-2 "); !diff.empty()) {
        return diff;
    }
    if (sa->slaveSH2Enabled != sb->slaveSH2Enabled) {
        return Describe("", "slaveSH2Enabled", sa->slaveSH2Enabled, sb->slaveSH2Enabled);
    }

    const auto compareRam = [](const char *name, const auto &ra, const auto &rb) -> std::string {
        if (std::memcmp(ra.data(), rb.data(), ra.size()) == 0) {
            return {};
        }
        for (size_t i = 0; i < ra.size(); ++i) {
            if (ra[i] != rb[i]) {
                char field[48];
                std::snprintf(field, sizeof(field), "%s[0x%05zX]", name, i);
                return Describe("", field, ra[i], rb[i]);
            }
        }
        return {};
    };
    if (std::string diff = compareRam("WRAMLow", sa->mem.WRAMLow, sb->mem.WRAMLow); !diff.empty()) {
        return diff;
    }
    if (std::string diff = compareRam("WRAMHigh", sa->mem.WRAMHigh, sb->mem.WRAMHigh); !diff.empty()) {
        return diff;
    }
    if (std::string diff = DiffSaturnState(*sa, *sb); !diff.empty()) {
        return diff;
    }

    const unsigned w = a.GetFramebufferWidth();
    const unsigned h = a.GetFramebufferHeight();
    const unsigned pitch = a.GetFramebufferPitch();
    if (w != b.GetFramebufferWidth() || h != b.GetFramebufferHeight() || pitch != b.GetFramebufferPitch()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "frame size differs: a=%ux%u (pitch %u) b=%ux%u (pitch %u)", w, h, pitch,
                      b.GetFramebufferWidth(), b.GetFramebufferHeight(), b.GetFramebufferPitch());
        return buf;
    }
    const auto *fa = static_cast<const uint8_t *>(a.GetFramebuffer());
    const auto *fb = static_cast<const uint8_t *>(b.GetFramebuffer());
    if (fa != nullptr && fb != nullptr) {
        for (unsigned row = 0; row < h; ++row) {
            const uint8_t *ra = fa + static_cast<size_t>(row) * pitch;
            const uint8_t *rb = fb + static_cast<size_t>(row) * pitch;
            if (std::memcmp(ra, rb, static_cast<size_t>(w) * 4) != 0) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "frame row %u differs", row);
                return buf;
            }
        }
    }
    return {};
}

namespace {

// Drains both cores' audio output and compares it. Returns "" if identical.
std::string CompareAudio(CoreWrapper &a, CoreWrapper &b) {
    constexpr size_t kMaxStereoSamples = 4096;
    static thread_local std::vector<int16_t> bufA(kMaxStereoSamples * 2);
    static thread_local std::vector<int16_t> bufB(kMaxStereoSamples * 2);
    const size_t na = a.GetAudioSamples(bufA.data(), kMaxStereoSamples);
    const size_t nb = b.GetAudioSamples(bufB.data(), kMaxStereoSamples);
    char buf[96];
    if (na != nb) {
        std::snprintf(buf, sizeof(buf), "audio sample count differs: a=%zu b=%zu", na, nb);
        return buf;
    }
    if (std::memcmp(bufA.data(), bufB.data(), na * 2 * sizeof(int16_t)) != 0) {
        for (size_t i = 0; i < na * 2; ++i) {
            if (bufA[i] != bufB[i]) {
                std::snprintf(buf, sizeof(buf), "audio differs at sample %zu", i / 2);
                return buf;
            }
        }
    }
    return {};
}

} // namespace

LockstepResult RunLockstep(CoreWrapper &a, CoreWrapper &b, int frames) {
    LockstepResult result;
    for (int frame = 0; frame < frames; ++frame) {
        a.RunFrame();
        b.RunFrame();
        ++result.framesRun;
        result.divergence = CompareAudio(a, b);
        if (!result.divergence.empty()) {
            break;
        }
        result.divergence = CompareCores(a, b);
        if (!result.divergence.empty()) {
            break;
        }
    }
    return result;
}

} // namespace brimir
