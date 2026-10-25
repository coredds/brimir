// GPU VDP2 scenes: every scene runs through the software renderer and the Vulkan renderer (VulkanVDPRenderer: the
// front half plus the Vulkan back half, VDP1 in software) with the same register, VRAM, CRAM, VDP1 and framebuffer
// writes (vdp2_scene.hpp RenderBoth), and the last frames must be identical in RGB. Each scene first requires the
// software frame to be non-trivial (several distinct colours), so a scene cannot pass with two blank frames. Scene
// contents are mostly seeded random data (pattern names, characters, bitmaps, CRAM, sprite framebuffers, coefficient
// tables), which reaches many pixel paths per scene.
//
// Known divergences between the ported shaders and the software renderer are tagged [!shouldfail], with their letter
// from stage1b-port-inventory.md section 4.1 in the name; Task 6 fixes (b)-(g) and removes the tag. The ones Task 5
// fixed ((a), (i), (k), (l) and (q)-(u) from the Task 3 and 4 reviews) keep their letter in the name, as do the
// guards (j), (m)-(o). Documented exceptions (software behaviour the GPU path does not reproduce, by a controller
// ruling) are tagged [!shouldfail] too, named "exception <id>". Any other failure is a bug.
//
// Also here: upload overflow (64 KiB upload blocks), detach/attach between frames, and FramePresenter::PresentDevice
// against FramePresenter::Present of the read-back composite.
//
// [gpu][vdp2][scene]; SKIP without a Vulkan device. Every scene runs on both drivers (the full suite takes ~25 s on the
// desktop GPU, ~40 s on lavapipe); [lavapipe] marks a quick cross-section for manual runs. BRIMIR_VDP2_POISON=1 runs
// them with the layer images poisoned every frame (any read of a texel the frame did not write changes the result).
#include "catch_amalgamated.hpp"

#include "gpu_test_device.hpp"
#include "vdp2_scene.hpp"

#include <gpu/frame_presenter.hpp>
#include <gpu/vdp/vulkan_vdp_renderer.hpp>
#include <gpu/vk_util.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>

using brimir::test::CloseAndCheckValidation;
using brimir::test::OpenDevice;
using brimir::test::ReadbackBuffer;
using brimir::test::RenderBoth;
using brimir::test::RenderBothOptions;
using brimir::test::SceneResult;
using brimir::test::Vdp2Scene;
using brimir::test::Vdp2SceneWriter;

namespace {

namespace gpu = brimir::gpu;

// VDP2 register offsets
constexpr uint32_t TVMD = 0x000, RAMCTL = 0x00E, CYCA0L = 0x010, CYCA0U = 0x012, CYCA1L = 0x014, CYCA1U = 0x016,
                   CYCB0L = 0x018, CYCB0U = 0x01A, CYCB1L = 0x01C, CYCB1U = 0x01E, BGON = 0x020, MZCTL = 0x022,
                   SFSEL = 0x024, SFCODE = 0x026, CHCTLA = 0x028, CHCTLB = 0x02A, BMPNA = 0x02C, PNCN0 = 0x030,
                   PNCR = 0x038, PLSZ = 0x03A, MPOFN = 0x03C, MPOFR = 0x03E, MPABN0 = 0x040, MPABRA = 0x050,
                   MPABRB = 0x060, SCXIN0 = 0x070, SCXDN0 = 0x072, SCYIN0 = 0x074, SCYDN0 = 0x076, ZMXIN0 = 0x078,
                   ZMXDN0 = 0x07A, ZMYIN0 = 0x07C, ZMYDN0 = 0x07E, SCXIN1 = 0x080, SCYIN1 = 0x084, SCXN2 = 0x090,
                   SCYN2 = 0x092, SCXN3 = 0x094, SCYN3 = 0x096, ZMCTL = 0x098, SCRCTL = 0x09A, VCSTAU = 0x09C,
                   VCSTAL = 0x09E, LSTA0U = 0x0A0, LSTA0L = 0x0A2, LCTAU = 0x0A8, LCTAL = 0x0AA, BKTAU = 0x0AC,
                   BKTAL = 0x0AE, RPMD = 0x0B0, KTCTL = 0x0B4, KTAOF = 0x0B6, OVPNRA = 0x0B8, RPTAU = 0x0BC,
                   RPTAL = 0x0BE, WPSX0 = 0x0C0, WPSY0 = 0x0C2, WPEX0 = 0x0C4, WPEY0 = 0x0C6, WPSX1 = 0x0C8,
                   WPSY1 = 0x0CA, WPEX1 = 0x0CC, WPEY1 = 0x0CE, WCTLA = 0x0D0, WCTLB = 0x0D2, WCTLC = 0x0D4,
                   WCTLD = 0x0D6, LWTA0U = 0x0D8, LWTA0L = 0x0DA, LWTA1U = 0x0DC, LWTA1L = 0x0DE, SPCTL = 0x0E0,
                   SDCTL = 0x0E2, CRAOFA = 0x0E4, CRAOFB = 0x0E6, LNCLEN = 0x0E8, SFPRMD = 0x0EA, CCCTL = 0x0EC,
                   SFCCMD = 0x0EE, PRISA = 0x0F0, PRISB = 0x0F2, PRISC = 0x0F4, PRISD = 0x0F6, PRINA = 0x0F8,
                   PRINB = 0x0FA, PRIR = 0x0FC, CCRSA = 0x100, CCRSB = 0x102, CCRSC = 0x104, CCRSD = 0x106,
                   CCRNA = 0x108, CCRNB = 0x10A, CCRR = 0x10C, CCRLB = 0x10E, CLOFEN = 0x110, CLOFSL = 0x112,
                   COAR = 0x114, COAG = 0x116, COAB = 0x118, COBR = 0x11A, COBG = 0x11C, COBB = 0x11E;

// VDP1 register offsets
constexpr uint32_t TVMR = 0x00, FBCR = 0x02;

// TVMD values
constexpr uint16_t kDisp = 0x8000, kBDCLMD = 0x0100;
constexpr uint16_t kLSMDSingle = 0x0080, kLSMDDouble = 0x00C0;

// Where scenes put things in VDP2 VRAM
constexpr uint32_t kCharBase = 0x00000;         // character patterns and bitmaps (bank A)
constexpr uint32_t kPageBase = 0x40000;         // NBG n pattern names at kPageBase + n * kPageStride (bank B)
constexpr uint32_t kPageStride = 0x8000;
constexpr uint32_t kRotTableAddress = 0x7FE00;  // rotation parameter tables A and B (+0x80)
constexpr uint32_t kLineWindowAddress = 0x7E000; // line window tables (W0, W1 at +0x800)
constexpr uint32_t kLineScrollAddress = 0x7C000; // NBG0 line scroll table
constexpr uint32_t kLineColorAddress = 0x7F000; // line colour table
constexpr uint32_t kBackTableAddress = 0x7F800; // per-line back screen table
constexpr uint32_t kBackAddress = 0x7FFF0;      // single back screen colour

// ---------------------------------------------------------------------------------------------------------------------
// Scene building blocks

uint32_t Rand(std::mt19937 &rng, uint32_t bound) {
    return std::uniform_int_distribution<uint32_t>(0, bound - 1)(rng);
}

void FillVRAM(Vdp2SceneWriter &w, uint32_t address, uint32_t bytes, std::mt19937 &rng) {
    for (uint32_t a = 0; a < bytes; a += 2) {
        w.VRAM16(address + a, static_cast<uint16_t>(rng()));
    }
}

// Random CRAM, all 4 KiB.
void FillCRAM(Vdp2SceneWriter &w, std::mt19937 &rng) {
    for (uint32_t a = 0; a < 0x1000; a += 2) {
        w.CRAM16(a, static_cast<uint16_t>(rng()));
    }
}

// Shared registers several helpers contribute fields to; written by Flush.
struct Shadow {
    uint16_t bgon = 0, chctla = 0, chctlb = 0, mpofn = 0, prina = 0, prinb = 0, ramctl = 0, lnclen = 0;
    // Every layer and the back screen take sprite shadows (random sprite data has shadow pixels): per-layer shadow
    // enables are divergence (a).
    uint16_t sdctl = 0x003F;
};

void Flush(Vdp2SceneWriter &w, const Shadow &s) {
    w.Reg(RAMCTL, s.ramctl);
    w.Reg(CHCTLA, s.chctla);
    w.Reg(CHCTLB, s.chctlb);
    w.Reg(MPOFN, s.mpofn);
    w.Reg(PRINA, s.prina);
    w.Reg(PRINB, s.prinb);
    w.Reg(LNCLEN, s.lnclen);
    w.Reg(SDCTL, s.sdctl);
    w.Reg(BGON, s.bgon);
}

// Back screen: one colour (RGB555) at kBackAddress, or a per-line table of random colours at kBackTableAddress.
void BackColor(Vdp2SceneWriter &w, uint16_t color) {
    w.VRAM16(kBackAddress, color);
    w.Reg(BKTAU, static_cast<uint16_t>((kBackAddress >> 17) & 7));
    w.Reg(BKTAL, static_cast<uint16_t>((kBackAddress >> 1) & 0xFFFF));
}

void BackPerLine(Vdp2SceneWriter &w, std::mt19937 &rng) {
    FillVRAM(w, kBackTableAddress, 512 * 2, rng);
    w.Reg(BKTAU, static_cast<uint16_t>(0x8000 | ((kBackTableAddress >> 17) & 7)));
    w.Reg(BKTAL, static_cast<uint16_t>((kBackTableAddress >> 1) & 0xFFFF));
}

// Cycle patterns for one NBG: character/bitmap reads in every slot of bank A (kCharBase), pattern names at T0 of bank
// B (kPageBase). Hi-res modes use T0-T3 (up to 4 character reads).
void CyclesSingle(Vdp2SceneWriter &w, uint32_t nbg) {
    const uint16_t cp = static_cast<uint16_t>((4 + nbg) * 0x1111);
    w.Reg(CYCA0L, cp);
    w.Reg(CYCA0U, cp);
    w.Reg(CYCA1L, cp);
    w.Reg(CYCA1U, cp);
    w.Reg(CYCB0L, static_cast<uint16_t>((nbg << 12) | 0x0FFF));
    w.Reg(CYCB0U, 0xFFFF);
    w.Reg(CYCB1L, static_cast<uint16_t>((nbg << 12) | 0x0FFF));
    w.Reg(CYCB1U, 0xFFFF);
}

// Cycle patterns for NBG0-NBG3 together (normal resolution, 16/256 colours): two legal character slots each.
void CyclesAll(Vdp2SceneWriter &w) {
    for (uint32_t reg : {CYCA0L, CYCA1L}) {
        w.Reg(reg, 0x7766);
        w.Reg(reg + 2, 0x4455);
    }
    for (uint32_t reg : {CYCB0L, CYCB1L}) {
        w.Reg(reg, 0x0123);
        w.Reg(reg + 2, 0xFFFF);
    }
}

struct NBG {
    uint32_t index = 0;      // NBG0-3
    uint32_t format = 0;     // 0 palette16, 1 palette256, 2 palette2048, 3 RGB555, 4 RGB888
    bool cell2x2 = false;
    bool twoWord = true;
    bool extChar = false;    // one-word only
    bool bitmap = false;
    uint32_t bitmapSize = 0; // BMSZ: 0 512x256, 1 512x512, 2 1024x256, 3 1024x512
    bool transparency = true;
    uint32_t priority = 4;
    uint32_t chars = 32;     // distinct characters used
    bool specialBits = false; // random special priority / colour calculation bits in the pattern names
    bool flips = true;
};

constexpr uint32_t kFormatCells[] = {1, 2, 4, 4, 8};

// Configures an NBG (into the shadow registers plus its own registers) and fills its pattern names and character data
// (or bitmap) with random data.
void SetupNBG(Vdp2SceneWriter &w, Shadow &s, const NBG &bg, std::mt19937 &rng) {
    const uint32_t n = bg.index;
    switch (n) {
    case 0:
        s.chctla = static_cast<uint16_t>((s.chctla & 0xFF00) | (bg.format << 4) | (bg.bitmapSize << 2) |
                                         (bg.bitmap << 1) | (bg.cell2x2 ? 1 : 0));
        break;
    case 1:
        s.chctla = static_cast<uint16_t>((s.chctla & 0x00FF) | ((bg.format & 3) << 12) | (bg.bitmapSize << 10) |
                                         (bg.bitmap << 9) | ((bg.cell2x2 ? 1 : 0) << 8));
        break;
    case 2:
        s.chctlb = static_cast<uint16_t>((s.chctlb & ~0x0003) | ((bg.format & 1) << 1) | (bg.cell2x2 ? 1 : 0));
        break;
    case 3:
        s.chctlb = static_cast<uint16_t>((s.chctlb & ~0x0030) | ((bg.format & 1) << 5) | ((bg.cell2x2 ? 1 : 0) << 4));
        break;
    }
    s.bgon |= static_cast<uint16_t>((1u << n) | (bg.transparency ? 0 : 0x100u << n));
    if (n < 2) {
        s.prina = static_cast<uint16_t>((s.prina & (n ? 0x00FF : 0xFF00)) | (bg.priority << (8 * n)));
    } else {
        s.prinb = static_cast<uint16_t>((s.prinb & (n == 3 ? 0x00FF : 0xFF00)) | (bg.priority << (8 * (n - 2))));
    }

    if (bg.bitmap) {
        // Bitmap at kCharBase (map offset 0), random palette bits
        s.mpofn = static_cast<uint16_t>(s.mpofn & ~(7u << (4 * n)));
        if (n < 2) {
            w.Reg(BMPNA, static_cast<uint16_t>(Rand(rng, 8) << (8 * n)));
        }
        static constexpr uint32_t kW[] = {512, 512, 1024, 1024};
        static constexpr uint32_t kH[] = {256, 512, 256, 512};
        static constexpr uint32_t kBits[] = {4, 8, 16, 16, 32};
        const uint32_t bytes =
            std::min<uint32_t>(kW[bg.bitmapSize] * kH[bg.bitmapSize] * kBits[bg.format] / 8, 0x40000);
        FillVRAM(w, kCharBase, bytes, rng);
        return;
    }

    // Pattern name control: one word (bit 15), extended characters (bit 14), no supplements
    w.Reg(PNCN0 + 2 * n, static_cast<uint16_t>((bg.twoWord ? 0 : 0x8000) | (bg.extChar ? 0x4000 : 0)));
    // One plane of one page at kPageBase + n * kPageStride for planes A-D
    const uint32_t pageShift = bg.cell2x2 ? (bg.twoWord ? 12 : 11) : (bg.twoWord ? 14 : 13);
    const uint32_t pageAddress = kPageBase + n * kPageStride;
    const uint32_t mapIndex = pageAddress >> pageShift;
    const uint16_t mp = static_cast<uint16_t>(((mapIndex & 0x3F) << 8) | (mapIndex & 0x3F));
    w.Reg(MPABN0 + 4 * n, mp);
    w.Reg(MPABN0 + 4 * n + 2, mp);
    s.mpofn = static_cast<uint16_t>((s.mpofn & ~(7u << (4 * n))) | ((mapIndex >> 6) << (4 * n)));

    // Pattern names
    const uint32_t entries = bg.cell2x2 ? 32 * 32 : 64 * 64;
    const uint32_t charStride = kFormatCells[bg.format] * (bg.cell2x2 ? 4 : 1);
    for (uint32_t i = 0; i < entries; ++i) {
        const uint32_t charNum = Rand(rng, bg.chars) * charStride;
        const uint32_t pal = Rand(rng, 128);
        const uint32_t flipH = bg.flips ? Rand(rng, 2) : 0;
        const uint32_t flipV = bg.flips ? Rand(rng, 2) : 0;
        const uint32_t special = bg.specialBits ? Rand(rng, 2) : 0;
        const uint32_t specialPri = bg.specialBits ? Rand(rng, 2) : 0;
        if (bg.twoWord) {
            const uint16_t w0 =
                static_cast<uint16_t>((flipV << 15) | (flipH << 14) | (specialPri << 13) | (special << 12) | pal);
            w.VRAM16(pageAddress + i * 4, w0);
            w.VRAM16(pageAddress + i * 4 + 2, static_cast<uint16_t>(charNum));
        } else {
            const uint32_t cn = bg.cell2x2 ? charNum >> 2 : charNum;
            uint32_t v = bg.format == 0 ? (pal & 0xF) << 12 : (pal & 0x7) << 12;
            v |= bg.extChar ? (cn & 0xFFF) : ((flipV << 11) | (flipH << 10) | (cn & 0x3FF));
            w.VRAM16(pageAddress + i * 2, static_cast<uint16_t>(v));
        }
    }
    FillVRAM(w, kCharBase, bg.chars * charStride * 32, rng);
}

// Display on, CRAM mode, back colour, random CRAM.
void Base(Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng, uint16_t tvmd = kDisp, uint32_t cramMode = 0) {
    w.Reg(TVMD, tvmd);
    s.ramctl = static_cast<uint16_t>((s.ramctl & ~0x3000) | (cramMode << 12));
    BackColor(w, 0x1234);
    FillCRAM(w, rng);
}

// A single NBG scene.
Vdp2Scene SingleNBG(NBG bg, uint32_t cramMode = 0, uint16_t tvmd = kDisp, uint32_t seed = 1) {
    Vdp2Scene scene;
    scene.setup = [=](Vdp2SceneWriter &w) {
        std::mt19937 rng{seed};
        Shadow s;
        Base(w, s, rng, tvmd, cramMode);
        CyclesSingle(w, bg.index);
        SetupNBG(w, s, bg, rng);
        Flush(w, s);
    };
    return scene;
}

// NBG0 and NBG1 (16/256 colours, cells), both random, NBG0 above NBG1 unless priorities say otherwise.
void TwoNBGs(Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng, uint32_t pri0 = 5, uint32_t pri1 = 3,
             bool specialBits = false) {
    CyclesAll(w);
    SetupNBG(w, s, {.index = 0, .format = 1, .priority = pri0, .specialBits = specialBits}, rng);
    SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = true, .priority = pri1, .specialBits = specialBits}, rng);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sprites (VDP1 framebuffer written directly, then swapped to display)

struct Sprites {
    uint32_t type = 0;          // SPCTL.SPTYPE
    bool mixed = false;         // SPCTL.SPCLMD
    bool window = false;        // SPCTL.SPWINEN
    uint16_t tvmr = 0;          // VDP1 TVMR (0 = 16 bpp, 1 = 8 bpp)
    uint32_t transparentPercent = 30;
    uint32_t shadowPercent = 0; // normal shadow pixels
    uint32_t colorCalcCond = 0; // SPCTL.SPCCCS
    uint32_t colorCalcNum = 7;  // SPCTL.SPCCN
    bool rowConstant = false;   // one value per framebuffer row (insensitive to horizontal readout scaling)
};

uint32_t SpriteColorBits(uint32_t type) {
    static constexpr uint32_t kBits[16] = {11, 11, 11, 11, 10, 11, 10, 9, 7, 6, 6, 6, 8, 8, 8, 8};
    return kBits[type & 15];
}

void SetupSprites(Vdp2SceneWriter &w, const Sprites &sp, std::mt19937 &rng) {
    constexpr uint32_t width = 512, lines = 256;
    w.Reg(SPCTL, static_cast<uint16_t>((sp.colorCalcCond << 12) | (sp.colorCalcNum << 8) | (sp.mixed << 5) |
                                       (sp.window << 4) | sp.type));
    // Random priorities 1-7 and ratios for the eight sprite registers
    for (uint32_t reg = PRISA; reg <= PRISD; reg += 2) {
        w.Reg(reg, static_cast<uint16_t>((1 + Rand(rng, 7)) | ((1 + Rand(rng, 7)) << 8)));
    }
    for (uint32_t reg = CCRSA; reg <= CCRSD; reg += 2) {
        w.Reg(reg, static_cast<uint16_t>(Rand(rng, 32) | (Rand(rng, 32) << 8)));
    }
    w.VDP1Reg(TVMR, sp.tvmr);
    w.VDP1Reg(FBCR, 0);

    const uint32_t fb = w.DrawFramebuffer();
    const bool eightBit = (sp.tvmr & 1) != 0;
    const uint32_t bits = SpriteColorBits(sp.type);
    for (uint32_t y = 0; y < lines; ++y) {
        const uint16_t rowValue = static_cast<uint16_t>(1 + Rand(rng, 0x7FFE));
        for (uint32_t x = 0; x < width; x += eightBit ? 2 : 1) {
            if (sp.rowConstant) {
                const uint32_t address = eightBit ? y * 1024 + x : (y * 512 + x) * 2;
                w.FBRAM16(fb, address & 0x3FFFE, rowValue);
                continue;
            }
            uint16_t value = static_cast<uint16_t>(rng());
            if (Rand(rng, 100) < sp.transparentPercent) {
                value = 0;
            } else if (Rand(rng, 100) < sp.shadowPercent) {
                const uint16_t mask = static_cast<uint16_t>((1u << bits) - 1);
                value = static_cast<uint16_t>(((value & ~mask) | (mask - 1)) & 0x7FFF);
            } else if (!sp.mixed) {
                value &= 0x7FFF;
            }
            if (eightBit) {
                // Two 8-bit pixels; types 8-F read one byte each
                const uint16_t lo = static_cast<uint16_t>(Rand(rng, 100) < sp.transparentPercent ? 0 : rng() & 0xFF);
                value = static_cast<uint16_t>((value & 0xFF00) | lo);
            }
            const uint32_t address = eightBit ? y * 1024 + x : (y * 512 + x) * 2;
            w.FBRAM16(fb, address & 0x3FFFE, value);
        }
    }
    w.SwapFramebuffers();
}

// ---------------------------------------------------------------------------------------------------------------------
// Rotation backgrounds

struct RotTable {
    double xst = 0, yst = 0, zst = 0, dxst = 0, dyst = 1, dx = 1, dy = 0;
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
    int px = 160, py = 112, pz = 0, cx = 160, cy = 112, cz = 0;
    double mx = 0, my = 0, kx = 1, ky = 1, kast = 0, dkast = 0, dkax = 0;
};

uint32_t Fixed(double value, double one, uint32_t shift) {
    return static_cast<uint32_t>(static_cast<int32_t>(std::lround(value * one))) << shift;
}

void WriteRotTable(Vdp2SceneWriter &w, uint32_t address, const RotTable &t) {
    const auto w32 = [&](uint32_t offset, uint32_t value) { w.VRAM32(address + offset, value); };
    const auto w16 = [&](uint32_t offset, int value) { w.VRAM16(address + offset, static_cast<uint16_t>(value)); };
    w32(0x00, Fixed(t.xst, 1024, 6));
    w32(0x04, Fixed(t.yst, 1024, 6));
    w32(0x08, Fixed(t.zst, 1024, 6));
    w32(0x0C, Fixed(t.dxst, 1024, 6));
    w32(0x10, Fixed(t.dyst, 1024, 6));
    w32(0x14, Fixed(t.dx, 1024, 6));
    w32(0x18, Fixed(t.dy, 1024, 6));
    w32(0x1C, Fixed(t.a, 1024, 6));
    w32(0x20, Fixed(t.b, 1024, 6));
    w32(0x24, Fixed(t.c, 1024, 6));
    w32(0x28, Fixed(t.d, 1024, 6));
    w32(0x2C, Fixed(t.e, 1024, 6));
    w32(0x30, Fixed(t.f, 1024, 6));
    w16(0x34, t.px);
    w16(0x36, t.py);
    w16(0x38, t.pz);
    w16(0x3A, 0);
    w16(0x3C, t.cx);
    w16(0x3E, t.cy);
    w16(0x40, t.cz);
    w16(0x42, 0);
    w32(0x44, Fixed(t.mx, 1024, 6));
    w32(0x48, Fixed(t.my, 1024, 6));
    w32(0x4C, Fixed(t.kx, 65536, 0) & 0xFFFFFF);
    w32(0x50, Fixed(t.ky, 65536, 0) & 0xFFFFFF);
    w32(0x54, Fixed(t.kast, 1024, 6));
    w32(0x58, Fixed(t.dkast, 1024, 6));
    w32(0x5C, Fixed(t.dkax, 1024, 6));
}

RotTable Rotated(double angle, double scale) {
    RotTable t;
    t.a = std::cos(angle) * scale;
    t.b = -std::sin(angle) * scale;
    t.d = std::sin(angle) * scale;
    t.e = std::cos(angle) * scale;
    return t;
}

struct RBG {
    uint32_t format = 1;          // 0-4
    bool cell2x2 = false;
    bool bitmap = false;
    uint32_t screenOver = 0;      // PLSZ.RAOVR: repeat, repeat character, transparent, fixed 512
    bool coeffTable = false;
    uint32_t coeffMode = 0;       // KTCTL.RAKMD
    bool coeffOneWord = false;    // KTCTL.RAKDBS
    bool coeffCRAM = false;       // RAMCTL.CRKTE (CRAM mode 1)
    bool perDot = true;           // coefficients per dot (dKAx != 0) or per line
    bool coeffLineColor = false;  // KTCTL.RAKLCE
    uint32_t rpmd = 0;
    RotTable a = Rotated(0.3, 0.9);
    RotTable b = Rotated(-0.5, 1.3);
    uint32_t priority = 5;
    bool transparency = true;
};

// RBG0 alone: characters in bank A, pattern names in bank B1, VRAM coefficients in bank B0 (or CRAM).
void SetupRBG0(Vdp2SceneWriter &w, Shadow &s, const RBG &r, std::mt19937 &rng) {
    // RAMCTL: B partitioned; A = characters, B0 = coefficients (VRAM), B1 = pattern names
    const uint16_t rdbs = static_cast<uint16_t>(0x0003 | ((r.coeffTable && !r.coeffCRAM ? 1 : 0) << 4) | (2 << 6));
    s.ramctl = static_cast<uint16_t>((s.ramctl & 0x3000) | (r.coeffCRAM ? 0x8000 : 0) | 0x0200 | rdbs);
    if (r.coeffCRAM) {
        s.ramctl = static_cast<uint16_t>((s.ramctl & ~0x3000) | 0x1000);
    }
    s.chctlb = static_cast<uint16_t>((s.chctlb & 0x00FF) | (r.format << 12) | (r.bitmap << 9) |
                                     ((r.cell2x2 ? 1 : 0) << 8));
    s.bgon |= static_cast<uint16_t>(0x0010 | (r.transparency ? 0 : 0x1000));
    w.Reg(PRIR, static_cast<uint16_t>(r.priority));

    // Plane size 1x1, screen-over process
    w.Reg(PLSZ, static_cast<uint16_t>(r.screenOver << 10));
    w.Reg(OVPNRA, 0x0123);

    if (r.bitmap) {
        w.Reg(MPOFR, 0);
        static constexpr uint32_t kBits[] = {4, 8, 16, 16, 32};
        FillVRAM(w, kCharBase, std::min<uint32_t>(512 * 512 * kBits[r.format] / 8, 0x40000), rng);
    } else {
        w.Reg(PNCR, 0x0000); // two-word pattern names
        const uint32_t pageShift = r.cell2x2 ? 12 : 14;
        const uint32_t pageAddress = 0x60000;
        const uint32_t mapIndex = pageAddress >> pageShift;
        w.Reg(MPOFR, static_cast<uint16_t>((mapIndex >> 6) | ((mapIndex >> 6) << 4)));
        const uint16_t mp = static_cast<uint16_t>(((mapIndex & 0x3F) << 8) | (mapIndex & 0x3F));
        for (uint32_t reg = 0; reg < 8; ++reg) {
            w.Reg(MPABRA + reg * 2, mp);
            w.Reg(MPABRB + reg * 2, mp);
        }
        const uint32_t entries = r.cell2x2 ? 32 * 32 : 64 * 64;
        const uint32_t charStride = kFormatCells[r.format] * (r.cell2x2 ? 4 : 1);
        for (uint32_t i = 0; i < entries; ++i) {
            const uint32_t charNum = Rand(rng, 32) * charStride;
            const uint16_t w0 = static_cast<uint16_t>((Rand(rng, 4) << 14) | Rand(rng, 128));
            w.VRAM16(pageAddress + i * 4, w0);
            w.VRAM16(pageAddress + i * 4 + 2, static_cast<uint16_t>(charNum));
        }
        FillVRAM(w, kCharBase, 32 * charStride * 32, rng);
    }

    // Rotation parameter tables A and B
    RotTable a = r.a, b = r.b;
    if (r.coeffTable) {
        a.kast = 0;
        a.dkast = r.perDot ? 0.25 : 1.0;
        a.dkax = r.perDot ? 1.0 : 0.0;
        b.kast = 0;
        b.dkast = a.dkast;
        b.dkax = a.dkax;
    }
    WriteRotTable(w, kRotTableAddress, a);
    WriteRotTable(w, kRotTableAddress + 0x80, b);
    w.Reg(RPTAU, static_cast<uint16_t>(kRotTableAddress >> 17));
    w.Reg(RPTAL, static_cast<uint16_t>((kRotTableAddress >> 1) & 0xFFFE));
    w.Reg(RPMD, static_cast<uint16_t>(r.rpmd));

    if (r.coeffTable) {
        const uint16_t kt = static_cast<uint16_t>(1 | (r.coeffOneWord << 1) | (r.coeffMode << 2) |
                                                  (r.coeffLineColor << 4));
        w.Reg(KTCTL, static_cast<uint16_t>(kt | (kt << 8)));
        // Coefficients around 1.0, 1 in 16 transparent; two-word ones with random line colour data.
        const auto oneWord = [&] {
            return static_cast<uint16_t>((0x400 + Rand(rng, 0x200) - 0x100) | (Rand(rng, 16) ? 0 : 0x8000));
        };
        const auto twoWord = [&] {
            const uint32_t coeff = (0x10000 + Rand(rng, 0x8000) - 0x4000) & 0xFFFFFF;
            return coeff | (Rand(rng, 128) << 24) | (Rand(rng, 16) ? 0 : 0x80000000u);
        };
        // Coefficient tables: VRAM at 0x40000 (KTAOF selects it), or the upper half of CRAM
        if (r.coeffCRAM) {
            w.Reg(KTAOF, 0);
            for (uint32_t i = 0; i < (r.coeffOneWord ? 1024u : 512u); ++i) {
                if (r.coeffOneWord) {
                    w.CRAM16(0x800 + i * 2, oneWord());
                } else {
                    const uint32_t c = twoWord();
                    w.CRAM16(0x800 + i * 4, static_cast<uint16_t>(c >> 16));
                    w.CRAM16(0x800 + i * 4 + 2, static_cast<uint16_t>(c));
                }
            }
        } else {
            w.Reg(KTAOF, static_cast<uint16_t>(r.coeffOneWord ? 0x0202 : 0x0101));
            for (uint32_t i = 0; i < 1024; ++i) {
                if (r.coeffOneWord) {
                    w.VRAM16(0x40000 + i * 2, oneWord());
                } else {
                    w.VRAM32(0x40000 + i * 4, twoWord());
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Running and checking

// uniformFrame: the software frame is expected to be one colour (display off), which must not be black.
// Failures are CHECKs, not REQUIREs, so the caller still runs CloseAndCheckValidation after a failing scene.
SceneResult Check(gpu::VulkanDevice &device, const Vdp2Scene &scene, const RenderBothOptions &options = {},
                  bool uniformFrame = false) {
    SceneResult result = RenderBoth(scene, device, options);
    INFO(result.Describe());
    CHECK(result.error.empty());
    if (!result.error.empty()) {
        return result;
    }
    if (uniformFrame) {
        CHECK((!result.software.empty() && (result.software[0] & 0xFFFFFF) != 0));
    } else {
        CHECK(result.distinctColors > 1); // the software frame is not trivial
    }
    CHECK(result.identical);
    return result;
}

// Opens a device, checks the scene, closes the device with the validation check.
void RunScene(const Vdp2Scene &scene, const RenderBothOptions &options = {}, bool uniformFrame = false) {
    auto device = OpenDevice();
    Check(*device, scene, options, uniformFrame);
    CloseAndCheckValidation(device);
}

Vdp2Scene Make(std::function<void(Vdp2SceneWriter &, Shadow &, std::mt19937 &)> setup,
               ymir::vdp::config::Enhancements enhancements = {}, uint32_t frames = 2, uint32_t seed = 7) {
    Vdp2Scene scene;
    scene.enhancements = enhancements;
    scene.frames = frames;
    scene.setup = [setup = std::move(setup), seed](Vdp2SceneWriter &w) {
        std::mt19937 rng{seed};
        Shadow s;
        setup(w, s, rng);
        Flush(w, s);
    };
    return scene;
}

} // namespace

// =====================================================================================================================
// NBG

TEST_CASE("Scene: NBG0 cells 1x1 and 2x2 with two-word pattern names", "[gpu][vdp2][scene][lavapipe]") {
    const bool cell2x2 = GENERATE(false, true);
    CAPTURE(cell2x2);
    RunScene(SingleNBG({.index = 0, .format = 1, .cell2x2 = cell2x2}));
}

TEST_CASE("Scene: NBG0 one-word pattern names with and without extended characters", "[gpu][vdp2][scene]") {
    const bool cell2x2 = GENERATE(false, true);
    const bool extChar = GENERATE(false, true);
    const uint32_t format = GENERATE(0u, 1u);
    CAPTURE(cell2x2, extChar, format);
    RunScene(SingleNBG({.index = 0, .format = format, .cell2x2 = cell2x2, .twoWord = false, .extChar = extChar}));
}

TEST_CASE("Scene: NBG0 bitmaps of every size", "[gpu][vdp2][scene]") {
    const uint32_t size = GENERATE(0u, 1u, 2u, 3u);
    CAPTURE(size);
    RunScene(SingleNBG({.index = 0, .format = 1, .bitmap = true, .bitmapSize = size}));
}

TEST_CASE("Scene: NBG0 bitmaps in every colour format", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t format = GENERATE(0u, 1u, 2u, 3u, 4u);
    CAPTURE(format);
    RunScene(SingleNBG({.index = 0, .format = format, .bitmap = true, .bitmapSize = 0}));
}

TEST_CASE("Scene: NBG0 colour formats in CRAM modes 0 1 and 2", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t format = GENERATE(0u, 1u, 2u, 3u, 4u);
    const uint32_t cramMode = GENERATE(0u, 1u, 2u);
    CAPTURE(format, cramMode);
    RunScene(SingleNBG({.index = 0, .format = format}, cramMode));
}

TEST_CASE("Scene: NBG1 to NBG3 cells", "[gpu][vdp2][scene]") {
    const uint32_t index = GENERATE(1u, 2u, 3u);
    const uint32_t format = GENERATE(0u, 1u);
    const bool cell2x2 = GENERATE(false, true);
    CAPTURE(index, format, cell2x2);
    RunScene(SingleNBG({.index = index, .format = format, .cell2x2 = cell2x2}));
}

TEST_CASE("Scene: NBG1 bitmap and transparency code off", "[gpu][vdp2][scene]") {
    RunScene(SingleNBG({.index = 1, .format = 1, .bitmap = true, .bitmapSize = 2}));
    RunScene(SingleNBG({.index = 0, .format = 0, .transparency = false}));
}

TEST_CASE("Scene: four NBGs by priority", "[gpu][vdp2][scene][lavapipe]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesAll(w);
        SetupNBG(w, s, {.index = 0, .format = 1, .priority = 3}, rng);
        SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = true, .priority = 6}, rng);
        SetupNBG(w, s, {.index = 2, .format = 1, .priority = 5}, rng);
        SetupNBG(w, s, {.index = 3, .format = 0, .priority = 2}, rng);
        w.Reg(SCXIN1, 13);
        w.Reg(SCYIN1, 7);
        w.Reg(SCXN2, 100);
        w.Reg(SCYN3, 50);
    }));
}

TEST_CASE("Scene: special priority modes", "[gpu][vdp2][scene]") {
    const uint32_t mode = GENERATE(0u, 1u, 2u);
    CAPTURE(mode);
    RunScene(Make([mode](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng, 4, 4, true);
        w.Reg(SFPRMD, static_cast<uint16_t>(mode | (mode << 2)));
        w.Reg(SFSEL, 0x0002);  // NBG1 uses code B
        w.Reg(SFCODE, 0x5AA5); // codes A and B
    }));
}

TEST_CASE("Scene: special colour calculation modes", "[gpu][vdp2][scene]") {
    const uint32_t mode = GENERATE(0u, 1u, 2u, 3u);
    CAPTURE(mode);
    RunScene(Make([mode](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng, 5, 3, true);
        w.Reg(CCCTL, 0x0003); // NBG0 and NBG1 colour calculation
        w.Reg(CCRNA, 0x0A14);
        w.Reg(SFCCMD, static_cast<uint16_t>(mode | (mode << 2)));
        w.Reg(SFCODE, 0x33CC);
    }));
}

TEST_CASE("Scene: NBG scroll and zoom", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t variant = GENERATE(0u, 1u, 2u, 3u);
    CAPTURE(variant);
    RunScene(Make([variant](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesSingle(w, 0);
        SetupNBG(w, s, {.index = 0, .format = 1, .cell2x2 = variant == 1}, rng);
        w.Reg(SCXIN0, 0x0123);
        w.Reg(SCXDN0, 0x4500);
        w.Reg(SCYIN0, 0x0045);
        w.Reg(SCYDN0, 0x8000);
        switch (variant) {
        case 0: break;                                                    // scroll only
        case 1: w.Reg(ZMXIN0, 0), w.Reg(ZMXDN0, 0xC000); break;           // zoom in 0.75 horizontally
        case 2: w.Reg(ZMXIN0, 1), w.Reg(ZMXDN0, 0x8000), w.Reg(ZMCTL, 1); break; // zoom out 1.5 (half reduction)
        case 3: w.Reg(ZMYIN0, 0), w.Reg(ZMYDN0, 0xA000); break;           // vertical zoom 0.625
        }
    }));
}

// =====================================================================================================================
// RBG0 and RBG1

TEST_CASE("Scene: RBG0 cells and bitmap without coefficients", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t variant = GENERATE(0u, 1u, 2u, 3u);
    CAPTURE(variant);
    RunScene(Make([variant](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        RBG r;
        r.cell2x2 = variant == 1;
        r.bitmap = variant == 2;
        r.format = variant == 3 ? 3 : 1;
        SetupRBG0(w, s, r, rng);
    }));
}

TEST_CASE("Scene: RBG0 coefficient modes per dot and per line from VRAM", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t mode = GENERATE(0u, 1u, 2u, 3u);
    const bool perDot = GENERATE(true, false);
    const bool oneWord = GENERATE(false, true);
    CAPTURE(mode, perDot, oneWord);
    RunScene(Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupRBG0(w, s, {.coeffTable = true, .coeffMode = mode, .coeffOneWord = oneWord, .perDot = perDot}, rng);
    }));
}

TEST_CASE("Scene: RBG0 coefficients from CRAM", "[gpu][vdp2][scene]") {
    const uint32_t mode = GENERATE(0u, 3u);
    const bool oneWord = GENERATE(false, true);
    CAPTURE(mode, oneWord);
    RunScene(Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, kDisp, 1);
        SetupRBG0(w, s, {.coeffTable = true, .coeffMode = mode, .coeffOneWord = oneWord, .coeffCRAM = true}, rng);
    }));
}

TEST_CASE("Scene: RBG0 screen-over modes", "[gpu][vdp2][scene]") {
    const uint32_t over = GENERATE(0u, 1u, 2u, 3u);
    CAPTURE(over);
    RunScene(Make([over](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        RBG r;
        r.screenOver = over;
        r.a = Rotated(0.2, 7.0);
        r.a.xst = -40;
        SetupRBG0(w, s, r, rng);
    }));
}

TEST_CASE("Scene: RBG0 rotation parameter modes and windows", "[gpu][vdp2][scene]") {
    const uint32_t rpmd = GENERATE(0u, 1u, 2u, 3u);
    CAPTURE(rpmd);
    RunScene(Make([rpmd](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupRBG0(w, s, {.coeffTable = rpmd == 2, .rpmd = rpmd}, rng);
        // Rotation parameter window: W0 (inverted) AND W1
        w.Reg(WPSX0, 80 << 1);
        w.Reg(WPEX0, 240 << 1);
        w.Reg(WPSY0, 40);
        w.Reg(WPEY0, 180);
        w.Reg(WPSX1, 20 << 1);
        w.Reg(WPEX1, 300 << 1);
        w.Reg(WPSY1, 10);
        w.Reg(WPEY1, 200);
        w.Reg(WCTLD, 0x008B);
    }));
}

TEST_CASE("Scene: RBG0 and RBG1 together", "[gpu][vdp2][scene]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        // RBG0: characters in A0, pattern names in A1. RBG1 (NBG0 registers, parameter B): characters in B0, pattern
        // names in B1.
        s.ramctl = static_cast<uint16_t>(0x0300 | 0x0003 | (2 << 2));
        s.chctlb = 0x1000; // RBG0 256 colours, 1x1
        s.chctla = 0x0010; // RBG1 256 colours, 1x1
        s.bgon = 0x0030;
        w.Reg(PRIR, 5);
        s.prina = 3;
        w.Reg(PNCR, 0);
        w.Reg(PNCN0, 0);
        w.Reg(PLSZ, 0);
        const uint32_t pnA = 0x20000, pnB = 0x60000;
        const uint32_t mapA = pnA >> 14, mapB = pnB >> 14;
        w.Reg(MPOFR, static_cast<uint16_t>((mapA >> 6) | ((mapB >> 6) << 4)));
        for (uint32_t reg = 0; reg < 8; ++reg) {
            w.Reg(MPABRA + reg * 2, static_cast<uint16_t>(((mapA & 0x3F) << 8) | (mapA & 0x3F)));
            w.Reg(MPABRB + reg * 2, static_cast<uint16_t>(((mapB & 0x3F) << 8) | (mapB & 0x3F)));
        }
        for (uint32_t i = 0; i < 64 * 64; ++i) {
            w.VRAM16(pnA + i * 4, static_cast<uint16_t>(Rand(rng, 128)));
            w.VRAM16(pnA + i * 4 + 2, static_cast<uint16_t>(Rand(rng, 32) * 2));
            w.VRAM16(pnB + i * 4, static_cast<uint16_t>(Rand(rng, 128)));
            w.VRAM16(pnB + i * 4 + 2, static_cast<uint16_t>(0x2000 + Rand(rng, 32) * 2)); // 0x40000
        }
        FillVRAM(w, 0x00000, 0x1000, rng);
        FillVRAM(w, 0x40000, 0x1000, rng);
        WriteRotTable(w, kRotTableAddress, Rotated(0.4, 1.1));
        WriteRotTable(w, kRotTableAddress + 0x80, Rotated(-0.2, 0.8));
        w.Reg(RPTAU, static_cast<uint16_t>(kRotTableAddress >> 17));
        w.Reg(RPTAL, static_cast<uint16_t>((kRotTableAddress >> 1) & 0xFFFE));
    }));
}

// =====================================================================================================================
// Windows

namespace {

void WindowRects(Vdp2SceneWriter &w, bool negative) {
    if (negative) {
        // Start coordinates beyond the end, and "negative" (bit 9 set) starts.
        w.Reg(WPSX0, 0x3F0);
        w.Reg(WPEX0, 200 << 1);
        w.Reg(WPSY0, 0x1F8);
        w.Reg(WPEY0, 120);
        w.Reg(WPSX1, 250 << 1);
        w.Reg(WPEX1, 100 << 1);
        w.Reg(WPSY1, 30);
        w.Reg(WPEY1, 0x3FF);
    } else {
        w.Reg(WPSX0, 40 << 1);
        w.Reg(WPEX0, 200 << 1);
        w.Reg(WPSY0, 20);
        w.Reg(WPEY0, 150);
        w.Reg(WPSX1, 120 << 1);
        w.Reg(WPEX1, 300 << 1);
        w.Reg(WPSY1, 90);
        w.Reg(WPEY1, 210);
    }
}

} // namespace

TEST_CASE("Scene: windows W0 W1 with AND OR and inversion", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t control = GENERATE(0x03u, 0x0Cu, 0x0Fu, 0x8Fu, 0x0Au, 0x8Bu, 0x8Eu);
    const bool negative = GENERATE(false, true);
    CAPTURE(control, negative);
    RunScene(Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        WindowRects(w, negative);
        // NBG0 windowed; NBG1 below it unwindowed, so fully covering windows still leave a non-trivial frame.
        w.Reg(WCTLA, static_cast<uint16_t>(control));
        w.Reg(WCTLC, static_cast<uint16_t>(control ^ 0x01)); // the sprite layer (empty) too
    }));
}

TEST_CASE("Scene: line windows", "[gpu][vdp2][scene]") {
    const bool both = GENERATE(false, true);
    CAPTURE(both);
    RunScene(Make([both](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        WindowRects(w, false);
        for (uint32_t y = 0; y < 256; ++y) {
            w.VRAM16(kLineWindowAddress + y * 4, static_cast<uint16_t>(Rand(rng, 0x280)));
            w.VRAM16(kLineWindowAddress + y * 4 + 2, static_cast<uint16_t>(Rand(rng, 0x280)));
            w.VRAM16(kLineWindowAddress + 0x800 + y * 4, static_cast<uint16_t>((y * 2) & 0x3FF));
            w.VRAM16(kLineWindowAddress + 0x800 + y * 4 + 2, static_cast<uint16_t>((y * 2 + 200) & 0x3FF));
        }
        w.Reg(LWTA0U, static_cast<uint16_t>(0x8000 | (kLineWindowAddress >> 17)));
        w.Reg(LWTA0L, static_cast<uint16_t>((kLineWindowAddress >> 1) & 0xFFFF));
        if (both) {
            w.Reg(LWTA1U, static_cast<uint16_t>(0x8000 | ((kLineWindowAddress + 0x800) >> 17)));
            w.Reg(LWTA1L, static_cast<uint16_t>(((kLineWindowAddress + 0x800) >> 1) & 0xFFFF));
        }
        w.Reg(WCTLA, both ? 0x0B8F : 0x0303);
    }));
}

TEST_CASE("Scene: sprite window", "[gpu][vdp2][scene]") {
    const bool inverted = GENERATE(false, true);
    const bool andLogic = GENERATE(false, true);
    CAPTURE(inverted, andLogic);
    RunScene(Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        WindowRects(w, false);
        SetupSprites(w, {.type = 3, .window = true, .transparentPercent = 50}, rng);
        const uint16_t c = static_cast<uint16_t>((andLogic ? 0x80 : 0) | 0x20 | (inverted ? 0x10 : 0) | 0x02);
        w.Reg(WCTLA, static_cast<uint16_t>(c | (c << 8)));
        w.Reg(WCTLC, static_cast<uint16_t>(0x0300)); // sprite layer: W0 inverted
    }));
}

// =====================================================================================================================
// Sprites

TEST_CASE("Scene: sprite types 0 to F", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t type = GENERATE(range(0u, 16u));
    CAPTURE(type);
    RunScene(Make([type](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesSingle(w, 0);
        SetupNBG(w, s, {.index = 0, .format = 1, .priority = 3}, rng);
        SetupSprites(w, {.type = type, .tvmr = static_cast<uint16_t>(type >= 8 ? 1 : 0)}, rng);
    }));
}

TEST_CASE("Scene: sprites with mixed format", "[gpu][vdp2][scene]") {
    const uint32_t type = GENERATE(0u, 3u, 7u);
    CAPTURE(type);
    RunScene(Make([type](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesSingle(w, 0);
        SetupNBG(w, s, {.index = 0, .format = 1, .priority = 3}, rng);
        SetupSprites(w, {.type = type, .mixed = true}, rng);
    }));
}

TEST_CASE("Scene: sprite MSB shadow and normal shadow on every layer", "[gpu][vdp2][scene][lavapipe]") {
    const bool msb = GENERATE(false, true);
    CAPTURE(msb);
    RunScene(Make([msb](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        SetupSprites(w, {.type = msb ? 2u : 0u, .shadowPercent = msb ? 0u : 30u}, rng);
    }));
}

// =====================================================================================================================
// Colour calculation, colour offset, line colour, back screen

TEST_CASE("Scene: colour calculation ratio additive and second-screen ratio", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t mode = GENERATE(0u, 1u, 2u);
    CAPTURE(mode);
    RunScene(Make([mode](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        SetupSprites(w, {.type = 1}, rng);
        const uint16_t ccmd = mode == 1 ? 0x0100 : mode == 2 ? 0x0200 : 0;
        w.Reg(CCCTL, static_cast<uint16_t>(ccmd | 0x0043)); // sprite, NBG0, NBG1
        w.Reg(CCRNA, 0x0A14);
    }));
}

TEST_CASE("Scene: extended colour calculation", "[gpu][vdp2][scene]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesAll(w);
        SetupNBG(w, s, {.index = 0, .format = 1, .priority = 6}, rng);
        SetupNBG(w, s, {.index = 1, .format = 0, .priority = 5}, rng);
        SetupNBG(w, s, {.index = 2, .format = 0, .priority = 4}, rng);
        w.Reg(CCCTL, 0x0407); // extended, NBG0-2
        w.Reg(CCRNA, 0x0810);
        w.Reg(CCRNB, 0x0004);
    }));
}

// Pixels where the gradation screen is one of the top two layers at x but not at x - 2 (software read the mask two
// pixels to the left until master 36ea9f9, stage 1B ruling 7).
TEST_CASE("Scene: colour gradation of partly covered screens", "[gpu][vdp2][scene]") {
    const uint32_t screen = GENERATE(0u, 2u, 4u);
    CAPTURE(screen);
    RunScene(Make([screen](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        SetupSprites(w, {.type = 1}, rng);
        w.Reg(CCCTL, static_cast<uint16_t>(0x8000 | (screen << 12) | 0x0043));
        w.Reg(CCRNA, 0x0A14);
    }));
}

TEST_CASE("Scene: colour gradation of an opaque top screen", "[gpu][vdp2][scene]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesAll(w);
        SetupNBG(w, s, {.index = 0, .format = 1, .transparency = false, .priority = 6}, rng);
        SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = true, .priority = 3}, rng);
        w.Reg(CCCTL, static_cast<uint16_t>(0x8000 | (2 << 12) | 0x0003));
        w.Reg(CCRNA, 0x0A14);
    }));
}

TEST_CASE("Scene: line colour screen", "[gpu][vdp2][scene]") {
    const bool perLine = GENERATE(false, true);
    const uint32_t cramMode = GENERATE(0u, 1u);
    CAPTURE(perLine, cramMode);
    RunScene(Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, kDisp, cramMode);
        TwoNBGs(w, s, rng);
        for (uint32_t y = 0; y < 512; ++y) {
            w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x800)));
        }
        w.Reg(LCTAU, static_cast<uint16_t>((perLine ? 0x8000 : 0) | (kLineColorAddress >> 17)));
        w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
        s.lnclen = 0x0003;
        w.Reg(CCCTL, 0x0023); // line colour, NBG0, NBG1 colour calculation
        w.Reg(CCRLB, 0x0010);
        w.Reg(CCRNA, 0x0A14);
    }));
}

TEST_CASE("Scene: colour offset A and B", "[gpu][vdp2][scene][lavapipe]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        SetupSprites(w, {.type = 1}, rng);
        w.Reg(CLOFEN, 0x0063); // sprite, back, NBG0, NBG1
        w.Reg(CLOFSL, 0x0042); // sprite and NBG1 use B
        w.Reg(COAR, 0x0040);
        w.Reg(COAG, 0x01C0); // negative
        w.Reg(COAB, 0x0010);
        w.Reg(COBR, 0x01F0);
        w.Reg(COBG, 0x0020);
        w.Reg(COBB, 0x0100);
    }));
}

TEST_CASE("Scene: LNCL and BACK per line", "[gpu][vdp2][scene][lavapipe]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, kDisp, 1);
        TwoNBGs(w, s, rng);
        BackPerLine(w, rng);
        for (uint32_t y = 0; y < 512; ++y) {
            w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x800)));
        }
        w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
        w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
        s.lnclen = 0x0001;
        w.Reg(CCCTL, 0x0021);
        w.Reg(CCRLB, 0x0C08);
    }));
}

TEST_CASE("Scene: RBG0 line colour screen from the line colour table and coefficients", "[gpu][vdp2][scene]") {
    const bool coeffLineColor = GENERATE(false, true);
    CAPTURE(coeffLineColor);
    RunScene(Make([coeffLineColor](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, kDisp, 1);
        SetupRBG0(w, s, {.coeffTable = coeffLineColor, .coeffLineColor = coeffLineColor}, rng);
        for (uint32_t y = 0; y < 512; ++y) {
            w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x800)));
        }
        w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
        w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
        s.lnclen = 0x0010; // RBG0
        w.Reg(CCCTL, 0x0030);
        w.Reg(CCRR, 0x0008);
    }));
}

TEST_CASE("Scene: rotated sprites", "[gpu][vdp2][scene]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupRBG0(w, s, {}, rng);
        SetupSprites(w, {.type = 0, .tvmr = 2}, rng);
    }));
}

TEST_CASE("Scene: NBG0 line scroll and vertical cell scroll in progressive modes", "[gpu][vdp2][scene]") {
    const uint32_t variant = GENERATE(0u, 1u);
    CAPTURE(variant);
    RunScene(Make([variant](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupNBG(w, s, {.index = 0, .format = 0}, rng);
        if (variant == 0) {
            CyclesSingle(w, 0);
            FillVRAM(w, kLineScrollAddress, 0x2000, rng);
            w.Reg(LSTA0U, static_cast<uint16_t>(kLineScrollAddress >> 17));
            w.Reg(LSTA0L, static_cast<uint16_t>((kLineScrollAddress >> 1) & 0xFFFF));
            w.Reg(SCRCTL, 0x0016); // every 2 lines, X and Y
        } else {
            w.Reg(CYCA0L, 0x4444);
            w.Reg(CYCA0U, 0x4444);
            w.Reg(CYCB0L, 0x0CFF);
            w.Reg(CYCB0U, 0xFFFF);
            constexpr uint32_t kVCellAddress = 0x7D000;
            FillVRAM(w, kVCellAddress, 0x400, rng);
            w.Reg(VCSTAU, static_cast<uint16_t>(kVCellAddress >> 17));
            w.Reg(VCSTAL, static_cast<uint16_t>((kVCellAddress >> 1) & 0xFFFE));
            w.Reg(SCRCTL, 0x0001);
        }
    }));
}

// =====================================================================================================================
// Resolutions: {320, 352, 640, 704} x {progressive, single-density interlace, double-density interlace} x
// deinterlacing on/off. NBG0 (16 colours, valid in hi-res) over random sprites.

namespace {

Vdp2Scene ResolutionScene(uint16_t tvmd, bool deinterlace) {
    return Make(
        [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | tvmd));
            CyclesSingle(w, 0);
            SetupNBG(w, s, {.index = 0, .format = 0, .priority = 3}, rng);
            SetupSprites(w, {.type = 0}, rng);
        },
        {.deinterlace = deinterlace}, 3);
}

} // namespace

// Single-density interlace without deinterlacing is the (t) scene, below.
TEST_CASE("Scene: resolutions and interlace modes", "[gpu][vdp2][scene]") {
    const uint16_t hreso = GENERATE(0, 1, 2, 3);
    const auto [lsmd, deinterlace] =
        GENERATE(std::pair{uint16_t{0}, false}, std::pair{uint16_t{0}, true}, std::pair{kLSMDSingle, true},
                 std::pair{kLSMDDouble, false}, std::pair{kLSMDDouble, true});
    CAPTURE(hreso, lsmd, deinterlace);
    RunScene(ResolutionScene(static_cast<uint16_t>(lsmd | hreso), deinterlace));
}

TEST_CASE("Scene: lavapipe subset of resolutions", "[gpu][vdp2][scene][lavapipe]") {
    const auto [tvmd, deinterlace] =
        GENERATE(std::pair{uint16_t{3}, false}, std::pair{uint16_t{1 | kLSMDSingle}, true},
                 std::pair{uint16_t{2 | kLSMDDouble}, false}, std::pair{uint16_t{3 | kLSMDDouble}, true});
    CAPTURE(tvmd, deinterlace);
    RunScene(ResolutionScene(tvmd, deinterlace));
}

// =====================================================================================================================
// Batching: changes between lines split the frame into batches that must render with the state of their lines.

namespace {

Vdp2Scene BatchScene(std::function<void(Vdp2SceneWriter &, uint32_t)> onLine, uint16_t tvmd = kDisp,
                     bool deinterlace = false) {
    Vdp2Scene scene = Make(
        [tvmd](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, tvmd);
            TwoNBGs(w, s, rng);
        },
        {.deinterlace = deinterlace}, 2);
    scene.onLine = std::move(onLine);
    return scene;
}

} // namespace

TEST_CASE("Scene: batching on a register change at line 100", "[gpu][vdp2][scene][lavapipe]") {
    RunScene(BatchScene([](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            w.Reg(SCXIN0, 0);
        } else if (y == 100) {
            w.Reg(SCXIN0, 37);
        }
    }));
}

TEST_CASE("Scene: batching on a VRAM change at line 100", "[gpu][vdp2][scene][lavapipe]") {
    RunScene(BatchScene([](Vdp2SceneWriter &w, uint32_t y) {
        // NBG0 pattern names (2 KiB) and the first characters change mid-frame, and change back at the top.
        if (y == 0 || y == 100) {
            std::mt19937 rng{y + 1};
            FillVRAM(w, kPageBase, 0x800, rng);
            FillVRAM(w, kCharBase, 0x400, rng);
        }
    }));
}

TEST_CASE("Scene: batching on a CRAM change at line 100", "[gpu][vdp2][scene][lavapipe]") {
    RunScene(BatchScene([](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0 || y == 100) {
            std::mt19937 rng{y + 3};
            for (uint32_t a = 0; a < 0x400; a += 2) {
                w.CRAM16(a, static_cast<uint16_t>(rng()));
            }
        }
    }));
}

TEST_CASE("Scene: batching on a BGON change at line 100", "[gpu][vdp2][scene]") {
    RunScene(BatchScene([](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            w.Reg(BGON, 0x0003);
        } else if (y == 100) {
            w.Reg(BGON, 0x0002);
        }
    }));
}

TEST_CASE("Scene: batching with a per-line colour offset fade", "[gpu][vdp2][scene][lavapipe]") {
    const uint16_t tvmd = GENERATE(kDisp, static_cast<uint16_t>(kDisp | kLSMDDouble));
    const bool deinterlace = GENERATE(false, true);
    CAPTURE(tvmd, deinterlace);
    Vdp2Scene scene = BatchScene(
        [](Vdp2SceneWriter &w, uint32_t y) {
            w.Reg(COAR, static_cast<uint16_t>(y & 0xFF));
            w.Reg(COAG, static_cast<uint16_t>((0x200 - y) & 0x1FF));
            w.Reg(COBB, static_cast<uint16_t>((y * 3) & 0x1FF));
        },
        tvmd, deinterlace);
    auto setup = scene.setup;
    scene.setup = [setup](Vdp2SceneWriter &w) {
        setup(w);
        w.Reg(CLOFEN, 0x0023);
        w.Reg(CLOFSL, 0x0002);
    };
    RunScene(scene);
}

TEST_CASE("Scene: batching with compose-only and layer-only changes on alternating lines", "[gpu][vdp2][scene]") {
    RunScene(BatchScene([](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            w.Reg(CCCTL, 0x0001);
        }
        if (y % 3 == 0) {
            w.Reg(CCRNA, static_cast<uint16_t>(y & 31)); // compose only
        }
        if (y % 5 == 0) {
            w.Reg(SCXIN1, static_cast<uint16_t>(y / 5)); // layers only
        }
    }));
}

// =====================================================================================================================
// Robustness

TEST_CASE("Scene: upload overflow with 64 KiB upload blocks", "[gpu][vdp2][scene][lavapipe]") {
    auto device = OpenDevice();
    uint32_t blocksAfterFirstFrame = 0;
    RenderBothOptions options;
    options.uploadBlockSize = 64 * 1024;
    options.beforeFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (frame == 1) {
            blocksAfterFirstFrame = vulkan.Backend()->UploadBlocksForTest(0);
        }
    };
    // Everything is uploaded on the first frame (VRAM, CRAM, sprite slices), and the CRAM changes on every line.
    Vdp2Scene scene = Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        SetupSprites(w, {.type = 1}, rng);
    });
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) { w.CRAM16((y * 2) & 0x7FE, static_cast<uint16_t>(y * 77)); };
    Check(*device, scene, options);
    CHECK(blocksAfterFirstFrame > 1);
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: detach and attach between frames", "[gpu][vdp2][scene][lavapipe]") {
    auto device = OpenDevice();
    RenderBothOptions options;
    options.beforeFrame = [](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &dev) {
        if (frame == 1) {
            vulkan.DetachDevice(); // frame 1 runs without a device
        } else if (frame == 2) {
            vulkan.AttachDevice(dev, 2);
        }
    };
    Vdp2Scene scene = Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        SetupSprites(w, {.type = 1}, rng);
    });
    scene.frames = 3;
    // Something changes on every frame, so frame 2 needs what frame 1 wrote while detached.
    scene.onLine = [counter = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            const uint32_t n = ++*counter;
            w.Reg(SCXIN0, static_cast<uint16_t>(n * 7));
            w.CRAM16(0x10, static_cast<uint16_t>(n * 0x1234));
            w.VRAM16(kPageBase + 0x40, static_cast<uint16_t>(n * 0x55));
        }
    };
    Check(*device, scene, options);
    CloseAndCheckValidation(device);
}

TEST_CASE("PresentDevice of the composite equals Present of its readback", "[gpu][vdp2][scene][lavapipe]") {
    namespace vdp = ymir::vdp;
    auto device = OpenDevice();
    {
        auto state = std::make_unique<vdp::VDPState>();
        vdp::config::VDP2DebugRender debug{};
        vdp::config::VDP2AccessPatternsConfig access{};
        auto vulkan = std::make_unique<gpu::VulkanVDPRenderer>(*state, debug, access);
        vulkan->AttachDevice(*device, 1);
        REQUIRE(vulkan->Attached());
        uint32_t width = 0, height = 0;
        vulkan->FrameReady = [&](uint32_t w, uint32_t h) {
            width = w;
            height = h;
        };
        Vdp2SceneWriter writer;
        writer.Attach(*state, *vulkan);
        Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | 1));
            TwoNBGs(w, s, rng);
        }).setup(writer);
        writer.RunFrame();
        REQUIRE(vulkan->HasPendingFrame());
        REQUIRE(gpu::vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                    REQUIRE(vulkan->RecordPendingFrame(0, cmd));
                }) == VK_SUCCESS);
        REQUIRE(width == 352);
        REQUIRE(height == 224);
        const std::vector<uint32_t> composite = ReadbackBuffer(*device, vulkan->CompositeBuffer(), width * height);
        REQUIRE(composite.size() == width * height);

        gpu::FramePresenter presenter(*device, 2);
        REQUIRE(presenter.Ok());
        using gpu::OutputSettings;
        const OutputSettings settings =
            GENERATE(OutputSettings{}, OutputSettings{.cropH = 16, .cropV = 8}, OutputSettings{.rotation = 90},
                     OutputSettings{.cropH = 8, .cropV = 16, .rotation = 270});
        CAPTURE(settings.cropH, settings.cropV, settings.rotation);
        REQUIRE(gpu::vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                    REQUIRE(presenter.Present(0, composite.data(), width, height, settings, cmd));
                    REQUIRE(presenter.PresentDevice(1, vulkan->CompositeBuffer(), width, height, settings, cmd));
                }) == VK_SUCCESS);
        const gpu::OutputSize size = gpu::WrittenSize(gpu::ComputeOutputSize(width, height, settings));
        const std::vector<uint32_t> uploaded = presenter.ReadbackXRGB(0, size);
        const std::vector<uint32_t> direct = presenter.ReadbackXRGB(1, size);
        REQUIRE(uploaded.size() == size.width * size.height);
        CHECK(uploaded == direct);
        // Not vacuous: the frame has content.
        CHECK(std::count(uploaded.begin(), uploaded.end(), uploaded[0]) < static_cast<long>(uploaded.size()));
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: a recorded frame that is never submitted is redone", "[gpu][vdp2][scene][lavapipe]") {
    // Frame 0 carries the resolution clear and every upload; its command buffer is dropped. The clear must be pending
    // again for frame 1, and frame 2 must still match software.
    auto device = OpenDevice();
    bool clearPendingAfterDrop = false;
    RenderBothOptions options;
    options.dropFrame = 0;
    options.beforeFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (frame == 1) {
            clearPendingAfterDrop = vulkan.Backend()->CompositeClearPendingForTest();
        }
    };
    Check(*device, Make(
                       [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                           Base(w, s, rng);
                           TwoNBGs(w, s, rng);
                           SetupSprites(w, {.type = 1}, rng);
                       },
                       {}, 3),
          options);
    CHECK(clearPendingAfterDrop);
    CloseAndCheckValidation(device);
}

TEST_CASE("PresentDevice across detach and attach", "[gpu][vdp2][scene][lavapipe]") {
    // A live presenter keeps presenting the composite of a back half that is destroyed and recreated every frame (its
    // buffer handle may be reused); the descriptor must follow, under validation.
    namespace vdp = ymir::vdp;
    auto device = OpenDevice();
    {
        auto state = std::make_unique<vdp::VDPState>();
        vdp::config::VDP2DebugRender debug{};
        vdp::config::VDP2AccessPatternsConfig access{};
        auto vulkan = std::make_unique<gpu::VulkanVDPRenderer>(*state, debug, access);
        gpu::FramePresenter presenter(*device, 1);
        REQUIRE(presenter.Ok());
        Vdp2SceneWriter writer;
        writer.Attach(*state, *vulkan);
        Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
        }).setup(writer);
        for (uint32_t i = 0; i < 4; ++i) {
            CAPTURE(i);
            vulkan->DetachDevice(); // the previous frame finished (SubmitOneShot waits)
            vulkan->AttachDevice(*device, 1);
            REQUIRE(vulkan->Attached());
            writer.Reg(SCXIN0, static_cast<uint16_t>(i * 11));
            writer.RunFrame();
            REQUIRE(vulkan->HasPendingFrame());
            REQUIRE(gpu::vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                        REQUIRE(vulkan->RecordPendingFrame(0, cmd));
                        REQUIRE(presenter.PresentDevice(0, vulkan->CompositeBuffer(), 320, 224, {}, cmd));
                    }) == VK_SUCCESS);
            const std::vector<uint32_t> composite = ReadbackBuffer(*device, vulkan->CompositeBuffer(), 320 * 224);
            const std::vector<uint32_t> shown = presenter.ReadbackXRGB(0, {320, 224});
            REQUIRE(composite.size() == shown.size());
            uint32_t mismatches = 0;
            for (size_t p = 0; p < shown.size(); ++p) {
                const uint32_t c = composite[p];
                const uint32_t xrgb = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
                mismatches += xrgb != shown[p];
            }
            CHECK(mismatches == 0);
        }
    }
    CloseAndCheckValidation(device);
}

// =====================================================================================================================
// Known divergences (stage1b-port-inventory.md section 4.1 letters). Each fails today; the task that fixes it removes
// [!shouldfail].

TEST_CASE("Scene: (b) NBG mosaic", "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        w.Reg(MZCTL, 0x3203); // NBG0 and NBG1, 3 pixels wide, 4 lines high
        // Scrolled, so the GPU's scroll-space mosaic differs from software's screen-space mosaic.
        w.Reg(SCXIN0, 5);
        w.Reg(SCYIN0, 2);
        w.Reg(SCXIN1, 7);
    }));
}

TEST_CASE("Scene: (c) RBG mosaic", "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupRBG0(w, s, {}, rng);
        w.Reg(MZCTL, 0x0310); // RBG0, 4 pixels wide
    }));
}

TEST_CASE("Scene: (d) line scroll in double density", "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | kLSMDDouble));
            CyclesSingle(w, 0);
            SetupNBG(w, s, {.index = 0, .format = 1}, rng);
            // Line scroll X and Y every 2 lines from a random table
            FillVRAM(w, kLineScrollAddress, 0x2000, rng);
            w.Reg(LSTA0U, static_cast<uint16_t>(kLineScrollAddress >> 17));
            w.Reg(LSTA0L, static_cast<uint16_t>((kLineScrollAddress >> 1) & 0xFFFF));
            w.Reg(SCRCTL, 0x0016); // N0LSS = 1, N0LSCY, N0LSCX
        },
        {.deinterlace = true}, 2));
}

TEST_CASE("Scene: (e) vertical scroll accumulation across a mid-frame zoom change",
          "[gpu][vdp2][scene][!shouldfail]") {
    Vdp2Scene scene = Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesSingle(w, 0);
        SetupNBG(w, s, {.index = 0, .format = 1}, rng);
    });
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            w.Reg(ZMYIN0, 1);
            w.Reg(ZMYDN0, 0);
        } else if (y == 100) {
            w.Reg(ZMYIN0, 0);
            w.Reg(ZMYDN0, 0x6000);
        }
    };
    RunScene(scene);
}

TEST_CASE("Scene: (f) vertical cell scroll with horizontal zoom", "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupNBG(w, s, {.index = 0, .format = 0}, rng);
        // PN0 at T0 and vertical cell scroll at T1 (bank B), characters everywhere in bank A
        w.Reg(CYCA0L, 0x4444);
        w.Reg(CYCA0U, 0x4444);
        w.Reg(CYCB0L, 0x0CFF);
        w.Reg(CYCB0U, 0xFFFF);
        constexpr uint32_t kVCellAddress = 0x7D000;
        FillVRAM(w, kVCellAddress, 0x400, rng);
        w.Reg(VCSTAU, static_cast<uint16_t>(kVCellAddress >> 17));
        w.Reg(VCSTAL, static_cast<uint16_t>((kVCellAddress >> 1) & 0xFFFE));
        w.Reg(SCRCTL, 0x0001);
        w.Reg(ZMXIN0, 0);
        w.Reg(ZMXDN0, 0xC000);
    }));
}

TEST_CASE("Scene: (g) character pattern delay", "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupNBG(w, s, {.index = 1, .format = 0}, rng);
        // PN1 at T3 and CP1 at T4-T5 in every bank: illegal T4-T7 reads with 1x1 cells delay the characters.
        for (uint32_t reg : {CYCA0L, CYCA1L, CYCB0L, CYCB1L}) {
            w.Reg(reg, 0xFFF1);
            w.Reg(reg + 2, 0x55FF);
        }
        w.Reg(SCXIN1, 3);
    }));
}

namespace {

// VDP1 command table: system clipping, local coordinates, `count` random polygons (every other one a mesh when
// `meshToo`; every fourth one coloured `meshColor` when it is not 0), end.
void DrawPolygons(Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t count, bool meshToo, uint16_t meshColor = 0) {
    uint32_t a = 0;
    const auto cmd = [&](std::array<uint16_t, 16> words) {
        for (uint32_t i = 0; i < 16; ++i) {
            w.VDP1VRAM16(a + i * 2, words[i]);
        }
        a += 0x20;
    };
    cmd({0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 511, 255, 0, 0, 0, 0}); // system clipping
    cmd({0x000A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});     // local coordinates 0,0
    for (uint32_t i = 0; i < count; ++i) {
        const bool mesh = meshToo && (i & 1);
        const int x = static_cast<int>(Rand(rng, 280)), y = static_cast<int>(Rand(rng, 190));
        const int wd = 20 + static_cast<int>(Rand(rng, 80)), ht = 20 + static_cast<int>(Rand(rng, 60));
        // Type 0 palette data: priority, ratio, colour
        const uint32_t pri = Rand(rng, 4), ratio = Rand(rng, 8), dot = 1 + Rand(rng, 0x7FF);
        const uint16_t color = meshColor != 0 && (i & 3) == 3 ? meshColor
                                                               : static_cast<uint16_t>((pri << 14) | (ratio << 11) | dot);
        const uint16_t pmod = static_cast<uint16_t>(mesh ? 0x0100 : 0);
        cmd({0x0004, 0, pmod, color, 0, 0, static_cast<uint16_t>(x), static_cast<uint16_t>(y),
             static_cast<uint16_t>(x + wd), static_cast<uint16_t>(y), static_cast<uint16_t>(x + wd),
             static_cast<uint16_t>(y + ht), static_cast<uint16_t>(x), static_cast<uint16_t>(y + ht), 0, 0});
    }
    cmd({0x8000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    w.VDP1Reg(TVMR, 0);
    w.VDP1Reg(FBCR, 0);
    w.DrawVDP1();
    w.SwapFramebuffers();
}

} // namespace

TEST_CASE("Scene: sprites drawn by VDP1 with transparent meshes off", "[gpu][vdp2][scene]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        w.Reg(SPCTL, 0x0000);
        w.Reg(PRISA, 0x0706);
        w.Reg(PRISB, 0x0402);
        DrawPolygons(w, rng, 16, true);
    }));
}

TEST_CASE("Scene: threaded VDP1 sprites and an erase without a swap", "[gpu][vdp2][scene][lavapipe]") {
    // The Vulkan renderer's VDP1 runs threaded. Frame 1 erases the display framebuffer (part of it) without a swap
    // first: the sprite capture at line 0 must wait for the queued erase (hybrid section 1.5).
    constexpr uint32_t EWDR = 0x06, EWLR = 0x08, EWRR = 0x0A;
    Vdp2Scene scene = Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        w.Reg(SPCTL, 0x0000);
        w.Reg(PRISA, 0x0706);
        w.Reg(PRISB, 0x0402);
        w.VDP1Reg(EWDR, 0x4321);
        w.VDP1Reg(EWLR, (8 << 9) | 40);   // from (64, 40)
        w.VDP1Reg(EWRR, (30 << 9) | 160); // to (240, 160)
        DrawPolygons(w, rng, 16, false);  // the swap latches the erase parameters
    });
    scene.onLine = [frame = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0 && ++*frame == 2) {
            w.EraseFramebuffer();
        }
    };
    RenderBothOptions options;
    options.threadedVDP1 = true;
    RunScene(scene, options);
}

namespace {

// NBG0/NBG1 under VDP1 polygons, half of them meshes, transparent meshes on; no colour calculation or colour offset
// (MeshColorScene, (l), has those).
Vdp2Scene MeshScene(uint16_t tvmd, bool deinterlace) {
    return Make(
        [tvmd](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | tvmd));
            TwoNBGs(w, s, rng);
            w.Reg(SPCTL, 0x0000);
            w.Reg(PRISA, 0x0706);
            w.Reg(PRISB, 0x0402);
            DrawPolygons(w, rng, 16, true);
        },
        {.deinterlace = deinterlace, .transparentMeshes = true}, 3);
}

} // namespace

TEST_CASE("Scene: transparent meshes in progressive and double-density deinterlaced modes",
          "[gpu][vdp2][scene][lavapipe]") {
    const uint16_t tvmd = GENERATE(uint16_t{0}, kLSMDDouble);
    CAPTURE(tvmd);
    RunScene(MeshScene(tvmd, tvmd != 0)); // double density deinterlaced uploads the mesh1 (alt field) slice
}

// Documented exception U2 (stage 1B ruling 8): in exclusive monitor modes software reads framebuffer pixel x / 2 and
// draws it at even output x only (xOutputShift = 1 without the doubleResH copy to x + 1, sw:2672-2727), leaving odd x
// transparent; the sprite shader draws every output x from framebuffer x. No known game uses exclusive monitor modes.
TEST_CASE("Scene: exception U2: sprites in exclusive monitor mode", "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, static_cast<uint16_t>(kDisp | 4));
        CyclesSingle(w, 0);
        SetupNBG(w, s, {.index = 0, .format = 0, .priority = 3}, rng);
        SetupSprites(w, {.type = 0}, rng);
    }));
}

// =====================================================================================================================
// Compose, sprite and line colour parity with the software renderer (stage 1B Task 5). Each scene was a divergence of
// the ported Ymir shaders, by its letter from stage1b-port-inventory.md section 4.1 ((a)-(o)) or the stage 1B Task 3
// and 4 reviews ((q)-(u)); (j) and (m)-(o) never diverged and stay as guards.

TEST_CASE("Scene: (a) per-layer shadow enable over NBGs and the back screen", "[gpu][vdp2][scene][lavapipe]") {
    // SDCTL: bit 0 NBG0, bit 1 NBG1, bit 5 back screen. Normal shadows (type 0) and MSB shadows (type 2).
    const uint16_t sdctl = GENERATE(uint16_t{0x00}, uint16_t{0x01}, uint16_t{0x02}, uint16_t{0x20}, uint16_t{0x23});
    const bool msb = GENERATE(false, true);
    CAPTURE(sdctl, msb);
    RunScene(Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        s.sdctl = sdctl;
        SetupSprites(w, {.type = msb ? 2u : 0u, .shadowPercent = msb ? 0u : 40u}, rng);
    }));
}

TEST_CASE("Scene: (a) per-layer shadow enable over RBG0", "[gpu][vdp2][scene]") {
    // SDCTL: bit 4 RBG0, bit 5 back screen. RBG0 at priority 1, below every sprite.
    const uint16_t sdctl = GENERATE(uint16_t{0x00}, uint16_t{0x10}, uint16_t{0x20});
    CAPTURE(sdctl);
    RunScene(Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupRBG0(w, s, {.priority = 1}, rng);
        s.sdctl = sdctl;
        SetupSprites(w, {.type = 0, .shadowPercent = 40}, rng);
    }));
}

TEST_CASE("Scene: (i) extended colour calculation with layer 1 special colour calculation", "[gpu][vdp2][scene]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesAll(w);
        SetupNBG(w, s, {.index = 0, .format = 1, .priority = 6}, rng);
        SetupNBG(w, s, {.index = 1, .format = 0, .priority = 5, .specialBits = true}, rng);
        SetupNBG(w, s, {.index = 2, .format = 0, .priority = 4}, rng);
        w.Reg(CCCTL, 0x0407);
        w.Reg(SFCCMD, 0x0004); // NBG1 per character
        w.Reg(CCRNA, 0x0810);
    }));
}

TEST_CASE("Scene: (i) extended colour calculation with the colour calculation window", "[gpu][vdp2][scene]") {
    // Layer 0 and layer 1 share the window result of their pixel, so only the special colour calculation bit (above)
    // can tell the layer-1 enables apart; this one guards the window path with sprites on layer 1.
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesAll(w);
        SetupNBG(w, s, {.index = 0, .format = 1, .priority = 7}, rng);
        SetupNBG(w, s, {.index = 1, .format = 0, .priority = 2}, rng);
        SetupSprites(w, {.type = 1}, rng);
        WindowRects(w, false);
        w.Reg(WCTLD, 0x0200); // colour calculation window: W0
        w.Reg(CCCTL, 0x0443); // extended; sprite, NBG0, NBG1
        w.Reg(CCRNA, 0x0810);
    }));
}

// (j) never diverged: the GPU's line-ratio branch for the back screen needs the back screen's colour calculation enable,
// which no register sets (VDP2Regs::backScreenParams.colorCalcEnable stays false). The shader now returns the back
// screen ratio directly, as software does.
TEST_CASE("Scene: (j) back screen ratio in second-screen ratio mode", "[gpu][vdp2][scene]") {
    const bool sprites = GENERATE(false, true);
    CAPTURE(sprites);
    RunScene(Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesSingle(w, 0);
        SetupNBG(w, s, {.index = 0, .format = 1}, rng);
        if (sprites) {
            SetupSprites(w, {.type = 1, .transparentPercent = 60}, rng);
        }
        w.Reg(CCCTL, 0x0261); // second-screen ratio, line colour, sprite and NBG0 colour calculation
        w.Reg(CCRLB, 0x0410); // back ratio 4, line ratio 16
        w.Reg(CCRNA, 0x0008);
    }));
}

TEST_CASE("Scene: (k) LNCL colours in CRAM mode 2", "[gpu][vdp2][scene][lavapipe]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, kDisp, 2);
        TwoNBGs(w, s, rng);
        for (uint32_t y = 0; y < 512; ++y) {
            w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x10000))); // any word: & 0x7FF
        }
        w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
        w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
        s.lnclen = 0x0003;
        w.Reg(CCCTL, 0x0023);
        w.Reg(CCRLB, 0x0010);
    }));
}

TEST_CASE("Scene: (k) display off after a frame of per-line back colours", "[gpu][vdp2][scene][lavapipe]") {
    // Frame 0 displays per-line back colours and turns the display off (border colour mode on) at its last line;
    // frame 1 has the display off. Software shows the last computed back colour on every line (the display latch is
    // off, so no line recomputes it).
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            BackPerLine(w, rng);
        },
        {}, 2);
    scene.onLine = [frame = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            ++*frame;
        }
        if (*frame == 1 && y == 223) {
            w.Reg(TVMD, kBDCLMD);
        }
    };
    RunScene(scene, {}, true);
}

TEST_CASE("Scene: (k) display switched off and on mid-frame", "[gpu][vdp2][scene]") {
    // The display goes off at line 60 and on again at line 120 (border colour mode on from then): the display latch
    // stays off to the end of the frame, so lines 120 and below are drawn with the line colour and back colour of line
    // 59 (software skips the per-line updates), and lines 60-119 show the border colour mode latched at the frame start
    // (on from the second frame: line 59's back colour).
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, kDisp, 1);
            TwoNBGs(w, s, rng);
            BackPerLine(w, rng);
            for (uint32_t y = 0; y < 512; ++y) {
                w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x800)));
            }
            w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
            w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
            s.lnclen = 0x0001;
            w.Reg(CCCTL, 0x0021);
            w.Reg(CCRLB, 0x0C08);
        },
        {}, 2);
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 60) {
            w.Reg(TVMD, kBDCLMD);
        } else if (y == 120) {
            w.Reg(TVMD, static_cast<uint16_t>(kDisp | kBDCLMD));
        } else if (y == 0) {
            w.Reg(TVMD, kDisp);
        }
    };
    RunScene(scene);
}

namespace {

// Transparent meshes over NBG0/NBG1 with sprite colour calculation and colour offsets. variant:
// 0 condition priority <= 7 (all), ratio blend; 1 priority == 4, additive; 2 priority >= 5, second-screen ratio;
// 3 colour MSB; 4 colour offset on NBG0/NBG1 only (not the sprite), sprite colour calculation off; 5 sprite colour
// offset B; 6 sprite type 2 with mesh pixels 0x8000 (transparent with the shadow bit: priority set, not normal).
Vdp2Scene MeshColorScene(uint32_t variant) {
    return Make(
        [variant](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            static constexpr uint16_t kSPCTL[] = {0x0700, 0x1400, 0x2500, 0x3000, 0x0700, 0x0700, 0x0702};
            w.Reg(SPCTL, kSPCTL[variant]);
            w.Reg(PRISA, 0x0706);
            w.Reg(PRISB, 0x0402);
            w.Reg(CCRSA, 0x0810);
            w.Reg(CCRSB, 0x1804);
            w.Reg(CCRSC, 0x1F00);
            w.Reg(CCRSD, 0x0C14);
            w.Reg(CCRNA, 0x0A14);
            const uint16_t ccmd = variant == 1 ? 0x0100 : variant == 2 ? 0x0200 : 0;
            w.Reg(CCCTL, static_cast<uint16_t>(ccmd | (variant == 4 ? 0x0003 : 0x0043)));
            w.Reg(CLOFEN, variant == 4 ? 0x0003 : 0x0041);
            w.Reg(CLOFSL, variant == 5 ? 0x0040 : 0x0002);
            w.Reg(COAR, 0x0080);
            w.Reg(COAG, 0x01E0);
            w.Reg(COBR, 0x01C0);
            w.Reg(COBB, 0x0050);
            DrawPolygons(w, rng, 24, true, variant == 6 ? uint16_t{0x8000} : uint16_t{0});
        },
        {.transparentMeshes = true});
}

} // namespace

TEST_CASE("Scene: (l) transparent mesh colour calculation and colour offset", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t variant = GENERATE(range(0u, 7u));
    CAPTURE(variant);
    RunScene(MeshColorScene(variant));
}

TEST_CASE("Scene: (m) colour offsets of zero", "[gpu][vdp2][scene]") {
    // Software applies an enabled offset only when it is non-zero; the shader adds it anyway (adding zero).
    RunScene(Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            w.Reg(SPCTL, 0x0700);
            w.Reg(PRISA, 0x0706);
            w.Reg(PRISB, 0x0402);
            w.Reg(CCCTL, 0x0043);
            w.Reg(CCRNA, 0x0A14);
            w.Reg(CLOFEN, 0x007F); // every layer
            w.Reg(CLOFSL, 0x0042); // sprite and NBG1 use B (zero), the others A
            w.Reg(COAR, 0x0030);
            w.Reg(COAB, 0x01F0);
            DrawPolygons(w, rng, 16, true);
        },
        {.transparentMeshes = true}));
}

TEST_CASE("Scene: (n) ratio blends at every ratio", "[gpu][vdp2][scene]") {
    auto device = OpenDevice();
    for (uint32_t ratio = 0; ratio < 32; ratio += 3) {
        CAPTURE(ratio);
        Check(*device, Make([ratio](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            SetupSprites(w, {.type = 1}, rng); // random sprite ratios
            w.Reg(CCCTL, 0x0043);
            w.Reg(CCRNA, static_cast<uint16_t>(ratio | ((31 - ratio) << 8)));
        }));
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: (o) colour gradation at the left edge", "[gpu][vdp2][scene]") {
    // Pixels 0 and 1 have no left neighbours: software uses input[0] and the average of input[0] and input[1], the
    // shader clamps its taps at x = 0 (the same values).
    const uint32_t screen = GENERATE(2u, 4u); // NBG0, NBG1
    CAPTURE(screen);
    RunScene(Make([screen](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        CyclesAll(w);
        SetupNBG(w, s, {.index = 0, .format = 1, .transparency = false, .priority = 6}, rng);
        SetupNBG(w, s, {.index = 1, .format = 0, .transparency = false, .priority = 3}, rng);
        w.Reg(SCXIN0, 3);
        w.Reg(SCXIN1, 5);
        w.Reg(CCCTL, static_cast<uint16_t>(0x8000 | (screen << 12) | 0x0003));
        w.Reg(CCRNA, 0x0A14);
    }));
}

TEST_CASE("Scene: (q) sprite reads past the 256 KiB framebuffer in exclusive monitor mode", "[gpu][vdp2][scene]") {
    // 480 lines read framebuffer rows 256-479, past the 256 KiB framebuffer: software wraps to rows 0-223 of the
    // display framebuffer (empty here); the shader read the next slice, framebuffer 1, which frame 0 displayed
    // (random). Rows 0-255 are empty on both, so only the wrap differs.
    Vdp2Scene scene = Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, static_cast<uint16_t>(kDisp | 4));
        CyclesSingle(w, 0);
        SetupNBG(w, s, {.index = 0, .format = 0, .priority = 3}, rng);
        if (w.DrawFramebuffer() != 1) {
            w.SwapFramebuffers();
        }
        SetupSprites(w, {.type = 0, .transparentPercent = 20}, rng); // framebuffer 1, displayed in frame 0
    });
    scene.onLine = [frame = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0 && ++*frame == 2) {
            w.SwapFramebuffers(); // frame 1 displays framebuffer 0, which is empty
        }
    };
    RunScene(scene);
}

TEST_CASE("Scene: (r) RBG line colour from CRAM indices of 2048 and above", "[gpu][vdp2][scene]") {
    const uint32_t cramMode = GENERATE(1u, 2u);
    const bool coeffLineColor = GENERATE(false, true);
    CAPTURE(cramMode, coeffLineColor);
    RunScene(Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, kDisp, cramMode);
        SetupRBG0(w, s, {.coeffTable = coeffLineColor, .coeffLineColor = coeffLineColor}, rng);
        for (uint32_t y = 0; y < 512; ++y) {
            w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(0x800 + Rand(rng, 0xF000)));
        }
        w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
        w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
        s.lnclen = 0x0010; // RBG0
        w.Reg(CCCTL, 0x0030);
        w.Reg(CCRR, 0x0008);
    }));
}

TEST_CASE("Scene: (s) rotated sprites under deinterlace", "[gpu][vdp2][scene][lavapipe]") {
    const uint16_t lsmd = GENERATE(kLSMDSingle, kLSMDDouble);
    CAPTURE(lsmd);
    RunScene(Make(
        [lsmd](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | lsmd));
            SetupRBG0(w, s, {}, rng);
            SetupSprites(w, {.type = 0, .tvmr = 2}, rng);
        },
        {.deinterlace = true}, 2));
}

TEST_CASE("Scene: (t) resolutions in single-density interlace without deinterlacing", "[gpu][vdp2][scene]") {
    // The back screen of field lines 112 and below landed on the wrong rows (GPU: black from row 224).
    auto device = OpenDevice();
    for (uint16_t hreso = 0; hreso < 4; ++hreso) {
        CAPTURE(hreso);
        Check(*device, ResolutionScene(static_cast<uint16_t>(kLSMDSingle | hreso), false));
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: (t) LNCL and BACK per line in interlaced modes", "[gpu][vdp2][scene][lavapipe]") {
    const auto [lsmd, deinterlace] =
        GENERATE(std::pair{kLSMDSingle, false}, std::pair{kLSMDSingle, true}, std::pair{kLSMDDouble, false},
                 std::pair{kLSMDDouble, true});
    CAPTURE(lsmd, deinterlace);
    const uint16_t tvmd = static_cast<uint16_t>(kDisp | lsmd);
    RunScene(Make(
        [tvmd](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, tvmd, 1);
            TwoNBGs(w, s, rng);
            BackPerLine(w, rng);
            for (uint32_t y = 0; y < 512; ++y) {
                w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x800)));
            }
            w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
            w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
            s.lnclen = 0x0001;
            w.Reg(CCCTL, 0x0021);
            w.Reg(CCRLB, 0x0C08);
        },
        {.deinterlace = deinterlace}, 3));
}

TEST_CASE("Scene: (u) mesh alt field in single-density interlace", "[gpu][vdp2][scene][lavapipe]") {
    // Software reads the alt mesh framebuffer (m_meshFBRAM[1]) for the alt-field rows whenever it deinterlaces
    // (sw:2690), in single density too, where VDP1 never draws into it (sw:1119): meshes show on the main field rows
    // only. The renderer captures that slice and the sprite shader reads it.
    RunScene(MeshScene(kLSMDSingle, true));
}

// =====================================================================================================================
// VulkanVDPRenderer without a device: forwarding, callbacks, save-state delegation ([vdp2][renderer], no GPU needed)

TEST_CASE("VulkanVDPRenderer without a device runs the front half and drops its frames", "[vdp2][renderer]") {
    namespace vdp = ymir::vdp;
    auto state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto vulkan = std::make_unique<gpu::VulkanVDPRenderer>(*state, debug, access);
    REQUIRE(vulkan->IsValid());
    CHECK(vulkan->GetType() == vdp::VDPRendererType::External);
    CHECK_FALSE(vulkan->Attached());

    struct Counts {
        uint32_t drawFinished = 0, resolutionChanged = 0, swaps = 0, vdp1Finished = 0, frameReady = 0;
        uint32_t w = 0, h = 0;
    } counts;
    auto &cb = vulkan->Callbacks;
    cb.VDP2DrawFinished.Rebind(&counts, [](void *c) { ++static_cast<Counts *>(c)->drawFinished; });
    cb.VDP2ResolutionChanged.Rebind(&counts, [](uint32_t w, uint32_t h, void *c) {
        auto &out = *static_cast<Counts *>(c);
        ++out.resolutionChanged;
        out.w = w;
        out.h = h;
    });
    cb.VDP1FramebufferSwap.Rebind(&counts, [](void *c) { ++static_cast<Counts *>(c)->swaps; });
    cb.VDP1DrawFinished.Rebind(&counts, [](void *c) { ++static_cast<Counts *>(c)->vdp1Finished; });
    vulkan->FrameReady = [&](uint32_t, uint32_t) { ++counts.frameReady; };

    Vdp2SceneWriter writer;
    writer.Attach(*state, *vulkan); // reports 320x224
    std::mt19937 rng{3};
    Shadow s;
    Base(writer, s, rng, static_cast<uint16_t>(kDisp | 1));
    TwoNBGs(writer, s, rng);
    Flush(writer, s);
    DrawPolygons(writer, rng, 4, false);
    writer.RunFrame(); // 352 wide from here
    writer.RunFrame();

    CHECK(counts.drawFinished == 2);
    CHECK(counts.frameReady == 2);
    CHECK(counts.resolutionChanged == 1);
    CHECK(counts.w == 352);
    CHECK(counts.h == 224);
    CHECK(counts.swaps == 1);
    CHECK(counts.vdp1Finished == 1);
    CHECK_FALSE(vulkan->HasPendingFrame());
    CHECK(vulkan->CompositeBuffer() == VK_NULL_HANDLE);
    CHECK_FALSE(vulkan->RecordPendingFrame(0, VK_NULL_HANDLE));
}

TEST_CASE("VulkanVDPRenderer delegates the renderer save state to its software VDP1", "[vdp2][renderer]") {
    namespace vdp = ymir::vdp;
    using RendererState = ymir::savestate::VDPSaveState::VDPRendererSaveState;
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto swState = std::make_unique<vdp::VDPState>();
    auto vkState = std::make_unique<vdp::VDPState>();
    auto software = std::make_unique<vdp::SoftwareVDPRenderer>(*swState, debug, access);
    auto vulkan = std::make_unique<gpu::VulkanVDPRenderer>(*vkState, debug, access);

    // The same VDP1 drawing (transparent meshes and deinterlace on) in both
    Vdp2SceneWriter writer{{.deinterlace = true, .transparentMeshes = true}};
    writer.Attach(*swState, *software);
    writer.Attach(*vkState, *vulkan);
    std::mt19937 rng{5};
    writer.Reg(TVMD, static_cast<uint16_t>(kDisp | kLSMDDouble));
    writer.RunFrame();
    DrawPolygons(writer, rng, 12, true);

    auto swSaved = std::make_unique<RendererState>();
    auto vkSaved = std::make_unique<RendererState>();
    software->SaveState(*swSaved);
    vulkan->SaveState(*vkSaved);
    CHECK(swSaved->vdp1State.doubleV == vkSaved->vdp1State.doubleV);
    CHECK(swSaved->vdp1State.meshFBRAM == vkSaved->vdp1State.meshFBRAM);
    CHECK(swSaved->vcellScrollInc == vkSaved->vcellScrollInc);
    CHECK(vulkan->ValidateState(*swSaved));

    // A software state loads into the Vulkan renderer, and saves back unchanged.
    vulkan->LoadState(*swSaved);
    vulkan->PostLoadStateSync();
    auto roundTrip = std::make_unique<RendererState>();
    vulkan->SaveState(*roundTrip);
    CHECK(roundTrip->vdp1State.meshFBRAM == swSaved->vdp1State.meshFBRAM);
}
