// Brimir - brimir_state_diff: field-level diff of the VDP renderer section of two Brimir save states
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
//   brimir_state_diff [--strict] <a.state> <b.state>
//   brimir_state_diff --drop-mouse-movement <in.state> <out.state>   (test only: DropMouseMovement below)
//
// Decodes two retro_serialize states (BRI2 header, LZ4 payload, savestate_codec.hpp) and compares the renderer section
// (VDPSaveState::VDPRendererSaveState) field by field. Both renderers save the software renderer's layout, but the
// Vulkan renderer's NBG fetchers come from its front half, so a few line-local caches may legitimately differ. Exit
// status: 0 when every difference in the renderer section is on the allowlist below and the rest of the VDP section
// (VDP1 framebuffers vdp.FBRAM and displayFB, VDP1/VDP2 VRAM, CRAM, VDP1/VDP2 registers, VDP1 state, phases) is
// identical, 3 otherwise, 2 on a read error. Other sections are only summarized (bytes that differ), with the
// differing work RAM offsets and SMPC output registers named: two runs of the same content started at different host
// times differ there, because the SMPC RTC follows the host clock (core default) and INTBACK returns the time in
// OREG, which the BIOS and games copy into work RAM. With --strict (runs with the RTC pinned: brimir_vkhost
// --fixed-date) every other section must be identical too (brimir::DiffSaveStates; the first difference is printed).
//
// Allowlist (fields the software renderer re-derives before any use, so their saved value never reaches a pixel):
// - vramFetchers[*][0..3]: currChar, nextChar, lastCharIndex, lastCellX, charData, charDataAddress. VDP2PrepareLine
//   resets lastCharIndex/lastCellX/charDataAddress at every line (sw: "force-fetch first character"), so the
//   character and its data are fetched again before they are read.
// - vramFetchers[*][2..3].lastVCellScroll: vertical cell scroll exists for NBG0 and NBG1 only; NBG2/3 never read it.
// - vramFetchers[*][4..5]: whole. The RBG fetchers are reset the same way, and RBGs have no vertical cell scroll.
// Anything else (vdp1State, nbgLayerStates, rotParamStates, lineBackLayerState, vcellScrollInc and NBG0/1
// lastVCellScroll, which is carried from line to line) must be identical.

#include <brimir/lockstep.hpp>
#include <brimir/savestate_codec.hpp>

#include <ymir/savestate/savestate.hpp>

#include <lz4.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

using ymir::savestate::SaveState;
using Renderer = ymir::savestate::VDPSaveState::VDPRendererSaveState;

constexpr uint32_t kMagic = 0x32524942; // "BRI2"

bool Load(const char *path, SaveState &state) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 16) {
        std::fprintf(stderr, "%s: not a Brimir state\n", path);
        return false;
    }
    uint32_t magic, version, payloadSize, compressedSize;
    std::memcpy(&magic, data.data(), 4);
    std::memcpy(&version, data.data() + 4, 4);
    std::memcpy(&payloadSize, data.data() + 8, 4);
    std::memcpy(&compressedSize, data.data() + 12, 4);
    if (magic != kMagic || version < 4 || compressedSize > data.size() - 16 ||
        payloadSize > brimir::MaxEncodedSaveStateSize(brimir::kMaxSaveStateCartDataSize)) {
        std::fprintf(stderr, "%s: unsupported state (magic %08x, version %u)\n", path, magic, version);
        return false;
    }
    std::vector<uint8_t> payload(payloadSize);
    if (LZ4_decompress_safe(reinterpret_cast<const char *>(data.data() + 16), reinterpret_cast<char *>(payload.data()),
                            static_cast<int>(compressedSize), static_cast<int>(payloadSize)) !=
            static_cast<int>(payloadSize) ||
        !brimir::DecodeSaveState(payload, state, version)) {
        std::fprintf(stderr, "%s: cannot decode the state\n", path);
        return false;
    }
    if (version == 4) {
        brimir::UpgradeVersion4SaveState(state);
    }
    return true;
}

template <typename T>
size_t DifferingBytes(const T &a, const T &b) {
    const auto *pa = reinterpret_cast<const uint8_t *>(&a);
    const auto *pb = reinterpret_cast<const uint8_t *>(&b);
    size_t n = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        n += pa[i] != pb[i];
    }
    return n;
}

struct Report {
    unsigned allowed = 0;
    unsigned unexpected = 0;

    template <typename T>
    void Field(const std::string &name, const T &a, const T &b, bool allow) {
        if (std::memcmp(&a, &b, sizeof(T)) == 0) {
            return;
        }
        uint64_t va = 0, vb = 0;
        std::memcpy(&va, &a, sizeof(T) < 8 ? sizeof(T) : 8);
        std::memcpy(&vb, &b, sizeof(T) < 8 ? sizeof(T) : 8);
        std::printf("  %s: 0x%llx vs 0x%llx%s\n", name.c_str(), static_cast<unsigned long long>(va),
                    static_cast<unsigned long long>(vb), allow ? " (allowed)" : "  <-- NOT ALLOWED");
        (allow ? allowed : unexpected)++;
    }
};

bool CharEq(const Renderer::CharacterSaveState &a, const Renderer::CharacterSaveState &b) {
    return a.charNum == b.charNum && a.palNum == b.palNum && a.specColorCalc == b.specColorCalc &&
           a.specPriority == b.specPriority && a.flipH == b.flipH && a.flipV == b.flipV;
}

} // namespace

// Test only (vkhost check peripherals_rollback_deterministic, negative control): writes a copy of a state without the
// Shuttle Mouse movement built up on either port, as a load that failed to restore it would leave it. Exit status 0 when
// a port had movement to drop, 4 when none had (the control would prove nothing), 2 on a read or write error.
int DropMouseMovement(const char *in, const char *out) {
    auto state = std::make_unique<SaveState>();
    if (!Load(in, *state)) {
        return 2;
    }
    bool any = false;
    for (size_t i = 0; i < state->brimir.mouse.ports.size(); ++i) {
        auto &port = state->brimir.mouse.ports[i];
        std::printf("port %zu: movement built up x=%d y=%d (hundredths of a count), dropped\n", i + 1,
                    static_cast<int>(port.accumX), static_cast<int>(port.accumY));
        any = any || port.accumX != 0 || port.accumY != 0;
        port.accumX = 0;
        port.accumY = 0;
    }
    std::vector<uint8_t> payload;
    if (!brimir::EncodeSaveState(*state, payload)) {
        return 2;
    }
    std::vector<uint8_t> data(16 + static_cast<size_t>(LZ4_compressBound(static_cast<int>(payload.size()))));
    const int compressed = LZ4_compress_default(reinterpret_cast<const char *>(payload.data()),
                                                reinterpret_cast<char *>(data.data() + 16),
                                                static_cast<int>(payload.size()), static_cast<int>(data.size() - 16));
    if (compressed <= 0) {
        return 2;
    }
    const uint32_t header[4] = {kMagic, brimir::kSaveStatePayloadVersion, static_cast<uint32_t>(payload.size()),
                                static_cast<uint32_t>(compressed)};
    std::memcpy(data.data(), header, sizeof(header));
    data.resize(16 + static_cast<size_t>(compressed));
    std::ofstream file(out, std::ios::binary);
    file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) {
        std::fprintf(stderr, "%s: cannot write\n", out);
        return 2;
    }
    return any ? 0 : 4;
}

int main(int argc, char **argv) {
    if (argc == 4 && std::strcmp(argv[1], "--drop-mouse-movement") == 0) {
        return DropMouseMovement(argv[2], argv[3]);
    }
    const bool strict = argc == 4 && std::strcmp(argv[1], "--strict") == 0;
    if (argc != 3 && !strict) {
        std::fprintf(stderr, "Usage: brimir_state_diff [--strict] <a.state> <b.state>\n"
                             "       brimir_state_diff --drop-mouse-movement <in.state> <out.state>\n");
        return 1;
    }
    const char *pathA = argv[argc - 2];
    const char *pathB = argv[argc - 1];
    auto a = std::make_unique<SaveState>();
    auto b = std::make_unique<SaveState>();
    if (!Load(pathA, *a) || !Load(pathB, *b)) {
        return 2;
    }

    std::printf("other sections (informational; RTC-driven memory differs between runs):\n");
    std::printf("  system %zu, msh2 %zu, ssh2 %zu, scsp %zu, vdp (without renderer) %zu bytes differ\n",
                DifferingBytes(a->system, b->system), DifferingBytes(a->msh2, b->msh2),
                DifferingBytes(a->ssh2, b->ssh2), DifferingBytes(a->scsp, b->scsp),
                DifferingBytes(a->vdp, b->vdp) - DifferingBytes(a->vdp.renderer, b->vdp.renderer));
    // Where they differ: work RAM offsets (first few) and the SMPC output registers and RTC fields.
    const auto ramOffsets = [](const char *name, const auto &ra, const auto &rb) {
        size_t n = 0;
        std::string first;
        for (size_t i = 0; i < ra.size(); ++i) {
            if (ra[i] != rb[i]) {
                if (n < 8) {
                    char text[40];
                    std::snprintf(text, sizeof(text), " %05zx (%02x vs %02x)", i, ra[i], rb[i]);
                    first += text;
                }
                ++n;
            }
        }
        if (n != 0) {
            std::printf("  %s: %zu bytes differ:%s%s\n", name, n, first.c_str(), n > 8 ? " ..." : "");
        }
    };
    ramOffsets("system.WRAMLow", a->system.WRAMLow, b->system.WRAMLow);
    ramOffsets("system.WRAMHigh", a->system.WRAMHigh, b->system.WRAMHigh);
    ramOffsets("smpc.OREG", a->smpc.OREG, b->smpc.OREG);
    if (a->smpc.rtcTimestamp != b->smpc.rtcTimestamp || a->smpc.rtcSysClockCount != b->smpc.rtcSysClockCount) {
        std::printf("  smpc.rtcTimestamp %lld vs %lld, rtcSysClockCount %llu vs %llu\n",
                    static_cast<long long>(a->smpc.rtcTimestamp), static_cast<long long>(b->smpc.rtcTimestamp),
                    static_cast<unsigned long long>(a->smpc.rtcSysClockCount),
                    static_cast<unsigned long long>(b->smpc.rtcSysClockCount));
    }

    Report r;
    // The rest of the VDP section is emulation state the renderers do not own: it must be identical (a difference
    // means the two runs diverged, e.g. a CPU framebuffer read returned other data).
    std::printf("VDP section outside the renderer:\n");
    const auto bytes = [&r](const char *name, const auto &va, const auto &vb) {
        const size_t n = DifferingBytes(va, vb);
        if (n != 0) {
            std::printf("  %s: %zu bytes differ  <-- NOT ALLOWED\n", name, n);
            ++r.unexpected;
        }
    };
    const auto memory = [&r, &ramOffsets](const char *name, const auto &va, const auto &vb) {
        if (DifferingBytes(va, vb) != 0) {
            ramOffsets(name, va, vb);
            std::printf("    <-- NOT ALLOWED\n");
            ++r.unexpected;
        }
    };
    memory("vdp.VRAM1", a->vdp.VRAM1, b->vdp.VRAM1);
    memory("vdp.VRAM2", a->vdp.VRAM2, b->vdp.VRAM2);
    memory("vdp.CRAM", a->vdp.CRAM, b->vdp.CRAM);
    bytes("vdp.vdp1State", a->vdp.vdp1State, b->vdp.vdp1State);
    bytes("vdp.regs1", a->vdp.regs1, b->vdp.regs1);
    bytes("vdp.regs2", a->vdp.regs2, b->vdp.regs2);
    bytes("vdp.HPhase", a->vdp.HPhase, b->vdp.HPhase);
    bytes("vdp.VPhase", a->vdp.VPhase, b->vdp.VPhase);
    if (r.unexpected != 0 && (a->smpc.OREG != b->smpc.OREG || DifferingBytes(a->system, b->system) != 0)) {
        // Seen in Fighters Megamix: CPU-written bytes of a VDP1 command table that depend on the time, also between two
        // software runs.
        std::printf("  note: work RAM / the SMPC output registers differ too: the runs may have diverged on the host-clock "
                    "RTC (started at different times); compare from a shared state (brimir_vkhost --load-state)\n");
    }

    // The VDP1 framebuffers are compared strictly too: the checks diff states saved at the same frame of the same
    // content, whose frames are identical, and with VDP1 on the GPU they come from the CPU shadow.
    std::printf("VDP1 framebuffers:\n");
    for (size_t fb = 0; fb < a->vdp.FBRAM.size(); ++fb) {
        r.Field("vdp.FBRAM[" + std::to_string(fb) + "]", a->vdp.FBRAM[fb], b->vdp.FBRAM[fb], false);
    }
    r.Field("vdp.displayFB", a->vdp.displayFB, b->vdp.displayFB, false);
    // The other field's framebuffers (save state extension, version 6 on)
    for (size_t fb = 0; fb < a->brimir.vdpRenderer.altFBRAM.size(); ++fb) {
        r.Field("brimir.vdpRenderer.altFBRAM[" + std::to_string(fb) + "]", a->brimir.vdpRenderer.altFBRAM[fb],
                b->brimir.vdpRenderer.altFBRAM[fb], false);
    }
    std::printf("renderer section:\n");
    const Renderer &ra = a->vdp.renderer;
    const Renderer &rb = b->vdp.renderer;
    r.Field("vdp1State", ra.vdp1State, rb.vdp1State, false);
    for (size_t i = 0; i < ra.nbgLayerStates.size(); ++i) {
        r.Field("nbgLayerStates[" + std::to_string(i) + "]", ra.nbgLayerStates[i], rb.nbgLayerStates[i], false);
    }
    for (size_t i = 0; i < ra.rotParamStates.size(); ++i) {
        r.Field("rotParamStates[" + std::to_string(i) + "]", ra.rotParamStates[i], rb.rotParamStates[i], false);
    }
    r.Field("lineBackLayerState", ra.lineBackLayerState, rb.lineBackLayerState, false);
    r.Field("vcellScrollInc", ra.vcellScrollInc, rb.vcellScrollInc, false);
    for (size_t field = 0; field < 2; ++field) {
        for (size_t i = 0; i < 6; ++i) {
            const auto &fa = ra.vramFetchers[field][i];
            const auto &fb = rb.vramFetchers[field][i];
            const std::string name = "vramFetchers[" + std::to_string(field) + "][" + std::to_string(i) + "].";
            const bool rbg = i >= 4;
            if (!CharEq(fa.currChar, fb.currChar)) {
                r.Field(name + "currChar", fa.currChar, fb.currChar, true);
            }
            if (!CharEq(fa.nextChar, fb.nextChar)) {
                r.Field(name + "nextChar", fa.nextChar, fb.nextChar, true);
            }
            r.Field(name + "lastCharIndex", fa.lastCharIndex, fb.lastCharIndex, true);
            r.Field(name + "lastCellX", fa.lastCellX, fb.lastCellX, true);
            r.Field(name + "charData", fa.charData, fb.charData, true);
            r.Field(name + "charDataAddress", fa.charDataAddress, fb.charDataAddress, true);
            r.Field(name + "lastVCellScroll", fa.lastVCellScroll, fb.lastVCellScroll, rbg || i >= 2);
        }
    }
    if (strict) {
        // Every other section too (the renderer section was compared above, up to its allowlist).
        std::memcpy(static_cast<void *>(&b->vdp.renderer), &a->vdp.renderer, sizeof(Renderer));
        const std::string d = brimir::DiffSaveStates(*a, *b);
        if (d.empty()) {
            std::printf("strict: every section identical (renderer section as above)\n");
        } else {
            std::printf("strict: %s  <-- NOT ALLOWED\n", d.c_str());
            ++r.unexpected;
        }
    }
    std::printf("%u allowed and %u unexpected renderer differences\n", r.allowed, r.unexpected);
    return r.unexpected == 0 ? 0 : 3;
}
