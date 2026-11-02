// GPU VDP2 scenes: every scene runs through the software renderer and the Vulkan renderer (VulkanVDPRenderer: the
// front half plus the Vulkan back half, VDP1 in software) with the same register, VRAM, CRAM, VDP1 and framebuffer
// writes (vdp2_scene.hpp RenderBoth), and the last frames must be identical in RGB. Each scene first requires the
// software frame to be non-trivial (several distinct colours), so a scene cannot pass with two blank frames. Scene
// contents are mostly seeded random data (pattern names, characters, bitmaps, CRAM, sprite framebuffers, coefficient
// tables), which reaches many pixel paths per scene.
//
// Scenes of the divergences between Ymir's shaders and the software renderer that stage 1B fixed keep their letter from
// stage1b-port-inventory.md section 4.1 (or a stage 1B ruling) in the name: (a), (i), (k), (l), (q)-(u) (Task 5),
// (b)-(h) and (v) (Task 6), and the guards (j), (m)-(o). Feature scenes (RunFeatureScene) also require the feature to
// change the software frame. Documented exceptions (U2, X1, W2-W5; software behaviour the GPU path does not reproduce,
// by a controller ruling) are tagged [!shouldfail], named "exception <id>". Any other failure is a bug.
//
// Also here: resolution changes and VDP resets inside a frame (finding F1), upload overflow (64 KiB upload blocks),
// detach/attach between frames, and FramePresenter::PresentDevice against FramePresenter::Present of the read-back
// composite.
//
// [gpu][vdp2][scene]; SKIP without a Vulkan device. Every scene runs on both drivers (the full suite takes ~25 s on the
// desktop GPU, ~40 s on lavapipe); [lavapipe] marks a quick cross-section for manual runs. BRIMIR_VDP2_POISON=1 runs
// them with the layer images poisoned every frame (any read of a texel the frame did not write changes the result).
// BRIMIR_VDP2_SCALE=N (2-4) runs every scene at internal resolution N, the N x composite compared with the software
// frame at the sample points outside the excluded lattice (vdp2_scene.hpp CompareSamplePointPixels; [scale] cases pin
// N, compare scenes without sub-pixel features as a nearest upscale, and check the sub-pixels themselves against
// vdp2_subpixel_reference.hpp with coordinate bitmaps). BRIMIR_VDP1_MODE=gpu runs the RenderBoth scenes with VDP1 on
// the GPU (GPU stage 2; ctest brimir_tests_gpu_vdp1): both VDP1 modes must pass them. BRIMIR_VDP1_UPSCALING=1 with
// those two runs them with VDP2 showing the N x VDP1 surface (GPU stage 3), at the sample points too. The [nxsprites]
// scene decodes the N x sprite pass's framebuffer mapping at every N x pixel.
#include "catch_amalgamated.hpp"

#include "gpu_test_device.hpp"
#include "vdp2_scene.hpp"
#include "vdp2_subpixel_reference.hpp"

#include <gpu/frame_presenter.hpp>
#include <gpu/vdp/vulkan_vdp_renderer.hpp>
#include <gpu/vdp1/vdp1_hw_params.hpp>
#include <gpu/vk_util.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using brimir::test::CloseAndCheckValidation;
using brimir::test::OpenDevice;
using brimir::test::ReadbackBuffer;
using brimir::test::RenderBoth;
using brimir::test::RenderBothOptions;
using brimir::test::RenderSoftware;
using brimir::test::SceneResult;
using brimir::test::Vdp1Cmd;
using brimir::test::Vdp1Size;
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
                   ZMXDN0 = 0x07A, ZMYIN0 = 0x07C, ZMYDN0 = 0x07E, SCXIN1 = 0x080, SCYIN1 = 0x084, ZMXIN1 = 0x088,
                   ZMXDN1 = 0x08A, ZMYIN1 = 0x08C, ZMYDN1 = 0x08E, LSTA1U = 0x0A4, LSTA1L = 0x0A6, SCXN2 = 0x090,
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
    uint32_t msbPercent = 0;    // pixels with bit 15 (SD) set, half of them with no colour data (types 2-7)
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
            if (sp.msbPercent > 0 && value != 0 && Rand(rng, 100) < sp.msbPercent) {
                value |= 0x8000;
                if (Rand(rng, 2)) {
                    value &= static_cast<uint16_t>(~((1u << bits) - 1)); // transparent, with the shadow bit
                }
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

// A scene built with and without the feature under test: the feature must change the software frame (so the scene
// cannot pass without exercising it), and the scene with the feature must match. The builder is called once per run, so
// state captured by onLine starts fresh each time.
void RunFeatureScene(const std::function<Vdp2Scene(bool feature)> &make, const RenderBothOptions &options = {}) {
    CHECK(brimir::test::RenderSoftware(make(true)) != brimir::test::RenderSoftware(make(false)));
    RunScene(make(true), options);
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
// Resolution changes inside a frame (stage 1B finding F1, Ruling 27): software clears its framebuffer the moment the
// resolution changes (VDP2UpdateResolution) or the VDP resets, so the rows composed before it in the frame are cleared
// and the later rows are drawn with the new resolution. The change comes in the last frame, so no top-border
// resolution update follows it.

namespace {

// Two NBGs; `change(w, y)` runs before each line of the last frame (`frames` frames).
Vdp2Scene MidFrameScene(uint16_t tvmd, bool deinterlace, uint32_t frames, bool backPerLine,
                        std::function<void(Vdp2SceneWriter &, uint32_t)> change) {
    Vdp2Scene scene = Make(
        [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, tvmd);
            if (backPerLine) {
                BackPerLine(w, rng);
            }
            TwoNBGs(w, s, rng);
        },
        {.deinterlace = deinterlace}, frames);
    scene.onLine = [change = std::move(change), frames, frame = 0u](Vdp2SceneWriter &w, uint32_t y) mutable {
        if (y == 0) {
            ++frame;
        }
        if (frame == frames) {
            change(w, y);
        }
    };
    return scene;
}

} // namespace

// The reproducer of F1 (Virtua Fighter 2 with deinterlacing off), optionally followed by a switch to double density.
TEST_CASE("Scene: F1 a mid-frame resolution change clears the composite at that line",
          "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t line = GENERATE(1u, 100u);
    const bool doubleDensity = GENERATE(false, true);
    const bool deinterlace = GENERATE(false, true);
    CAPTURE(line, doubleDensity, deinterlace);
    RunScene(MidFrameScene(kDisp, deinterlace, 2, false, [=](Vdp2SceneWriter &w, uint32_t y) {
        if (y == line) {
            w.SetResolution(320, 224, false);
            if (doubleDensity) {
                w.Reg(TVMD, static_cast<uint16_t>(kDisp | kLSMDDouble));
            }
        }
    }));
}

// BDCLMD=1: the clear colour is the back colour of the last line prepared (a different one on every line).
TEST_CASE("Scene: a mid-frame resolution change clears to the back colour with BDCLMD", "[gpu][vdp2][scene]") {
    const uint32_t line = GENERATE(1u, 100u);
    const bool deinterlace = GENERATE(false, true);
    CAPTURE(line, deinterlace);
    RunScene(MidFrameScene(static_cast<uint16_t>(kDisp | kBDCLMD), deinterlace, 2, true,
                           [=](Vdp2SceneWriter &w, uint32_t y) {
                               if (y == line) {
                                   w.SetResolution(320, 224, false);
                               }
                           }));
}

TEST_CASE("Scene: a mid-frame switch from progressive to interlaced and back", "[gpu][vdp2][scene]") {
    const bool deinterlace = GENERATE(false, true);
    const uint16_t lsmd = GENERATE(kLSMDSingle, kLSMDDouble);
    CAPTURE(deinterlace, lsmd);
    RunScene(MidFrameScene(kDisp, deinterlace, 2, false, [=](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 60) {
            w.Reg(TVMD, static_cast<uint16_t>(kDisp | lsmd));
            w.SetResolution(320, 448, false);
        } else if (y == 150) {
            w.Reg(TVMD, kDisp);
            w.SetResolution(320, 224, false);
        }
    }));
}

// A TVMD write that switches the interlace mode inside the active display, without the resolution update (VDP::
// UpdateResolution runs at the next frame's top border): software places each line's rows by the TVMD in effect for
// that line (VDP2GetY: 2y and, deinterlacing, 2y + 1 when interlaced, else y) into the frame of the old resolution,
// so rows past it are not shown, and the rows of the other mode written earlier stay. Every frame is compared: the
// change comes in frame 1 of 3, and frame 2 starts with the new resolution.
TEST_CASE("Scene: a mid-frame interlace switch without a resolution change places rows as software",
          "[gpu][vdp2][scene]") {
    const auto [from, to] = GENERATE(std::pair{kDisp, static_cast<uint16_t>(kDisp | kLSMDSingle)},
                                     std::pair{kDisp, static_cast<uint16_t>(kDisp | kLSMDDouble)},
                                     std::pair{static_cast<uint16_t>(kDisp | kLSMDSingle), kDisp},
                                     std::pair{static_cast<uint16_t>(kDisp | kLSMDDouble), kDisp},
                                     std::pair{static_cast<uint16_t>(kDisp | kLSMDSingle),
                                               static_cast<uint16_t>(kDisp | kLSMDDouble)},
                                     std::pair{static_cast<uint16_t>(kDisp | kLSMDDouble),
                                               static_cast<uint16_t>(kDisp | kLSMDSingle)});
    const uint32_t line = GENERATE(1u, 100u, 150u, 222u);
    const bool deinterlace = GENERATE(false, true);
    // With two NBGs, or the back screen alone (nothing else splits the frame's batches)
    const bool nbgs = GENERATE(true, false);
    // The switch in the first frame (rows not written in it show nothing yet) or in the second
    const uint32_t changeFrame = GENERATE(1u, 2u);
    CAPTURE(from, to, line, deinterlace, nbgs, changeFrame);
    Vdp2Scene scene = nbgs ? MidFrameScene(from, deinterlace, 3, false, [](Vdp2SceneWriter &, uint32_t) {})
                           : Make(
                                 [from](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                                     Base(w, s, rng, from);
                                     BackPerLine(w, rng);
                                 },
                                 {.deinterlace = deinterlace}, 3);
    scene.onLine = [to, line, changeFrame, frame = 0u](Vdp2SceneWriter &w, uint32_t y) mutable {
        if (y == 0) {
            ++frame;
        }
        if (frame == changeFrame && y == line) {
            w.Reg(TVMD, to);
        }
    };
    RenderBothOptions options;
    options.compareFrame = [](uint32_t) { return true; };
    RunScene(scene, options);
}

// Software indexes its framebuffer with the new HRes from the change on: rows before it were written with the old
// pitch, then cleared.
TEST_CASE("Scene: a mid-frame width change", "[gpu][vdp2][scene][lavapipe]") {
    const auto [from, to] = GENERATE(std::pair{uint16_t{0}, uint16_t{1}}, std::pair{uint16_t{1}, uint16_t{0}});
    const bool deinterlace = GENERATE(false, true);
    CAPTURE(from, to, deinterlace);
    RunScene(MidFrameScene(static_cast<uint16_t>(kDisp | from), deinterlace, 2, false,
                           [to](Vdp2SceneWriter &w, uint32_t y) {
                               if (y == 100) {
                                   w.Reg(TVMD, static_cast<uint16_t>(kDisp | to));
                                   w.SetResolution(to ? 352 : 320, 224, false);
                               }
                           }));
}

// Virtua Fighter 2's path to F1: an SMPC clock change resets the VDP mid-frame (VDP::Reset: renderer reset, resolution
// update, line 0 again); the game then selects double-density hi-res with the display still off, so with deinterlacing
// off only one field's rows are written. Every frame is compared; the last one is the reset frame. With `content`, the
// restarted frame turns the display and two NBGs (per-line back colours, BDCLMD) on at line 10, so the restarted lines
// carry visible content.
TEST_CASE("Scene: a VDP reset mid-frame clears the composite and restarts the frame", "[gpu][vdp2][scene][lavapipe]") {
    const uint32_t line = GENERATE(1u, 149u);
    const bool doubleDensity = GENERATE(false, true);
    const bool deinterlace = GENERATE(false, true);
    const bool content = GENERATE(false, true);
    CAPTURE(line, doubleDensity, deinterlace, content);
    Vdp2Scene scene = MidFrameScene(kDisp, deinterlace, 3, false,
                                    [=, reset = false](Vdp2SceneWriter &w, uint32_t y) mutable {
                                        if (!reset && y == line) {
                                            w.ResetVDP();
                                            reset = true;
                                        } else if (reset && y == 6 && doubleDensity) {
                                            w.Reg(TVMD, static_cast<uint16_t>(kLSMDDouble | kBDCLMD | 3));
                                        } else if (reset && y == 10 && content) {
                                            std::mt19937 rng{11};
                                            Shadow s;
                                            const uint16_t mode = doubleDensity ? kLSMDDouble | 3 : 0;
                                            Base(w, s, rng, static_cast<uint16_t>(kDisp | kBDCLMD | mode));
                                            BackPerLine(w, rng);
                                            TwoNBGs(w, s, rng);
                                            Flush(w, s);
                                        }
                                    });
    RenderBothOptions options;
    options.compareFrame = [](uint32_t) { return true; };
    auto device = OpenDevice();
    const SceneResult result = RenderBoth(scene, *device, options);
    INFO(result.Describe());
    CHECK(result.error.empty());
    CHECK(result.comparedFrames == 3);
    if (content) {
        CHECK(result.distinctColors > 1); // the restarted lines are visible in software
    }
    CHECK(result.identical);
    CloseAndCheckValidation(device);
}

// =====================================================================================================================
// Robustness

TEST_CASE("Scene: upload overflow with 64 KiB upload blocks", "[gpu][vdp2][scene][lavapipe]") {
    // Everything is uploaded on the first frame (VRAM, CRAM, sprite slices), and the CRAM changes on every line. Each
    // context's first frame (frames 0 and 1) overflows its 64 KiB base block; the next base block of the context
    // holds that frame's total (the high-water mark), so frames 2 and 3 create no overflow block.
    auto device = OpenDevice();
    uint32_t blocksAfterFirstFrame = 0;
    std::vector<gpu::vdp2::Vdp2VulkanBackend::RecordStats> stats;
    RenderBothOptions options;
    options.uploadBlockSize = 64 * 1024;
    options.beforeFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (frame == 1) {
            blocksAfterFirstFrame = vulkan.Backend()->UploadBlocksForTest(0);
        }
    };
    options.afterFrame = [&](uint32_t, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        stats.push_back(vulkan.Backend()->LastRecordStatsForTest());
    };
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            SetupSprites(w, {.type = 1}, rng);
        },
        {}, 4);
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) { w.CRAM16((y * 2) & 0x7FE, static_cast<uint16_t>(y * 77)); };
    Check(*device, scene, options);
    CHECK(blocksAfterFirstFrame > 1);
    REQUIRE(stats.size() == 4);
    for (uint32_t frame = 0; frame < 4; ++frame) {
        CAPTURE(frame, stats[frame].overflowBlocks, stats[frame].uploadBytes, stats[frame].baseBlockSize);
        if (frame < 2) {
            CHECK(stats[frame].overflowBlocks > 0);
        } else {
            CHECK(stats[frame].overflowBlocks == 0);
            CHECK(stats[frame].baseBlockSize >= stats[frame].uploadBytes);
        }
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: CRAM written on every line uploads the written words", "[gpu][vdp2][scene][lavapipe]") {
    // One CRAM word per line (CRAM mode 0, so it is mirrored into the coefficient half): every line is a CRAM flush.
    // Frame 0 uploads everything; afterwards each flush uploads the colour cache and RGB555 table entries of the
    // written word and its mirror (4 x 4 bytes) and the coefficient word (4 bytes), and no overflow block is created
    // with the default upload block size.
    auto device = OpenDevice();
    std::vector<gpu::vdp2::Vdp2VulkanBackend::RecordStats> stats;
    RenderBothOptions options;
    options.afterFrame = [&](uint32_t, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        stats.push_back(vulkan.Backend()->LastRecordStatsForTest());
    };
    constexpr uint32_t kFrames = 4;
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
        },
        {}, kFrames);
    scene.onLine = [frame = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            ++*frame;
        }
        // NBG0 (256 colours, palettes 0-127 * 16) and NBG1 read these words; a different value every frame
        w.CRAM16((y * 6) & 0x7FE, static_cast<uint16_t>(y * 77 + *frame * 0x0421));
    };
    Check(*device, scene, options);
    REQUIRE(stats.size() == kFrames);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const auto &s = stats[frame];
        CAPTURE(frame, s.overflowBlocks, s.uploadBytes, s.cramCopyBytes, s.vramCopyBytes);
        CHECK(s.overflowBlocks == 0);
        if (frame >= 1) {
            CHECK(s.vramCopyBytes == 0);
            CHECK(s.cramCopyBytes == 224u * 20u);
        }
    }
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

        using gpu::OutputSettings;
        const OutputSettings settings =
            GENERATE(OutputSettings{}, OutputSettings{.cropH = 16, .cropV = 8}, OutputSettings{.rotation = 90},
                     OutputSettings{.cropH = 8, .cropV = 16, .rotation = 270});
        CAPTURE(settings.cropH, settings.cropV, settings.rotation);
        const gpu::OutputSize size = gpu::ComputeOutputSize(width, height, settings);
        gpu::FramePresenter presenter(*device, 2, size);
        REQUIRE(presenter.Ok());
        REQUIRE(gpu::vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                    REQUIRE(presenter.Present(0, composite.data(), width, height, settings, cmd));
                    REQUIRE(presenter.PresentDevice(1, vulkan->CompositeBuffer(), width, height, settings, cmd));
                }) == VK_SUCCESS);
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
        gpu::FramePresenter presenter(*device, 1, {320, 224});
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
// Background parity (stage1b-port-inventory.md section 4.1 (b)-(h)): per-line NBG state, mosaic, line and vertical
// scroll, vertical cell scroll, character pattern delay, data access shift.

TEST_CASE("Scene: (b) NBG mosaic", "[gpu][vdp2][scene]") {
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

TEST_CASE("Scene: (c) RBG mosaic", "[gpu][vdp2][scene]") {
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupRBG0(w, s, {}, rng);
        w.Reg(MZCTL, 0x0310); // RBG0, 4 pixels wide
    }));
}

TEST_CASE("Scene: (d) line scroll in double density", "[gpu][vdp2][scene]") {
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
          "[gpu][vdp2][scene]") {
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

TEST_CASE("Scene: (f) vertical cell scroll with horizontal zoom", "[gpu][vdp2][scene]") {
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

TEST_CASE("Scene: (g) character pattern delay", "[gpu][vdp2][scene]") {
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

// NBG n scroll registers (integer parts)
constexpr uint32_t kSCXIN[4] = {SCXIN0, SCXIN1, SCXN2, SCXN3};
constexpr uint32_t kSCYIN[4] = {SCYIN0, SCYIN1, SCYN2, SCYN3};

// Interlace modes: progressive, single density, single density deinterlaced, double density, double density
// deinterlaced.
struct InterlaceMode {
    uint16_t lsmd;
    bool deinterlace;
};
constexpr InterlaceMode kInterlaceModes[] = {
    {0, false}, {kLSMDSingle, false}, {kLSMDSingle, true}, {kLSMDDouble, false}, {kLSMDDouble, true}};

} // namespace

TEST_CASE("Scene: (b) NBG mosaic on each NBG, cells and bitmaps", "[gpu][vdp2][scene]") {
    // Horizontal and vertical mosaic with a window, scrolled. A layer split at line 101 (a scroll write) makes lines
    // 101-103 repeat line 100 from an earlier batch, and the mosaic goes off at line 150 and on again at line 170
    // (the counter keeps its value meanwhile).
    const auto config = GENERATE(std::pair{0u, false}, std::pair{1u, false}, std::pair{2u, false},
                                 std::pair{3u, false}, std::pair{0u, true}, std::pair{1u, true});
    const uint32_t nbg = config.first;
    const bool bitmap = config.second;
    CAPTURE(nbg, bitmap);
    const uint16_t mzctl = static_cast<uint16_t>((1u << nbg) | ((2 + nbg) << 8) | (3u << 12)); // (3+n) x 4
    RunFeatureScene([=](bool feature) {
        Vdp2Scene scene = Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            CyclesAll(w);
            SetupNBG(w, s, {.index = nbg, .format = 0, .bitmap = bitmap, .priority = 6}, rng);
            w.Reg(kSCYIN[nbg], 3);
            WindowRects(w, false);
            w.Reg(nbg < 2 ? WCTLA : WCTLB, static_cast<uint16_t>(0x02 << (8 * (nbg & 1)))); // W0 inside
            if (feature) {
                w.Reg(MZCTL, mzctl);
            }
        });
        scene.onLine = [=](Vdp2SceneWriter &w, uint32_t y) {
            if (y == 0) {
                w.Reg(kSCXIN[nbg], static_cast<uint16_t>(5 + nbg));
                if (feature) {
                    w.Reg(MZCTL, mzctl);
                }
            } else if (y == 101) {
                w.Reg(kSCXIN[nbg], static_cast<uint16_t>(9 + nbg));
            } else if (y == 150 && feature) {
                w.Reg(MZCTL, 0);
            } else if (y == 170 && feature) {
                w.Reg(MZCTL, mzctl);
            }
        };
        return scene;
    });
}

TEST_CASE("Scene: (b) NBG vertical mosaic in interlaced modes", "[gpu][vdp2][scene]") {
    const InterlaceMode mode = GENERATE(from_range(std::begin(kInterlaceModes), std::end(kInterlaceModes)));
    CAPTURE(mode.lsmd, mode.deinterlace);
    RunFeatureScene([=](bool feature) {
        Vdp2Scene scene = Make(
            [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                Base(w, s, rng, static_cast<uint16_t>(kDisp | mode.lsmd));
                TwoNBGs(w, s, rng);
                w.Reg(SCYIN0, 7);
                w.Reg(ZMYIN0, 0);
                w.Reg(ZMYDN0, 0xC000);
                if (feature) {
                    w.Reg(MZCTL, 0x5103); // NBG0 and NBG1, 2 x 6
                }
            },
            {.deinterlace = mode.deinterlace}, 3);
        scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) {
            if (y == 0) {
                w.Reg(SCXIN1, 0);
            } else if (y == 64) {
                w.Reg(SCXIN1, 3); // a layer split inside a mosaic block
            }
        };
        return scene;
    });
}

TEST_CASE("Scene: (c) RBG mosaic in normal and hi-res modes", "[gpu][vdp2][scene]") {
    const uint16_t hreso = GENERATE(uint16_t{0}, uint16_t{2});
    const bool window = GENERATE(false, true);
    CAPTURE(hreso, window);
    RunFeatureScene([=](bool feature) {
        return Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | hreso));
            SetupRBG0(w, s, {}, rng);
            if (window) {
                w.Reg(WPSX0, 41 << 1);
                w.Reg(WPEX0, 203 << 1);
                w.Reg(WPSY0, 20);
                w.Reg(WPEY0, 150);
                w.Reg(WCTLC, 0x0002);
            }
            if (feature) {
                w.Reg(MZCTL, 0x0410); // RBG0, 5 pixels wide
            }
        });
    });
}

TEST_CASE("Scene: (d) line scroll X Y and zoom at every interval in every interlace mode", "[gpu][vdp2][scene]") {
    // NBG0: X, Y and zoom; NBG1: X and Y; NBG0 drops the zoom at line 90 (a mid-frame flag change).
    const uint32_t interval = GENERATE(0u, 1u, 2u, 3u);
    const InterlaceMode mode = GENERATE(from_range(std::begin(kInterlaceModes), std::end(kInterlaceModes)));
    CAPTURE(interval, mode.lsmd, mode.deinterlace);
    constexpr uint32_t kLineScroll1Address = 0x7A000;
    const uint16_t scrctl = static_cast<uint16_t>((0x0E | (interval << 4)) | ((0x06 | (interval << 4)) << 8));
    RunFeatureScene([=](bool feature) {
        Vdp2Scene scene = Make(
            [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                Base(w, s, rng, static_cast<uint16_t>(kDisp | mode.lsmd));
                TwoNBGs(w, s, rng);
                FillVRAM(w, kLineScrollAddress, 0x2000, rng);
                FillVRAM(w, kLineScroll1Address, 0x2000, rng);
                w.Reg(LSTA0U, static_cast<uint16_t>(kLineScrollAddress >> 17));
                w.Reg(LSTA0L, static_cast<uint16_t>((kLineScrollAddress >> 1) & 0xFFFF));
                w.Reg(LSTA1U, static_cast<uint16_t>(kLineScroll1Address >> 17));
                w.Reg(LSTA1L, static_cast<uint16_t>((kLineScroll1Address >> 1) & 0xFFFF));
                w.Reg(SCYIN0, 9);
                w.Reg(ZMYIN1, 0);
                w.Reg(ZMYDN1, 0x8000);
            },
            {.deinterlace = mode.deinterlace}, 3);
        scene.onLine = [=](Vdp2SceneWriter &w, uint32_t y) {
            if (!feature) {
                return;
            }
            if (y == 0) {
                w.Reg(SCRCTL, scrctl);
            } else if (y == 90) {
                w.Reg(SCRCTL, static_cast<uint16_t>(scrctl & ~0x0008));
            }
        };
        return scene;
    });
}

TEST_CASE("Scene: (e) vertical scroll with SCYN2 SCYN3 and ZMYIN writes mid-frame", "[gpu][vdp2][scene]") {
    const InterlaceMode mode = GENERATE(InterlaceMode{0, false}, InterlaceMode{kLSMDDouble, false},
                                        InterlaceMode{kLSMDDouble, true});
    CAPTURE(mode.lsmd, mode.deinterlace);
    RunFeatureScene([=](bool feature) {
        Vdp2Scene scene = Make(
            [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                Base(w, s, rng, static_cast<uint16_t>(kDisp | mode.lsmd));
                CyclesAll(w);
                SetupNBG(w, s, {.index = 0, .format = 0, .priority = 2}, rng);
                SetupNBG(w, s, {.index = 2, .format = 0, .priority = 6}, rng);
                SetupNBG(w, s, {.index = 3, .format = 0, .cell2x2 = true, .priority = 4}, rng);
                w.Reg(ZMYIN0, 0);
                w.Reg(ZMYDN0, 0x8000);
            },
            {.deinterlace = mode.deinterlace}, 3);
        scene.onLine = [=](Vdp2SceneWriter &w, uint32_t y) {
            if (y == 0) {
                w.Reg(SCYN2, 0);
                w.Reg(SCYN3, 0);
                w.Reg(ZMYIN0, 0);
                w.Reg(ZMYDN0, 0x8000);
            }
            if (!feature) {
                return;
            }
            if (y == 77) {
                w.Reg(SCYN2, 40); // the counter restarts at line 77
            } else if (y == 130) {
                w.Reg(SCYN3, 7);
            } else if (y == 50) {
                w.Reg(ZMYIN0, 2);
            } else if (y == 160) {
                w.Reg(ZMYIN0, 0);
                w.Reg(ZMYDN0, 0x4000);
            }
        };
        return scene;
    });
}

TEST_CASE("Scene: (f) vertical cell scroll with zoom, repeat and delay", "[gpu][vdp2][scene]") {
    // NBG0 (1x1) and NBG1 (2x2) with vertical cell scroll, horizontally reduced 1, 1/2 or 1/4. Vertical cell scroll
    // reads in bank B1 at:
    //   0: NBG0 T1, NBG1 T2 (no delay)
    //   1: NBG0 T2 (repeat), NBG1 T3 (delay)
    //   2: NBG0 T3 (repeat and delay), NBG1 T4 (delay)
    //   3: NBG0 T5 (repeat and delay) only
    //   4: as 2, with horizontal mosaic on NBG0 (the line's first value applies to the whole line)
    // With a delay, the first reads of a line return the previous line's last value (offset -1).
    const uint32_t reduction = GENERATE(1u, 2u, 4u);
    const uint32_t variant = GENERATE(0u, 1u, 2u, 3u, 4u);
    CAPTURE(reduction, variant);
    static constexpr uint16_t kCYCB1L[] = {0xFCDF, 0xFFCD, 0xFFFC, 0xFFFF, 0xFFFC};
    static constexpr uint16_t kCYCB1U[] = {0xFFFF, 0xFFFF, 0xDFFF, 0xFCFF, 0xDFFF};
    RunFeatureScene([=](bool feature) {
        return Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            SetupNBG(w, s, {.index = 0, .format = 0, .priority = 5}, rng);
            SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = true, .priority = 3}, rng);
            // CP0 T0-T1, CP1 T2-T3 in A0; PN0 T0, PN1 T1 in B0; vertical cell scroll in B1
            w.Reg(CYCA0L, 0x4455);
            w.Reg(CYCA0U, 0xFFFF);
            w.Reg(CYCB0L, 0x01FF);
            w.Reg(CYCB0U, 0xFFFF);
            w.Reg(CYCB1L, kCYCB1L[variant]);
            w.Reg(CYCB1U, kCYCB1U[variant]);
            constexpr uint32_t kVCellAddress = 0x7D000;
            FillVRAM(w, kVCellAddress, 0x800, rng);
            w.Reg(VCSTAU, static_cast<uint16_t>(kVCellAddress >> 17));
            w.Reg(VCSTAL, static_cast<uint16_t>((kVCellAddress >> 1) & 0xFFFE));
            if (feature) {
                w.Reg(SCRCTL, variant == 3 ? 0x0001 : 0x0101);
            }
            w.Reg(SCXIN0, 3);
            w.Reg(SCXDN0, 0x4000);
            w.Reg(ZMXIN0, static_cast<uint16_t>(reduction));
            w.Reg(ZMXIN1, static_cast<uint16_t>(reduction));
            w.Reg(SCXIN1, 13);
            if (variant == 4) {
                w.Reg(MZCTL, 0x0201); // NBG0, 3 pixels wide
            }
        });
    });
}

TEST_CASE("Scene: (g) character pattern delay of 1x1 and 2x2 characters", "[gpu][vdp2][scene]") {
    // NBG1 with CP1 at T0 and PN1 at T7 in every bank: an illegal T0-T3 character read, which delays the characters
    // of 1x1 and 2x2 cells alike. Extras: 0 none, 1 a window, 2 zoomed in 0.75, 3 reduced 1.5, 4 horizontal mosaic,
    // 5 one-word pattern names.
    const bool cell2x2 = GENERATE(false, true);
    const uint32_t extra = GENERATE(0u, 1u, 2u, 3u, 4u, 5u);
    CAPTURE(cell2x2, extra);
    RunFeatureScene([=](bool feature) {
        Vdp2Scene scene = Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = cell2x2, .twoWord = extra != 5}, rng);
            for (uint32_t reg : {CYCA0L, CYCA1L, CYCB0L, CYCB1L}) {
                // Without the feature: PN1 at T0 (no delay check for NBG0/NBG1) and CP1 at T1
                w.Reg(reg, feature ? 0x5FFF : 0x15FF);
                w.Reg(reg + 2, feature ? 0xFFF1 : 0xFFFF);
            }
            w.Reg(SCYIN1, 5);
            switch (extra) {
            case 1:
                WindowRects(w, false);
                w.Reg(WCTLA, 0x0A00); // NBG1: W0 inside or W1 inside
                break;
            case 2: w.Reg(ZMXIN1, 0), w.Reg(ZMXDN1, 0xC000); break;
            case 3: w.Reg(ZMXIN1, 1), w.Reg(ZMXDN1, 0x8000); break;
            case 4: w.Reg(MZCTL, 0x0202); break; // NBG1, 3 pixels wide
            }
        });
        scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) {
            w.Reg(SCXIN1, static_cast<uint16_t>(y / 3)); // a different left edge on every line
        };
        return scene;
    });
}

TEST_CASE("Scene: (h) data access shift of scroll and bitmap NBGs", "[gpu][vdp2][scene]") {
    // 0: NBG2 with PN2 at T0 in bank B0 and its only CP2 at T3 in bank A0, illegal: one character read early, every
    //    character shifted by 8 dots (vramDataOffset 8 in every bank, sw:5167-5170).
    // 1: NBG0 bitmap in hi-res with CP0 at T0-T1 in bank A0 and T2-T3 in bank B0: the bank B reads are delayed
    //    (vramDataOffset 8 in bank B, sw:5370-5372).
    const uint32_t variant = GENERATE(0u, 1u);
    CAPTURE(variant);
    RunFeatureScene([=](bool feature) {
        return Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            if (variant == 0) {
                Base(w, s, rng);
                SetupNBG(w, s, {.index = 2, .format = 0}, rng);
                w.Reg(CYCA0L, feature ? 0xFFF6 : 0xF6FF);
                w.Reg(CYCA0U, 0xFFFF);
                w.Reg(CYCB0L, 0x2FFF);
                w.Reg(CYCB0U, 0xFFFF);
                w.Reg(SCXN2, 3);
            } else {
                Base(w, s, rng, static_cast<uint16_t>(kDisp | 2));
                SetupNBG(w, s, {.index = 0, .format = 1, .bitmap = true, .bitmapSize = 3}, rng);
                // The 1024x512 bitmap spans banks A and B
                FillVRAM(w, 0x40000, 0x40000, rng);
                w.Reg(CYCA0L, 0x44FF);
                w.Reg(CYCB0L, feature ? 0xFF44 : 0x44FF);
                w.Reg(CYCA0U, 0xFFFF);
                w.Reg(CYCB0U, 0xFFFF);
                w.Reg(SCYIN0, 256); // rows 256 and below are in bank B
            }
        });
    });
}

TEST_CASE("Scene: display enabled mid-frame with RBG0 and NBG0 line scroll", "[gpu][vdp2][scene]") {
    // Ruling 13: the second frame turns the display off at line 200 (the latch goes off), the third turns it on at line
    // 100: the display latch stays off, so lines 100 and below are drawn with the per-line state of the last line
    // computed (line 199 of the second frame): RBG0's rotation bases and NBG0's line scroll and zoom do not advance,
    // while NBG0's vertical scroll counter does (sw:2513-2565).
    const bool rbg = GENERATE(false, true);
    CAPTURE(rbg);
    RunFeatureScene([=](bool feature) {
        Vdp2Scene scene = Make(
            [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                Base(w, s, rng);
                if (rbg) {
                    RBG r;
                    r.a = Rotated(0.3, 0.9);
                    r.a.dxst = feature ? 0.7 : 0.0;
                    r.a.dyst = feature ? 1.3 : 1.0;
                    SetupRBG0(w, s, r, rng);
                } else {
                    CyclesSingle(w, 0);
                    SetupNBG(w, s, {.index = 0, .format = 0}, rng);
                    FillVRAM(w, kLineScrollAddress, 0x2000, rng);
                    w.Reg(LSTA0U, static_cast<uint16_t>(kLineScrollAddress >> 17));
                    w.Reg(LSTA0L, static_cast<uint16_t>((kLineScrollAddress >> 1) & 0xFFFF));
                    w.Reg(SCRCTL, feature ? 0x000E : 0x0000);
                }
            },
            {}, 3);
        scene.onLine = [frame = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
            if (y == 0) {
                ++*frame;
            }
            if (*frame == 2 && y == 200) {
                w.Reg(TVMD, 0);
            } else if (*frame == 3 && y == 100) {
                w.Reg(TVMD, kDisp);
            }
        };
        return scene;
    });
}

TEST_CASE("Scene: (g) delayed characters after pattern name rewrites", "[gpu][vdp2][scene]") {
    // Ruling 18: the delayed character at a line's left edge is the character the previous drawn line fetched last,
    // decoded with the VRAM and registers of that line. The pattern names are rewritten before line 0 (between
    // frames) and before line 120, and the supplementary palette bits change at line 60.
    const bool cell2x2 = GENERATE(false, true);
    CAPTURE(cell2x2);
    RunFeatureScene([=](bool feature) {
        Vdp2Scene scene = Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = cell2x2, .twoWord = false}, rng);
            for (uint32_t reg : {CYCA0L, CYCA1L, CYCB0L, CYCB1L}) {
                w.Reg(reg, 0x5FFF); // CP1 at T0, PN1 at T7: delayed characters
                w.Reg(reg + 2, 0xFFF1);
            }
            w.Reg(SCYIN1, 5);
        });
        scene.onLine = [=, frame = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
            w.Reg(SCXIN1, static_cast<uint16_t>(y / 3 + 1));
            if (y == 0) {
                ++*frame;
                w.Reg(PNCN0 + 2, 0x8000); // one word, no supplementary bits
            }
            if (!feature) {
                return;
            }
            if (y == 0 || y == 120) {
                // One-word pattern names of NBG1's page, different every frame and from line 120
                std::mt19937 rng{*frame * 1000 + y};
                const uint32_t entries = cell2x2 ? 32 * 32 : 64 * 64;
                for (uint32_t i = 0; i < entries; ++i) {
                    const uint32_t cn = Rand(rng, 32); // as SetupNBG: characters 0-31 (2x2: in units of 4 cells)
                    w.VRAM16(kPageBase + kPageStride + i * 2,
                             static_cast<uint16_t>((Rand(rng, 16) << 12) | (Rand(rng, 4) << 10) | cn));
                }
            } else if (y == 60) {
                w.Reg(PNCN0 + 2, 0x80E0); // one word, supplementary palette bits
            }
        };
        return scene;
    });
}

namespace {

// NBG1 with CP1 at T0 and PN1 at T7 in every bank when `delay` (delayed characters of 1x1 and 2x2 cells), else PN1 at
// T0 and CP1 at T1, as "(g) character pattern delay of 1x1 and 2x2 characters"; a different left edge on every line.
Vdp2Scene DelayedNBG1Scene(bool delay, bool cell2x2,
                           std::function<void(Vdp2SceneWriter &, Shadow &, std::mt19937 &)> extra) {
    Vdp2Scene scene = Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = cell2x2}, rng);
        for (uint32_t reg : {CYCA0L, CYCA1L, CYCB0L, CYCB1L}) {
            w.Reg(reg, delay ? 0x5FFF : 0x15FF);
            w.Reg(reg + 2, delay ? 0xFFF1 : 0xFFFF);
        }
        w.Reg(SCYIN1, 5);
        extra(w, s, rng);
    });
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) { w.Reg(SCXIN1, static_cast<uint16_t>(y / 3)); };
    return scene;
}

} // namespace

// Ruling 28: the NBG fetch pass replays the fetcher with windows, so a delayed pixel after windowed pixels shows the
// character software's fetcher shows there.
TEST_CASE("Scene: (g) character pattern delay with a sprite window", "[gpu][vdp2][scene]") {
    // NBG1 window control: 0 the sprite window, 1 the sprite window inverted AND W0, 2 the sprite window AND W0, 3 the
    // sprite window OR W1 inverted. The sprite window is random per pixel (half the sprite pixels are transparent).
    const bool cell2x2 = GENERATE(false, true);
    const uint32_t variant = GENERATE(0u, 1u, 2u, 3u);
    CAPTURE(cell2x2, variant);
    static constexpr uint16_t kWCTL[] = {0x20, 0xB2, 0xA2, 0x2D};
    RunFeatureScene([=](bool feature) {
        return DelayedNBG1Scene(feature, cell2x2, [=](Vdp2SceneWriter &w, Shadow &, std::mt19937 &rng) {
            WindowRects(w, false);
            SetupSprites(w, {.type = 3, .window = true, .transparentPercent = 50}, rng);
            w.Reg(WCTLA, static_cast<uint16_t>(kWCTL[variant] << 8));
            w.Reg(WCTLC, 0x0300); // the sprite layer: W0 inverted
        });
    });
}

TEST_CASE("Scene: (g) character pattern delay with W0 hiding most of the line", "[gpu][vdp2][scene]") {
    // 0: W0 hides 8-311; 1: W0 hides 0-318; 2: W0 inverted, only 8-311 shown; 3: as 0 with horizontal mosaic 3;
    // 4: as 0, reduced 1.5 (450 dots hidden).
    const bool cell2x2 = GENERATE(false, true);
    const uint32_t variant = GENERATE(0u, 1u, 2u, 3u, 4u);
    CAPTURE(cell2x2, variant);
    RunFeatureScene([=](bool feature) {
        return DelayedNBG1Scene(feature, cell2x2, [=](Vdp2SceneWriter &w, Shadow &, std::mt19937 &) {
            w.Reg(WPSX0, variant == 1 ? 0 : 8 << 1);
            w.Reg(WPEX0, variant == 1 ? 318 << 1 : 311 << 1);
            w.Reg(WPSY0, 0);
            w.Reg(WPEY0, 223);
            w.Reg(WCTLA, variant == 2 ? 0x0300 : 0x0200);
            if (variant == 3) {
                w.Reg(MZCTL, 0x0202); // NBG1, 3 pixels wide
            } else if (variant == 4) {
                w.Reg(ZMXIN1, 1);
                w.Reg(ZMXDN1, 0x8000);
            }
        });
    });
}

TEST_CASE("Scene: (g) character pattern delay with a line window", "[gpu][vdp2][scene]") {
    // A random W0 line window per line, alone or ANDed with a W1 line window moving with the line.
    const bool cell2x2 = GENERATE(false, true);
    const bool both = GENERATE(false, true);
    CAPTURE(cell2x2, both);
    RunFeatureScene([=](bool feature) {
        return DelayedNBG1Scene(feature, cell2x2, [=](Vdp2SceneWriter &w, Shadow &, std::mt19937 &rng) {
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
            w.Reg(WCTLA, both ? 0x8B00 : 0x0200);
        });
    });
}

TEST_CASE("Scene: (f) vertical cell scroll on 2x2 characters with a window", "[gpu][vdp2][scene]") {
    // No delay: NBG1's 2x2 characters with vertical cell scroll, reduced 1 or 1/2, so a character's two cell columns
    // read different table values; the character of a run is its first fetch's, windows included. Windows: 0 W0
    // hiding 8-311, 1 W0 or W1, 2 the sprite window.
    const uint32_t reduction = GENERATE(1u, 2u);
    const uint32_t variant = GENERATE(0u, 1u, 2u);
    CAPTURE(reduction, variant);
    RunFeatureScene([=](bool feature) {
        return Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = true}, rng);
            // CP1 T2-T3 in A0; PN1 T1 in B0; vertical cell scroll in B1 at T2 (no delay)
            w.Reg(CYCA0L, 0xFF55);
            w.Reg(CYCA0U, 0xFFFF);
            w.Reg(CYCB0L, 0xF1FF);
            w.Reg(CYCB0U, 0xFFFF);
            w.Reg(CYCB1L, 0xFFDF);
            w.Reg(CYCB1U, 0xFFFF);
            constexpr uint32_t kVCellAddress = 0x7D000;
            FillVRAM(w, kVCellAddress, 0x800, rng);
            w.Reg(VCSTAU, static_cast<uint16_t>(kVCellAddress >> 17));
            w.Reg(VCSTAL, static_cast<uint16_t>((kVCellAddress >> 1) & 0xFFFE));
            if (feature) {
                w.Reg(SCRCTL, 0x0100);
            }
            w.Reg(ZMXIN1, static_cast<uint16_t>(reduction));
            w.Reg(SCXIN1, 13);
            switch (variant) {
            case 0:
                w.Reg(WPSX0, 8 << 1);
                w.Reg(WPEX0, 311 << 1);
                w.Reg(WPSY0, 0);
                w.Reg(WPEY0, 223);
                w.Reg(WCTLA, 0x0200);
                break;
            case 1:
                WindowRects(w, false);
                w.Reg(WCTLA, 0x0A00);
                break;
            case 2:
                SetupSprites(w, {.type = 3, .window = true, .transparentPercent = 50}, rng);
                w.Reg(WCTLA, 0x2000);
                w.Reg(WCTLC, 0x0300);
                break;
            }
        });
    });
}

TEST_CASE("Scene: (g) character pattern delay in one bank across a plane boundary, with windows",
          "[gpu][vdp2][scene]") {
    // NBG1's plane A pattern names are in bank B0 (0x48000), plane B's in bank B1 (0x68000), with a delay only in
    // `delayBank`, so the delay bit changes from fetch to fetch inside a row. CalcAccessPatterns sets the delay in
    // every bank or none, so the scene sets the derived bgParams.charPatDelay itself before each line, in both
    // renderers' states, after computing the access patterns as PrepareLine would (which then keeps them: they are
    // recomputed only after a write that makes them dirty).
    // Layout 0: zoom 1, scroll X 300 + y / 3: the plane boundary near pixel 212; W0 hides 180-240 across it, W1 40-60.
    // Layout 1: 2 dots per pixel, scroll X y / 3: W0 hides pixels 31-286 (512 dots), so the fetch at pixel 287 (plane
    // B) has the index of the fetch at pixel 30 (plane A) on most lines and continues its run. With 2x2 characters, a
    // run started delayed (delay in plane A's bank) must not switch at plane B's undelayed fetches; a run started
    // undelayed (delay in plane B's bank) switches at a delayed fetch, which shows the run start's own character, as
    // before the switch, and sets lastCellX for the runs after it.
    const bool cell2x2 = GENERATE(false, true);
    const uint32_t delayBank = GENERATE(2u, 3u);
    const uint32_t layout = GENERATE(0u, 1u);
    CAPTURE(cell2x2, delayBank, layout);
    RunFeatureScene([=](bool feature) {
        Vdp2Scene scene = Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = cell2x2}, rng);
            for (uint32_t reg : {CYCA0L, CYCA1L, CYCB0L, CYCB1L}) {
                w.Reg(reg, 0x15FF); // PN1 at T0, CP1 at T1: no delay from the access patterns
                w.Reg(reg + 2, 0xFFFF);
            }
            // Plane B on a page in bank B1 with its own pattern names
            constexpr uint32_t kPlaneA = kPageBase + kPageStride, kPlaneB = kPlaneA + 0x20000;
            const uint32_t pageShift = cell2x2 ? 12 : 14;
            const uint32_t mapA = kPlaneA >> pageShift, mapB = kPlaneB >> pageShift;
            REQUIRE((mapA >> 6) == (mapB >> 6)); // the same MPOFN bits
            w.Reg(MPABN0 + 4, static_cast<uint16_t>(((mapB & 0x3F) << 8) | (mapA & 0x3F)));
            const uint32_t entries = cell2x2 ? 32 * 32 : 64 * 64;
            for (uint32_t i = 0; i < entries; ++i) {
                const uint32_t charNum = Rand(rng, 32) * (cell2x2 ? 4 : 1);
                w.VRAM16(kPlaneB + i * 4, static_cast<uint16_t>((Rand(rng, 4) << 14) | Rand(rng, 128)));
                w.VRAM16(kPlaneB + i * 4 + 2, static_cast<uint16_t>(charNum));
            }
            w.Reg(SCYIN1, 5);
            if (layout == 0) {
                w.Reg(WPSX0, 180 << 1);
                w.Reg(WPEX0, 240 << 1);
                w.Reg(WPSX1, 40 << 1);
                w.Reg(WPEX1, 60 << 1);
                w.Reg(WPSY1, 0);
                w.Reg(WPEY1, 223);
                w.Reg(WCTLA, 0x0A00); // NBG1: W0 inside or W1 inside
            } else {
                w.Reg(ZMXIN1, 2);
                w.Reg(WPSX0, 31 << 1);
                w.Reg(WPEX0, 286 << 1);
                w.Reg(WCTLA, 0x0200); // NBG1: W0 inside
            }
            w.Reg(WPSY0, 0);
            w.Reg(WPEY0, 223);
        });
        scene.onLine = [=](Vdp2SceneWriter &w, uint32_t y) {
            w.Reg(SCXIN1, static_cast<uint16_t>((layout == 0 ? 300 : 0) + y / 3));
            w.ForEachState([=](ymir::vdp::VDPState &state) {
                state.state2.CalcAccessPatterns(state.regs2, ymir::vdp::config::VDP2AccessPatternsConfig{});
                auto &delay = state.regs2.bgParams[2].charPatDelay; // NBG1
                delay.fill(false);
                delay[delayBank] = feature;
            });
        };
        return scene;
    });
}

TEST_CASE("Scene: (g) character pattern delay in hi-res and deinterlaced modes, with windows", "[gpu][vdp2][scene]") {
    // The NBG fetch pass over hi-res rows (640 and 704 pixels, the pass's whole row) and the alternate field's rows
    // when deinterlacing: 0 640x224, 1 704x224, 2 320x448 double density, 3 704x448 double density, 4 320x448 single
    // density (2-4 deinterlaced). NBG1 delayed by its access patterns (hi-res: CP1 at T0 before PN1 at T3; normal: CP1
    // at T0, PN1 at T7), W0 hiding a part of each row, a different left edge on every line.
    const bool cell2x2 = GENERATE(false, true);
    const uint32_t mode = GENERATE(0u, 1u, 2u, 3u, 4u);
    CAPTURE(cell2x2, mode);
    static constexpr uint16_t kTVMD[] = {2, 3, kLSMDDouble, 3 | kLSMDDouble, kLSMDSingle};
    const bool hires = (kTVMD[mode] & 2) != 0;
    RunFeatureScene([=](bool feature) {
        Vdp2Scene scene = Make(
            [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                Base(w, s, rng, static_cast<uint16_t>(kDisp | kTVMD[mode]));
                SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = cell2x2}, rng);
                for (uint32_t reg : {CYCA0L, CYCA1L, CYCB0L, CYCB1L}) {
                    if (hires) {
                        w.Reg(reg, feature ? 0x5FF1 : 0x15FF);
                        w.Reg(reg + 2, 0xFFFF);
                    } else {
                        w.Reg(reg, feature ? 0x5FFF : 0x15FF);
                        w.Reg(reg + 2, feature ? 0xFFF1 : 0xFFFF);
                    }
                }
                w.Reg(SCYIN1, 5);
                // Hi-res window coordinates are pixels; normal ones are doubled
                w.Reg(WPSX0, hires ? 210 : 70 << 1);
                w.Reg(WPEX0, hires ? 430 : 150 << 1);
                w.Reg(WPSY0, 0);
                w.Reg(WPEY0, 511);
                w.Reg(WCTLA, 0x0200); // NBG1: W0 inside
            },
            {.deinterlace = mode >= 2}, 3);
        scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) { w.Reg(SCXIN1, static_cast<uint16_t>(y / 3)); };
        return scene;
    });
}

TEST_CASE("Scene: save and load of the Vulkan renderer mid-frame continues exactly", "[gpu][vdp2][scene]") {
    // Ruling 16: before line 100 of the last frame the Vulkan renderer's state is saved, lost and loaded again; the
    // frame must still be software's. NBG0 has vertical cell scroll with repeat and delay (the first values of a line
    // are the previous line's last value), NBG1 delayed 256-colour 2x2 characters (the left edge shows the fetcher's
    // nextChar), both carried by the renderer section of the save state.
    const bool doubleDensity = GENERATE(false, true);
    CAPTURE(doubleDensity);
    Vdp2Scene scene = Make(
        [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | (doubleDensity ? kLSMDDouble : 0)));
            SetupNBG(w, s, {.index = 0, .format = 0, .priority = 5}, rng);
            SetupNBG(w, s, {.index = 1, .format = 1, .cell2x2 = true, .priority = 3}, rng);
            w.Reg(CYCA0L, 0x455F); // CP0 at T0, CP1 at T1-T2 of bank A0
            w.Reg(CYCA0U, 0xFFFF);
            w.Reg(CYCB0L, 0xFFFF);
            w.Reg(CYCB0U, 0x0FF1); // PN0 at T4, PN1 at T7 of bank B0: CP1 is illegal for PN1 at T7
            w.Reg(CYCB1L, 0xFFFC); // NBG0 vertical cell scroll at T3: repeat and delay
            w.Reg(CYCB1U, 0xFFFF);
            constexpr uint32_t kVCellAddress = 0x7D000;
            FillVRAM(w, kVCellAddress, 0x800, rng);
            w.Reg(VCSTAU, static_cast<uint16_t>(kVCellAddress >> 17));
            w.Reg(VCSTAL, static_cast<uint16_t>((kVCellAddress >> 1) & 0xFFFE));
            w.Reg(SCRCTL, 0x0001);
            w.Reg(SCXIN0, 3);
            w.Reg(SCXIN1, 13);
        },
        {.deinterlace = doubleDensity}, 3);
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) { w.Reg(SCXIN1, static_cast<uint16_t>(13 + y / 5)); };
    RunScene(scene, {.reloadFrame = 2, .reloadLine = 100});
}

TEST_CASE("Scene: (c) RBG mosaic with the line colour screen", "[gpu][vdp2][scene]") {
    // Ruling 17 (master 53bfa4c): mosaic-repeated RBG dots show the line colour of the dot they repeat, from the line
    // colour table and from coefficient line colour data.
    const uint16_t hreso = GENERATE(uint16_t{0}, uint16_t{2});
    const bool coeffLineColor = GENERATE(false, true);
    CAPTURE(hreso, coeffLineColor);
    RunFeatureScene([=](bool feature) {
        return Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | hreso), 1);
            SetupRBG0(w, s, {.coeffTable = coeffLineColor, .coeffLineColor = coeffLineColor}, rng);
            for (uint32_t y = 0; y < 512; ++y) {
                w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x800)));
            }
            w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
            w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
            s.lnclen = 0x0010; // RBG0
            w.Reg(CCCTL, 0x0030);
            w.Reg(CCRR, 0x0008);
            if (feature) {
                w.Reg(MZCTL, 0x0310); // RBG0, 4 dots wide
            }
        });
    });
}

TEST_CASE("Scene: rotated 8-bit sprites in hi-res modes", "[gpu][vdp2][scene]") {
    // Ruling 17 (master 53bfa4c): with an 8-bit framebuffer the output is not doubled in hi-res modes, and each
    // pixel reads the rotated coordinates of dot x / 2.
    const uint16_t hreso = GENERATE(uint16_t{2}, uint16_t{3});
    CAPTURE(hreso);
    RunFeatureScene([=](bool feature) {
        return Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | hreso));
            SetupRBG0(w, s, {}, rng);
            // TVMR bit 0: 8-bit framebuffer; bit 1: rotation
            SetupSprites(w, {.type = 8, .tvmr = static_cast<uint16_t>(feature ? 3 : 1)}, rng);
        });
    });
}

TEST_CASE("Scene: bitmap NBG line scroll and vertical cell scroll", "[gpu][vdp2][scene]") {
    // Bitmap NBGs: in double density deinterlaced, the alternate line is one vertical increment further only without
    // line scroll Y (sw:4625-4628); vertical cell scroll applies to bitmaps too (sw:4630-4678), here reduced by 1.5.
    // 0: line scroll X and Y, double density deinterlaced; 1: line scroll X only, double density deinterlaced;
    // 2: vertical cell scroll with a delay, progressive.
    const uint32_t variant = GENERATE(0u, 1u, 2u);
    CAPTURE(variant);
    RunFeatureScene([=](bool feature) {
        return Make(
            [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                Base(w, s, rng, static_cast<uint16_t>(kDisp | (variant < 2 ? kLSMDDouble : 0)));
                SetupNBG(w, s, {.index = 0, .format = 1, .bitmap = true, .bitmapSize = 1}, rng);
                w.Reg(CYCA0L, 0x4444);
                w.Reg(CYCA0U, 0x4444);
                w.Reg(ZMYIN0, 0);
                w.Reg(ZMYDN0, 0xA000);
                if (variant < 2) {
                    FillVRAM(w, kLineScrollAddress, 0x2000, rng);
                    w.Reg(LSTA0U, static_cast<uint16_t>(kLineScrollAddress >> 17));
                    w.Reg(LSTA0L, static_cast<uint16_t>((kLineScrollAddress >> 1) & 0xFFFF));
                    if (feature) {
                        w.Reg(SCRCTL, variant == 0 ? 0x0006 : 0x0002);
                    }
                } else {
                    constexpr uint32_t kVCellAddress = 0x7D000;
                    FillVRAM(w, kVCellAddress, 0x800, rng);
                    w.Reg(CYCB1L, 0xFFFC); // vertical cell scroll at T3: repeat and delay
                    w.Reg(VCSTAU, static_cast<uint16_t>(kVCellAddress >> 17));
                    w.Reg(VCSTAL, static_cast<uint16_t>((kVCellAddress >> 1) & 0xFFFE));
                    w.Reg(ZMXIN0, 1);
                    w.Reg(ZMXDN0, 0x8000);
                    if (feature) {
                        w.Reg(SCRCTL, 0x0001);
                    }
                }
            },
            {.deinterlace = variant < 2}, 3);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Documented exceptions (stage 1B Rulings 17 and 18): stale or undefined software behaviour the GPU path does not
// reproduce. Each scene shows the case; it fails, as expected. Each also has a must-pass companion ("exception <id>
// is confined"): the harness and validation are clean, the scene still differs, and every differing pixel is one the
// exception describes (a mask from the scene's construction), so a broken harness or a wider difference cannot pass
// as the expected failure.

namespace {

// The companion of a documented exception: see above. `allowed(x, y)` is the exception's mask.
SceneResult CheckExceptionConfined(gpu::VulkanDevice &device, const Vdp2Scene &scene,
                                   const std::function<bool(uint32_t x, uint32_t y)> &allowed,
                                   const RenderBothOptions &options = {}) {
    SceneResult result = RenderBoth(scene, device, options);
    INFO(result.Describe());
    CHECK(result.error.empty());
    if (!result.error.empty()) {
        return result;
    }
    CHECK(result.distinctColors > 1);
    CHECK(result.diffCount > 0); // the exception still shows (its [!shouldfail] scene fails)
    uint32_t outside = 0, firstX = 0, firstY = 0;
    for (uint32_t y = 0; y < result.height; ++y) {
        for (uint32_t x = 0; x < result.width; ++x) {
            const size_t i = static_cast<size_t>(y) * result.width + x;
            if ((result.software[i] & 0xFFFFFF) != (result.gpu[i] & 0xFFFFFF) && !allowed(x, y) && outside++ == 0) {
                firstX = x;
                firstY = y;
            }
        }
    }
    INFO(outside << " differing pixels outside the exception's mask; first at (" << firstX << ", " << firstY << ")");
    CHECK(outside == 0);
    return result;
}

void RunExceptionConfined(const Vdp2Scene &scene, const std::function<bool(uint32_t x, uint32_t y)> &allowed,
                          const RenderBothOptions &options = {}) {
    auto device = OpenDevice();
    CheckExceptionConfined(*device, scene, allowed, options);
    CloseAndCheckValidation(device);
}

// W2: hi-res RBG0 under W0 over pixels 101-404 (odd start and end: hi-res windows use bit 0) of rows 20-180.
Vdp2Scene ExceptionW2Scene() {
    return Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, static_cast<uint16_t>(kDisp | 2));
        SetupRBG0(w, s, {}, rng);
        w.Reg(WPSX0, 101);
        w.Reg(WPEX0, 404);
        w.Reg(WPSY0, 20);
        w.Reg(WPEY0, 180);
        w.Reg(WCTLC, 0x0002); // RBG0: W0 inside
    });
}

} // namespace

// W2: in hi-res modes, when only one pixel of an RBG dot's pair is inside a window, software leaves that pixel's layer
// output from the previous line drawn (sw:4763-4779, 4854-4870 set only the unwindowed one); the GPU makes it
// transparent.
TEST_CASE("Scene: exception W2: hi-res RBG0 with one pixel of a pair windowed", "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(ExceptionW2Scene());
}

TEST_CASE("Scene: exception W2 is confined to the windowed pixel of each split pair", "[gpu][vdp2][scene]") {
    // The split pairs are (100, 101) and (404, 405): pixels 101 and 404 are windowed, on the window's rows.
    RunExceptionConfined(ExceptionW2Scene(), [](uint32_t x, uint32_t y) {
        return (x == 101 || x == 404) && y >= 20 && y <= 180;
    });
}

// W3: a vertical mosaic repeat with no line of the NBG drawn yet in the frame (here NBG0 is off at line 0 while the
// mosaic counter advances) reuses the layer output of the previous frame's last drawn line in software
// (sw:2889-2892); the GPU's layer images do not survive frames, so the GPU draws the line.
namespace {

Vdp2Scene ExceptionW3Scene() {
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            w.Reg(MZCTL, 0x3001); // NBG0, 1 x 4
        },
        {}, 3);
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            w.Reg(BGON, 0x0002); // NBG1 only
        } else if (y == 1) {
            w.Reg(BGON, 0x0003);
            w.Reg(SCXIN0, 0);
        } else if (y == 150) {
            w.Reg(SCXIN0, 40); // the last lines drawn differ from the first ones
        }
    };
    return scene;
}

} // namespace

TEST_CASE("Scene: exception W3: vertical mosaic repeating a line of the previous frame",
          "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(ExceptionW3Scene());
}

TEST_CASE("Scene: exception W3 is confined to the repeats of the first mosaic block", "[gpu][vdp2][scene]") {
    // Line 0 (mosaic counter 0) does not draw NBG0; lines 1-3 repeat it (counter 1-3 of the 4-line block); line 4
    // starts a block that is drawn.
    RunExceptionConfined(ExceptionW3Scene(), [](uint32_t, uint32_t y) { return y >= 1 && y <= 3; });
}

// W4: on lines prepared with the display latch off (the display enabled mid-frame), software does not reset the
// fetchers' lastCharIndex and lastCellX (sw:2518-2539), so with a character pattern delay a line can start without a
// character fetch and show the previous line's current character; the GPU starts every line with a fetch.
namespace {

Vdp2Scene ExceptionW4Scene() {
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            SetupNBG(w, s, {.index = 1, .format = 0}, rng);
            for (uint32_t reg : {CYCA0L, CYCA1L, CYCB0L, CYCB1L}) {
                w.Reg(reg, 0x5FFF); // CP1 at T0, PN1 at T7: delayed characters
                w.Reg(reg + 2, 0xFFF1);
            }
            // 409/256 dots per pixel: the extra tile is 511.25 dots from the first pixel, in the same cell column
            w.Reg(ZMXIN1, 1);
            w.Reg(ZMXDN1, 0x9900);
            w.Reg(SCXIN1, 3);
        },
        {}, 3);
    scene.onLine = [frame = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            ++*frame;
        }
        if (*frame == 2 && y == 200) {
            w.Reg(TVMD, 0);
        } else if (*frame == 3 && y == 100) {
            w.Reg(TVMD, kDisp);
        }
    };
    return scene;
}

} // namespace

TEST_CASE("Scene: exception W4: character pattern delay on display-latch-off lines",
          "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(ExceptionW4Scene());
}

TEST_CASE("Scene: exception W4 is confined to the first character of display-latch-off lines", "[gpu][vdp2][scene]") {
    // The display comes on at line 100 of the last frame with the latch off. The difference is the line's first
    // character run: 8 dots from scroll X 3 at 409/256 dots per pixel end before pixel 4 (dot 9.4).
    RunExceptionConfined(ExceptionW4Scene(), [](uint32_t x, uint32_t y) { return y >= 100 && x < 4; });
}

// W5: a character is fetched only when the character index changes (sw:5194). When a window hides 512 dots or more of
// a line, the next fetch can have the same index on another page, and software keeps the earlier page's character for
// that run; without a delay (and without vertical cell scroll on 2x2 characters) the GPU shows the character at the
// pixel's own position: the NBG fetch pass, which replays software's fetcher with windows, runs only for the NBGs that
// need it otherwise. On the delayed path the pass matches software inside the line, but when such a window reaches the
// right edge the front half, which does not know windows, decodes the extra tile's character for nextChar, while
// software keeps the earlier page's, so the next line's left edge can differ (and the saved nextChar).
namespace {

Vdp2Scene ExceptionW5Scene() {
    return Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, static_cast<uint16_t>(kDisp | 3)); // 704 wide
        CyclesSingle(w, 0);
        SetupNBG(w, s, {.index = 0, .format = 0}, rng);
        // Plane B on another page with other pattern names
        constexpr uint32_t kPlaneB = kPageBase + 0x8000 * 4;
        const uint32_t mapA = kPageBase >> 14, mapB = kPlaneB >> 14;
        w.Reg(MPABN0, static_cast<uint16_t>(((mapB & 0x3F) << 8) | (mapA & 0x3F)));
        for (uint32_t i = 0; i < 64 * 64; ++i) {
            w.VRAM16(kPlaneB + i * 4, static_cast<uint16_t>(Rand(rng, 128)));
            w.VRAM16(kPlaneB + i * 4 + 2, static_cast<uint16_t>(Rand(rng, 32)));
        }
        // Pixels 100-610 hidden: pixel 99 (dot 99, plane A) and pixel 611 (dot 611, plane B) are in cell column 12
        w.Reg(WPSX0, 100);
        w.Reg(WPEX0, 610);
        w.Reg(WPSY0, 0);
        w.Reg(WPEY0, 223);
        w.Reg(WCTLA, 0x0002);
    });
}

} // namespace

TEST_CASE("Scene: exception W5: a window hiding 512 dots between two fetches of one character index",
          "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(ExceptionW5Scene());
}

TEST_CASE("Scene: exception W5 is confined to the run after the window", "[gpu][vdp2][scene]") {
    // The run after the window is pixels 611-615 (dots 611-615, cell column 76 of the line, 12 of plane B).
    RunExceptionConfined(ExceptionW5Scene(), [](uint32_t x, uint32_t) { return x >= 611 && x <= 615; });
}

namespace {

// VDP1 command table: system clipping, local coordinates, `count` random polygons (every other one a mesh when
// `meshToo`; every fourth one coloured `meshColor` when it is not 0; with `meshOnly`, every one a mesh coloured
// `meshColor`), end.
void DrawPolygons(Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t count, bool meshToo, uint16_t meshColor = 0,
                  bool meshOnly = false) {
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
        const bool mesh = meshOnly || (meshToo && (i & 1));
        const int x = static_cast<int>(Rand(rng, 280)), y = static_cast<int>(Rand(rng, 190));
        const int wd = 20 + static_cast<int>(Rand(rng, 80)), ht = 20 + static_cast<int>(Rand(rng, 60));
        // Type 0 palette data: priority, ratio, colour
        const uint32_t pri = Rand(rng, 4), ratio = Rand(rng, 8), dot = 1 + Rand(rng, 0x7FF);
        const uint16_t color = meshColor != 0 && (meshOnly || (i & 3) == 3) ? meshColor
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
namespace {

Vdp2Scene ExceptionU2Scene() {
    return Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, static_cast<uint16_t>(kDisp | 4));
        CyclesSingle(w, 0);
        SetupNBG(w, s, {.index = 0, .format = 0, .priority = 3}, rng);
        SetupSprites(w, {.type = 0}, rng);
    });
}

} // namespace

TEST_CASE("Scene: exception U2: sprites in exclusive monitor mode", "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(ExceptionU2Scene());
}

TEST_CASE("Scene: exception U2 is confined to the sprite readout", "[gpu][vdp2][scene]") {
    // Software draws framebuffer pixel x / 2 at even x and nothing at odd x; the GPU draws framebuffer pixel x at x. So
    // the difference is every odd x, and the even x whose framebuffer words at x and x / 2 differ.
    const Vdp2Scene scene = ExceptionU2Scene();
    std::vector<uint8_t> fb;
    {
        auto state = std::make_unique<ymir::vdp::VDPState>();
        ymir::vdp::config::VDP2DebugRender debug{};
        ymir::vdp::config::VDP2AccessPatternsConfig access{};
        auto renderer = std::make_unique<ymir::vdp::SoftwareVDPRenderer>(*state, debug, access);
        Vdp2SceneWriter writer;
        writer.Attach(*state, *renderer);
        scene.setup(writer);
        const auto &display = state->mem1.FBRAM[state->fbIndex.display & 1];
        fb.assign(display.begin(), display.end());
    }
    const auto word = [&](uint32_t x, uint32_t y) {
        const size_t address = ((static_cast<size_t>(y) * 512 + x) * 2) & (fb.size() - 2);
        return static_cast<uint16_t>((fb[address] << 8) | fb[address + 1]);
    };
    RunExceptionConfined(scene, [&](uint32_t x, uint32_t y) { return (x & 1) != 0 || word(x, y) != word(x >> 1, y); });
}

// Documented exception X1 (stage 1B ruling 31): with an exclusive monitor mode, double-density interlace and deinterlacing
// on, software draws the alternate field's line as well (sw:560-565, VDP2GetY at sw:5666-5671 returns the same row y for
// both fields in exclusive modes), so the visible row y is the alternate line (one scrollIncV further in double density,
// plus the alternate-field sprite framebuffer) and overwrites the field line. The GPU never draws the alternate field in
// exclusive modes (vdp2_front.cpp, vulkan_vdp_renderer.cpp), so row y keeps the field line. Alternate-field output into
// the same row is not hardware behaviour anyone relies on, and no known game uses exclusive monitor modes. The scene
// is NBG only, so U2's sprite difference does not mask the reason.
namespace {

Vdp2Scene ExceptionX1Scene(bool deinterlace) {
    return Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | 4 | kLSMDDouble));
            CyclesSingle(w, 0);
            SetupNBG(w, s, {.index = 0, .format = 0, .priority = 3}, rng);
        },
        {.deinterlace = deinterlace}, 3);
}

} // namespace

TEST_CASE("Scene: exception X1: exclusive monitor with interlace and deinterlacing (software overwrites row y with the "
          "alternate line)",
          "[gpu][vdp2][scene][!shouldfail]") {
    RunScene(ExceptionX1Scene(true));
}

TEST_CASE("Scene: exception X1 is confined to the alternate line", "[gpu][vdp2][scene]") {
    // The difference covers the whole NBG (every row is the other line), so the mask is the frame; what pins it is
    // that the GPU's rows are exactly the field lines: the same scene without deinterlacing, whose last frame is an
    // even field (no alternate line, no odd-field scroll offset), is software's frame with the field lines.
    auto device = OpenDevice();
    const SceneResult result = CheckExceptionConfined(*device, ExceptionX1Scene(true), [](uint32_t, uint32_t) {
        return true;
    });
    if (result.error.empty()) {
        const std::vector<uint32_t> fieldLines = RenderSoftware(ExceptionX1Scene(false));
        REQUIRE(fieldLines.size() == result.gpu.size());
        uint32_t differ = 0;
        for (size_t i = 0; i < fieldLines.size(); ++i) {
            differ += (fieldLines[i] & 0xFFFFFF) != (result.gpu[i] & 0xFFFFFF) ? 1 : 0;
        }
        CHECK(differ == 0);
    }
    CloseAndCheckValidation(device);
}

// X1's control: the same exclusive monitor double-density interlaced scene without deinterlacing (and a progressive
// exclusive one) matches software, so X1 is the alternate-field draw and nothing else.
TEST_CASE("Scene: exclusive monitor with interlace, deinterlacing off", "[gpu][vdp2][scene]") {
    const uint16_t lsmd = GENERATE(uint16_t{0}, kLSMDSingle, kLSMDDouble);
    CAPTURE(lsmd);
    RunScene(Make(
        [lsmd](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | 4 | lsmd));
            CyclesSingle(w, 0);
            SetupNBG(w, s, {.index = 0, .format = 0, .priority = 3}, rng);
        },
        {.deinterlace = false}, 3));
}

// =====================================================================================================================
// Compose, sprite and line colour parity with the software renderer (stage 1B Task 5). Each scene was a divergence of
// the ported Ymir shaders, by its letter from stage1b-port-inventory.md section 4.1 ((a)-(o)) or the stage 1B Task 3
// and 4 reviews ((q)-(u)); (j) and (m)-(o) never diverged and stay as guards.

namespace {

// Pixels (RGB) where the software frames of two scenes differ.
uint32_t SoftwareDifference(const Vdp2Scene &a, const Vdp2Scene &b) {
    const std::vector<uint32_t> fa = RenderSoftware(a), fb = RenderSoftware(b);
    if (fa.size() != fb.size()) {
        return UINT32_MAX;
    }
    uint32_t count = 0;
    for (size_t i = 0; i < fa.size(); ++i) {
        count += ((fa[i] ^ fb[i]) & 0xFFFFFF) != 0;
    }
    return count;
}

enum class ShadowBGs { TwoNBGs, FourNBGs, RBG0 };

// Sprites with normal shadows (type 0) or MSB shadows (type 2: SD set on opaque pixels and on transparent ones) over
// the given backgrounds and the back screen, with SDCTL `sdctl`.
Vdp2Scene ShadowScene(ShadowBGs bgs, uint16_t sdctl, bool msb) {
    return Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        switch (bgs) {
        case ShadowBGs::TwoNBGs:
            TwoNBGs(w, s, rng);
            WindowRects(w, false);
            w.Reg(WCTLA, 0x0202); // both NBGs hidden inside W0: the back screen shows there
            break;
        case ShadowBGs::FourNBGs:
            CyclesAll(w);
            SetupNBG(w, s, {.index = 0, .format = 1, .priority = 2}, rng);
            SetupNBG(w, s, {.index = 1, .format = 0, .priority = 3}, rng);
            SetupNBG(w, s, {.index = 2, .format = 1, .priority = 4}, rng);
            SetupNBG(w, s, {.index = 3, .format = 0, .priority = 5}, rng);
            break;
        case ShadowBGs::RBG0: SetupRBG0(w, s, {.priority = 1}, rng); break; // below every sprite
        }
        s.sdctl = sdctl;
        if (msb) {
            SetupSprites(w, {.type = 2, .msbPercent = 40}, rng);
        } else {
            SetupSprites(w, {.type = 0, .shadowPercent = 40}, rng);
        }
    });
}

// The scene matches software, and (with any SDCTL bit set) its shadows reach the software frame.
void RunShadowScene(ShadowBGs bgs, uint16_t sdctl, bool msb) {
    RunScene(ShadowScene(bgs, sdctl, msb));
    if (sdctl != 0) {
        CHECK(SoftwareDifference(ShadowScene(bgs, sdctl, msb), ShadowScene(bgs, 0, msb)) > 0);
    }
}

} // namespace

TEST_CASE("Scene: (a) per-layer shadow enable over NBGs and the back screen", "[gpu][vdp2][scene][lavapipe]") {
    // SDCTL: bit 0 NBG0, bit 1 NBG1, bit 5 back screen.
    const uint16_t sdctl = GENERATE(uint16_t{0x00}, uint16_t{0x01}, uint16_t{0x02}, uint16_t{0x20}, uint16_t{0x23});
    const bool msb = GENERATE(false, true);
    CAPTURE(sdctl, msb);
    RunShadowScene(ShadowBGs::TwoNBGs, sdctl, msb);
}

TEST_CASE("Scene: (a) per-layer shadow enable over NBG2 and NBG3", "[gpu][vdp2][scene]") {
    // SDCTL: bit 2 NBG2, bit 3 NBG3.
    const uint16_t sdctl = GENERATE(uint16_t{0x04}, uint16_t{0x08}, uint16_t{0x0C});
    const bool msb = GENERATE(false, true);
    CAPTURE(sdctl, msb);
    RunShadowScene(ShadowBGs::FourNBGs, sdctl, msb);
}

TEST_CASE("Scene: (a) per-layer shadow enable over RBG0", "[gpu][vdp2][scene]") {
    // SDCTL: bit 4 RBG0, bit 5 back screen (RBG0 covers almost all of it; the NBG scene above covers the back screen).
    const uint16_t sdctl = GENERATE(uint16_t{0x00}, uint16_t{0x10}, uint16_t{0x30});
    const bool msb = GENERATE(false, true);
    CAPTURE(sdctl, msb);
    RunShadowScene(ShadowBGs::RBG0, sdctl, msb);
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
    // Software reads the alt mesh framebuffer for the alt-field rows only in double density (m_meshFBRAM[doubleDensity
    // && altField], master f001c11, stage 1B ruling 11), like the alt sprite framebuffer; the shader and the capture
    // follow the same rule.
    RunScene(MeshScene(kLSMDSingle, true));
}

TEST_CASE("Scene: (u) alt sprite slices after a mid-frame switch to double density", "[gpu][vdp2][scene]") {
    // VDP1 draws in double density with deinterlacing (both fields, sprites and meshes). Every frame then starts in
    // single density and switches to double density at line 100: the alt slices are read from line 100 only, so the
    // capture must not decide at line 0. No NBGs: their per-line vertical scroll across the switch is Task 6's.
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | kLSMDDouble));
            w.Reg(SPCTL, 0x0000);
            w.Reg(PRISA, 0x0706);
            w.Reg(PRISB, 0x0402);
            DrawPolygons(w, rng, 24, true);
        },
        {.deinterlace = true, .transparentMeshes = true}, 3);
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            w.Reg(TVMD, static_cast<uint16_t>(kDisp | kLSMDSingle));
        } else if (y == 100) {
            w.Reg(TVMD, static_cast<uint16_t>(kDisp | kLSMDDouble));
        }
    };
    RunScene(scene);
}

TEST_CASE("Scene: mesh colour calculation on the mesh pixel's colour MSB", "[gpu][vdp2][scene][lavapipe]") {
    // Meshes only (the normal sprite layer is transparent under them, its colour CRAM[0]), sprite colour calculation
    // condition "colour MSB": the mesh pixel's own MSB decides (master 7d1cb66, stage 1B ruling 14), with CRAM[0]'s MSB
    // set to the opposite.
    const bool meshMSB = GENERATE(false, true);
    CAPTURE(meshMSB);
    RunScene(Make(
        [meshMSB](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            w.CRAM16(0x0000, static_cast<uint16_t>((meshMSB ? 0 : 0x8000) | 0x2345));
            w.CRAM16(0x0020, static_cast<uint16_t>((meshMSB ? 0x8000 : 0) | 0x1234)); // the mesh colour, CRAM[0x10]
            w.Reg(SPCTL, 0x3000); // colour MSB condition, type 0
            w.Reg(PRISA, 0x0706);
            w.Reg(PRISB, 0x0402);
            w.Reg(CCRSA, 0x0010);
            w.Reg(CCCTL, 0x0041); // sprite and NBG0 colour calculation
            DrawPolygons(w, rng, 16, true, 0x0010, true);
        },
        {.transparentMeshes = true}));
}

// Divergence (v) (stage 1B ruling 12): with VDP1 framebuffer rotation in hi-res modes, software computes the sprite
// coordinates for HRes / 2 pixels (sw:2223-2225, 2333-2346) and the sprite layer reads them at half resolution and
// doubles each pixel (sw:2672-2727); Ymir's shader computed them at full output x.
TEST_CASE("Scene: (v) rotated sprites in hi-res modes", "[gpu][vdp2][scene]") {
    const uint16_t hreso = GENERATE(uint16_t{2}, uint16_t{3});
    CAPTURE(hreso);
    RunScene(Make([hreso](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, static_cast<uint16_t>(kDisp | hreso));
        SetupRBG0(w, s, {}, rng);
        SetupSprites(w, {.type = 0, .tvmr = 2}, rng); // TVMR bit 1: framebuffer rotation
    }));
}

TEST_CASE("Scene: (v) sprite windows in hi-res modes repeat the even pixel", "[gpu][vdp2][scene]") {
    // With the output doubled (hi-res, 16-bit framebuffer, rotated or not), software draws each even pixel, its window
    // included, and copies it to the odd one (sw:2694-2739); W0 starts and ends on odd pixels here.
    const bool rotate = GENERATE(false, true);
    CAPTURE(rotate);
    RunFeatureScene([=](bool feature) {
        return Make([=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | 2));
            SetupRBG0(w, s, {}, rng);
            SetupSprites(w, {.type = 0, .tvmr = static_cast<uint16_t>(rotate ? 2 : 0)}, rng);
            w.Reg(WPSX0, 101);
            w.Reg(WPEX0, 403);
            w.Reg(WPSY0, 20);
            w.Reg(WPEY0, 180);
            if (feature) {
                w.Reg(WCTLC, 0x0200); // sprite layer: W0 inside
            }
        });
    });
}

TEST_CASE("Scene: RBG line colours read the RGB555 table at kVDP2CRAM555TableBase", "[gpu][vdp2][scene]") {
    // The host places the RGB555 table at entry kVDP2CRAM555TableBase of the CRAM colour buffer, the shader reads it
    // at kCRAM555TableBase (vdp2_defs.hlsli). In CRAM mode 2 the two halves of the buffer hold different colours for
    // the same index, and with ratio 0 the RBG0 pixels are the line colours themselves, so any other base fails here.
    STATIC_CHECK(gpu::vdp2::kVDP2CRAM555TableBase == 2048);
    RunScene(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng, kDisp, 2);
        SetupRBG0(w, s, {.transparency = false}, rng);
        for (uint32_t y = 0; y < 512; ++y) {
            w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x800)));
        }
        w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
        w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
        s.lnclen = 0x0010; // RBG0
        w.Reg(CCCTL, 0x0010);
        w.Reg(CCRR, 0x0000);
    }));
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

TEST_CASE("VulkanVDPRenderer saves and loads the NBG fetchers as software does", "[vdp2][renderer]") {
    // Ruling 16: the renderer section's vramFetchers[field][NBG] are the front half's (character pipeline and last
    // vertical cell scroll value), in software's representation, mid-frame; a load restores them.
    namespace vdp = ymir::vdp;
    using RendererState = ymir::savestate::VDPSaveState::VDPRendererSaveState;
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto swState = std::make_unique<vdp::VDPState>();
    auto vkState = std::make_unique<vdp::VDPState>();
    auto software = std::make_unique<vdp::SoftwareVDPRenderer>(*swState, debug, access);
    auto vulkan = std::make_unique<gpu::VulkanVDPRenderer>(*vkState, debug, access);

    Vdp2SceneWriter writer;
    writer.Attach(*swState, *software);
    writer.Attach(*vkState, *vulkan);
    std::mt19937 rng{11};
    Shadow s;
    Base(writer, s, rng);
    SetupNBG(writer, s, {.index = 0, .format = 0}, rng);
    SetupNBG(writer, s, {.index = 1, .format = 1, .cell2x2 = true}, rng);
    writer.Reg(CYCA0L, 0x455F);
    writer.Reg(CYCB0U, 0x0FF1); // NBG1 characters delayed
    writer.Reg(CYCB1L, 0xFFFC); // NBG0 vertical cell scroll with repeat and delay
    FillVRAM(writer, 0x7D000, 0x800, rng);
    writer.Reg(VCSTAU, 0x0003);
    writer.Reg(VCSTAL, 0xE800);
    writer.Reg(SCRCTL, 0x0001);
    Flush(writer, s);
    writer.RunFrame();
    writer.RunFrame({}, 0, 77);

    const auto charEq = [](const RendererState::CharacterSaveState &a, const RendererState::CharacterSaveState &b) {
        return a.charNum == b.charNum && a.palNum == b.palNum && a.specColorCalc == b.specColorCalc &&
               a.specPriority == b.specPriority && a.flipH == b.flipH && a.flipV == b.flipV;
    };
    const auto fetchersEq = [&](const RendererState &a, const RendererState &b) {
        bool same = true;
        for (uint32_t field = 0; field < 2; ++field) {
            for (uint32_t nbg = 0; nbg < 4; ++nbg) {
                const auto &fa = a.vramFetchers[field][nbg];
                const auto &fb = b.vramFetchers[field][nbg];
                same &= charEq(fa.currChar, fb.currChar) && charEq(fa.nextChar, fb.nextChar) &&
                        fa.lastCharIndex == fb.lastCharIndex && fa.lastCellX == fb.lastCellX &&
                        fa.lastVCellScroll == fb.lastVCellScroll;
            }
        }
        return same;
    };
    auto swSaved = std::make_unique<RendererState>();
    auto vkSaved = std::make_unique<RendererState>();
    software->SaveState(*swSaved);
    vulkan->SaveState(*vkSaved);
    CHECK(fetchersEq(*swSaved, *vkSaved));
    CHECK(vkSaved->vramFetchers[0][0].lastVCellScroll != 0xFFFFFFFF);
    CHECK(vkSaved->vramFetchers[0][1].nextChar.charNum != 0);

    // A blank load loses them; loading the saved state restores them.
    auto blank = std::make_unique<RendererState>();
    vulkan->LoadState(*blank);
    auto lost = std::make_unique<RendererState>();
    vulkan->SaveState(*lost);
    CHECK_FALSE(fetchersEq(*lost, *vkSaved));
    vulkan->LoadState(*swSaved);
    vulkan->PostLoadStateSync();
    auto restored = std::make_unique<RendererState>();
    vulkan->SaveState(*restored);
    CHECK(fetchersEq(*restored, *swSaved));
}

// =====================================================================================================================
// Weave across frame-context changes (Task 7): with deinterlacing off on interlaced modes each frame writes one field
// and the composite keeps the other, as the software framebuffer does. Every frame is compared.

namespace {

// NBG0 over sprites, scrolled by a different amount every frame, so the kept field differs from the new one.
Vdp2Scene WeaveScene(uint16_t lsmd, uint32_t frames) {
    Vdp2Scene scene = ResolutionScene(static_cast<uint16_t>(lsmd | 1), false);
    scene.frames = frames;
    auto frame = std::make_shared<uint32_t>(0);
    scene.onLine = [frame](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            w.Reg(SCXIN0, static_cast<uint16_t>(*frame * 13));
            ++*frame;
        }
    };
    return scene;
}

} // namespace

TEST_CASE("Scene: weave keeps the other field when the frame contexts grow", "[gpu][vdp2][scene][lavapipe]") {
    const uint16_t lsmd = GENERATE(kLSMDSingle, kLSMDDouble);
    CAPTURE(lsmd);
    // 1 sync slot + 2 hidden contexts, growing to 3 + 2 before frame 4 (a larger sync index mask), as VulkanOutput
    // does: the back half is kept (the composite holds the other field), the hidden contexts move up.
    RenderBothOptions options;
    options.frameContexts = 3;
    options.contextForFrame = [](uint32_t frame) { return frame < 4 ? 0u : frame % 3; };
    options.beforeFrame = [](uint32_t frame, gpu::VulkanVDPRenderer &renderer, gpu::VulkanDevice &) {
        if (frame == 4) {
            REQUIRE(renderer.EnsureFrameContexts(5));
            // With VDP1 on the GPU the back halves have one more context, the sync submits'
            REQUIRE(renderer.Backend()->FrameContexts() == 5 + (renderer.GpuVdp1() ? 1u : 0u));
        }
    };
    options.compareFrame = [](uint32_t) { return true; };
    const Vdp2Scene scene = WeaveScene(lsmd, 8);
    auto device = OpenDevice();
    const SceneResult result = Check(*device, scene, options);
    CHECK(result.comparedFrames == 8);
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: weave across hidden frames recorded into hidden contexts", "[gpu][vdp2][scene][lavapipe]") {
    const uint16_t lsmd = GENERATE(kLSMDSingle, kLSMDDouble);
    CAPTURE(lsmd);
    // 2 sync slots + 2 hidden contexts; odd frames are hidden (video disabled: recorded into a hidden context, not
    // presented), shown frames weave against the hidden frames' fields.
    RenderBothOptions options;
    options.frameContexts = 4;
    options.contextForFrame = [](uint32_t frame) { return frame % 2 == 0 ? (frame / 2) % 2 : 2 + (frame / 2) % 2; };
    options.compareFrame = [](uint32_t frame) { return frame % 2 == 0; };
    auto device = OpenDevice();
    const SceneResult result = Check(*device, WeaveScene(lsmd, 10), options);
    CHECK(result.comparedFrames == 5);
    CloseAndCheckValidation(device);
}

// Pre-approved operational exception (Ruling 29): a context reset with deinterlacing off clears the other field for one
// frame. The new back half starts with a cleared composite, so the first weave frame after the attach has black rows
// where software still shows the field of the frame before; that frame's own field and every later frame match. (The
// BIOS menu, the content of the vkhost checks, is progressive, so this is a scene.)
TEST_CASE("Scene: operational exception: context reset with deinterlacing off clears the other field for one frame",
          "[gpu][vdp2][scene][lavapipe]") {
    const uint16_t lsmd = GENERATE(kLSMDSingle, kLSMDDouble);
    CAPTURE(lsmd);
    static constexpr uint32_t kAttachFrame = 3;
    RenderBothOptions options;
    options.beforeFrame = [](uint32_t frame, gpu::VulkanVDPRenderer &renderer, gpu::VulkanDevice &dev) {
        if (frame == kAttachFrame) {
            // context_destroy then context_reset between two shown frames
            renderer.DetachDevice();
            renderer.AttachDevice(dev, 2);
        }
    };
    auto device = OpenDevice();

    // The first frame after the attach: one field identical, the other black on the GPU and not black in software.
    const SceneResult first = RenderBoth(WeaveScene(lsmd, kAttachFrame + 1), *device, options);
    INFO(first.Describe());
    REQUIRE(first.error.empty());
    REQUIRE(first.height == 448);
    std::array<uint32_t, 2> sameRows{}, clearedRows{}, softwareBlackRows{};
    for (uint32_t y = 0; y < first.height; ++y) {
        bool same = true, gpuBlack = true, swBlack = true;
        for (uint32_t x = 0; x < first.width; ++x) {
            const uint32_t sw = first.software[y * first.width + x] & 0xFFFFFF;
            const uint32_t gpu = first.gpu[y * first.width + x] & 0xFFFFFF;
            same &= sw == gpu;
            gpuBlack &= gpu == 0;
            swBlack &= sw == 0;
        }
        sameRows[y & 1] += same;
        clearedRows[y & 1] += gpuBlack;
        softwareBlackRows[y & 1] += swBlack;
    }
    const uint32_t fieldRows = first.height / 2;
    const uint32_t written = sameRows[0] == fieldRows ? 0 : 1; // the field the frame rendered
    CHECK(sameRows[written] == fieldRows);
    CHECK(clearedRows[written ^ 1] == fieldRows);   // exactly the other field is cleared...
    CHECK(softwareBlackRows[written ^ 1] == 0);     // ...where software still shows the previous frame's field
    CHECK(sameRows[written ^ 1] == 0);

    // Every other frame, before and after, is identical to software.
    options.compareFrame = [](uint32_t frame) { return frame != kAttachFrame; };
    const SceneResult rest = RenderBoth(WeaveScene(lsmd, kAttachFrame + 3), *device, options);
    INFO(rest.Describe());
    CHECK(rest.error.empty());
    CHECK(rest.comparedFrames == kAttachFrame + 2);
    CHECK(rest.identical);
    CloseAndCheckValidation(device);
}

// =====================================================================================================================
// Internal resolution (stage 1C Tasks 3 and 4): the background layers and the composite at N x, each background sample
// evaluated at its own sub-pixel position (Task 4, vdp2_subpixel_reference.hpp), so the N x composite equals software's
// frame at the sample points (N x, N y) outside the excluded lattice (CompareSamplePoints), and is exactly a nearest
// upscale of it for scenes without sub-pixel features (CompareNearest). With BRIMIR_VDP2_SCALE=N every scene above
// runs at N, compared at the sample points; these cases pin N.

namespace {

using brimir::test::CompareNearest;
using brimir::test::CompareNearestPixels;
using brimir::test::CompareSamplePointPixels;
using brimir::test::CompareSamplePoints;
using brimir::test::SampleLatticeRow;
namespace sp = brimir::test::subpixel;

constexpr uint32_t SCXDN1 = 0x082, RPRCTL = 0x0B2;

// RBG0 with per-dot coefficients and the line colour screen (coefficient line colour data), in TVMD mode `tvmd`.
Vdp2Scene RBGLineColorScene(uint16_t tvmd, bool deinterlace) {
    return Make(
        [tvmd](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | tvmd), 1);
            SetupRBG0(w, s, {.coeffTable = true, .coeffLineColor = true}, rng);
            for (uint32_t y = 0; y < 512; ++y) {
                w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x800)));
            }
            w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
            w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
            s.lnclen = 0x0010;
            w.Reg(CCCTL, 0x0030);
            w.Reg(CCRR, 0x0008);
        },
        {.deinterlace = deinterlace});
}

// Scenes without a sub-pixel feature: NBGs unzoomed with integer scroll, sprites, windows, colour calculation,
// gradation, line colour, shadow, meshes, colour offset, mosaic (NBG mosaic wider and taller than one and RBG mosaic
// are native at N, also zoomed or rotated; a vertical-only NBG mosaic keeps its horizontal sub-pixels, a coordinate
// scene) and NBGs using the character table (character pattern delay, vertical cell scroll on 2x2 characters; native
// at N, also zoomed). Every sub-pixel of their N x frames shows its native pixel.
std::vector<std::pair<std::string, Vdp2Scene>> NearestScenes() {
    std::vector<std::pair<std::string, Vdp2Scene>> scenes;
    scenes.emplace_back("NBGs, sprites, colour offset", Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                            Base(w, s, rng);
                            TwoNBGs(w, s, rng);
                            SetupSprites(w, {.type = 1}, rng);
                            w.Reg(CLOFEN, 0x0021);
                            w.Reg(COAR, 0x0040);
                        }));
    scenes.emplace_back("colour gradation", Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                            Base(w, s, rng);
                            TwoNBGs(w, s, rng);
                            SetupSprites(w, {.type = 1}, rng);
                            w.Reg(CCCTL, static_cast<uint16_t>(0x8000 | (0 << 12) | 0x0043));
                            w.Reg(CCRNA, 0x0A14);
                        }));
    scenes.emplace_back("windows", Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                            Base(w, s, rng);
                            TwoNBGs(w, s, rng);
                            WindowRects(w, false);
                            w.Reg(WCTLA, 0x008B);
                        }));
    scenes.emplace_back("line colour screen", Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                            Base(w, s, rng, kDisp, 1);
                            TwoNBGs(w, s, rng);
                            BackPerLine(w, rng);
                            for (uint32_t y = 0; y < 512; ++y) {
                                w.VRAM16(kLineColorAddress + y * 2, static_cast<uint16_t>(Rand(rng, 0x800)));
                            }
                            w.Reg(LCTAU, static_cast<uint16_t>(0x8000 | (kLineColorAddress >> 17)));
                            w.Reg(LCTAL, static_cast<uint16_t>((kLineColorAddress >> 1) & 0xFFFF));
                            s.lnclen = 0x0003;
                            w.Reg(CCCTL, 0x0023);
                            w.Reg(CCRLB, 0x0C08);
                        }));
    scenes.emplace_back("transparent meshes", MeshScene(0, false));
    {
        Vdp2Scene mosaic = Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            w.Reg(MZCTL, 0x3203); // NBG0 and NBG1, 3 pixels wide, 4 lines high
            // Zoomed with a fractional scroll: native on both axes under a 3x4 mosaic
            w.Reg(ZMXDN0, 0xC000);
            w.Reg(SCXDN0, 0x8000);
            w.Reg(ZMYDN0, 0xA000);
        });
        mosaic.onLine = [](Vdp2SceneWriter &w, uint32_t y) {
            w.Reg(SCXIN0, static_cast<uint16_t>(y >= 102 ? 9 : 0)); // a new batch at line 102, inside a block
        };
        scenes.emplace_back("zoomed NBG mosaic across a batch split", mosaic);
    }
    scenes.emplace_back("rotated RBG0 mosaic", Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                            Base(w, s, rng);
                            SetupRBG0(w, s, {}, rng);
                            w.Reg(MZCTL, 0x0310); // RBG0, 4 pixels wide
                        }));
    scenes.emplace_back("delayed NBG under a window", DelayedNBG1Scene(true, true, [](Vdp2SceneWriter &w, Shadow &,
                                                                                       std::mt19937 &) {
                            WindowRects(w, false);
                            w.Reg(WCTLA, 0x0200);
                        }));
    scenes.emplace_back("delayed NBG zoomed with a fractional scroll",
                        DelayedNBG1Scene(true, false, [](Vdp2SceneWriter &w, Shadow &, std::mt19937 &) {
                            w.Reg(ZMXDN1, 0xC000);
                            w.Reg(SCXDN1, 0x8000);
                            w.Reg(ZMYDN1, 0xA000);
                        }));
    scenes.emplace_back("vertical cell scroll on zoomed 2x2 characters",
                        Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                            Base(w, s, rng);
                            SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = true}, rng);
                            w.Reg(CYCA0L, 0xFF55);
                            w.Reg(CYCA0U, 0xFFFF);
                            w.Reg(CYCB0L, 0xF1FF);
                            w.Reg(CYCB0U, 0xFFFF);
                            w.Reg(CYCB1L, 0xFFDF);
                            w.Reg(CYCB1U, 0xFFFF);
                            constexpr uint32_t kVCellAddress = 0x7D000;
                            FillVRAM(w, kVCellAddress, 0x800, rng);
                            w.Reg(VCSTAU, static_cast<uint16_t>(kVCellAddress >> 17));
                            w.Reg(VCSTAL, static_cast<uint16_t>((kVCellAddress >> 1) & 0xFFFE));
                            w.Reg(SCRCTL, 0x0100);
                            w.Reg(ZMXDN1, 0xC000);
                            w.Reg(SCXIN1, 13);
                        }));
    scenes.emplace_back("hi-res double density deinterlaced",
                        ResolutionScene(static_cast<uint16_t>(3 | kLSMDDouble), true));
    scenes.emplace_back("single density deinterlaced", ResolutionScene(kLSMDSingle, true));
    scenes.emplace_back("sprite MSB shadow", ShadowScene(ShadowBGs::TwoNBGs, 0x23, true));
    return scenes;
}

// A cross-section with sub-pixel features, compared at the sample points: the nearest scenes, plus zoomed and finely
// scrolled NBGs (vertical cell scroll, line scroll in double density), rotated RBG0 with coefficients and the line
// colour screen in progressive, hi-res and deinterlaced double-density modes (the first sub-pixel of each pair's
// second column or row stores the native line colour), and rotation parameter windows.
std::vector<std::pair<std::string, Vdp2Scene>> SamplePointScenes() {
    std::vector<std::pair<std::string, Vdp2Scene>> scenes = NearestScenes();
    scenes.emplace_back("NBG zoom and fractional scroll", Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                            Base(w, s, rng);
                            TwoNBGs(w, s, rng);
                            SetupSprites(w, {.type = 1}, rng);
                            w.Reg(SCXDN0, 0x4500);
                            w.Reg(SCYDN0, 0x8000);
                            w.Reg(ZMXDN0, 0xC000);
                            w.Reg(ZMYDN0, 0xA000);
                            w.Reg(ZMXIN1, 1);
                            w.Reg(ZMXDN1, 0x8000);
                            w.Reg(ZMCTL, 0x0100);
                        }));
    scenes.emplace_back("vertical cell scroll with zoom", Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                            Base(w, s, rng);
                            SetupNBG(w, s, {.index = 0, .format = 0}, rng);
                            w.Reg(CYCA0L, 0x4444);
                            w.Reg(CYCA0U, 0x4444);
                            w.Reg(CYCB0L, 0x0CFF);
                            w.Reg(CYCB0U, 0xFFFF);
                            constexpr uint32_t kVCellAddress = 0x7D000;
                            FillVRAM(w, kVCellAddress, 0x400, rng);
                            w.Reg(VCSTAU, static_cast<uint16_t>(kVCellAddress >> 17));
                            w.Reg(VCSTAL, static_cast<uint16_t>((kVCellAddress >> 1) & 0xFFFE));
                            w.Reg(SCRCTL, 0x0001);
                            w.Reg(ZMXDN0, 0xC000);
                            w.Reg(SCXDN0, 0x6000);
                        }));
    scenes.emplace_back("zoomed line scroll in double density",
                        Make(
                            [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                                Base(w, s, rng, static_cast<uint16_t>(kDisp | kLSMDDouble));
                                CyclesSingle(w, 0);
                                SetupNBG(w, s, {.index = 0, .format = 1}, rng);
                                FillVRAM(w, kLineScrollAddress, 0x2000, rng);
                                w.Reg(LSTA0U, static_cast<uint16_t>(kLineScrollAddress >> 17));
                                w.Reg(LSTA0L, static_cast<uint16_t>((kLineScrollAddress >> 1) & 0xFFFF));
                                w.Reg(SCRCTL, 0x0016); // N0LSS = 1, N0LSCY, N0LSCX
                                w.Reg(ZMXDN0, 0xC000);
                                w.Reg(ZMYDN0, 0xA000);
                            },
                            {.deinterlace = true}, 2));
    scenes.emplace_back("RBG0 coefficients and line colour", RBGLineColorScene(0, false));
    scenes.emplace_back("hi-res RBG0 coefficients and line colour", RBGLineColorScene(2, false));
    scenes.emplace_back("double density RBG0 coefficients and line colour, deinterlaced",
                        RBGLineColorScene(kLSMDDouble, true));
    scenes.emplace_back("double density RBG0 coefficients and line colour, woven",
                        RBGLineColorScene(kLSMDDouble, false));
    scenes.emplace_back("RBG0 rotation parameter windows", Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                            Base(w, s, rng);
                            SetupRBG0(w, s, {.rpmd = 3}, rng);
                            WindowRects(w, false);
                            w.Reg(WCTLD, 0x008B);
                        }));
    return scenes;
}

// The N x frame (n * width x n * height) has blocks that are not one colour (it is not a nearest upscale).
uint64_t NonConstantBlocks(const std::vector<uint32_t> &scaled, uint32_t width, uint32_t height, uint32_t n) {
    uint64_t count = 0;
    if (scaled.size() != static_cast<size_t>(width) * height * n * n) {
        return 0;
    }
    const uint32_t pitch = width * n;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t first = scaled[static_cast<size_t>(y * n) * pitch + x * n] & 0xFFFFFF;
            bool constant = true;
            for (uint32_t j = 0; j < n && constant; ++j) {
                for (uint32_t i = 0; i < n && constant; ++i) {
                    constant = (scaled[static_cast<size_t>(y * n + j) * pitch + x * n + i] & 0xFFFFFF) == first;
                }
            }
            count += constant ? 0 : 1;
        }
    }
    return count;
}

} // namespace

TEST_CASE("Scene: internal resolution N is a nearest upscale of software without sub-pixel features",
          "[gpu][vdp2][scene][scale][lavapipe]") {
    const uint32_t n = GENERATE(2u, 3u, 4u);
    auto device = OpenDevice();
    for (const auto &[name, scene] : NearestScenes()) {
        CAPTURE(n, name);
        const SceneResult result = CompareNearest(scene, *device, n);
        INFO(result.Describe());
        CHECK(result.error.empty());
        CHECK(result.scale == n);
        CHECK(result.comparedPixels == uint64_t{result.width} * result.height * n * n);
        CHECK(result.identical);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: internal resolution N matches software at the sample points", "[gpu][vdp2][scene][scale]") {
    const uint32_t n = GENERATE(2u, 3u, 4u);
    auto device = OpenDevice();
    for (const auto &[name, scene] : SamplePointScenes()) {
        CAPTURE(n, name);
        const SceneResult result = CompareSamplePoints(scene, *device, n);
        INFO(result.Describe());
        CHECK(result.error.empty());
        CHECK(result.scale == n);
        CHECK(result.comparedPixels > 0);
        CHECK(result.comparedPixels + result.skippedPixels == uint64_t{result.width} * result.height);
        CHECK(result.identical);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: internal resolution N with poisoned images", "[gpu][vdp2][scene][scale]") {
    // Every texel compose or a vertical mosaic load reads at N was written by the frame (both layer images, the
    // native ones and the character table start as 0xA5 every frame): every pixel of the nearest scenes, the sample
    // points of the others (the coordinate-bitmap scenes check every pixel with poison too).
    const uint32_t n = GENERATE(2u, 4u);
    auto device = OpenDevice();
    RenderBothOptions options;
    options.poisonImages = true;
    for (const auto &[name, scene] : NearestScenes()) {
        CAPTURE(n, name);
        const SceneResult result = CompareNearest(scene, *device, n, options);
        INFO(result.Describe());
        CHECK(result.error.empty());
        CHECK(result.identical);
    }
    for (const auto &[name, scene] : SamplePointScenes()) {
        CAPTURE(n, name);
        const SceneResult result = CompareSamplePoints(scene, *device, n, options);
        INFO(result.Describe());
        CHECK(result.error.empty());
        CHECK(result.comparedPixels > 0);
        CHECK(result.identical);
    }
    CloseAndCheckValidation(device);
}

// ---------------------------------------------------------------------------------------------------------------------
// Coordinate bitmaps (stage 1C Task 4): an RGB555 bitmap whose texels encode their own (u, v) (u mod 128 in bits 0-6,
// v mod 256 in bits 7-14), drawn by NBG0 or RBG0 alone. Every pixel of the N x composite is decoded and compared with
// the sub-pixel reference (vdp2_subpixel_reference.hpp, int64), and software's 1x frame with the reference at 1x.

namespace {

uint16_t CoordTexel(uint32_t u, uint32_t v) {
    return static_cast<uint16_t>(0x8000 | (u & 127) | ((v & 255) << 7));
}

// The 512x256 coordinate bitmap at kCharBase (banks A0 and A1).
void WriteCoordBitmap(Vdp2SceneWriter &w) {
    for (uint32_t v = 0; v < 256; ++v) {
        for (uint32_t u = 0; u < 512; ++u) {
            w.VRAM16(kCharBase + (v * 512 + u) * 2, CoordTexel(u, v));
        }
    }
}

// The texel coordinates of a composite pixel (XBGR) that shows a coordinate texel (no colour calculation); false when
// the pixel is not one (low colour bits set).
bool DecodeCoord(uint32_t pixel, uint32_t &u, uint32_t &v) {
    const uint32_t r = pixel & 0xFF, g = (pixel >> 8) & 0xFF, b = (pixel >> 16) & 0xFF;
    if (((r | g | b) & 7) != 0) {
        return false;
    }
    const uint32_t code = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
    u = code & 127;
    v = code >> 7;
    return true;
}

using CoordExpect = std::function<sp::Dot(uint32_t n, uint32_t X, uint32_t Y)>;

struct CoordCheck {
    uint64_t pixels = 0, mismatches = 0, transparent = 0;
    uint32_t x = 0, y = 0, pixel = 0; // the first mismatch
    int64_t wantU = 0, wantV = 0;
    bool wantTransparent = false;

    std::string Describe() const {
        std::ostringstream out;
        out << pixels << " pixels decoded (" << transparent << " transparent), " << mismatches
            << " differ from the reference";
        if (mismatches != 0) {
            uint32_t u = 0, v = 0;
            const bool decoded = DecodeCoord(pixel, u, v);
            out << "; first at (" << x << ", " << y << "): reference "
                << (wantTransparent ? std::string("transparent")
                                    : "(" + std::to_string(wantU & 127) + ", " + std::to_string(wantV & 255) + ")")
                << ", frame " << (decoded ? "(" + std::to_string(u) + ", " + std::to_string(v) + ")" : "not a texel")
                << " (0x" << std::hex << (pixel & 0xFFFFFF) << ")";
        }
        return out.str();
    }
};

// Every pixel of an n x frame (n * width x n * height) against the reference. Where the reference has no texel (a
// transparent coefficient), the pixel is the 1x frame's (the back screen) at its native position.
CoordCheck CheckCoords(const std::vector<uint32_t> &frame, uint32_t width, uint32_t height, uint32_t n,
                       const CoordExpect &expect, const std::vector<uint32_t> &native) {
    CoordCheck check;
    if (frame.size() != static_cast<size_t>(width) * height * n * n ||
        native.size() != static_cast<size_t>(width) * height) {
        return check;
    }
    const uint32_t pitch = width * n;
    for (uint32_t Y = 0; Y < height * n; ++Y) {
        for (uint32_t X = 0; X < pitch; ++X) {
            const uint32_t pixel = frame[static_cast<size_t>(Y) * pitch + X];
            const sp::Dot want = expect(n, X, Y);
            uint32_t u = 0, v = 0;
            ++check.pixels;
            const bool match =
                want.transparent
                    ? (pixel & 0xFFFFFF) == (native[static_cast<size_t>(Y / n) * width + X / n] & 0xFFFFFF)
                    : DecodeCoord(pixel, u, v) && u == (want.u & 127) && v == (want.v & 255);
            if (want.transparent) {
                ++check.transparent;
            }
            if (!match) {
                if (check.mismatches++ == 0) {
                    check.x = X;
                    check.y = Y;
                    check.pixel = pixel;
                    check.wantU = want.u;
                    check.wantV = want.v;
                    check.wantTransparent = want.transparent;
                }
            }
        }
    }
    return check;
}

struct CoordScene {
    std::string name;
    Vdp2Scene scene;
    CoordExpect expect;
    // v never decreases from one N x row to the next (woven interlace: both fields from the same bases)
    bool monotoneV = false;
    // the scene has transparent coefficients (pixels the reference expects to show the back screen)
    bool transparency = false;
};

// The backward vertical steps of an n x frame: pixels (X, Y + 1) whose decoded v is below that of (X, Y) (mod 256,
// steps under half the range). `compared` counts the decoded pairs.
uint64_t BackwardSteps(const std::vector<uint32_t> &frame, uint32_t width, uint32_t height, uint32_t n,
                       uint64_t &compared) {
    uint64_t back = 0;
    compared = 0;
    if (frame.size() != static_cast<size_t>(width) * height * n * n) {
        return 0;
    }
    const uint32_t pitch = width * n;
    for (uint32_t Y = 0; Y + 1 < height * n; ++Y) {
        for (uint32_t X = 0; X < pitch; ++X) {
            uint32_t u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            if (!DecodeCoord(frame[static_cast<size_t>(Y) * pitch + X], u0, v0) ||
                !DecodeCoord(frame[static_cast<size_t>(Y + 1) * pitch + X], u1, v1)) {
                continue;
            }
            ++compared;
            const uint32_t step = (v1 - v0) & 255;
            back += step >= 128 ? 1 : 0;
        }
    }
    return back;
}

sp::Geometry CoordGeometry(uint16_t tvmd, bool deinterlace) {
    sp::Geometry g;
    g.hiRes = (tvmd & 2) != 0;
    const uint16_t lsmd = tvmd & 0x00C0;
    // Exclusive monitor modes (HRESO bit 2) never double rows: their composite rows are lines, as progressive
    g.interlace = (tvmd & 4) != 0       ? sp::Interlace::None
                  : lsmd == kLSMDDouble ? sp::Interlace::DoubleDensity
                  : lsmd == kLSMDSingle ? sp::Interlace::SingleDensity
                                        : sp::Interlace::None;
    g.deinterlace = deinterlace;
    return g;
}

// NBG0 drawing the coordinate bitmap (RGB555, 512x256, transparency off), with an 11.8 scroll and 3.8 increments.
struct CoordNBG {
    uint16_t tvmd = 0;
    bool deinterlace = false;
    uint32_t scrollX = 0x12345, scrollY = 0x04580; // 0x123.45, 0x45.80
    uint32_t incH = 0x100, incV = 0x100;
    bool lineTable = false;                // line scroll X and line zoom on every line (progressive)
    uint32_t zoomLine = UINT32_MAX;        // the horizontal increment changes to zoomAfter at this line
    uint32_t zoomAfter = 0x100;
    bool cells = false;                    // 1x1 RGB555 characters (WriteCoordCells) instead of the bitmap
    uint32_t mosaicV = 1;                  // vertical mosaic only (MZCTL), progressive
    bool vcellScroll = false;              // vertical cell scroll (CoordVCellScroll), progressive
};

// The coordinate plane in cells: 1x1 RGB555 characters (128 bytes) at kCharBase whose texels encode their plane
// position (u mod 128, v mod 256: 16 x 32 distinct characters), and one page of two-word pattern names at kPageBase,
// one plane of one page (512x512 dots) for planes A-D.
void WriteCoordCells(Vdp2SceneWriter &w) {
    for (uint32_t c = 0; c < 16 * 32; ++c) {
        const uint32_t cu = (c % 16) * 8, cv = (c / 16) * 8;
        for (uint32_t py = 0; py < 8; ++py) {
            for (uint32_t px = 0; px < 8; ++px) {
                w.VRAM16(kCharBase + c * 128 + (py * 8 + px) * 2, CoordTexel(cu + px, cv + py));
            }
        }
    }
    for (uint32_t cy = 0; cy < 64; ++cy) {
        for (uint32_t cx = 0; cx < 64; ++cx) {
            const uint32_t c = (cy % 32) * 16 + cx % 16;
            const uint32_t entry = kPageBase + (cy * 64 + cx) * 4;
            w.VRAM16(entry, 0);
            w.VRAM16(entry + 2, static_cast<uint16_t>(c * 4)); // character number in 32-byte units
        }
    }
    w.Reg(PNCN0, 0x0000); // two-word pattern names
    const uint32_t mapIndex = kPageBase >> 14;
    const uint16_t mp = static_cast<uint16_t>(((mapIndex & 0x3F) << 8) | (mapIndex & 0x3F));
    w.Reg(MPABN0, mp);
    w.Reg(MPABN0 + 2, mp);
}

// The vertical cell scroll table values (11.8) of the coordinate scenes: distinct, fractional, some negative.
constexpr uint32_t kCoordVCellAddress = 0x7D000;
int64_t CoordVCellScroll(uint32_t k) {
    return static_cast<int64_t>((k * 0x2D7u + 0x31u) % 0x6000u) - 0x1800;
}

uint32_t CoordLineScrollX(uint32_t line) {
    return (line * 0x1D3u) & 0x7FFFF;
}

uint32_t CoordLineIncH(uint32_t line) {
    return 0x90 + (line * 13u) % 0x100;
}

void WriteNBG0Increment(Vdp2SceneWriter &w, uint32_t incH) {
    w.Reg(ZMXIN0, static_cast<uint16_t>(incH >> 8));
    w.Reg(ZMXDN0, static_cast<uint16_t>((incH & 0xFF) << 8));
}

CoordScene CoordNBGScene(std::string name, CoordNBG c) {
    Vdp2Scene scene = Make(
        [c](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | c.tvmd));
            CyclesSingle(w, 0);
            // RGB555 512x256 bitmap, or 1x1 RGB555 characters
            s.chctla = static_cast<uint16_t>((s.chctla & 0xFF00) | (3 << 4) | (c.cells ? 0 : 1 << 1));
            s.bgon |= 0x0101; // transparency off
            s.prina = static_cast<uint16_t>((s.prina & 0xFF00) | 4);
            s.mpofn = static_cast<uint16_t>(s.mpofn & ~7u);
            if (c.cells) {
                WriteCoordCells(w);
            } else {
                WriteCoordBitmap(w);
            }
            if (c.mosaicV > 1) {
                w.Reg(MZCTL, static_cast<uint16_t>(((c.mosaicV - 1) << 12) | 0x0001)); // NBG0, 1 dot wide
            }
            if (c.vcellScroll) {
                for (uint32_t k = 0; k < 128; ++k) {
                    w.VRAM32(kCoordVCellAddress + k * 4, static_cast<uint32_t>(CoordVCellScroll(k) & 0x7FFFF) << 8);
                }
                w.Reg(CYCB0L, 0x0CFF); // NBG0 pattern names at T0, its vertical cell scroll table at T1
                w.Reg(VCSTAU, static_cast<uint16_t>(kCoordVCellAddress >> 17));
                w.Reg(VCSTAL, static_cast<uint16_t>((kCoordVCellAddress >> 1) & 0xFFFE));
            }
            w.Reg(SCXIN0, static_cast<uint16_t>((c.scrollX >> 8) & 0x7FF));
            w.Reg(SCXDN0, static_cast<uint16_t>((c.scrollX & 0xFF) << 8));
            w.Reg(SCYIN0, static_cast<uint16_t>((c.scrollY >> 8) & 0x7FF));
            w.Reg(SCYDN0, static_cast<uint16_t>((c.scrollY & 0xFF) << 8));
            WriteNBG0Increment(w, c.incH);
            w.Reg(ZMYIN0, static_cast<uint16_t>(c.incV >> 8));
            w.Reg(ZMYDN0, static_cast<uint16_t>((c.incV & 0xFF) << 8));
            const bool reduced = c.incH > 0x100 || c.lineTable || c.zoomAfter > 0x100;
            w.Reg(ZMCTL, static_cast<uint16_t>(reduced ? 1 : 0));
            if (c.lineTable) {
                for (uint32_t line = 0; line < 512; ++line) {
                    w.VRAM32(kLineScrollAddress + line * 8, CoordLineScrollX(line) << 8);
                    w.VRAM32(kLineScrollAddress + line * 8 + 4, CoordLineIncH(line) << 8);
                }
                w.Reg(LSTA0U, static_cast<uint16_t>(kLineScrollAddress >> 17));
                w.Reg(LSTA0L, static_cast<uint16_t>((kLineScrollAddress >> 1) & 0xFFFF));
            }
            // Line scroll X and line zoom, every line; vertical cell scroll
            w.Reg(SCRCTL, static_cast<uint16_t>((c.lineTable ? 0x000A : 0) | (c.vcellScroll ? 0x0001 : 0)));
        },
        {.deinterlace = c.deinterlace});
    if (c.zoomLine != UINT32_MAX) {
        scene.onLine = [c](Vdp2SceneWriter &w, uint32_t y) {
            if (y == 0) {
                WriteNBG0Increment(w, c.incH);
            } else if (y == c.zoomLine) {
                WriteNBG0Increment(w, c.zoomAfter);
            }
        };
    }

    sp::NBGFrame frame;
    frame.geometry = CoordGeometry(c.tvmd, c.deinterlace);
    frame.scrollX = c.scrollX;
    frame.scrollY = c.scrollY;
    frame.incH = c.incH;
    frame.incV = c.incV;
    frame.mosaicV = c.mosaicV;
    if (c.vcellScroll) {
        for (uint32_t k = 0; k < 128; ++k) {
            frame.vcellScroll.push_back(CoordVCellScroll(k));
        }
    }
    if (c.lineTable) {
        for (uint32_t line = 0; line < 512; ++line) {
            frame.lineScrollX.push_back(CoordLineScrollX(line));
            frame.lineIncH.push_back(CoordLineIncH(line));
        }
    } else if (c.zoomLine != UINT32_MAX) {
        for (uint32_t line = 0; line < 512; ++line) {
            frame.lineIncH.push_back(line < c.zoomLine ? c.incH : c.zoomAfter);
        }
    }
    CoordExpect expect = [frame](uint32_t n, uint32_t X, uint32_t Y) {
        sp::NBGFrame f = frame;
        f.geometry.n = n;
        return sp::NBGDotAt(f, X, Y);
    };
    return {std::move(name), std::move(scene), std::move(expect)};
}

// A table value as the register holds it: rounded to `one` and sign-extended from `bits` bits.
int64_t FitFixed(double value, double one, uint32_t bits) {
    const int64_t raw = static_cast<int64_t>(std::lround(value * one));
    const int64_t mask = (int64_t{1} << bits) - 1;
    const int64_t low = raw & mask;
    return (low & (int64_t{1} << (bits - 1))) != 0 ? low - (int64_t{1} << bits) : low;
}

// The rotation parameter table WriteRotTable writes, as the hardware reads it.
sp::RotParams ToRotParams(const RotTable &t) {
    sp::RotParams p;
    p.Xst = FitFixed(t.xst, 1024, 23);
    p.Yst = FitFixed(t.yst, 1024, 23);
    p.Zst = FitFixed(t.zst, 1024, 23);
    p.deltaXst = FitFixed(t.dxst, 1024, 13);
    p.deltaYst = FitFixed(t.dyst, 1024, 13);
    p.deltaX = FitFixed(t.dx, 1024, 13);
    p.deltaY = FitFixed(t.dy, 1024, 13);
    p.A = FitFixed(t.a, 1024, 14);
    p.B = FitFixed(t.b, 1024, 14);
    p.C = FitFixed(t.c, 1024, 14);
    p.D = FitFixed(t.d, 1024, 14);
    p.E = FitFixed(t.e, 1024, 14);
    p.F = FitFixed(t.f, 1024, 14);
    p.Px = FitFixed(t.px, 1, 14);
    p.Py = FitFixed(t.py, 1, 14);
    p.Pz = FitFixed(t.pz, 1, 14);
    p.Cx = FitFixed(t.cx, 1, 14);
    p.Cy = FitFixed(t.cy, 1, 14);
    p.Cz = FitFixed(t.cz, 1, 14);
    p.Mx = FitFixed(t.mx, 1024, 24);
    p.My = FitFixed(t.my, 1024, 24);
    p.kx = FitFixed(t.kx, 65536, 24);
    p.ky = FitFixed(t.ky, 65536, 24);
    p.KAst = static_cast<int64_t>(std::lround(t.kast * 1024)) & 0x3FFFFFF;
    p.dKAst = FitFixed(t.dkast, 1024, 20);
    p.dKAx = FitFixed(t.dkax, 1024, 20);
    return p;
}

// Coefficient entry values (8.16, about 0.875 to 1.125), never transparent.
int64_t CoordCoefficient(uint32_t entry) {
    return 0x10000 + static_cast<int64_t>((entry * 7919u) % 0x4000u) - 0x2000;
}

// RBG0 drawing the coordinate bitmap (RGB555, 512x256, screen-over repeat, transparency off) with parameter A.
struct CoordRBG {
    uint16_t tvmd = 0;
    bool deinterlace = false;
    RotTable table = Rotated(0.3, 0.9);
    bool coefficients = false; // mode 0 (kx = ky), two words, from VRAM bank B0 (CoordCoefficient)
    bool perDot = true;
    uint32_t reloadLine = UINT32_MAX; // the table becomes `reloaded` here, Xst and Yst re-read (RPRCTL)
    RotTable reloaded;
    bool reloadKAst = false; // KAst re-read at reloadLine too
    // A coefficient table of its own (enables coefficients, with the tables' kast, dkast and dkax as given): the data
    // word of each entry (two-word, or the low 16 bits one-word) for entries coeffFirst .. coeffFirst + coeffCount - 1
    // (relative to the table's address offset), from bank B0 (bank B1 is not designated: per-dot reads there fail).
    std::function<uint32_t(uint32_t entry)> coeffData;
    uint32_t coeffFirst = 0, coeffCount = 1024;
    bool oneWord = false;
    sp::CoeffMode coeffMode = sp::CoeffMode::ScaleXY;
    bool transparency = false; // some entries are transparent
};

// The VRAM address of coefficient entry `entry` (KTAOF = 1 two-word, 2 one-word: the table starts at 0x40000).
uint32_t CoordCoefficientAddress(uint32_t entry, bool oneWord) {
    return oneWord ? ((0x20000 + entry) * 2) & 0x7FFFF : ((0x10000 + entry) * 4) & 0x7FFFF;
}

// A coefficient entry as the hardware decodes it (two-word: 8.16 value in bits 0-23, transparency bit 31; one-word:
// 1.5.10 value, transparency bit 15; viewpoint X shifts the data by 8 or 14 instead of 0 or 6).
sp::Coefficient DecodeCoordCoefficient(uint32_t data, bool oneWord, sp::CoeffMode mode, uint32_t entry) {
    sp::Coefficient c;
    const uint32_t address = CoordCoefficientAddress(entry, oneWord);
    c.fetchable = address >= 0x40000 && address < 0x60000;
    const bool vpx = mode == sp::CoeffMode::ViewpointX;
    if (oneWord) {
        const int64_t value = static_cast<int16_t>(static_cast<uint16_t>(data << 1)) >> 1; // 15 bits, signed
        c.value = value * (vpx ? 1 << 14 : 1 << 6);
        c.transparent = (data & 0x8000) != 0;
    } else {
        const int64_t value = static_cast<int32_t>(data << 8) >> 8; // 24 bits, signed
        c.value = value * (vpx ? 1 << 8 : 1);
        c.transparent = (data & 0x80000000u) != 0;
    }
    return c;
}

CoordScene CoordRBGScene(std::string name, CoordRBG c) {
    if (c.coefficients && !c.coeffData) {
        for (RotTable *t : {&c.table, &c.reloaded}) {
            t->kast = 0;
            t->dkast = c.perDot ? 0.25 : 1.0;
            t->dkax = c.perDot ? 1.0 : 0.0;
        }
        c.coeffData = [](uint32_t entry) { return static_cast<uint32_t>(CoordCoefficient(entry)) & 0xFFFFFF; };
    }
    c.coefficients = static_cast<bool>(c.coeffData);
    Vdp2Scene scene = Make(
        [c](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | c.tvmd));
            // A0 and A1 bitmap, B0 coefficients
            s.ramctl = static_cast<uint16_t>((s.ramctl & 0x3000) | 0x0300 | 0x000F | (c.coefficients ? 0x0010 : 0));
            s.chctlb = static_cast<uint16_t>((s.chctlb & 0x00FF) | 0x3200); // RGB555 512x256 bitmap
            s.bgon |= 0x1010;                                                // transparency off
            w.Reg(PRIR, 5);
            w.Reg(PLSZ, 0);
            w.Reg(MPOFR, 0);
            WriteCoordBitmap(w);
            WriteRotTable(w, kRotTableAddress, c.table);
            w.Reg(RPTAU, static_cast<uint16_t>(kRotTableAddress >> 17));
            w.Reg(RPTAL, static_cast<uint16_t>((kRotTableAddress >> 1) & 0xFFFE));
            w.Reg(RPMD, 0);
            if (c.coefficients) {
                w.Reg(KTCTL, static_cast<uint16_t>(0x0001 | (c.oneWord ? 0x0002 : 0) |
                                                   (static_cast<uint32_t>(c.coeffMode) << 2)));
                w.Reg(KTAOF, c.oneWord ? 0x0202 : 0x0101);
                for (uint32_t i = c.coeffFirst; i < c.coeffFirst + c.coeffCount; ++i) {
                    const uint32_t address = CoordCoefficientAddress(i, c.oneWord);
                    if (address < 0x40000 || address >= 0x60000) {
                        continue;
                    }
                    if (c.oneWord) {
                        w.VRAM16(address, static_cast<uint16_t>(c.coeffData(i)));
                    } else {
                        w.VRAM32(address, c.coeffData(i));
                    }
                }
            }
        },
        {.deinterlace = c.deinterlace});
    if (c.reloadLine != UINT32_MAX) {
        scene.onLine = [c](Vdp2SceneWriter &w, uint32_t y) {
            if (y == 0) {
                WriteRotTable(w, kRotTableAddress, c.table);
            } else if (y == c.reloadLine) {
                WriteRotTable(w, kRotTableAddress, c.reloaded);
                w.Reg(RPRCTL, c.reloadKAst ? 0x0007 : 0x0003); // re-read Xst and Yst (and KAst)
            }
        };
    }

    sp::RBGFrame frame;
    frame.geometry = CoordGeometry(c.tvmd, c.deinterlace);
    frame.table = ToRotParams(c.table);
    frame.reloadLine = c.reloadLine;
    frame.reloaded = ToRotParams(c.reloaded);
    frame.reloadKAst = c.reloadKAst;
    frame.coeffMode = c.coeffMode;
    if (c.coefficients) {
        frame.coefficient = [data = c.coeffData, oneWord = c.oneWord, mode = c.coeffMode, first = c.coeffFirst,
                             count = c.coeffCount](uint32_t entry) {
            // Entries outside the written range hold 0 (the scenes start from a hard reset)
            const uint32_t word = entry >= first && entry - first < count ? data(entry) : 0;
            return DecodeCoordCoefficient(word, oneWord, mode, entry);
        };
    }
    CoordExpect expect = [frame](uint32_t n, uint32_t X, uint32_t Y) {
        sp::RBGFrame f = frame;
        f.geometry.n = n;
        return sp::RBGDotAt(f, X, Y);
    };
    CoordScene scene_{std::move(name), std::move(scene), std::move(expect)};
    scene_.transparency = c.transparency;
    return scene_;
}

// Coefficient tables of the interpolation scenes (stage3-research.md section 4), two-word data words.
uint32_t CoeffWord(int64_t value, bool transparent = false) {
    return (static_cast<uint32_t>(value) & 0xFFFFFF) | (transparent ? 0x80000000u : 0);
}

// A perspective floor as VF2 and Panzer Dragoon II Zwei write theirs, one entry per line: transparent sky (entries
// 0-29), a clamped horizon entry (8.0, which fails the guard against the next), then k = 40 / (e - 20) falling to
// about 0.17 (consecutive entries pass).
uint32_t FloorCoefficient(uint32_t e) {
    if (e < 30) {
        return CoeffWord(0x10000, true);
    }
    if (e == 30) {
        return CoeffWord(0x80000);
    }
    return CoeffWord(std::lround(65536.0 * 40.0 / (static_cast<double>(e) - 20.0)));
}

// The guard's cases in a 6-entry cycle: 1 -> 2 and 2 -> 1 (difference exactly half the larger: interpolated),
// 1 -> 2 + 2^-16 (just over: native), a sign flip either way (native), -1 -> -1.25 (interpolated).
uint32_t GuardCoefficient(uint32_t e) {
    static constexpr int64_t kCycle[6] = {0x10000, 0x20000, 0x10000, 0x20001, -0x10000, -0x14000};
    return CoeffWord(kCycle[e % 6]);
}

std::vector<CoordScene> CoordScenes() {
    std::vector<CoordScene> scenes;
    scenes.push_back(CoordNBGScene("NBG0 zoom 0.5x", {.incH = 0x200}));
    scenes.push_back(CoordNBGScene("NBG0 zoom 1.25x", {.incH = 0xCD, .incV = 0xCD}));
    scenes.push_back(CoordNBGScene("NBG0 zoom 2x", {.incH = 0x80, .incV = 0x80}));
    scenes.push_back(CoordNBGScene("NBG0 line scroll X and line zoom", {.incV = 0xC0, .lineTable = true}));
    scenes.push_back(CoordNBGScene("NBG0 zoom change at line 100", {.incH = 0xCD, .zoomLine = 100, .zoomAfter = 0x80}));
    scenes.push_back(CoordNBGScene("NBG0 hi-res zoom", {.tvmd = 2, .incH = 0xCD, .incV = 0xA0}));
    for (const bool deinterlace : {true, false}) {
        const std::string mode = deinterlace ? ", deinterlaced" : ", woven";
        scenes.push_back(CoordNBGScene("NBG0 vertical zoom in double density" + mode,
                                       {.tvmd = kLSMDDouble, .deinterlace = deinterlace, .incH = 0xCD, .incV = 0xA0}));
        scenes.back().monotoneV = true;
        scenes.push_back(CoordNBGScene("NBG0 vertical zoom in single density" + mode,
                                       {.tvmd = kLSMDSingle, .deinterlace = deinterlace, .incH = 0xCD, .incV = 0xA0}));
        scenes.back().monotoneV = true;
    }
    // Mosaic is per axis: a vertical-only mosaic keeps the horizontal sub-pixels (each row of a block repeats the
    // block's first line, vertically native).
    scenes.push_back(CoordNBGScene("NBG0 zoom with a vertical-only mosaic", {.incH = 0xCD, .incV = 0xA0, .mosaicV = 3}));
    // Tile edges: 1x1 characters (each sub-pixel's character and dot from its own position), zoomed with a fractional
    // scroll, enlarged and reduced; vertical cell scroll on 1x1 characters (each sub-pixel reads the table entry of
    // its own cell column).
    scenes.push_back(CoordNBGScene("NBG0 cells zoom 1.25x", {.incH = 0xCD, .incV = 0xCD, .cells = true}));
    scenes.push_back(CoordNBGScene("NBG0 cells zoom 0.75x", {.incH = 0x155, .incV = 0x155, .cells = true}));
    scenes.push_back(CoordNBGScene("NBG0 cells vertical cell scroll",
                                   {.incH = 0x100, .incV = 0xC0, .cells = true, .vcellScroll = true}));
    scenes.push_back(CoordNBGScene("NBG0 cells vertical cell scroll with zoom 1.25x",
                                   {.incH = 0xCD, .incV = 0xC0, .cells = true, .vcellScroll = true}));

    scenes.push_back(CoordRBGScene("RBG0 fixed matrix", {}));
    scenes.push_back(CoordRBGScene("RBG0 per-dot coefficients", {.coefficients = true}));
    scenes.push_back(CoordRBGScene("RBG0 per-line coefficients", {.coefficients = true, .perDot = false}));
    scenes.push_back(CoordRBGScene("RBG0 hi-res", {.tvmd = 2}));
    scenes.push_back(CoordRBGScene("RBG0 hi-res 704 per-dot coefficients", {.tvmd = 3, .coefficients = true}));
    // Interlaced, deinterlaced or woven: both rows of a pair show the RBG line from the same bases, the odd one half
    // a line on (t = (row & 1) N + j over 2N), so v never steps back (rotation 0.3: v grows with the line).
    scenes.push_back(CoordRBGScene("RBG0 double density, deinterlaced", {.tvmd = kLSMDDouble, .deinterlace = true}));
    scenes.back().monotoneV = true;
    scenes.push_back(CoordRBGScene("RBG0 double density, woven", {.tvmd = kLSMDDouble}));
    scenes.back().monotoneV = true;
    scenes.push_back(CoordRBGScene("RBG0 single density, deinterlaced", {.tvmd = kLSMDSingle, .deinterlace = true}));
    scenes.back().monotoneV = true;
    scenes.push_back(CoordRBGScene("RBG0 single density, woven", {.tvmd = kLSMDSingle}));
    scenes.back().monotoneV = true;
    // Exclusive monitor modes never double rows: one row per RBG line (R = 1), as progressive.
    scenes.push_back(CoordRBGScene("RBG0 exclusive monitor double density, woven",
                                   {.tvmd = static_cast<uint16_t>(4 | kLSMDDouble)}));
    {
        // Xst, Yst and their increments (and the horizontal increment) reloaded at line 100: the sub-lines of each
        // line use that line's own increments.
        RotTable reloaded = Rotated(0.3, 0.9);
        reloaded.xst = 37.25;
        reloaded.yst = -12.5;
        reloaded.dxst = 0.4;
        reloaded.dyst = 0.7;
        reloaded.dx = 1.1;
        scenes.push_back(CoordRBGScene("RBG0 mid-frame Xst reload", {.reloadLine = 100, .reloaded = reloaded}));
        scenes.push_back(CoordRBGScene("RBG0 mid-frame Xst reload, deinterlaced",
                                       {.tvmd = kLSMDDouble, .deinterlace = true, .reloadLine = 100,
                                        .reloaded = reloaded}));
    }
    {
        // Every increment negative or mixed: the truncating divisions of negative offsets.
        RotTable t = Rotated(2.5, 1.2);
        t.xst = 50;
        t.yst = 80;
        t.dxst = -0.35;
        t.dyst = -0.9;
        t.dx = -0.95;
        t.dy = 0.3;
        scenes.push_back(CoordRBGScene("RBG0 negative increments", {.table = t}));
        scenes.push_back(CoordRBGScene("RBG0 negative increments, hi-res deinterlaced",
                                       {.tvmd = static_cast<uint16_t>(2 | kLSMDDouble), .deinterlace = true,
                                        .table = t}));
    }
    // Coefficient interpolation between lines (stage 3 Task 2): the sub-lines of a line in a scale mode take
    // c0 + ((c1 - c0) t) / (R N) between the line's entry and the one dKAst on, when both are opaque and fetchable
    // and the guard passes; KA, transparency, per-dot steps and viewpoint X stay native.
    {
        RotTable floor = Rotated(0.3, 0.9);
        floor.kast = 0;
        floor.dkast = 1;
        floor.dkax = 0;
        const CoordRBG perLine = {.table = floor, .coeffData = FloorCoefficient, .coeffCount = 256,
                                  .transparency = true};
        scenes.push_back(CoordRBGScene("RBG0 perspective floor coefficients per line", perLine));
        CoordRBG c = perLine;
        c.table.dkast = 0.75; // fractional: KA + dKAst is sometimes the same entry
        scenes.push_back(CoordRBGScene("RBG0 perspective floor, fractional dKAst", c));
        c = perLine;
        c.tvmd = kLSMDDouble;
        c.deinterlace = true;
        scenes.push_back(CoordRBGScene("RBG0 perspective floor, double density deinterlaced", c));
        c.deinterlace = false;
        scenes.push_back(CoordRBGScene("RBG0 perspective floor, double density woven", c));
        c = perLine;
        c.tvmd = 3; // 704 wide
        scenes.push_back(CoordRBGScene("RBG0 perspective floor, hi-res 704", c));

        c = perLine;
        c.coeffData = GuardCoefficient;
        c.transparency = false;
        scenes.push_back(CoordRBGScene("RBG0 coefficient discontinuity guard", c));

        // Per dot, every 7th entry transparent: transparent own entries show the back screen, transparent next
        // entries keep the native coefficient.
        c = perLine;
        c.table.dkast = 2;
        c.table.dkax = 1;
        c.coeffData = [](uint32_t e) { return CoeffWord(CoordCoefficient(e), e % 7 == 3); };
        c.coeffCount = 1024;
        scenes.push_back(CoordRBGScene("RBG0 per-dot coefficients with transparent entries", c));

        // Scale X only, per dot with half-entry horizontal steps and a 3-entry vertical step
        c = perLine;
        c.table.dkast = 3;
        c.table.dkax = 0.5;
        c.coeffMode = sp::CoeffMode::ScaleX;
        c.coeffData = [](uint32_t e) { return CoeffWord(CoordCoefficient(e)); };
        c.coeffCount = 1024;
        c.transparency = false;
        scenes.push_back(CoordRBGScene("RBG0 per-dot scale X coefficients", c));

        // Scale Y only, one-word data (1.5.10), every 11th entry transparent
        c = perLine;
        c.oneWord = true;
        c.coeffMode = sp::CoeffMode::ScaleY;
        c.coeffData = [](uint32_t e) {
            const auto value = static_cast<uint32_t>(std::lround((0.6 + e / 300.0) * 1024)) & 0x7FFF;
            return value | (e % 11 == 5 ? 0x8000u : 0);
        };
        scenes.push_back(CoordRBGScene("RBG0 one-word scale Y coefficients", c));

        // Viewpoint X per line: Xp = the coefficient (integer dots here) on every sub-line, never interpolated
        c = perLine;
        c.coeffMode = sp::CoeffMode::ViewpointX;
        c.coeffData = [](uint32_t e) { return CoeffWord(int64_t{e} * 3 + 7); };
        c.transparency = false;
        scenes.push_back(CoordRBGScene("RBG0 viewpoint X coefficients", c));

        // The table at the end of bank B0 (per-dot mode): line 149's next entry is in bank B1, not designated for
        // coefficients (native); from line 150 on its own entry is (transparent).
        c = perLine;
        c.table.kast = 0x8000 - 150;
        c.coeffData = [](uint32_t e) { return CoeffWord(CoordCoefficient(e)); };
        c.coeffFirst = 0x8000 - 200;
        c.coeffCount = 200;
        scenes.push_back(CoordRBGScene("RBG0 coefficients at the end of the coefficient bank", c));

        // Mid-frame reloads at line 100: KAst re-read (a new place in the table, a half-entry dKAst), or KA continuing
        // with the reloaded table's 3-entry dKAst. Line 99's sub-lines use line 99's own dKAst either way.
        c = perLine;
        c.coeffData = [](uint32_t e) { return CoeffWord(CoordCoefficient(e)); };
        c.coeffCount = 1024;
        c.transparency = false;
        c.reloadLine = 100;
        c.reloaded = floor;
        c.reloaded.xst = 37.25;
        c.reloaded.yst = -12.5;
        c.reloaded.kast = 600;
        c.reloaded.dkast = 0.5;
        c.reloadKAst = true;
        scenes.push_back(CoordRBGScene("RBG0 mid-frame KAst reload", c));
        c.reloaded.dkast = 3;
        c.reloadKAst = false;
        scenes.push_back(CoordRBGScene("RBG0 mid-frame dKAst change without a KAst reload", c));
    }
    return scenes;
}

// Software's 1x frame is the reference at 1x; the N x composite matches software at the sample points and the
// reference at every pixel, and has sub-pixel detail (blocks that are not one colour).
void CheckCoordScene(gpu::VulkanDevice &device, const CoordScene &c, uint32_t n, const RenderBothOptions &options = {}) {
    CAPTURE(c.name, n);
    const SceneResult result = CompareSamplePoints(c.scene, device, n, options);
    INFO(result.Describe());
    REQUIRE(result.error.empty());
    CHECK(result.identical);
    // (Software's transparent pixels are its own back screen; the GPU's must equal them.)
    const CoordCheck native = CheckCoords(result.software, result.width, result.height, 1, c.expect, result.software);
    INFO("software at 1x: " << native.Describe());
    CHECK(native.pixels == uint64_t{result.width} * result.height);
    CHECK(native.mismatches == 0);
    const CoordCheck scaled =
        CheckCoords(result.gpuScaled, result.width, result.height, n, c.expect, result.software);
    INFO("GPU at " << n << "x: " << scaled.Describe());
    CHECK(scaled.pixels == uint64_t{result.width} * result.height * n * n);
    CHECK(scaled.mismatches == 0);
    CHECK((scaled.transparent > 0) == c.transparency);
    CHECK(NonConstantBlocks(result.gpuScaled, result.width, result.height, n) > 0);
    if (c.monotoneV) {
        uint64_t compared = 0;
        CHECK(BackwardSteps(result.gpuScaled, result.width, result.height, n, compared) == 0);
        CHECK(compared == uint64_t{result.width} * n * (result.height * n - 1));
    }
}

} // namespace

TEST_CASE("Scene: coordinate bitmaps at N match the sub-pixel reference", "[gpu][vdp2][scene][scale]") {
    const uint32_t n = GENERATE(2u, 3u, 4u);
    auto device = OpenDevice();
    for (const CoordScene &c : CoordScenes()) {
        CheckCoordScene(*device, c, n);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: coordinate bitmaps with poisoned images", "[gpu][vdp2][scene][scale]") {
    const uint32_t n = GENERATE(2u, 4u);
    auto device = OpenDevice();
    RenderBothOptions options;
    options.poisonImages = true;
    for (const CoordScene &c : CoordScenes()) {
        CheckCoordScene(*device, c, n, options);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: coordinate bitmaps at 2x, and one at 4x", "[gpu][vdp2][scene][scale][lavapipe]") {
    // The lavapipe cross-section (the case above covers every scene at 2-4).
    auto device = OpenDevice();
    const std::vector<CoordScene> scenes = CoordScenes();
    for (const CoordScene &c : scenes) {
        CheckCoordScene(*device, c, 2);
    }
    for (const CoordScene &c : scenes) {
        if (c.name == "RBG0 negative increments, hi-res deinterlaced") {
            CheckCoordScene(*device, c, 4);
        }
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: batching at internal resolution N", "[gpu][vdp2][scene][scale]") {
    // A register, VRAM or CRAM change at line 100 on zoomed, finely scrolled NBGs over rotated RBG0: each batch
    // renders its lines' sub-pixels with its own state.
    const uint32_t n = GENERATE(2u, 3u, 4u);
    const uint32_t change = GENERATE(0u, 1u, 2u);
    CAPTURE(n, change);
    Vdp2Scene scene = Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
        Base(w, s, rng);
        TwoNBGs(w, s, rng);
        w.Reg(SCXDN0, 0x4500);
        w.Reg(SCYDN0, 0x8000);
        w.Reg(ZMXDN0, 0xC000);
        w.Reg(ZMYDN0, 0xA000);
    });
    scene.onLine = [change](Vdp2SceneWriter &w, uint32_t y) {
        if (y != 0 && y != 100) {
            return;
        }
        switch (change) {
        case 0:
            w.Reg(SCXIN0, static_cast<uint16_t>(y == 0 ? 0 : 37));
            w.Reg(ZMXDN0, static_cast<uint16_t>(y == 0 ? 0xC000 : 0x9000));
            break;
        case 1: {
            std::mt19937 rng{y + 1};
            FillVRAM(w, kPageBase, 0x800, rng);
            FillVRAM(w, kCharBase, 0x400, rng);
            break;
        }
        case 2: {
            std::mt19937 rng{y + 3};
            for (uint32_t a = 0; a < 0x400; a += 2) {
                w.CRAM16(a, static_cast<uint16_t>(rng()));
            }
            break;
        }
        }
    };
    auto device = OpenDevice();
    const SceneResult result = CompareSamplePoints(scene, *device, n);
    INFO(result.Describe());
    CHECK(result.error.empty());
    CHECK(result.identical);
    CHECK(NonConstantBlocks(result.gpuScaled, result.width, result.height, n) > 0);
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: single-density interlace with NBGs off on some lines", "[gpu][vdp2][scene][scale]") {
    // The lattice skips the odd rows of single density per line, from the line's BGON: NBG0 is on above line 100 only,
    // so below it the odd rows (sprites, the per-line back screen) are compared too.
    const uint32_t n = GENERATE(2u, 3u, 4u);
    const bool deinterlace = GENERATE(false, true);
    CAPTURE(n, deinterlace);
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, static_cast<uint16_t>(kDisp | kLSMDSingle));
            CyclesSingle(w, 0);
            SetupNBG(w, s, {.index = 0, .format = 1, .priority = 3}, rng);
            SetupSprites(w, {.type = 1}, rng);
            BackPerLine(w, rng);
            w.Reg(SCYDN0, 0x8000);
            w.Reg(ZMYDN0, 0xC000);
        },
        {.deinterlace = deinterlace}, 3);
    scene.onLine = [](Vdp2SceneWriter &w, uint32_t y) { w.Reg(BGON, static_cast<uint16_t>(y < 100 ? 0x0001 : 0)); };
    auto device = OpenDevice();
    const SceneResult result = CompareSamplePoints(scene, *device, n);
    INFO(result.Describe());
    CHECK(result.error.empty());
    CHECK(result.identical);
    REQUIRE(result.height == 448);
    CHECK(result.skippedPixels == uint64_t{100} * result.width); // odd rows 1-199
    CHECK(result.comparedPixels == uint64_t{348} * result.width);
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: the internal resolution changes between frames", "[gpu][vdp2][scene][scale][lavapipe]") {
    // 1x, then 3x, then 2x (the device is idle between the harness's frames, as SetInternalScale requires): every frame
    // matches software at its own scale.
    auto device = OpenDevice();
    constexpr uint32_t kScales[] = {1, 3, 2};
    std::vector<uint32_t> recorded;
    RenderBothOptions options;
    options.scale = 1;
    options.beforeFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        REQUIRE(vulkan.SetInternalScale(kScales[frame]));
        CHECK(vulkan.InternalScale() == kScales[frame]);
    };
    options.afterFrame = [&](uint32_t, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        recorded.push_back(vulkan.Backend()->LastRecordStatsForTest().scale);
    };
    options.compareFrame = [](uint32_t) { return true; };
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            SetupSprites(w, {.type = 1}, rng);
        },
        {}, 3);
    scene.onLine = [frame = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            w.Reg(SCXIN0, static_cast<uint16_t>(++*frame * 11));
        }
    };
    const SceneResult result = Check(*device, scene, options);
    CHECK(result.comparedFrames == 3);
    CHECK(result.scale == 2);
    CHECK(recorded == std::vector<uint32_t>(std::begin(kScales), std::end(kScales)));
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: a live internal resolution change keeps the woven field (nearest rescale)",
          "[gpu][vdp2][scene][scale][lavapipe]") {
    // Deinterlacing off on an interlaced mode: each frame writes one field, the composite keeps the other. A scale
    // change between two frames rescales the old composite into the new one (nearest: N x pixel X of a native pixel x
    // reads x * from + (X % to) * from / to), so the next frame weaves against the previous field, not a black one:
    // every frame matches software at the sample points, and the kept field of the first frame at the new scale is
    // exactly the nearest rescale of the previous composite.
    const uint16_t lsmd = GENERATE(kLSMDSingle, kLSMDDouble);
    const auto [from, to] = GENERATE(std::pair{1u, 2u}, std::pair{2u, 4u}, std::pair{4u, 1u}, std::pair{3u, 2u});
    CAPTURE(lsmd, from, to);
    constexpr uint32_t kChange = 3;
    auto device = OpenDevice();
    std::vector<uint32_t> before;
    std::vector<uint32_t> after;
    uint32_t width = 0;
    uint32_t height = 0;
    RenderBothOptions options;
    options.scale = from;
    options.beforeFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (frame == kChange) {
            REQUIRE(vulkan.SetInternalScale(to));
            // The rescale replaces the clear a new composite gets.
            CHECK(vulkan.Backend()->RescalePendingForTest());
            CHECK_FALSE(vulkan.Backend()->CompositeClearPendingForTest());
        }
    };
    options.afterFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &dev) {
        width = vulkan.Backend()->CompositeWidthForTest();
        height = vulkan.Backend()->CompositeHeightForTest();
        const uint32_t n = vulkan.InternalScale();
        if (frame == kChange - 1) {
            before = ReadbackBuffer(dev, vulkan.CompositeBuffer(), width * height * n * n);
        } else if (frame == kChange) {
            CHECK_FALSE(vulkan.Backend()->RescalePendingForTest());
            after = ReadbackBuffer(dev, vulkan.CompositeBuffer(), width * height * n * n);
        }
    };
    options.compareFrame = [](uint32_t) { return true; };
    const SceneResult result = Check(*device, WeaveScene(lsmd, 6), options);
    CHECK(result.comparedFrames == 6);
    CHECK(result.scale == to);
    REQUIRE(width != 0);
    REQUIRE(before.size() == size_t{width} * height * from * from);
    REQUIRE(after.size() == size_t{width} * height * to * to);
    // Rows of each native parity that equal the nearest rescale of the previous composite.
    const auto rescaled = [&](uint32_t X, uint32_t Y) {
        const uint32_t sx = (X / to) * from + (X % to) * from / to;
        const uint32_t sy = (Y / to) * from + (Y % to) * from / to;
        return before[size_t{sy} * width * from + sx];
    };
    uint32_t keptRows[2] = {0, 0};
    for (uint32_t y = 0; y < height; ++y) {
        bool kept = true;
        for (uint32_t Y = y * to; Y < (y + 1) * to && kept; ++Y) {
            for (uint32_t X = 0; X < width * to; ++X) {
                if (after[size_t{Y} * width * to + X] != rescaled(X, Y)) {
                    kept = false;
                    break;
                }
            }
        }
        keptRows[y & 1] += kept ? 1 : 0;
    }
    CAPTURE(keptRows[0], keptRows[1], height);
    // One field is kept entirely, the other was drawn anew (it scrolled).
    CHECK(((keptRows[0] == height / 2 && keptRows[1] < height / 2) ||
           (keptRows[1] == height / 2 && keptRows[0] < height / 2)));
    // ... and it is not black.
    CHECK(std::any_of(before.begin(), before.end(), [](uint32_t px) { return (px & 0xFFFFFFu) != 0; }));
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: a dropped frame re-arms the rescale of a scale change", "[gpu][vdp2][scene][scale][lavapipe]") {
    // The frame recorded right after the scale change (it carries the rescale) is never submitted: the rescale is
    // pending again for the next frame, which rescales the previous composite (still alive). That frame lacks the
    // dropped frame's field, as any dropped weave frame does; from the frame after it on, every frame matches.
    constexpr uint32_t kChange = 3;
    auto device = OpenDevice();
    bool pendingAgain = false;
    RenderBothOptions options;
    options.scale = 1;
    options.dropFrame = kChange;
    options.beforeFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (frame == kChange) {
            REQUIRE(vulkan.SetInternalScale(2));
        } else if (frame == kChange + 1) {
            pendingAgain = vulkan.Backend()->RescalePendingForTest();
        }
    };
    options.compareFrame = [](uint32_t frame) { return frame < kChange || frame > kChange + 1; };
    const SceneResult result = Check(*device, WeaveScene(kLSMDDouble, 7), options);
    CHECK(pendingAgain);
    CHECK(result.comparedFrames == 5);
    CHECK(result.scale == 2);
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: an internal resolution that cannot be allocated keeps the previous one", "[gpu][vdp2][scene][scale]") {
    // The allocation limit admits 2x but not 4x: SetInternalScale(4) fails, logs once, and the renderer keeps
    // rendering at 2x; a later re-attach (a new back half) falls back to 1x when even 2x does not fit.
    namespace vdp2 = gpu::vdp2;
    auto device = OpenDevice();
    std::vector<std::string> logs;
    bool failed = true;
    RenderBothOptions options;
    options.scale = 2;
    options.beforeFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (frame == 1) {
            // (VDP1 upscaling, BRIMIR_VDP1_UPSCALING=1, logs its surface as it is freed and made again)
            vulkan.SetLog([&](const std::string &text) {
                if (text.rfind("VDP1 upscaling:", 0) != 0) {
                    logs.push_back(text);
                }
            });
            vulkan.Backend()->SetScaleAllocationLimitForTest(vdp2::Vdp2VulkanBackend::ScaledResourceBytes(2));
            failed = !vulkan.SetInternalScale(4);
            CHECK(vulkan.InternalScale() == 2);
            CHECK(vulkan.Backend()->Scale() == 2);
            CHECK_FALSE(vulkan.SetInternalScale(0));
            CHECK_FALSE(vulkan.SetInternalScale(5));
        }
    };
    options.compareFrame = [](uint32_t) { return true; };
    const SceneResult result = Check(*device, Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                                         Base(w, s, rng);
                                         TwoNBGs(w, s, rng);
                                     }, {}, 3),
                                     options);
    CHECK(failed);
    CHECK(result.scale == 2);
    CHECK(result.comparedFrames == 3);
    REQUIRE(logs.size() == 1);
    CHECK(logs[0].find("4x") != std::string::npos);
    CloseAndCheckValidation(device);
}

TEST_CASE("Vdp2VulkanBackend: scale resources per N, and the renderer's requested N at attach", "[gpu][vdp2][scale]") {
    namespace vdp2 = gpu::vdp2;
    // Background layers (6 x 4 bytes) and the composite (4 bytes) per N x pixel of 704 x 512.
    for (uint32_t n = 1; n <= 4; ++n) {
        CAPTURE(n);
        CHECK(vdp2::Vdp2VulkanBackend::ScaledResourceBytes(n) == uint64_t{704 * n} * (512 * n) * 28);
    }
    auto device = OpenDevice();
    {
        vdp2::Vdp2VulkanBackend backend(*device, 1);
        REQUIRE(backend.Ok());
        CHECK(backend.Scale() == 1);
        std::vector<std::string> logs;
        backend.SetLog([&](const std::string &text) { logs.push_back(text); });
        backend.SetScaleAllocationLimitForTest(1);
        CHECK_FALSE(backend.SetScale(3));
        CHECK(backend.Scale() == 1);
        CHECK(logs.size() == 1);
        backend.SetScaleAllocationLimitForTest(0);
        for (uint32_t n : {4u, 2u, 3u, 1u}) {
            CHECK(backend.SetScale(n));
            CHECK(backend.Scale() == n);
        }
        CHECK(logs.size() == 1);
    }
    {
        // A back half created at N allocates N directly (no 1x first), or falls back to 1x when N does not fit.
        vdp2::Vdp2VulkanBackend at3(*device, 1, 3);
        REQUIRE(at3.Ok());
        CHECK(at3.Scale() == 3);
        std::vector<std::string> logs;
        vdp2::Vdp2VulkanBackend tooBig(*device, 1, 4, vdp2::Vdp2VulkanBackend::ScaledResourceBytes(2),
                                       [&](const std::string &text) { logs.push_back(text); });
        REQUIRE(tooBig.Ok());
        CHECK(tooBig.Scale() == 1);
        REQUIRE(logs.size() == 1);
        CHECK(logs[0].find("4x") != std::string::npos);
        CHECK(logs[0].find("trying 1x") != std::string::npos);
        // ... or to a fallback scale that fits, allocated directly (one failure logged, never 1x first).
        logs.clear();
        vdp2::Vdp2VulkanBackend fallback(*device, 1, 4, vdp2::Vdp2VulkanBackend::ScaledResourceBytes(2),
                                         [&](const std::string &text) { logs.push_back(text); }, 2);
        REQUIRE(fallback.Ok());
        CHECK(fallback.Scale() == 2);
        REQUIRE(logs.size() == 1);
        CHECK(logs[0].find("trying 2x") != std::string::npos);
        // A fallback above the scale is ignored.
        vdp2::Vdp2VulkanBackend ignored(*device, 1, 2, 0, {}, 3);
        CHECK(ignored.Scale() == 2);
    }
    {
        // A renderer asked for 4x gets 4x at attach; with the allocation limited (the renderer's test hook, applied to
        // every back half it creates) the attach falls back to 1x, logged with the effective scale, keeps the request,
        // and the next attach that can allocate it gets 4x again.
        namespace vdp = ymir::vdp;
        auto state = std::make_unique<vdp::VDPState>();
        vdp::config::VDP2DebugRender debug{};
        vdp::config::VDP2AccessPatternsConfig access{};
        auto vulkan = std::make_unique<gpu::VulkanVDPRenderer>(*state, debug, access);
        std::vector<std::string> logs;
        vulkan->SetLog([&](const std::string &text) { logs.push_back(text); });
        CHECK(vulkan->SetInternalScale(4)); // detached: kept for the attach
        CHECK(vulkan->InternalScale() == 4);
        CHECK(vulkan->RequestedInternalScale() == 4);
        vulkan->AttachDevice(*device, 1);
        REQUIRE(vulkan->Attached());
        CHECK(vulkan->Backend()->Scale() == 4);
        CHECK(logs.empty());
        vulkan->DetachDevice();
        CHECK(vulkan->InternalScale() == 4);

        vulkan->SetScaleAllocationLimitForTest(vdp2::Vdp2VulkanBackend::ScaledResourceBytes(2));
        vulkan->AttachDevice(*device, 1);
        REQUIRE(vulkan->Attached());
        CHECK(vulkan->Backend()->Scale() == 1);
        CHECK(vulkan->InternalScale() == 1);
        CHECK(vulkan->RequestedInternalScale() == 4);
        REQUIRE(logs.size() == 2); // the back half's allocation failure, the renderer's effective scale
        CHECK(logs[1].find("requested 4x") != std::string::npos);
        CHECK(logs[1].find("using 1x") != std::string::npos);
        // 2x fits: a live change to it works and is the request now.
        CHECK(vulkan->SetInternalScale(2));
        CHECK(vulkan->InternalScale() == 2);
        CHECK(vulkan->RequestedInternalScale() == 2);
        CHECK_FALSE(vulkan->SetInternalScale(4));
        CHECK(vulkan->InternalScale() == 2);
        CHECK(vulkan->RequestedInternalScale() == 4);
        // A re-attach (context reset) under the limit falls back to the previous effective 2x, not 1x: 4x fails, 2x is
        // allocated directly (the back half logs the one failure, the renderer the effective scale).
        vulkan->DetachDevice();
        logs.clear();
        vulkan->AttachDevice(*device, 1);
        REQUIRE(vulkan->Attached());
        CHECK(vulkan->Backend()->Scale() == 2);
        CHECK(vulkan->InternalScale() == 2);
        REQUIRE(logs.size() == 2);
        CHECK(logs[0].find("trying 2x") != std::string::npos);
        CHECK(logs[1].find("requested 4x, using 2x") != std::string::npos);
        // A cap (what the output can present) below the request: attached at the cap, the request kept.
        vulkan->DetachDevice();
        vulkan->SetScaleAllocationLimitForTest(0);
        vulkan->AttachDevice(*device, 1, 3);
        REQUIRE(vulkan->Attached());
        CHECK(vulkan->InternalScale() == 3);
        CHECK(vulkan->RequestedInternalScale() == 4);
        vulkan->SetScaleAllocationLimitForTest(vdp2::Vdp2VulkanBackend::ScaledResourceBytes(2));
        vulkan->DetachDevice();
        vulkan->SetScaleAllocationLimitForTest(0);
        vulkan->AttachDevice(*device, 1);
        REQUIRE(vulkan->Attached());
        CHECK(vulkan->Backend()->Scale() == 4);
        CHECK(vulkan->InternalScale() == 4);
        vulkan->DetachDevice();
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: the NBG fetch pass and compose get the native output size at N", "[gpu][vdp2][scene][scale]") {
    // The fetch pass is native at every N (its groupshared rows hold kMaxResH pixels: a scaled width would be clamped
    // without notice); compose gets the native size too and scales the pitch itself.
    const uint32_t n = GENERATE(2u, 4u);
    CAPTURE(n);
    auto device = OpenDevice();
    std::vector<gpu::vdp2::Vdp2VulkanBackend::RecordStats> stats;
    RenderBothOptions options;
    options.afterFrame = [&](uint32_t, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        stats.push_back(vulkan.Backend()->LastRecordStatsForTest());
    };
    const SceneResult result =
        CompareNearest(DelayedNBG1Scene(true, false, [](Vdp2SceneWriter &, Shadow &, std::mt19937 &) {}), *device, n,
                       options);
    INFO(result.Describe());
    CHECK(result.error.empty());
    CHECK(result.identical);
    REQUIRE_FALSE(stats.empty());
    for (const auto &s : stats) {
        CHECK(s.scale == n);
        CHECK(s.fetchPasses > 0);
        CHECK(s.fetchOutputSize == gpu::vdp2::MakeOutputSize(320, 224));
        CHECK(s.composeOutputSize == gpu::vdp2::MakeOutputSize(320, 224));
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: the upload high-water mark decays after frames that fit a smaller block", "[gpu][vdp2][scene]") {
    // 512 KiB blocks: frame 0 (one context) uploads everything and overflows, setting the high-water mark; the later
    // frames change nothing (the per-line arrays and parameters only). After 4 Records that fit the default block (the decay length here), the mark is
    // gone and the base block is back at 512 KiB.
    constexpr uint32_t kBlock = 512 * 1024;
    auto device = OpenDevice();
    std::vector<gpu::vdp2::Vdp2VulkanBackend::RecordStats> stats;
    RenderBothOptions options;
    options.frameContexts = 1;
    options.uploadBlockSize = kBlock;
    options.beforeFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (frame == 0) {
            vulkan.Backend()->SetHighWaterDecayRecordsForTest(4);
        }
    };
    options.afterFrame = [&](uint32_t, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        stats.push_back(vulkan.Backend()->LastRecordStatsForTest());
    };
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
        },
        {}, 8);
    Check(*device, scene, options);
    REQUIRE(stats.size() == 8);
    CHECK(stats[0].overflowBlocks > 0);
    CHECK(stats[0].highWater > kBlock);
    for (uint32_t frame = 1; frame < 8; ++frame) {
        CAPTURE(frame, stats[frame].uploadBytes, stats[frame].baseBlockSize, stats[frame].highWater);
        CHECK(stats[frame].overflowBlocks == 0);
        CHECK(stats[frame].uploadBytes < kBlock);
        // Frames 1-4 still use the high-water block; the 4th small Record (frame 4) drops the mark, so frame 5 has the
        // default block again.
        CHECK(stats[frame].highWater == (frame < 4 ? stats[0].highWater : 0));
        CHECK(stats[frame].baseBlockSize == (frame <= 4 ? stats[1].baseBlockSize : kBlock));
    }
    CHECK(stats[1].baseBlockSize > kBlock);
    CloseAndCheckValidation(device);
}

TEST_CASE("Scene: a high-water upload block that cannot be allocated is logged once and not retried",
          "[gpu][vdp2][scene]") {
    // 64 KiB blocks and every larger base block failing: each context's first frame overflows; its next Record cannot
    // get the high-water block, logs (once for the backend), falls back to 64 KiB plus overflow blocks, and no context
    // asks for a high-water block again. Every frame still matches software.
    auto device = OpenDevice();
    std::vector<std::string> logs;
    std::vector<gpu::vdp2::Vdp2VulkanBackend::RecordStats> stats;
    RenderBothOptions options;
    options.uploadBlockSize = 64 * 1024;
    options.beforeFrame = [&](uint32_t frame, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (frame == 0) {
            vulkan.SetLog([&](const std::string &text) { logs.push_back(text); });
            vulkan.Backend()->FailHighWaterBlocksForTest(true);
        }
    };
    options.afterFrame = [&](uint32_t, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        stats.push_back(vulkan.Backend()->LastRecordStatsForTest());
    };
    options.compareFrame = [](uint32_t) { return true; };
    Vdp2Scene scene = Make(
        [](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng);
            TwoNBGs(w, s, rng);
            SetupSprites(w, {.type = 1}, rng);
        },
        {}, 6);
    // Every frame uploads more than 64 KiB (the per-line arrays alone, and a CRAM word per line).
    scene.onLine = [frame = std::make_shared<uint32_t>(0)](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            ++*frame;
        }
        w.CRAM16((y * 2) & 0x7FE, static_cast<uint16_t>(y * 77 + *frame));
    };
    const SceneResult result = Check(*device, scene, options);
    CHECK(result.comparedFrames == 6);
    REQUIRE(stats.size() == 6);
    for (uint32_t frame = 0; frame < 6; ++frame) {
        CAPTURE(frame, stats[frame].uploadBytes, stats[frame].baseBlockSize, stats[frame].highWater);
        CHECK(stats[frame].uploadBytes > 64 * 1024);
        CHECK(stats[frame].overflowBlocks > 0);
        CHECK(stats[frame].baseBlockSize == 64 * 1024);
        // Contexts 0 and 1 overflow first (a mark each); from frame 2 on, after the failure, no mark is kept.
        if (frame < 2) {
            CHECK(stats[frame].highWater > 64 * 1024);
        } else {
            CHECK(stats[frame].highWater == 0);
        }
    }
    CHECK(logs.size() == 1);
    CloseAndCheckValidation(device);
}

TEST_CASE("CompareNearestPixels and CompareSamplePointPixels", "[vdp2][scale]") {
    // A 4x2 software frame and its 2x nearest upscale.
    const std::vector<uint32_t> sw{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint32_t> up(4 * 2 * 4);
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            up[y * 8 + x] = sw[(y / 2) * 4 + x / 2] | 0xFF000000u;
        }
    }
    SceneResult r;
    REQUIRE(CompareNearestPixels(sw, 4, 2, up, 2, r));
    CHECK(r.identical);
    CHECK(r.comparedPixels == 32);
    CHECK(r.gpu == sw);

    // One sub-pixel off: nearest finds it (native pixel (3, 1)), the sample points do not.
    std::vector<uint32_t> off = up;
    off[3 * 8 + 7] = 0x99;
    REQUIRE(CompareNearestPixels(sw, 4, 2, off, 2, r));
    CHECK_FALSE(r.identical);
    CHECK(r.diffCount == 1);
    CHECK(r.diffX == 3);
    CHECK(r.diffY == 1);
    CHECK(r.gpu[7] == 0x99);
    REQUIRE(CompareSamplePointPixels(sw, 4, 2, off, 2, {}, r));
    CHECK(r.identical);
    CHECK(r.comparedPixels == 8);

    // A sample point off: found, unless the lattice excludes it.
    off = up;
    off[2 * 8 + 2] = 0x99; // native (1, 1)
    REQUIRE(CompareSamplePointPixels(sw, 4, 2, off, 2, {}, r));
    CHECK(r.diffCount == 1);
    REQUIRE(CompareSamplePointPixels(sw, 4, 2, off, 2, {{}, {.skipOddColumns = true}}, r));
    CHECK(r.identical);
    CHECK(r.skippedPixels == 2);
    REQUIRE(CompareSamplePointPixels(sw, 4, 2, off, 2, {{}, {.skipRow = true}}, r));
    CHECK(r.identical);
    CHECK(r.skippedPixels == 4);

    // Vacuous comparisons are refused: wrong sizes, nothing left to compare.
    SceneResult bad;
    CHECK_FALSE(CompareNearestPixels(sw, 4, 2, std::vector<uint32_t>(31), 2, bad));
    CHECK_FALSE(bad.error.empty());
    bad = {};
    CHECK_FALSE(CompareNearestPixels({}, 0, 0, {}, 2, bad));
    bad = {};
    CHECK_FALSE(CompareSamplePointPixels(sw, 4, 2, up, 2, {{.skipRow = true}, {.skipRow = true}}, bad));
    CHECK_FALSE(bad.error.empty());
}

TEST_CASE("The sample-point lattice from TVMD and BGON", "[vdp2][scale]") {
    namespace vdp = ymir::vdp;
    // Records the lattice rows of line 3 in each mode.
    const auto rowsOf = [](uint16_t tvmd, uint16_t bgon, bool deinterlace, bool odd) {
        auto state = std::make_unique<vdp::VDPState>();
        vdp::config::VDP2DebugRender debug{};
        vdp::config::VDP2AccessPatternsConfig access{};
        auto renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
        Vdp2SceneWriter writer{{.deinterlace = deinterlace}};
        writer.Attach(*state, *renderer);
        writer.Reg(TVMD, tvmd);
        writer.Reg(BGON, bgon);
        std::vector<std::pair<uint32_t, SampleLatticeRow>> rows;
        writer.lineObserver = [&](const Vdp2SceneWriter &w, const vdp::VDPState &s, uint32_t y) {
            if (y == 3) {
                brimir::test::LatticeForLine(w, s, y, deinterlace, [&](uint32_t row, SampleLatticeRow flags) {
                    rows.emplace_back(row, flags);
                });
            }
        };
        writer.Field(odd);
        writer.RunFrame();
        return rows;
    };
    // Progressive normal width, RBG0 and NBG0: nothing excluded.
    auto rows = rowsOf(kDisp, 0x11, false, false);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].first == 3);
    CHECK_FALSE(rows[0].second.skipOddColumns);
    CHECK_FALSE(rows[0].second.skipRow);
    // Hi-res with RBG0: odd columns; without an RBG: nothing.
    rows = rowsOf(kDisp | 2, 0x10, false, false);
    CHECK(rows[0].second.skipOddColumns);
    rows = rowsOf(kDisp | 2, 0x01, false, false);
    CHECK_FALSE(rows[0].second.skipOddColumns);
    // Double density deinterlaced with RBG0: rows 6 and 7, the odd one excluded; with NBGs only: neither.
    rows = rowsOf(kDisp | kLSMDDouble, 0x10, true, false);
    REQUIRE(rows.size() == 2);
    CHECK((rows[0].first == 6 && !rows[0].second.skipRow));
    CHECK((rows[1].first == 7 && rows[1].second.skipRow));
    rows = rowsOf(kDisp | kLSMDDouble, 0x01, true, false);
    CHECK_FALSE(rows[1].second.skipRow);
    // Double density woven, RBG0: the field's row only, the odd field's excluded (half a line on, as deinterlaced);
    // with NBGs only: not excluded.
    rows = rowsOf(kDisp | kLSMDDouble, 0x10, false, true);
    REQUIRE(rows.size() == 1);
    CHECK((rows[0].first == 7 && rows[0].second.skipRow));
    rows = rowsOf(kDisp | kLSMDDouble, 0x10, false, false);
    REQUIRE(rows.size() == 1);
    CHECK((rows[0].first == 6 && !rows[0].second.skipRow));
    rows = rowsOf(kDisp | kLSMDDouble, 0x01, false, true);
    REQUIRE(rows.size() == 1);
    CHECK((rows[0].first == 7 && !rows[0].second.skipRow));
    // Single density with an NBG, woven or deinterlaced: the odd row is excluded.
    rows = rowsOf(kDisp | kLSMDSingle, 0x01, false, true);
    CHECK((rows[0].first == 7 && rows[0].second.skipRow));
    rows = rowsOf(kDisp | kLSMDSingle, 0x01, false, false);
    CHECK((rows[0].first == 6 && !rows[0].second.skipRow));
    rows = rowsOf(kDisp | kLSMDSingle, 0x01, true, false);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].second.skipRow);
}

TEST_CASE("Scene: CompareSamplePoints skips the odd columns of a hi-res RBG", "[gpu][vdp2][scene][scale]") {
    // Hi-res RBG0: the odd columns' sample points are half an RBG dot on (skipped), the even ones match.
    auto device = OpenDevice();
    const SceneResult result = CompareSamplePoints(Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                                                       Base(w, s, rng, static_cast<uint16_t>(kDisp | 2));
                                                       SetupRBG0(w, s, {}, rng);
                                                   }),
                                                   *device, 2);
    INFO(result.Describe());
    CHECK(result.error.empty());
    CHECK(result.identical);
    CHECK(result.skippedPixels == uint64_t{result.height} * (result.width / 2));
    CloseAndCheckValidation(device);
}

// Bench (hidden, run with "[bench]"): GPU time per frame at N = 1-4 of the sub-pixel paths: zoomed, finely scrolled
// NBG0 and NBG1 over sprites, and RBG0 with per-dot coefficients and the line colour screen (a new Xst every frame).
// Prints the median of `frames` frames per N; no threshold. Every frame must still match software at the sample points.
TEST_CASE("Bench: sub-pixel backgrounds at N", "[gpu][vdp2][bench][.]") {
    constexpr uint32_t kFrames = 30;
    auto device = OpenDevice();
    for (const bool rbg : {false, true}) {
        for (uint32_t n = 1; n <= 4; ++n) {
            Vdp2Scene scene = rbg ? RBGLineColorScene(0, false) : Make([](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                Base(w, s, rng);
                TwoNBGs(w, s, rng);
                SetupSprites(w, {.type = 1}, rng);
                w.Reg(SCXDN0, 0x4500);
                w.Reg(ZMXDN0, 0xC000);
                w.Reg(ZMYDN0, 0xA000);
            });
            scene.frames = kFrames;
            scene.onLine = [rbg, frame = 0u](Vdp2SceneWriter &w, uint32_t y) mutable {
                if (y == 0) {
                    ++frame;
                    if (rbg) {
                        w.VRAM32(kRotTableAddress, static_cast<uint32_t>(frame * 1024) << 6);
                    } else {
                        w.Reg(SCXIN0, static_cast<uint16_t>(frame));
                    }
                }
            };
            std::vector<double> ms;
            RenderBothOptions options;
            options.scale = n;
            options.gpuMilliseconds = &ms;
            const SceneResult result = RenderBoth(scene, *device, options);
            CHECK(result.error.empty());
            CHECK(result.identical);
            if (ms.size() < 2) {
                WARN("no timestamps on this queue");
                continue;
            }
            std::vector<double> sorted(ms.begin() + 1, ms.end());
            std::sort(sorted.begin(), sorted.end());
            WARN((rbg ? "RBG0 coefficients and line colour" : "zoomed NBGs and sprites")
                 << " at " << n << "x: GPU " << sorted[sorted.size() / 2] << " ms/frame median, " << sorted.back()
                 << " max");
        }
    }
    CloseAndCheckValidation(device);
}

// Bench (hidden, run with "[bench]"): GPU time per frame of NBG1 with character pattern delay and windows, against the
// same layer without the delay. The delayed layer runs the NBG fetch pass (cs_vdp2_nbg_fetch.hlsl) before bgs in every
// batch, one batch per line here (stage 1B walked back over windowed pixels per pixel instead: 23-36 ms/frame with a
// window hiding most of the line on an RX 9060 XT). Prints the median and maximum of `frames` frames per configuration
// (timestamps around the frame's commands: uploads, sprite, fetch, bgs and compose dispatches) in ms/frame; no
// threshold (stage 1C target: at most 3 ms). Every configuration must still match software.
TEST_CASE("Bench: character pattern delay with windows", "[gpu][vdp2][bench][.]") {
    struct Config {
        const char *name;
        bool delay;
        uint16_t wctla;              // NBG1 window control (high byte)
        uint16_t wpsx0, wpex0;       // W0 horizontal range (in half dots)
        uint16_t wpsx1, wpex1;       // W1 horizontal range
    };
    const Config configs[] = {
        {"no delay, no window", false, 0, 0, 0, 0, 0},
        {"delay, no window", true, 0, 0, 0, 0, 0},
        {"no delay, W0 hides 8-311", false, 0x0200, 8 << 1, 311 << 1, 0, 0},
        {"delay, W0 hides 8-311", true, 0x0200, 8 << 1, 311 << 1, 0, 0},
        {"delay, W0 hides 0-318", true, 0x0200, 0, 318 << 1, 0, 0},
        {"delay, W0 or W1 hide 4-155 and 164-315", true, 0x0A00, 4 << 1, 155 << 1, 164 << 1, 315 << 1},
    };
    constexpr uint32_t kFrames = 60;
    auto device = OpenDevice();
    for (const bool cell2x2 : {false, true}) {
        for (const Config &config : configs) {
            Vdp2Scene scene = Make(
                [=](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
                    Base(w, s, rng);
                    SetupNBG(w, s, {.index = 1, .format = 0, .cell2x2 = cell2x2}, rng);
                    for (uint32_t reg : {CYCA0L, CYCA1L, CYCB0L, CYCB1L}) {
                        // As "(g) character pattern delay of 1x1 and 2x2 characters"
                        w.Reg(reg, config.delay ? 0x5FFF : 0x15FF);
                        w.Reg(reg + 2, config.delay ? 0xFFF1 : 0xFFFF);
                    }
                    if (config.wctla != 0) {
                        w.Reg(WPSX0, config.wpsx0);
                        w.Reg(WPEX0, config.wpex0);
                        w.Reg(WPSY0, 0);
                        w.Reg(WPEY0, 223);
                        w.Reg(WPSX1, config.wpsx1);
                        w.Reg(WPEX1, config.wpex1);
                        w.Reg(WPSY1, 0);
                        w.Reg(WPEY1, 223);
                        w.Reg(WCTLA, config.wctla);
                    }
                },
                {}, kFrames);
            // A different left edge on every line and frame: every frame re-renders every line.
            scene.onLine = [frame = 0u](Vdp2SceneWriter &w, uint32_t y) mutable {
                if (y == 0) {
                    ++frame;
                }
                w.Reg(SCXIN1, static_cast<uint16_t>(y / 3 + frame));
            };
            std::vector<double> ms;
            RenderBothOptions options;
            options.gpuMilliseconds = &ms;
            const SceneResult result = Check(*device, scene, options);
            if (ms.empty()) {
                WARN("no timestamps on this queue");
                continue;
            }
            // The first frame uploads everything
            std::vector<double> sorted(ms.begin() + 1, ms.end());
            std::sort(sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            WARN((cell2x2 ? "2x2 " : "1x1 ") << config.name << ": GPU " << median << " ms/frame median, "
                                              << sorted.back() << " max (" << sorted.size() << " frames"
                                              << (result.identical ? "" : ", DIFFERS from software") << ")");
        }
    }
    CloseAndCheckValidation(device);
}

// =====================================================================================================================
// VDP1 upscaling: the N x sprite pass's framebuffer mapping (GPU stage 3, Task 6; stage3-research.md section 3.2). A
// static scene: two magnified scaled sprites whose texels encode (u, v) are drawn once (VDP1 on the GPU at N x) and
// shown. The N x composite is decoded against the reference mapping below at every N x pixel, reading the N x surface
// as read back (itself compared with the N x reference by RenderBothFBRAM): normal, deinterlaced double density, the
// doubled 8-bit input, the doubled hi-res output, and rotation (lo-res and hi-res).

namespace {

enum class FbMapping { Normal, Deinterlace, HalfInput, DoubledOutput, Rotation, RotationHiRes };

struct FbMappingCase {
    const char *name;
    FbMapping mapping;
    uint16_t tvmd;
    uint16_t tvmr;
};

constexpr FbMappingCase kFbMappingCases[] = {
    {"normal", FbMapping::Normal, kDisp, 0},
    {"deinterlaced double density", FbMapping::Deinterlace, kDisp | kLSMDDouble, 0},
    {"8 bpp, doubled input", FbMapping::HalfInput, kDisp, 1},
    {"hi-res 640, doubled output", FbMapping::DoubledOutput, kDisp | 0x0002, 0},
    {"rotation", FbMapping::Rotation, kDisp, 2},
    {"rotation, hi-res 640", FbMapping::RotationHiRes, kDisp | 0x0002, 2},
};

uint32_t Xbgr555(uint16_t v) {
    return ((v & 31u) << 3) | (((v >> 5) & 31u) << 11) | (((v >> 10) & 31u) << 19);
}

uint16_t MappingCram(uint32_t k) {
    return static_cast<uint16_t>((k * 0x2E5u + 0x1111u) & 0x7FFF);
}

RotTable SpriteRotation() {
    RotTable t;
    const double angle = 0.4, scale = 0.55;
    t.xst = 3.25;
    t.yst = 2.5;
    t.dx = std::cos(angle) * scale;
    t.dy = std::sin(angle) * scale;
    t.dxst = -std::sin(angle) * scale;
    t.dyst = std::cos(angle) * scale;
    return t;
}

int32_t RotFixed(double value) {
    return static_cast<int32_t>(std::lround(value * 1024));
}

Vdp2Scene FbMappingScene(const FbMappingCase &c, std::shared_ptr<uint32_t> displayFB) {
    return Make(
        [c, displayFB](Vdp2SceneWriter &w, Shadow &s, std::mt19937 &rng) {
            Base(w, s, rng, c.tvmd);
            const bool eightBit = (c.tvmr & 1) != 0;
            for (uint32_t k = 0; k < 128; ++k) {
                w.CRAM16(k * 2, MappingCram(k));
            }
            // 16 bpp: mixed format (RGB words); 8 bpp: sprite type 8 (7-bit colour, CRAM)
            w.Reg(SPCTL, static_cast<uint16_t>(eightBit ? 0x0008 : 0x0020));
            for (uint32_t reg = PRISA; reg <= PRISD; reg += 2) {
                w.Reg(reg, 0x0707);
            }
            if (c.mapping == FbMapping::Rotation || c.mapping == FbMapping::RotationHiRes) {
                // Rotation parameter A (an RBG0 of priority 0 enables the tables; it is never shown)
                WriteRotTable(w, kRotTableAddress, SpriteRotation());
                w.Reg(RPTAU, static_cast<uint16_t>(kRotTableAddress >> 17));
                w.Reg(RPTAL, static_cast<uint16_t>((kRotTableAddress >> 1) & 0xFFFE));
                w.Reg(RPMD, 0);
                w.Reg(PRIR, 0);
                s.bgon |= 0x0010;
            }
            // A 64 x 64 texture whose texels encode (u, v): RGB words, or 8-bit colours 1-125. Shrunk sprites give the
            // N x surface sub-texel detail (a magnified texel boundary stays on the native pixel's block)
            std::vector<uint8_t> texture;
            for (uint32_t v = 0; v < 64; ++v) {
                for (uint32_t u = 0; u < 64; ++u) {
                    if (eightBit) {
                        texture.push_back(static_cast<uint8_t>(1 + (u * 3 + v * 7) % 125));
                    } else {
                        const uint16_t word = static_cast<uint16_t>(0x8000 | u | (v << 6) | (((u ^ v) & 7) << 12));
                        texture.push_back(static_cast<uint8_t>(word >> 8));
                        texture.push_back(static_cast<uint8_t>(word));
                    }
                }
            }
            constexpr uint32_t kTexture = 0x10000;
            w.VDP1VRAMBytes(kTexture, texture);
            const uint16_t pmod = eightBit ? 0x0020 : 0x0028; // colour mode 4 (256 colours) or 5 (RGB)
            uint32_t slot = 0;
            const auto put = [&](const Vdp1Cmd &cmd) { w.VDP1Command(0x20 * slot++, cmd); };
            put(Vdp1Cmd::SystemClip(eightBit ? 1023 : 511, 255));
            put(Vdp1Cmd::Local(0, 0));
            put(Vdp1Cmd::Scaled(pmod, 0, kTexture / 8, Vdp1Size(64, 64), 60, 120, 170, 200)); // magnified
            put(Vdp1Cmd::Scaled(pmod, 0, kTexture / 8, Vdp1Size(64, 64), 13, 9, 52, 44));     // shrunk
            Vdp1Cmd flipped = Vdp1Cmd::Scaled(pmod, 0, kTexture / 8, Vdp1Size(64, 64), 180, 120, 229, 149);
            flipped.Flip(true, false);
            put(flipped);
            put(Vdp1Cmd::Scaled(pmod, 0, kTexture / 8, Vdp1Size(64, 64), 240, 20, 299, 99)); // shrunk across, tall
            put(Vdp1Cmd::Scaled(pmod, 0, kTexture / 8, Vdp1Size(64, 64), 100, 10, 120, 26)); // shrunk 3x
            put(Vdp1Cmd::End());
            w.VDP1Reg(TVMR, c.tvmr);
            w.VDP1Reg(FBCR, 0);
            *displayFB = w.DrawFramebuffer();
            w.DrawVDP1();
            w.SwapFramebuffers();
        },
        {.deinterlace = c.mapping == FbMapping::Deinterlace}, 2);
}

// The colour the frame shows at N x pixel (X, Y) by the reference mapping, from the N x surface; back: the back screen
struct MappingCheck {
    uint64_t mismatches = 0, sprite = 0, detail = 0;
    std::string first;
    std::vector<uint32_t> colors;
};

MappingCheck CheckFbMapping(const FbMappingCase &c, uint32_t n, uint32_t displayFB, const SceneResult &frame,
                            std::span<const uint8_t> surface) {
    MappingCheck m;
    const uint32_t width = frame.width, height = frame.height;
    const bool eightBit = (c.tvmr & 1) != 0;
    const bool rotate = c.mapping == FbMapping::Rotation || c.mapping == FbMapping::RotationHiRes;
    const bool hiRes = width >= 640;
    const uint32_t fbWidth = eightBit ? 1024 : 512, fbHeight = 256;
    const uint32_t sliceSize = gpu::vdp1::Vdp1NxFbramSliceSize(n);
    const uint32_t back = Xbgr555(0x1234);
    const RotTable t = SpriteRotation();
    const int32_t xst = RotFixed(t.xst), yst = RotFixed(t.yst), dxst = RotFixed(t.dxst), dyst = RotFixed(t.dyst),
                  dx = RotFixed(t.dx), dy = RotFixed(t.dy);
    const auto expected = [&](uint32_t X, uint32_t Y) -> uint32_t {
        const uint32_t x = X / n, row = Y / n, i = X % n, j = Y % n;
        uint32_t fieldLine = row, field = 0;
        if (c.mapping == FbMapping::Deinterlace) {
            fieldLine = row >> 1;
            field = row & 1;
        }
        int64_t fx = 0, fy = 0; // FB_N
        if (rotate) {
            const uint32_t rx = hiRes ? x >> 1 : x;
            const int64_t cx = xst + int64_t(fieldLine) * dxst + int64_t(rx) * dx;
            const int64_t cy = yst + int64_t(fieldLine) * dyst + int64_t(rx) * dy;
            if (hiRes) {
                const int64_t sx = (x & 1) * n + i;
                fx = (2 * n * cx + sx * dx + 2 * int64_t(j) * dxst) >> 11;
                fy = (2 * n * cy + sx * dy + 2 * int64_t(j) * dyst) >> 11;
            } else {
                fx = (n * cx + int64_t(i) * dx + int64_t(j) * dxst) >> 10;
                fy = (n * cy + int64_t(i) * dy + int64_t(j) * dyst) >> 10;
            }
            if (fx < 0 || fy < 0 || fx / n >= fbWidth || fy / n >= fbHeight) {
                return back;
            }
        } else {
            fy = int64_t(n) * fieldLine + j;
            switch (c.mapping) {
            case FbMapping::HalfInput: fx = 2 * (int64_t(n) * x + i); break;
            case FbMapping::DoubledOutput: fx = int64_t(n) * (x >> 1) + ((x & 1) * n + i) / 2; break;
            default: fx = int64_t(n) * x + i; break;
            }
        }
        const uint32_t nx = static_cast<uint32_t>(fx / n), ny = static_cast<uint32_t>(fy / n);
        const uint32_t sub = static_cast<uint32_t>(fy % n) * n + static_cast<uint32_t>(fx % n);
        const uint32_t slice = displayFB + field * 2;
        if (eightBit) {
            const uint32_t a = (nx + ny * fbWidth) & 0x3FFFF;
            const uint8_t b = surface[slice * sliceSize + ((a >> 1) * n * n + sub) * 2 + (a & 1)];
            return b == 0 ? back : Xbgr555(MappingCram(b & 0x7F));
        }
        const uint32_t word = (nx + ny * fbWidth) & 0x1FFFF;
        const uint32_t at = slice * sliceSize + (word * n * n + sub) * 2;
        const uint16_t v = static_cast<uint16_t>((surface[at] << 8) | surface[at + 1]);
        return (v & 0x8000) != 0 ? Xbgr555(v) : back;
    };
    const uint32_t pitch = width * n;
    for (uint32_t Y = 0; Y < height * n; ++Y) {
        for (uint32_t X = 0; X < pitch; ++X) {
            const uint32_t want = expected(X, Y);
            const uint32_t got = frame.gpuScaled[Y * pitch + X] & 0xFFFFFF;
            if (want != back) {
                ++m.sprite;
                if (m.colors.size() < 256 && std::find(m.colors.begin(), m.colors.end(), want) == m.colors.end()) {
                    m.colors.push_back(want);
                }
            }
            if (want != expected(X - X % n, Y - Y % n)) {
                ++m.detail;
            }
            if (got != want && m.mismatches++ == 0) {
                std::ostringstream text;
                text << std::hex << "at N x (" << std::dec << X << ", " << Y << "): expected 0x" << std::hex << want
                     << ", got 0x" << got;
                m.first = text.str();
            }
        }
    }
    return m;
}

} // namespace

TEST_CASE("Scene: VDP1 upscaling, the N x sprite pass's framebuffer mapping", "[gpu][vdp2][scene][nx][nxsprites]") {
    auto device = OpenDevice();
    const bool cpu = device && device->Properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    for (const FbMappingCase &c : kFbMappingCases) {
        for (const uint32_t n : {2u, 3u, 4u}) {
            if (cpu && n == 3) {
                continue;
            }
            DYNAMIC_SECTION(c.name << " at " << n << "x") {
                auto displayFB = std::make_shared<uint32_t>(0);
                std::vector<uint8_t> surface;
                RenderBothOptions options;
                options.vdp1Upscaling = true;
                options.scale = n;
                options.nxInspect = [&](const std::string &, uint32_t scale, std::span<const uint8_t> s,
                                        std::span<const uint8_t>) {
                    if (scale == n) {
                        surface.assign(s.begin(), s.end());
                    }
                };
                const brimir::test::FbramResult r =
                    brimir::test::RenderBothFBRAM(FbMappingScene(c, displayFB), *device, options);
                INFO(r.Describe());
                REQUIRE(r.error.empty());
                REQUIRE(r.frame.error.empty());
                CHECK(r.identical);
                CHECK(r.nxCount == 0);
                CHECK(r.nxDetailBlocks > 0);
                REQUIRE(r.frame.scale == n);
                REQUIRE(surface.size() == gpu::vdp1::Vdp1NxFbramBufferSize(n));
                REQUIRE(r.frame.gpuScaled.size() == size_t{r.frame.width} * r.frame.height * n * n);
                const MappingCheck m = CheckFbMapping(c, n, *displayFB, r.frame, surface);
                INFO(m.first);
                CHECK(m.mismatches == 0);
                CHECK(m.sprite > 10000);
                CHECK(m.colors.size() >= 100);
                // Sub-texel detail visible in the frame (at 2x a doubled output pair reads one N x column per pixel)
                if (c.mapping != FbMapping::DoubledOutput || n > 2) {
                    CHECK(m.detail > 100);
                }
                if (c.mapping == FbMapping::Normal || c.mapping == FbMapping::Deinterlace ||
                    c.mapping == FbMapping::HalfInput) {
                    // The sample points read sub-texel (0, 0): 1x software there
                    CHECK(r.frame.identical);
                }
            }
        }
    }
    CloseAndCheckValidation(device);
}
