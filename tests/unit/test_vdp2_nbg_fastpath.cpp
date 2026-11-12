// The software renderer's NBG character-run fast path (SoftwareVDPRenderer::VDP2DrawNormalScrollCharRuns) against its
// per-dot path: two renderers get the same seeded random scenes through the scene harness, one with the fast path
// forced off (SetNBGFastPathForTesting), and must agree on every NBG layer output (colour, priority, special colour
// calculation flag; both fields) after every line, on every frame and on the VRAM fetchers' saved state.
//
// The scenes cover every character mode, cell size, colour format and CRAM mode on NBG0 (by seed), and at random the
// other NBGs, plane sizes and maps across banks (data access offset and character pattern delay via random cycle
// patterns), fractional and line scroll, horizontal zoom and line zoom, windows (rectangles, line windows,
// inside/outside), transparency, special priority and colour calculation modes, palette supplements, normal/hi-res
// and interlaced modes with and without deinterlacing. The "mixed" scenes add what the fast path leaves to the per-dot
// path (mosaic, vertical cell scroll), switched on and off from line to line, so fast and per-dot lines alternate on
// the same fetchers.

#include "catch_amalgamated.hpp"

#include "vdp2_scene.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using brimir::test::Vdp2Scene;
using brimir::test::Vdp2SceneWriter;

namespace {

using namespace ymir;
using namespace ymir::vdp;

// VDP2 register offsets
constexpr uint32_t TVMD = 0x000, RAMCTL = 0x00E, CYCA0L = 0x010, BGON = 0x020, MZCTL = 0x022, SFSEL = 0x024,
                   SFCODE = 0x026, CHCTLA = 0x028, CHCTLB = 0x02A, PNCN0 = 0x030, PLSZ = 0x03A, MPOFN = 0x03C,
                   MPABN0 = 0x040, SCXIN0 = 0x070, ZMXIN0 = 0x078, ZMXDN0 = 0x07A, ZMYIN0 = 0x07C, ZMYDN0 = 0x07E,
                   SCXIN1 = 0x080, ZMXIN1 = 0x088, ZMXDN1 = 0x08A, ZMYIN1 = 0x08C, ZMYDN1 = 0x08E, SCXN2 = 0x090,
                   ZMCTL = 0x098, SCRCTL = 0x09A, VCSTAU = 0x09C, VCSTAL = 0x09E, LSTA0U = 0x0A0, LSTA0L = 0x0A2,
                   LSTA1U = 0x0A4, LSTA1L = 0x0A6, BKTAU = 0x0AC, BKTAL = 0x0AE, WPSX0 = 0x0C0, WCTLA = 0x0D0,
                   WCTLB = 0x0D2, LWTA0U = 0x0D8, LWTA0L = 0x0DA, LWTA1U = 0x0DC, LWTA1L = 0x0DE, CRAOFA = 0x0E4,
                   SFPRMD = 0x0EA, CCCTL = 0x0EC, SFCCMD = 0x0EE, PRINA = 0x0F8, PRINB = 0x0FA, CCRNA = 0x108,
                   CCRNB = 0x10A;

uint32_t Rand(std::mt19937 &rng, uint32_t bound) {
    return std::uniform_int_distribution<uint32_t>(0, bound - 1)(rng);
}

uint16_t Rand16(std::mt19937 &rng) {
    return static_cast<uint16_t>(rng());
}

// What a scene leaves out: eligible scenes only use features the fast path covers, mixed scenes switch mosaic and
// vertical cell scroll on and off from line to line.
enum class Kind { Eligible, Mixed };

// Cycle patterns (pattern names in bank B, characters in bank A): one legal character read per NBG (16 colours), every
// bank A slot for NBG0 characters (enough for any colour format NBG0 has), or (half of the mixed scenes) random
// nibbles (no access, delayed patterns, the data access offset; vdp_state.hpp CalcAccessPatterns).
void Cycles(Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t nbg0Format, bool randomPatterns) {
    const uint32_t choice = randomPatterns && Rand(rng, 2) == 0 ? 2 : (nbg0Format == 0 ? Rand(rng, 2) : 1);
    if (choice == 1) {
        for (uint32_t i = 0; i < 4; ++i) {
            w.Reg(CYCA0L + i * 2, 0x4444);
            w.Reg(CYCA0L + 8 + i * 2, (i & 1) ? 0xFFFF : 0x0123);
        }
    } else if (choice == 0) {
        for (uint32_t i = 0; i < 2; ++i) {
            w.Reg(CYCA0L + i * 4, 0x4567);
            w.Reg(CYCA0L + i * 4 + 2, 0x4567);
            w.Reg(CYCA0L + 8 + i * 4, 0x0123);
            w.Reg(CYCA0L + 8 + i * 4 + 2, 0x0123);
        }
    } else {
        for (uint32_t i = 0; i < 8; ++i) {
            uint16_t v = 0;
            for (uint32_t n = 0; n < 4; ++n) {
                // Mostly NBG pattern name (0-3) and character (4-7) reads, some no-access (F) and others
                const uint32_t r = Rand(rng, 10);
                const uint32_t nibble = r < 8 ? r : (r == 8 ? 0xF : 0xC + Rand(rng, 3));
                v = static_cast<uint16_t>(v | (nibble << (4 * n)));
            }
            w.Reg(CYCA0L + i * 2, v);
        }
    }
}

// Random VRAM with many zero nibbles (transparent dots in every colour format) and random CRAM.
void FillSceneMemory(Vdp2SceneWriter &w, std::mt19937 &rng) {
    for (uint32_t a = 0; a < 0x80000; a += 2) {
        uint16_t v = Rand16(rng);
        const uint32_t zeros = rng();
        for (uint32_t n = 0; n < 4; ++n) {
            if (((zeros >> (2 * n)) & 3) == 0) {
                v = static_cast<uint16_t>(v & ~(0xFu << (4 * n)));
            }
        }
        w.VRAM16(a, v);
    }
    for (uint32_t a = 0; a < 0x1000; a += 2) {
        w.CRAM16(a, Rand16(rng));
    }
}

// SCRCTL with what the scene kind allows: line scroll X/Y, line zoom and the line scroll interval always; vertical
// cell scroll only in mixed scenes.
uint16_t ScrollControl(Kind kind, std::mt19937 &rng) {
    uint16_t v = Rand16(rng) & 0x3F3F;
    if (kind == Kind::Eligible) {
        v &= ~0x0101; // VCSCn of NBG0 and NBG1
    }
    return v;
}

// Horizontal zoom of NBG0 and NBG1: 1.0 (one dot per step) or a random step (zoom in and out, fractional)
void HorizontalZoom(Vdp2SceneWriter &w, std::mt19937 &rng) {
    for (uint32_t base : {ZMXIN0, ZMXIN1}) {
        const uint32_t dnReg = base == ZMXIN0 ? ZMXDN0 : ZMXDN1;
        if (Rand(rng, 2) == 0) {
            w.Reg(base, 1);
            w.Reg(dnReg, 0);
        } else {
            w.Reg(base, static_cast<uint16_t>(Rand(rng, 4)));
            w.Reg(dnReg, static_cast<uint16_t>(Rand16(rng) & 0xFF00));
        }
    }
}

void Windows(Vdp2SceneWriter &w, std::mt19937 &rng) {
    for (uint32_t i = 0; i < 8; i += 2) {
        // WPSX/WPEX are in hi-res units (shifted for normal resolution), WPSY/WPEY in lines
        const bool horizontal = (i & 2) == 0;
        w.Reg(WPSX0 + i, static_cast<uint16_t>(Rand(rng, horizontal ? 0x2C0 : 0x100)));
        w.Reg(WPSX0 + 8 + i, static_cast<uint16_t>(Rand(rng, horizontal ? 0x2C0 : 0x100)));
    }
    // Window 0/1 enables and inside/outside for NBG0-3, the AND/OR logic, sometimes all off
    const uint16_t mask = Rand(rng, 4) == 0 ? 0 : 0x8F8F;
    w.Reg(WCTLA, static_cast<uint16_t>(Rand16(rng) & mask & 0x8F8F & ~0x2020));
    w.Reg(WCTLB, static_cast<uint16_t>(Rand16(rng) & mask & 0x8F8F & ~0x2020));
    // Line windows (table in the top of VRAM, random data) on some
    w.Reg(LWTA0U, static_cast<uint16_t>(Rand(rng, 2) << 15 | 3));
    w.Reg(LWTA0L, 0xC000);
    w.Reg(LWTA1U, static_cast<uint16_t>(Rand(rng, 2) << 15 | 3));
    w.Reg(LWTA1L, 0xD000);
}

// The scene's registers. NBG0's character mode, cell size, colour format and the CRAM mode come from `combo`.
void Setup(Vdp2SceneWriter &w, std::mt19937 &rng, Kind kind, uint32_t combo, uint16_t tvmd) {
    const uint32_t format = combo % 5;
    const uint32_t cell2x2 = (combo / 5) % 2;
    const uint32_t charMode = (combo / 10) % 3; // 0 two words, 1 one word, 2 one word extended
    const uint32_t cramMode = (combo / 30) % 3;

    // 32-bit dots need 8 character reads: normal resolution only
    w.Reg(TVMD, static_cast<uint16_t>(format == 4 ? tvmd & ~0x0002 : tvmd));
    // The CRAM mode before the CRAM writes (mode 0 mirrors them into both halves)
    w.Reg(RAMCTL, static_cast<uint16_t>((cramMode << 12) | (Rand16(rng) & 0x0300)));
    FillSceneMemory(w, rng);
    Cycles(w, rng, format, kind == Kind::Mixed);

    // NBG0 from the combo, NBG1-3 at random (no bitmaps: they have their own path)
    uint16_t chctla = static_cast<uint16_t>((format << 4) | cell2x2);
    chctla = static_cast<uint16_t>(chctla | ((Rand16(rng) & 0x3100)));
    w.Reg(CHCTLA, chctla);
    w.Reg(CHCTLB, static_cast<uint16_t>(Rand16(rng) & 0x0033));
    // One-word supplements: palette, special bits, character number bits 14-10 with bits 14-13 clear (characters in
    // bank A, which the cycle patterns read)
    w.Reg(PNCN0, static_cast<uint16_t>((charMode == 0 ? 0 : 0x8000) | (charMode == 2 ? 0x4000 : 0) |
                                       (Rand16(rng) & 0x03E7)));
    for (uint32_t n = 1; n < 4; ++n) {
        w.Reg(PNCN0 + 2 * n, static_cast<uint16_t>(Rand16(rng) & 0xC3FF));
    }
    w.Reg(PLSZ, static_cast<uint16_t>(Rand16(rng) & 0x00FF));
    // NBG0's planes A-D: random pages of bank B (where the cycle patterns read pattern names); NBG1-3 anywhere
    const uint32_t pageShift = cell2x2 ? (charMode == 0 ? 12 : 11) : (charMode == 0 ? 14 : 13);
    const uint32_t bankBMap = 0x40000 >> pageShift;
    const uint32_t mapOffset = bankBMap >> 6;
    auto nbg0Map = [&] { return (bankBMap & 0x3F) + Rand(rng, std::min(bankBMap, 0x40u)); };
    w.Reg(MPOFN, static_cast<uint16_t>((Rand16(rng) & 0x7770) | mapOffset));
    w.Reg(MPABN0, static_cast<uint16_t>(nbg0Map() | (nbg0Map() << 8)));
    w.Reg(MPABN0 + 2, static_cast<uint16_t>(nbg0Map() | (nbg0Map() << 8)));
    for (uint32_t i = 2; i < 8; ++i) {
        w.Reg(MPABN0 + 2 * i, static_cast<uint16_t>(Rand16(rng) & 0x3F3F));
    }

    // Scroll (integer and fraction), vertical zoom, horizontal zoom by kind
    for (uint32_t i = 0; i < 4; ++i) {
        w.Reg(SCXIN0 + 2 * i, Rand16(rng));
        w.Reg(SCXIN1 + 2 * i, Rand16(rng));
    }
    for (uint32_t i = 0; i < 4; ++i) {
        w.Reg(SCXN2 + 2 * i, Rand16(rng));
    }
    w.Reg(ZMYIN0, static_cast<uint16_t>(Rand(rng, 2)));
    w.Reg(ZMYDN0, static_cast<uint16_t>(Rand16(rng) & 0xFF00));
    w.Reg(ZMYIN1, 1);
    w.Reg(ZMYDN1, 0);
    w.Reg(ZMCTL, static_cast<uint16_t>(Rand16(rng) & 0x0303));
    HorizontalZoom(w, rng);

    // Line scroll / vertical cell scroll tables (random data)
    w.Reg(SCRCTL, ScrollControl(kind, rng));
    w.Reg(VCSTAU, 3);
    w.Reg(VCSTAL, 0x8000);
    w.Reg(LSTA0U, 3);
    w.Reg(LSTA0L, 0xA000);
    w.Reg(LSTA1U, 3);
    w.Reg(LSTA1L, 0xB000);
    w.Reg(MZCTL, static_cast<uint16_t>(kind == Kind::Mixed ? Rand16(rng) & 0xFF0F : 0));

    Windows(w, rng);

    // Special functions, priorities, colour calculation, CRAM offsets
    w.Reg(SFSEL, static_cast<uint16_t>(Rand16(rng) & 0x000F));
    w.Reg(SFCODE, Rand16(rng));
    w.Reg(SFPRMD, static_cast<uint16_t>(Rand16(rng) & 0x00FF));
    w.Reg(SFCCMD, static_cast<uint16_t>(Rand16(rng) & 0x00FF));
    w.Reg(CCCTL, static_cast<uint16_t>(Rand16(rng) & 0x000F));
    w.Reg(CCRNA, static_cast<uint16_t>(Rand16(rng) & 0x1F1F));
    w.Reg(CCRNB, static_cast<uint16_t>(Rand16(rng) & 0x1F1F));
    // NBG0 priority 2-7: never 0 after the special priority modes clear bit 0
    w.Reg(PRINA, static_cast<uint16_t>((2 + Rand(rng, 6)) | ((1 + Rand(rng, 7)) << 8)));
    w.Reg(PRINB, static_cast<uint16_t>((1 + Rand(rng, 7)) | ((1 + Rand(rng, 7)) << 8)));
    w.Reg(CRAOFA, static_cast<uint16_t>(Rand16(rng) & 0x7777));
    w.Reg(BKTAU, 3);
    w.Reg(BKTAL, 0xFFF0);

    // NBG0 always, NBG1-3 at random; transparency off on some
    w.Reg(BGON, static_cast<uint16_t>(1 | (Rand16(rng) & 0x0F0E)));
}

// Per-line changes: scroll and windows always; in mixed scenes the per-dot features too.
void OnLine(Vdp2SceneWriter &w, std::mt19937 &rng, Kind kind) {
    if (Rand(rng, 8) == 0) {
        w.Reg(SCXIN0 + 2 * Rand(rng, 2), Rand16(rng));
    }
    if (Rand(rng, 8) == 0) {
        w.Reg(SCXN2, Rand16(rng));
    }
    if (Rand(rng, 16) == 0) {
        Windows(w, rng);
    }
    if (Rand(rng, 32) == 0) {
        w.Reg(PNCN0 + 2 * Rand(rng, 4), static_cast<uint16_t>(Rand16(rng) & 0xC3FF));
    }
    if (Rand(rng, 8) == 0) {
        HorizontalZoom(w, rng);
    }
    if (kind == Kind::Mixed) {
        if (Rand(rng, 6) == 0) {
            w.Reg(MZCTL, static_cast<uint16_t>(Rand(rng, 2) ? Rand16(rng) & 0xFF0F : 0));
        }
        if (Rand(rng, 6) == 0) {
            w.Reg(SCRCTL, ScrollControl(Rand(rng, 2) ? Kind::Mixed : Kind::Eligible, rng));
        }
    }
}

struct PairResult {
    std::string firstDifference;
    uint64_t comparedLines = 0;
    uint64_t visibleDots = 0;  // NBG0 dots (field 0) with a non-zero priority over the compared lines
    uint64_t distinctColors = 0; // distinct NBG0 colours among them (up to 64 counted)
    uint64_t fastDots = 0;     // the fast-path renderer
    uint64_t fallbackDots = 0; // the forced-off renderer (must stay 0)
    uint32_t frames = 0;
    std::array<std::array<bool, 4>, 4> charPatDelay{};    // [NBG][bank] after the last frame
    std::array<std::array<uint32, 4>, 4> vramDataOffset{}; // [NBG][bank]
};

// Renders `frames` frames of a scene (setup, then onLine before every line) with the fast path on (renderer 0) and
// forced off (renderer 1) and compares.
PairResult RunPair(const std::function<void(Vdp2SceneWriter &)> &setup,
                   const std::function<void(Vdp2SceneWriter &)> &onLine, bool deinterlace, uint32_t frames) {
    config::Enhancements enhancements{};
    enhancements.deinterlace = deinterlace;
    std::array<std::unique_ptr<VDPState>, 2> states{std::make_unique<VDPState>(), std::make_unique<VDPState>()};
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    std::array<std::unique_ptr<SoftwareVDPRenderer>, 2> renderers{
        std::make_unique<SoftwareVDPRenderer>(*states[0], debug, access),
        std::make_unique<SoftwareVDPRenderer>(*states[1], debug, access)};
    renderers[1]->SetNBGFastPathForTesting(false);

    std::array<std::vector<uint32_t>, 2> outputs;
    for (uint32_t i = 0; i < 2; ++i) {
        renderers[i]->SwCallbacks.FrameComplete.Rebind(&outputs[i], [](uint32 *fb, uint32 w, uint32 h, void *context) {
            static_cast<std::vector<uint32_t> *>(context)->assign(fb, fb + static_cast<size_t>(w) * h);
        });
    }

    PairResult result;
    Vdp2SceneWriter writer{enhancements};
    writer.Attach(*states[0], *renderers[0]);
    writer.Attach(*states[1], *renderers[1]);

    uint32_t frame = 0;
    std::vector<uint32_t> seenColors;
    auto compareLayers = [&](const char *where, uint32_t y) {
        if (!result.firstDifference.empty()) {
            return;
        }
        for (uint32_t field = 0; field < 2; ++field) {
            for (uint32_t n = 0; n < 4; ++n) {
                const auto &c0 = renderers[0]->GetNBGColorsForTesting(field, n);
                const auto &c1 = renderers[1]->GetNBGColorsForTesting(field, n);
                const auto &p0 = renderers[0]->GetNBGPrioritiesForTesting(field, n);
                const auto &p1 = renderers[1]->GetNBGPrioritiesForTesting(field, n);
                const auto &s0 = renderers[0]->GetNBGSpecialColorCalcForTesting(field, n);
                const auto &s1 = renderers[1]->GetNBGSpecialColorCalcForTesting(field, n);
                for (uint32_t x = 0; x < kMaxResH; ++x) {
                    if (c0[x].u32 != c1[x].u32 || p0[x] != p1[x] || s0[x] != s1[x]) {
                        std::ostringstream out;
                        out << where << " frame " << frame << " line " << y << " field " << field << " NBG" << n
                            << " x " << x << ": fast color " << std::hex << c0[x].u32 << " prio " << std::dec
                            << int(p0[x]) << " cc " << s0[x] << "; per-dot color " << std::hex << c1[x].u32
                            << " prio " << std::dec << int(p1[x]) << " cc " << s1[x];
                        result.firstDifference = out.str();
                        return;
                    }
                }
            }
        }
        result.comparedLines++;
        const auto &colors = renderers[0]->GetNBGColorsForTesting(false, 0);
        const auto &priorities = renderers[0]->GetNBGPrioritiesForTesting(false, 0);
        for (uint32_t x = 0; x < writer.HRes(); ++x) {
            if (priorities[x] != 0) {
                result.visibleDots++;
                const uint32_t color = colors[x].u32 & 0xFFFFFF;
                if (result.distinctColors < 64 &&
                    std::find(seenColors.begin(), seenColors.end(), color) == seenColors.end()) {
                    seenColors.push_back(color);
                    result.distinctColors++;
                }
            }
        }
    };
    // Before RenderLine(y) the layer outputs hold line y - 1's
    writer.lineObserver = [&](const Vdp2SceneWriter &, const VDPState &, uint32_t y) {
        if (y > 0) {
            compareLayers("after", y - 1);
        }
    };

    setup(writer);
    for (frame = 0; frame < frames; ++frame) {
        writer.RunFrame([&](Vdp2SceneWriter &w, uint32_t) {
            if (onLine) {
                onLine(w);
            }
        });
        compareLayers("end of", writer.FieldLines() - 1);
        if (result.firstDifference.empty() && outputs[0] != outputs[1]) {
            result.firstDifference = "frame " + std::to_string(frame) + " output differs";
        }
        result.frames++;
    }

    // The fetchers' saved state (character pipeline, character data cache)
    const auto saved = writer.SaveStates();
    for (uint32_t field = 0; field < 2 && result.firstDifference.empty(); ++field) {
        for (uint32_t n = 0; n < 4; ++n) {
            const auto &a = saved[0]->renderer.vramFetchers[field][n];
            const auto &b = saved[1]->renderer.vramFetchers[field][n];
            const auto charEq = [](const auto &l, const auto &r) {
                return l.charNum == r.charNum && l.palNum == r.palNum && l.specColorCalc == r.specColorCalc &&
                       l.specPriority == r.specPriority && l.flipH == r.flipH && l.flipV == r.flipV;
            };
            if (!charEq(a.currChar, b.currChar) || !charEq(a.nextChar, b.nextChar) ||
                a.lastCharIndex != b.lastCharIndex || a.lastCellX != b.lastCellX || a.charData != b.charData ||
                a.charDataAddress != b.charDataAddress || a.lastVCellScroll != b.lastVCellScroll) {
                result.firstDifference = "saved VRAM fetcher " + std::to_string(field) + "/" + std::to_string(n);
                break;
            }
        }
    }
    for (uint32_t n = 0; n < 4; ++n) {
        result.charPatDelay[n] = states[0]->regs2.bgParams[n + 1].charPatDelay;
        result.vramDataOffset[n] = states[0]->regs2.bgParams[n + 1].vramDataOffset;
    }
    result.fastDots = renderers[0]->GetNBGFastPathDotsForTesting();
    result.fallbackDots = renderers[1]->GetNBGFastPathDotsForTesting();
    return result;
}

PairResult RunRandomPair(uint32_t seed, Kind kind, uint32_t combo, uint16_t tvmd, bool deinterlace, uint32_t frames) {
    std::mt19937 lineRng{seed ^ 0x9E3779B9u};
    return RunPair(
        [&](Vdp2SceneWriter &w) {
            std::mt19937 rng{seed};
            Setup(w, rng, kind, combo, tvmd);
        },
        [&](Vdp2SceneWriter &w) { OnLine(w, lineRng, kind); }, deinterlace, frames);
}

// TVMD: display on, horizontal resolution (normal 320/352, hi-res 640/704), interlace (none, single, double density)
uint16_t TvmdFor(std::mt19937 &rng) {
    static constexpr uint16_t kInterlace[] = {0x0000, 0x0080, 0x00C0};
    return static_cast<uint16_t>(0x8000 | Rand(rng, 4) | kInterlace[Rand(rng, 3)] | (Rand(rng, 2) << 4));
}

} // namespace

TEST_CASE("NBG character-run fast path matches the per-dot path on eligible scenes",
          "[vdp][vdp2][nbg][fastpath][brimir]") {
    // 90 combos: NBG0 colour format x cell size x character mode x CRAM mode
    const uint32_t combo = GENERATE(range(0u, 90u));
    std::mt19937 modeRng{combo * 7919u + 1u};
    const uint16_t tvmd = TvmdFor(modeRng);
    const bool deinterlace = Rand(modeRng, 2) == 1;
    CAPTURE(combo, tvmd, deinterlace);

    const PairResult r = RunRandomPair(1000 + combo, Kind::Eligible, combo, tvmd, deinterlace, 3);
    INFO(r.firstDifference);
    CHECK(r.firstDifference.empty());
    CHECK(r.frames == 3);
    CHECK(r.comparedLines >= 3 * 224);
    // Non-vacuous: the fast path drew most of the line dots of NBG0 alone, the forced-off renderer none
    CHECK(r.fastDots >= 3 * 224 * 320 / 4);
    // ... and NBG0 shows content: visible dots in many colours
    CAPTURE(r.visibleDots, r.distinctColors);
    CHECK(r.visibleDots >= 1000);
    CHECK(r.distinctColors >= 16);
    CHECK(r.fallbackDots == 0);
}

TEST_CASE("NBG character-run fast path matches the per-dot path when lines switch between the paths",
          "[vdp][vdp2][nbg][fastpath][brimir]") {
    const uint32_t combo = GENERATE(range(0u, 60u));
    std::mt19937 modeRng{combo * 104729u + 7u};
    const uint16_t tvmd = TvmdFor(modeRng);
    const bool deinterlace = Rand(modeRng, 2) == 1;
    CAPTURE(combo, tvmd, deinterlace);

    const PairResult r = RunRandomPair(5000 + combo, Kind::Mixed, (combo * 37u) % 90u, tvmd, deinterlace, 3);
    INFO(r.firstDifference);
    CHECK(r.firstDifference.empty());
    CHECK(r.comparedLines >= 3 * 224);
    CHECK(r.fallbackDots == 0);
}

TEST_CASE("NBG character-run fast path keeps the character pattern delay and the data access offset",
          "[vdp][vdp2][nbg][fastpath][brimir]") {
    // Cycle patterns that CalcAccessPatterns (vdp_state.hpp) turns into a character pattern delay (NBG0: a character
    // read on T0 of the bank whose pattern name read is on T5) or a data access offset of 8 dots (NBG2: pattern name
    // read on T0, character read on the illegal T3), on 1x1 and 2x2 characters, at every horizontal scroll offset of a
    // 2x2 character (the delayed 2x2 pipeline switches currChar on the second dot of cell X 1).
    const bool offsetCase = GENERATE(false, true);
    const uint32_t format = GENERATE(0u, 1u); // 16 / 256 colours
    const uint32_t cell2x2 = GENERATE(0u, 1u);
    const uint32_t scrollX = GENERATE(range(0u, 16u));
    const uint32_t zoom = GENERATE(0x100u, 0xC0u, 0x168u); // NBG0's horizontal step (8.8): 1, 0.75, 1.40625
    const uint32_t nbg = offsetCase ? 2 : 0;
    CAPTURE(offsetCase, format, cell2x2, scrollX, zoom);
    if (offsetCase && zoom != 0x100u) {
        return; // NBG2 has no zoom
    }

    const auto setup = [&](Vdp2SceneWriter &w) {
        std::mt19937 rng{format * 1000 + cell2x2 * 100 + scrollX + (offsetCase ? 50000 : 0)};
        FillSceneMemory(w, rng);
        w.Reg(TVMD, 0x8000);
        // VRAM A partitioned for the delay case (A1's own cycle patterns count), not for the offset case
        w.Reg(RAMCTL, static_cast<uint16_t>(offsetCase ? 0x0000 : 0x0100));
        std::array<uint16_t, 8> cyc{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        if (!offsetCase) {
            // A0: T0 CP0 (and T1 CP0 for 256 colours), T5 PN0; A1: T0 CP0 for 256 colours
            cyc[0] = format == 0 ? 0x4FFF : 0x44FF;
            cyc[1] = 0xF0FF;
            cyc[2] = format == 0 ? 0xFFFF : 0x4FFF;
        } else {
            // A0: T0 PN2, T3 CP2; A1: T4 CP2 (a second character read for 256 colours)
            cyc[0] = 0x2FF6;
            cyc[3] = 0x6FFF;
        }
        for (uint32_t i = 0; i < 8; ++i) {
            w.Reg(CYCA0L + 2 * i, cyc[i]);
        }
        if (!offsetCase) {
            w.Reg(CHCTLA, static_cast<uint16_t>((format << 4) | cell2x2));
        } else {
            w.Reg(CHCTLB, static_cast<uint16_t>((format << 1) | cell2x2));
        }
        w.Reg(PNCN0 + 2 * nbg, static_cast<uint16_t>(Rand16(rng) & 0x83FF)); // one or two words, supplements
        w.Reg(PLSZ, static_cast<uint16_t>(3u << (2 * nbg)));                // 2x2 pages per plane
        // Planes A/B in bank A (pattern names and characters readable), C/D in bank B (no access)
        w.Reg(MPABN0 + 4 * nbg, 0x0400);
        w.Reg(MPABN0 + 4 * nbg + 2, 0x3830);
        w.Reg(MPOFN, 0);
        if (!offsetCase) {
            w.Reg(SCXIN0, static_cast<uint16_t>(scrollX + 1000));
            w.Reg(SCXIN0 + 2, static_cast<uint16_t>(Rand16(rng) & 0xFF00));
            w.Reg(SCXIN0 + 4, static_cast<uint16_t>(Rand(rng, 1024)));
            w.Reg(ZMXIN0, static_cast<uint16_t>(zoom >> 8));
            w.Reg(ZMXDN0, static_cast<uint16_t>((zoom & 0xFF) << 8));
            w.Reg(ZMYIN0, 1);
            w.Reg(PRINA, 5);
        } else {
            w.Reg(SCXN2, static_cast<uint16_t>(scrollX + 1000));
            w.Reg(SCXN2 + 2, static_cast<uint16_t>(Rand(rng, 1024)));
            w.Reg(PRINB, 5);
        }
        w.Reg(SFPRMD, static_cast<uint16_t>(Rand(rng, 3) << (2 * nbg)));
        w.Reg(SFCCMD, static_cast<uint16_t>(Rand(rng, 4) << (2 * nbg)));
        w.Reg(BKTAU, 3);
        w.Reg(BKTAL, 0xFFF0);
        w.Reg(BGON, static_cast<uint16_t>(1u << nbg));
    };
    std::mt19937 lineRng{scrollX * 31u + format};
    const auto onLine = [&](Vdp2SceneWriter &w) {
        // Move the line start to every dot of the cell now and then
        if (Rand(lineRng, 16) == 0) {
            w.Reg(offsetCase ? SCXN2 : SCXIN0, static_cast<uint16_t>(Rand(lineRng, 4096)));
        }
    };
    const PairResult r = RunPair(setup, onLine, false, 2);
    INFO(r.firstDifference);
    CHECK(r.firstDifference.empty());
    CHECK(r.comparedLines >= 2 * 224);
    if (offsetCase) {
        CHECK(r.vramDataOffset[nbg][0] == 8u);
    } else {
        CHECK(r.charPatDelay[nbg][0]);
    }
    CHECK(r.fastDots >= 2 * 223 * 320);
    CHECK(r.fallbackDots == 0);
}

