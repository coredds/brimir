// Brimir - state comparison for SH-2 JIT validation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "brimir/lockstep.hpp"
#include "brimir/core_wrapper.hpp"

#include <ymir/sys/saturn.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace brimir {

namespace {

std::string Describe(const char *label, const char *field, uint64_t a, uint64_t b) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s%s differs: a=0x%llX b=0x%llX", label, field,
                  static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
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

LockstepResult RunLockstep(CoreWrapper &a, CoreWrapper &b, int frames) {
    LockstepResult result;
    for (int frame = 0; frame < frames; ++frame) {
        a.RunFrame();
        b.RunFrame();
        ++result.framesRun;
        result.divergence = CompareCores(a, b);
        if (!result.divergence.empty()) {
            break;
        }
    }
    return result;
}

} // namespace brimir
