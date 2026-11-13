// The software renderer's sprite layer fast path (SoftwareVDPRenderer::VDP2DrawSpriteLayerFast) against upstream's
// per-dot code: two renderers get the same seeded random scenes through the scene harness, one with the fast path
// forced off (SetSpriteFastPathForTesting), and must agree on the sprite and transparent mesh layer outputs (colour,
// priority, special colour calculation flag, colour calculation ratio, shadow/window flag, special type; both fields)
// after every line and on every frame.
//
// The scenes cover every sprite type (by combo) on 16-bit and 8-bit framebuffers, rotated or not (TVMR), palette and
// mixed palette/RGB data, the sprite window (normal and inverted), framebuffer contents with transparent runs of every
// length and alignment, normal shadow and MSB patterns, transparent meshes drawn by VDP1, and the compose inputs at
// random: NBG0-3 and RBG0, layer priorities 0-7 (ties between layers), special priority and colour calculation, every
// colour calculation mode (ratio and additive, second screen ratio, extended, gradation), the colour calculation
// window, layer windows, line colour and back screens (single and per line), shadow enables, colour offset A/B, CRAM
// modes, normal/hi-res/exclusive monitor widths and interlace with and without deinterlacing. Registers change from
// line to line (sprite type and format, colour calculation, shadow, offsets, priorities, windows, layer enables).

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

using brimir::test::Vdp1Cmd;
using brimir::test::Vdp2SceneWriter;

namespace {

using namespace ymir;
using namespace ymir::vdp;

// VDP2 register offsets
constexpr uint32_t TVMD = 0x000, RAMCTL = 0x00E, CYCA0L = 0x010, CYCA1L = 0x014, CYCB0L = 0x018, CYCB1L = 0x01C,
                   BGON = 0x020, SFSEL = 0x024, SFCODE = 0x026, CHCTLA = 0x028, CHCTLB = 0x02A, PNCN0 = 0x030,
                   PNCR = 0x038, PLSZ = 0x03A, MPOFN = 0x03C, MPOFR = 0x03E, MPABN0 = 0x040, MPABRA = 0x050,
                   MPABRB = 0x060, SCXIN0 = 0x070, ZMXIN0 = 0x078, ZMYIN0 = 0x07C, ZMXIN1 = 0x088, ZMYIN1 = 0x08C,
                   SCXN2 = 0x090, LCTAU = 0x0A8, LCTAL = 0x0AA, BKTAU = 0x0AC, BKTAL = 0x0AE, RPMD = 0x0B0,
                   RPRCTL = 0x0B2, KTCTL = 0x0B4, KTAOF = 0x0B6, OVPNRA = 0x0B8, RPTAU = 0x0BC, RPTAL = 0x0BE,
                   WPSX0 = 0x0C0, WCTLA = 0x0D0, WCTLB = 0x0D2, WCTLC = 0x0D4, WCTLD = 0x0D6, LWTA0U = 0x0D8,
                   LWTA0L = 0x0DA, LWTA1U = 0x0DC, LWTA1L = 0x0DE, SPCTL = 0x0E0, SDCTL = 0x0E2, CRAOFA = 0x0E4,
                   CRAOFB = 0x0E6, LNCLEN = 0x0E8, SFPRMD = 0x0EA, CCCTL = 0x0EC, SFCCMD = 0x0EE, PRISA = 0x0F0,
                   PRINA = 0x0F8, PRINB = 0x0FA, PRIR = 0x0FC, CCRSA = 0x100, CCRNA = 0x108, CCRNB = 0x10A,
                   CCRR = 0x10C, CCRLB = 0x10E, CLOFEN = 0x110, CLOFSL = 0x112, COAR = 0x114;
// VDP1 register offsets
constexpr uint32_t TVMR = 0x00, FBCR = 0x02;

// VDP2 VRAM layout: characters in bank A, NBG pattern names in bank B, tables at the top
constexpr uint32_t kPageBase = 0x40000;
constexpr uint32_t kRbgPageBase = 0x60000;
constexpr uint32_t kRotTableAddress = 0x7FE00;
constexpr uint32_t kLineWindowAddress = 0x7E000;
constexpr uint32_t kLineColorAddress = 0x7F000;
constexpr uint32_t kBackTableAddress = 0x7F800;

uint32_t Rand(std::mt19937 &rng, uint32_t bound) {
    return std::uniform_int_distribution<uint32_t>(0, bound - 1)(rng);
}

int RandI(std::mt19937 &rng, int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}

uint16_t Rand16(std::mt19937 &rng) {
    return static_cast<uint16_t>(rng());
}

// Colour data width of each sprite type (normal shadow = all ones but the LSB)
uint32_t SpriteColorBits(uint32_t type) {
    static constexpr uint32_t kBits[16] = {11, 11, 11, 11, 10, 11, 10, 9, 7, 6, 6, 6, 8, 8, 8, 8};
    return kBits[type & 15];
}

struct SceneConfig {
    uint32_t spriteType = 0;
    uint16_t tvmr = 0; // VDP1 TVMR: bit 0 8-bit framebuffer, bit 1 rotation
    bool mixed = false;
    bool spriteWindow = false;
    bool meshes = false; // transparent meshes enhancement, meshed polygons drawn
    bool rbg0 = false;
    uint16_t tvmd = 0x8000;
    bool deinterlace = false;
    uint32_t transparentPercent = 40;
    uint32_t shadowPercent = 10;
    uint32_t msbPercent = 10;
};

// Combo -> scene: sprite type and framebuffer mode from the combo (every type on 16-bit, 8-bit, rotated 16-bit and
// rotated 8-bit framebuffers over 64 combos), the rest seeded.
SceneConfig ConfigFor(uint32_t combo) {
    std::mt19937 rng{combo * 2654435761u + 17u};
    SceneConfig c;
    c.spriteType = combo % 16;
    c.tvmr = static_cast<uint16_t>((combo / 16) % 4);
    c.mixed = Rand(rng, 2) == 1;
    c.spriteWindow = Rand(rng, 3) == 0;
    c.meshes = Rand(rng, 2) == 1;
    // Rotated sprites read the RBG0 rotation parameters' sprite coordinates
    c.rbg0 = (c.tvmr & 2) != 0 || Rand(rng, 4) == 0;
    static constexpr uint16_t kInterlace[] = {0x0000, 0x0080, 0x00C0};
    c.tvmd = static_cast<uint16_t>(0x8000 | Rand(rng, 8) | kInterlace[Rand(rng, 3)] | (Rand(rng, 2) << 4) |
                                   (Rand(rng, 2) << 8));
    c.deinterlace = Rand(rng, 2) == 1;
    c.transparentPercent = 10 + Rand(rng, 70);
    c.shadowPercent = Rand(rng, 4) == 0 ? 0 : Rand(rng, 25);
    c.msbPercent = Rand(rng, 4) == 0 ? 0 : Rand(rng, 25);
    return c;
}

// The draw framebuffer: runs of transparent (zero) values of every length and alignment, runs of random values,
// normal shadow patterns and MSB values, then swapped to display after the meshed polygons (if any) are drawn.
void FillFramebuffer(Vdp2SceneWriter &w, std::mt19937 &rng, const SceneConfig &c) {
    const uint32_t fb = w.DrawFramebuffer();
    const bool eightBit = (c.tvmr & 1) != 0;
    const uint32_t mask = (1u << SpriteColorBits(c.spriteType)) - 1u;
    uint32_t run = 0;
    bool zeroRun = false;
    auto nextValue = [&]() -> uint16_t {
        if (run == 0) {
            zeroRun = Rand(rng, 100) < c.transparentPercent;
            run = zeroRun ? 1 + Rand(rng, Rand(rng, 4) == 0 ? 200 : 40) : 1 + Rand(rng, 24);
        }
        run--;
        if (zeroRun) {
            return 0;
        }
        uint16_t value = Rand16(rng);
        if (Rand(rng, 100) < c.shadowPercent) {
            value = static_cast<uint16_t>((value & ~mask) | (mask - 1));
        } else if (!c.mixed && Rand(rng, 2) == 0) {
            value &= 0x7FFF;
        }
        if (Rand(rng, 100) < c.msbPercent) {
            value |= 0x8000;
            if (Rand(rng, 2) == 0) {
                value &= static_cast<uint16_t>(~mask); // transparent colour data with the shadow/window bit
            }
        }
        return value;
    };
    for (uint32_t address = 0; address < 0x40000; address += 2) {
        uint16_t value;
        if (eightBit) {
            // Two 8-bit dots per word
            const uint16_t hi = static_cast<uint16_t>(nextValue() & 0xFF);
            const uint16_t lo = static_cast<uint16_t>(nextValue() & 0xFF);
            value = static_cast<uint16_t>((hi << 8) | lo);
        } else {
            value = nextValue();
        }
        w.FBRAM16(fb, address, value);
    }
}

// Meshed polygons (VDP1, transparent meshes enhancement): random rectangles in RGB or palette colours, some with MSB on
void DrawMeshes(Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t width, uint32_t height) {
    uint32_t slot = 0;
    const auto put = [&](const Vdp1Cmd &cmd) { w.VDP1Command(0x20 * slot++, cmd); };
    put(Vdp1Cmd::SystemClip(static_cast<int>(width) - 1, static_cast<int>(height) - 1));
    put(Vdp1Cmd::Local(0, 0));
    for (uint32_t i = 0; i < 12; ++i) {
        const int ax = RandI(rng, -20, static_cast<int>(width));
        const int ay = RandI(rng, -20, static_cast<int>(height));
        const int wd = RandI(rng, 4, 160), ht = RandI(rng, 4, 120);
        uint16_t pmod = 0x0100; // mesh
        if (Rand(rng, 4) == 0) {
            pmod |= 0x8000; // MSB on
        }
        const uint16_t colr = Rand(rng, 2) ? static_cast<uint16_t>(0x8000 | Rand16(rng))
                                           : static_cast<uint16_t>(Rand16(rng) & 0x7FFF);
        put(Vdp1Cmd::Polygon(pmod, colr, ax, ay, ax + wd, ay, ax + wd, ay + ht, ax, ay + ht));
    }
    put(Vdp1Cmd::End());
    w.DrawVDP1();
}

// Two-word cells for NBG n (16 or 256 colours) with random pattern names (special bits, palettes, flips)
void SetupNBG(Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t n, uint16_t &chctla, uint16_t &chctlb, uint16_t &mpofn) {
    const uint32_t format = n < 2 ? Rand(rng, 2) : 0;
    const bool cell2x2 = Rand(rng, 2) == 1;
    switch (n) {
    case 0: chctla = static_cast<uint16_t>((chctla & 0xFF00) | (format << 4) | (cell2x2 ? 1 : 0)); break;
    case 1: chctla = static_cast<uint16_t>((chctla & 0x00FF) | (format << 12) | ((cell2x2 ? 1 : 0) << 8)); break;
    case 2: chctlb = static_cast<uint16_t>((chctlb & ~0x0003) | (cell2x2 ? 1 : 0)); break;
    default: chctlb = static_cast<uint16_t>((chctlb & ~0x0030) | ((cell2x2 ? 1 : 0) << 4)); break;
    }
    w.Reg(PNCN0 + 2 * n, 0x0000);
    const uint32_t pageShift = cell2x2 ? 12 : 14;
    const uint32_t pageAddress = kPageBase + n * 0x8000;
    const uint32_t mapIndex = pageAddress >> pageShift;
    const uint16_t mp = static_cast<uint16_t>(((mapIndex & 0x3F) << 8) | (mapIndex & 0x3F));
    w.Reg(MPABN0 + 4 * n, mp);
    w.Reg(MPABN0 + 4 * n + 2, mp);
    mpofn = static_cast<uint16_t>((mpofn & ~(7u << (4 * n))) | ((mapIndex >> 6) << (4 * n)));
    const uint32_t entries = cell2x2 ? 32 * 32 : 64 * 64;
    const uint32_t charStride = (format == 0 ? 1 : 2) * (cell2x2 ? 4 : 1);
    for (uint32_t i = 0; i < entries; ++i) {
        const uint16_t w0 = static_cast<uint16_t>((Rand16(rng) & 0xF000) | Rand(rng, 128));
        w.VRAM16(pageAddress + i * 4, w0);
        w.VRAM16(pageAddress + i * 4 + 2, static_cast<uint16_t>(Rand(rng, 64) * charStride));
    }
}

// RBG0 with random rotation parameter tables (two-word cells, pattern names in bank B1): random coordinates, so the
// rotated sprite coordinates fall inside and outside the framebuffer
void SetupRBG0(Vdp2SceneWriter &w, std::mt19937 &rng, uint16_t &chctlb, uint16_t &ramctl) {
    ramctl = static_cast<uint16_t>((ramctl & 0x3000) | 0x0200 | 0x0003 | (2 << 6));
    chctlb = static_cast<uint16_t>((chctlb & 0x00FF) | (Rand(rng, 2) << 12) | (Rand(rng, 2) << 8));
    w.Reg(PRIR, static_cast<uint16_t>(Rand(rng, 8)));
    w.Reg(PLSZ, static_cast<uint16_t>(Rand(rng, 4) << 10));
    w.Reg(OVPNRA, Rand16(rng));
    w.Reg(PNCR, 0x0000);
    const uint32_t mapIndex = kRbgPageBase >> 14;
    w.Reg(MPOFR, static_cast<uint16_t>((mapIndex >> 6) | ((mapIndex >> 6) << 4)));
    const uint16_t mp = static_cast<uint16_t>(((mapIndex & 0x3F) << 8) | (mapIndex & 0x3F));
    for (uint32_t reg = 0; reg < 8; ++reg) {
        w.Reg(MPABRA + reg * 2, mp);
        w.Reg(MPABRB + reg * 2, mp);
    }
    for (uint32_t i = 0; i < 64 * 64; ++i) {
        w.VRAM16(kRbgPageBase + i * 4, static_cast<uint16_t>((Rand16(rng) & 0xF000) | Rand(rng, 128)));
        w.VRAM16(kRbgPageBase + i * 4 + 2, static_cast<uint16_t>(Rand(rng, 64) * 2));
    }
    // Rotation parameter tables A and B: matrix and scale around 1 (16.10 / 8.16), random start and pivots
    for (uint32_t t = 0; t < 2; ++t) {
        const uint32_t base = kRotTableAddress + t * 0x80;
        const auto fixed = [&](int lo, int hi) {
            return static_cast<uint32_t>(RandI(rng, lo, hi)) << 6;
        };
        w.VRAM32(base + 0x00, fixed(-64 * 1024, 256 * 1024)); // Xst
        w.VRAM32(base + 0x04, fixed(-64 * 1024, 256 * 1024)); // Yst
        w.VRAM32(base + 0x08, 0);                              // Zst
        w.VRAM32(base + 0x0C, fixed(-1024, 1024));             // dXst
        w.VRAM32(base + 0x10, fixed(512, 1536));               // dYst
        w.VRAM32(base + 0x14, fixed(512, 1536));               // dX
        w.VRAM32(base + 0x18, fixed(-512, 512));               // dY
        for (uint32_t m = 0; m < 6; ++m) {
            const bool diagonal = m == 0 || m == 4;
            w.VRAM32(base + 0x1C + m * 4, diagonal ? fixed(700, 1400) : fixed(-500, 500)); // A-F
        }
        for (uint32_t p = 0; p < 8; ++p) {
            w.VRAM16(base + 0x34 + p * 2, static_cast<uint16_t>(RandI(rng, -32, 200))); // Px..Cz
        }
        w.VRAM32(base + 0x44, fixed(-32 * 1024, 32 * 1024)); // Mx
        w.VRAM32(base + 0x48, fixed(-32 * 1024, 32 * 1024)); // My
        w.VRAM32(base + 0x4C, static_cast<uint32_t>(RandI(rng, 0xC000, 0x14000))); // kx
        w.VRAM32(base + 0x50, static_cast<uint32_t>(RandI(rng, 0xC000, 0x14000))); // ky
        w.VRAM32(base + 0x54, 0);
        w.VRAM32(base + 0x58, 0);
        w.VRAM32(base + 0x5C, 0);
    }
    w.Reg(RPTAU, static_cast<uint16_t>(kRotTableAddress >> 17));
    w.Reg(RPTAL, static_cast<uint16_t>((kRotTableAddress >> 1) & 0xFFFE));
    w.Reg(RPMD, static_cast<uint16_t>(Rand(rng, 4)));
    w.Reg(RPRCTL, 0);
    w.Reg(KTCTL, 0);
    w.Reg(KTAOF, 0);
}

// Windows: random rectangles (hi-res units horizontally), line windows on some, random enables/inside-outside/logic
// for every layer, the sprite window, the rotation parameter and colour calculation windows
void Windows(Vdp2SceneWriter &w, std::mt19937 &rng) {
    for (uint32_t i = 0; i < 8; i += 2) {
        const bool horizontal = (i & 2) == 0;
        w.Reg(WPSX0 + i, static_cast<uint16_t>(Rand(rng, horizontal ? 0x2C0 : 0x100)));
        w.Reg(WPSX0 + 8 + i, static_cast<uint16_t>(Rand(rng, horizontal ? 0x2C0 : 0x100)));
    }
    const bool none = Rand(rng, 3) == 0;
    w.Reg(WCTLA, none ? 0 : Rand16(rng));
    w.Reg(WCTLB, none ? 0 : Rand16(rng));
    w.Reg(WCTLC, none ? 0 : Rand16(rng));
    w.Reg(WCTLD, none ? 0 : Rand16(rng));
    w.Reg(LWTA0U, static_cast<uint16_t>(Rand(rng, 2) << 15 | (kLineWindowAddress >> 17)));
    w.Reg(LWTA0L, static_cast<uint16_t>((kLineWindowAddress >> 1) & 0xFFFE));
    w.Reg(LWTA1U, static_cast<uint16_t>(Rand(rng, 2) << 15 | ((kLineWindowAddress + 0x800) >> 17)));
    w.Reg(LWTA1L, static_cast<uint16_t>(((kLineWindowAddress + 0x800) >> 1) & 0xFFFE));
}

// Sprite control: the scene's type and format, random colour calculation condition and number
uint16_t SpriteControl(std::mt19937 &rng, uint32_t type, bool mixed, bool window) {
    return static_cast<uint16_t>((Rand(rng, 4) << 12) | (Rand(rng, 8) << 8) | (mixed ? 0x20 : 0) |
                                 (window ? 0x10 : 0) | type);
}

// Colour calculation, shadow, line colour, colour offset and priority registers, all at random
void ComposeRegs(Vdp2SceneWriter &w, std::mt19937 &rng) {
    w.Reg(SDCTL, static_cast<uint16_t>(Rand16(rng) & 0x013F));
    w.Reg(CCCTL, static_cast<uint16_t>(Rand16(rng) & 0xF77F));
    w.Reg(SFCCMD, static_cast<uint16_t>(Rand16(rng) & 0x03FF));
    w.Reg(SFPRMD, static_cast<uint16_t>(Rand16(rng) & 0x03FF));
    w.Reg(SFSEL, static_cast<uint16_t>(Rand16(rng) & 0x001F));
    w.Reg(SFCODE, Rand16(rng));
    w.Reg(LNCLEN, static_cast<uint16_t>(Rand16(rng) & 0x003F));
    w.Reg(CLOFEN, static_cast<uint16_t>(Rand16(rng) & 0x007F));
    w.Reg(CLOFSL, static_cast<uint16_t>(Rand16(rng) & 0x007F));
    for (uint32_t i = 0; i < 6; ++i) {
        // 9-bit signed offsets; zero now and then (an offset set with nothing to add)
        w.Reg(COAR + 2 * i, static_cast<uint16_t>(Rand(rng, 4) == 0 ? 0 : Rand16(rng) & 0x01FF));
    }
    for (uint32_t i = 0; i < 4; ++i) {
        w.Reg(PRISA + 2 * i, static_cast<uint16_t>(Rand16(rng) & 0x0707));
        w.Reg(CCRSA + 2 * i, static_cast<uint16_t>(Rand16(rng) & 0x1F1F));
    }
    w.Reg(PRINA, static_cast<uint16_t>(Rand16(rng) & 0x0707));
    w.Reg(PRINB, static_cast<uint16_t>(Rand16(rng) & 0x0707));
    w.Reg(CCRNA, static_cast<uint16_t>(Rand16(rng) & 0x1F1F));
    w.Reg(CCRNB, static_cast<uint16_t>(Rand16(rng) & 0x1F1F));
    w.Reg(CCRR, static_cast<uint16_t>(Rand16(rng) & 0x001F));
    w.Reg(CCRLB, static_cast<uint16_t>(Rand16(rng) & 0x1F1F));
    w.Reg(CRAOFA, static_cast<uint16_t>(Rand16(rng) & 0x7777));
    w.Reg(CRAOFB, static_cast<uint16_t>(Rand16(rng) & 0x0077));
}

void Setup(Vdp2SceneWriter &w, std::mt19937 &rng, const SceneConfig &c) {
    w.Reg(TVMD, c.tvmd);
    uint16_t ramctl = static_cast<uint16_t>(Rand(rng, 3) << 12); // CRAM mode before the CRAM writes
    uint16_t chctla = 0, chctlb = 0, mpofn = 0;
    w.Reg(RAMCTL, ramctl);
    for (uint32_t a = 0; a < 0x1000; a += 2) {
        w.CRAM16(a, Rand16(rng));
    }
    // Characters (bank A) and the tables at the top of VRAM: random
    for (uint32_t a = 0; a < 0x8000; a += 2) {
        w.VRAM16(a, Rand16(rng));
    }
    for (uint32_t a = kLineWindowAddress; a < 0x80000; a += 2) {
        w.VRAM16(a, Rand16(rng));
    }
    // Every NBG two character reads and a pattern name read (normal resolution, 16/256 colours)
    for (uint32_t reg : {CYCA0L, CYCA1L}) {
        w.Reg(reg, 0x7766);
        w.Reg(reg + 2, 0x4455);
    }
    for (uint32_t reg : {CYCB0L, CYCB1L}) {
        w.Reg(reg, 0x0123);
        w.Reg(reg + 2, 0xFFFF);
    }
    for (uint32_t n = 0; n < 4; ++n) {
        SetupNBG(w, rng, n, chctla, chctlb, mpofn);
    }
    // NBG0/NBG1 integer scroll X and Y (SCXIN0, SCYIN0, SCXIN1, SCYIN1), NBG2/NBG3 scroll
    for (uint32_t reg : {SCXIN0, SCXIN0 + 4, SCXIN0 + 0x10, SCXIN0 + 0x14}) {
        w.Reg(reg, Rand16(rng));
    }
    w.Reg(SCXN2, Rand16(rng));
    w.Reg(SCXN2 + 2, Rand16(rng));
    w.Reg(SCXN2 + 4, Rand16(rng));
    w.Reg(SCXN2 + 6, Rand16(rng));
    w.Reg(ZMXIN0, 1);
    w.Reg(ZMYIN0, 1);
    w.Reg(ZMXIN1, 1);
    w.Reg(ZMYIN1, 1);
    uint16_t bgon = static_cast<uint16_t>(Rand16(rng) & 0x0F0F);
    if (c.rbg0) {
        SetupRBG0(w, rng, chctlb, ramctl);
        bgon = static_cast<uint16_t>(bgon | 0x0010 | (Rand(rng, 4) == 0 ? 0x1000 : 0));
    }
    w.Reg(RAMCTL, ramctl);
    w.Reg(CHCTLA, chctla);
    w.Reg(CHCTLB, chctlb);
    w.Reg(MPOFN, mpofn);
    w.Reg(BGON, bgon);

    // Line colour and back screens: per line or single
    w.Reg(LCTAU, static_cast<uint16_t>((Rand(rng, 2) << 15) | (kLineColorAddress >> 17)));
    w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
    w.Reg(BKTAU, static_cast<uint16_t>((Rand(rng, 2) << 15) | (kBackTableAddress >> 17)));
    w.Reg(BKTAL, static_cast<uint16_t>((kBackTableAddress >> 1) & 0xFFFF));

    Windows(w, rng);
    ComposeRegs(w, rng);
    w.Reg(SPCTL, SpriteControl(rng, c.spriteType, c.mixed, c.spriteWindow));

    // Sprites: VDP1 framebuffer mode, contents, meshes, then the swap that displays them
    w.VDP1Reg(TVMR, c.tvmr);
    w.VDP1Reg(FBCR, 0);
    FillFramebuffer(w, rng, c);
    if (c.meshes) {
        DrawMeshes(w, rng, w.HRes(), w.VRes());
    }
    w.SwapFramebuffers();
}

// Per-line changes (both renderers see the same writes)
void OnLine(Vdp2SceneWriter &w, std::mt19937 &rng, const SceneConfig &c) {
    if (Rand(rng, 24) == 0) {
        // Another sprite type and format for the rest of the frame
        w.Reg(SPCTL, SpriteControl(rng, Rand(rng, 16), Rand(rng, 2) == 1, Rand(rng, 3) == 0));
    } else if (Rand(rng, 16) == 0) {
        w.Reg(SPCTL, SpriteControl(rng, c.spriteType, c.mixed, c.spriteWindow));
    }
    if (Rand(rng, 12) == 0) {
        ComposeRegs(w, rng);
    }
    if (Rand(rng, 16) == 0) {
        Windows(w, rng);
    }
    if (Rand(rng, 20) == 0) {
        w.Reg(BGON, static_cast<uint16_t>((Rand16(rng) & 0x0F0F) | (c.rbg0 ? 0x0010 : 0)));
    }
    if (Rand(rng, 10) == 0) {
        w.Reg(SCXIN0, Rand16(rng));
    }
}

struct PairResult {
    std::string firstDifference;
    uint64_t comparedLines = 0;
    uint64_t spriteDots = 0;     // field 0 sprite layer dots with a non-zero priority
    uint64_t meshDots = 0;       // ... of the mesh layer
    uint64_t shadowDots = 0;     // field 0 sprite dots of the normal shadow type
    uint64_t windowDots = 0;     // field 0 sprite dots with the shadow/window flag set
    uint64_t distinctColors = 0; // in the last frame (up to 64 counted)
    uint64_t fastSpriteLines = 0;
    uint64_t fallbackSpriteLines = 0;
    uint32_t frames = 0;
};

// Renders `frames` frames of a scene with the fast path on (renderer 0) and forced off (renderer 1) and compares.
PairResult RunPair(uint32_t seed, const SceneConfig &c, uint32_t frames, bool threaded = false) {
    config::Enhancements enhancements{};
    enhancements.deinterlace = c.deinterlace;
    enhancements.transparentMeshes = c.meshes;
    std::array<std::unique_ptr<VDPState>, 2> states{std::make_unique<VDPState>(), std::make_unique<VDPState>()};
    config::VDP2DebugRender debug{};
    config::VDP2AccessPatternsConfig access{};
    std::array<std::unique_ptr<SoftwareVDPRenderer>, 2> renderers{
        std::make_unique<SoftwareVDPRenderer>(*states[0], debug, access),
        std::make_unique<SoftwareVDPRenderer>(*states[1], debug, access)};
    renderers[1]->SetSpriteFastPathForTesting(false);

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
    if (threaded) {
        for (auto &renderer : renderers) {
            renderer->EnableThreadedVDP2(true);
            renderer->EnableThreadedDeinterlacer(true);
        }
    }

    uint32_t frame = 0;
    auto compareLayers = [&](const char *where, uint32_t y) {
        if (!result.firstDifference.empty()) {
            return;
        }
        for (uint32_t field = 0; field < 2; ++field) {
            for (uint32_t mesh = 0; mesh < 2; ++mesh) {
                const auto a = renderers[0]->GetSpriteLayerForTesting(field, mesh);
                const auto b = renderers[1]->GetSpriteLayerForTesting(field, mesh);
                for (uint32_t x = 0; x < kMaxResH; ++x) {
                    if (a.color[x].u32 != b.color[x].u32 || a.priority[x] != b.priority[x] ||
                        a.specialColorCalc[x] != b.specialColorCalc[x] || a.colorCalcRatio[x] != b.colorCalcRatio[x] ||
                        a.shadowOrWindow[x] != b.shadowOrWindow[x] || a.specialType[x] != b.specialType[x]) {
                        std::ostringstream out;
                        out << where << " frame " << frame << " line " << y << " field " << field
                            << (mesh ? " mesh" : " sprite") << " x " << x << ": fast color " << std::hex
                            << a.color[x].u32 << std::dec << " prio " << int(a.priority[x]) << " scc "
                            << a.specialColorCalc[x] << " ratio " << int(a.colorCalcRatio[x]) << " sow "
                            << a.shadowOrWindow[x] << " special " << int(a.specialType[x]) << "; per-dot color "
                            << std::hex << b.color[x].u32 << std::dec << " prio " << int(b.priority[x]) << " scc "
                            << b.specialColorCalc[x] << " ratio " << int(b.colorCalcRatio[x]) << " sow "
                            << b.shadowOrWindow[x] << " special " << int(b.specialType[x]);
                        result.firstDifference = out.str();
                        return;
                    }
                }
            }
        }
        result.comparedLines++;
        const auto s = renderers[0]->GetSpriteLayerForTesting(false, false);
        const auto m = renderers[0]->GetSpriteLayerForTesting(false, true);
        for (uint32_t x = 0; x < writer.HRes(); ++x) {
            result.spriteDots += s.priority[x] != 0;
            result.meshDots += m.priority[x] != 0;
            result.shadowDots += s.specialType[x] == SpriteData::Special::Shadow;
            result.windowDots += s.shadowOrWindow[x];
        }
    };
    // Before RenderLine(y) the layer outputs hold line y - 1's
    if (!threaded) {
        writer.lineObserver = [&](const Vdp2SceneWriter &, const VDPState &, uint32_t y) {
            if (y > 0) {
                compareLayers("after", y - 1);
            }
        };
    }

    std::mt19937 rng{seed};
    Setup(writer, rng, c);
    std::mt19937 lineRng{seed ^ 0x9E3779B9u};
    for (frame = 0; frame < frames; ++frame) {
        writer.RunFrame([&](Vdp2SceneWriter &w, uint32_t) { OnLine(w, lineRng, c); });
        compareLayers("end of", writer.FieldLines() - 1);
        if (result.firstDifference.empty() && outputs[0] != outputs[1]) {
            const size_t i = static_cast<size_t>(
                std::mismatch(outputs[0].begin(), outputs[0].end(), outputs[1].begin()).first - outputs[0].begin());
            std::ostringstream out;
            out << "frame " << frame << " output differs";
            if (outputs[0].size() == outputs[1].size() && i < outputs[0].size()) {
                out << " at pixel " << i << " (" << i % writer.HRes() << ", " << i / writer.HRes() << "): fast "
                    << std::hex << outputs[0][i] << " per-dot " << outputs[1][i];
            }
            result.firstDifference = out.str();
        }
        result.frames++;
    }
    std::vector<uint32_t> seen;
    for (uint32_t color : outputs[0]) {
        color &= 0xFFFFFF;
        if (seen.size() < 64 && std::find(seen.begin(), seen.end(), color) == seen.end()) {
            seen.push_back(color);
        }
    }
    result.distinctColors = seen.size();
    if (threaded) {
        for (auto &renderer : renderers) {
            renderer->EnableThreadedVDP2(false);
        }
    }
    result.fastSpriteLines = renderers[0]->GetSpriteFastPathLinesForTesting();
    result.fallbackSpriteLines = renderers[1]->GetSpriteFastPathLinesForTesting();
    return result;
}

} // namespace

TEST_CASE("Sprite layer fast path matches upstream's per-dot code on random scenes",
          "[vdp][vdp2][sprite][fastpath][brimir]") {
    // 64 combos: sprite type 0-F x 16-bit, 8-bit, rotated 16-bit, rotated 8-bit framebuffer
    const uint32_t combo = GENERATE(range(0u, 64u));
    const SceneConfig c = ConfigFor(combo);
    CAPTURE(combo, c.spriteType, c.tvmr, c.mixed, c.spriteWindow, c.meshes, c.rbg0, c.tvmd, c.deinterlace);

    const PairResult r = RunPair(3000 + combo, c, 3);
    INFO(r.firstDifference);
    CHECK(r.firstDifference.empty());
    CHECK(r.frames == 3);
    CHECK(r.comparedLines >= 3 * 224);
    // Non-vacuous: the fast path drew every line, the forced-off renderer none ...
    CHECK(r.fastSpriteLines >= 3 * 224);
    CHECK(r.fallbackSpriteLines == 0);
    // ... the sprite layer showed dots, the frame has content
    CAPTURE(r.spriteDots, r.meshDots, r.shadowDots, r.windowDots, r.distinctColors);
    CHECK(r.distinctColors >= 16);
    if ((c.tvmr & 2) == 0) {
        CHECK(r.spriteDots >= 1000);
        if (c.meshes) {
            CHECK(r.meshDots > 0);
        }
    } else {
        // Rotated: the random rotation parameters put part of the screen outside the framebuffer
        CHECK(r.spriteDots > 0);
    }
}

TEST_CASE("Sprite layer fast path covers normal shadow and shadow/window dots",
          "[vdp][vdp2][sprite][fastpath][brimir]") {
    // Shadow-heavy scenes on the types with a shadow/window bit (2-7) and without, 16- and 8-bit, sprite window on
    // (normal and inverted, from the random WCTLC) and off
    const uint32_t type = GENERATE(range(0u, 16u));
    const uint16_t tvmr = GENERATE(uint16_t{0}, uint16_t{1});
    const bool spriteWindow = GENERATE(false, true);
    SceneConfig c = ConfigFor(type + tvmr * 16);
    c.tvmr = tvmr;
    c.rbg0 = false;
    c.spriteWindow = spriteWindow;
    c.shadowPercent = 30;
    c.msbPercent = 30;
    c.transparentPercent = 30;
    CAPTURE(type, tvmr, spriteWindow, c.mixed, c.meshes, c.tvmd, c.deinterlace);

    const PairResult r = RunPair(7000 + type * 4 + tvmr * 2 + (spriteWindow ? 1 : 0), c, 2);
    INFO(r.firstDifference);
    CHECK(r.firstDifference.empty());
    CHECK(r.comparedLines >= 2 * 224);
    CAPTURE(r.spriteDots, r.shadowDots, r.windowDots);
    // 8-bit reads of the 16-bit types (0-7) set the upper byte: no normal shadow pattern there
    if (!(tvmr == 1 && type < 8)) {
        CHECK(r.shadowDots > 0);
    }
    if (type >= 2 && type <= 7 && tvmr == 0) {
        CHECK(r.windowDots > 0);
    }
}

TEST_CASE("Sprite layer fast path matches with the threaded VDP2 and deinterlace renderers",
          "[vdp][vdp2][sprite][fastpath][threaded][brimir]") {
    // The libretro core's default: threaded VDP2 plus the threaded deinterlacer (the alternate field drawn on its own
    // thread); frames compared
    const uint32_t combo = GENERATE(range(0u, 64u, 9u));
    const uint16_t interlace = GENERATE(uint16_t{0x0080}, uint16_t{0x00C0});
    SceneConfig c = ConfigFor(combo);
    c.tvmd = static_cast<uint16_t>(0x8000 | interlace | (combo & 3));
    c.deinterlace = true;
    CAPTURE(combo, c.tvmd);

    const PairResult r = RunPair(11000 + combo, c, 3, true);
    INFO(r.firstDifference);
    CHECK(r.firstDifference.empty());
    CHECK(r.frames == 3);
    CHECK(r.fastSpriteLines > 0);
    CHECK(r.fallbackSpriteLines == 0);
}
