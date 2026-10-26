// Brimir - brimir_state_diff: field-level diff of the VDP renderer section of two Brimir save states
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
//   brimir_state_diff <a.state> <b.state>
//
// Decodes two retro_serialize states (BRI2 header, LZ4 payload, savestate_codec.hpp) and compares the renderer section
// (VDPSaveState::VDPRendererSaveState) field by field. Both renderers save the software renderer's layout, but the
// Vulkan renderer's NBG fetchers come from its front half, so a few line-local caches may legitimately differ. Exit
// status: 0 when every difference in the renderer section is on the allowlist below, 3 otherwise, 2 on a read error.
// Other sections are only summarized (bytes that differ): two runs of the same content differ in RTC-driven memory.
//
// Allowlist (fields the software renderer re-derives before any use, so their saved value never reaches a pixel):
// - vramFetchers[*][0..3]: currChar, nextChar, lastCharIndex, lastCellX, charData, charDataAddress. VDP2PrepareLine
//   resets lastCharIndex/lastCellX/charDataAddress at every line (sw: "force-fetch first character"), so the
//   character and its data are fetched again before they are read.
// - vramFetchers[*][2..3].lastVCellScroll: vertical cell scroll exists for NBG0 and NBG1 only; NBG2/3 never read it.
// - vramFetchers[*][4..5]: whole. The RBG fetchers are reset the same way, and RBGs have no vertical cell scroll.
// Anything else (vdp1State, nbgLayerStates, rotParamStates, lineBackLayerState, vcellScrollInc and NBG0/1
// lastVCellScroll, which is carried from line to line) must be identical.

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
        !brimir::DecodeSaveState(payload, state)) {
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

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: brimir_state_diff <a.state> <b.state>\n");
        return 1;
    }
    auto a = std::make_unique<SaveState>();
    auto b = std::make_unique<SaveState>();
    if (!Load(argv[1], *a) || !Load(argv[2], *b)) {
        return 2;
    }

    std::printf("other sections (informational; RTC-driven memory differs between runs):\n");
    std::printf("  system %zu, msh2 %zu, ssh2 %zu, scsp %zu, vdp (without renderer) %zu bytes differ\n",
                DifferingBytes(a->system, b->system), DifferingBytes(a->msh2, b->msh2),
                DifferingBytes(a->ssh2, b->ssh2), DifferingBytes(a->scsp, b->scsp),
                DifferingBytes(a->vdp, b->vdp) - DifferingBytes(a->vdp.renderer, b->vdp.renderer));

    std::printf("renderer section:\n");
    Report r;
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
    std::printf("%u allowed and %u unexpected renderer differences\n", r.allowed, r.unexpected);
    return r.unexpected == 0 ? 0 : 3;
}
