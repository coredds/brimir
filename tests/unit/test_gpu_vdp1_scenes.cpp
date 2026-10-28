// GPU VDP1 scenes (GPU stage 2, Task 5): the Vulkan renderer with VDP1 on the GPU (VulkanVDPRenderer, Vdp1Mode::Gpu:
// the VDP1 front half's op lists replayed by Vdp1VulkanBackend into the VDP2 back half's sprite framebuffer) against
// the software renderer, with the same writes. Every scene compares
// - the GPU FBRAM (main fb0/fb1, alt fb0/fb1, mesh[alt][fb]) with software's byte for byte after every frame and at
//   every checkpoint (vdp2_scene.hpp RenderBothFBRAM; non-vacuous: software's FBRAM is not all zero), and
// - every frame's composite with software's frame.
// The VDP's VDP1 frame control is reproduced per frame (vdp.cpp:864-901 at the last line, 933-947 in blanking): the
// VBlank erase, the 1-cycle or manual erase and swap (FCM, FCT), the draw after the swap, the display erase after the
// VDP2 frame's end (a trailing op). Scenes cover every command type, colour mode and draw mode, double density with and
// without deinterlacing (doubleV, alternate fields, DIE/DIL), 8 bpp, erases with the cycle limit and clamps, manual
// swaps, erases without a swap, a game that stops swapping, mid-frame register, VRAM and CPU framebuffer writes between
// commands, a dropped command buffer, a save and load between commands, and seeded random command tables driven through
// the renderer (vdp1_scenario.hpp) with FBRAM compared after every take.
// [gpu][vdp1][scene]; SKIP without a Vulkan device. The random tables run 40 seeds (10 on a CPU device such as lavapipe;
// BRIMIR_VDP1_GPU_SEEDS=N for N).
#include "catch_amalgamated.hpp"

#include "gpu_test_device.hpp"
#include "vdp1_scenario.hpp"
#include "vdp2_scene.hpp"

#include <gpu/vdp/vulkan_vdp_renderer.hpp>
#include <gpu/vk_util.hpp>

#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using brimir::test::CloseAndCheckValidation;
using brimir::test::FbramResult;
using brimir::test::OpenDevice;
using brimir::test::ReadbackBuffer;
using brimir::test::RenderBothFBRAM;
using brimir::test::RenderBothOptions;
using brimir::test::Vdp1Cmd;
using brimir::test::Vdp1Size;
using brimir::test::Vdp2Scene;
using brimir::test::Vdp2SceneWriter;

namespace {

namespace gpu = brimir::gpu;
using ymir::vdp::VDPState;

// VDP2 registers
constexpr uint32_t TVMD = 0x000, BGON = 0x020, BKTAU = 0x0AC, BKTAL = 0x0AE, SPCTL = 0x0E0, PRISA = 0x0F0,
                   PRISB = 0x0F2, PRISC = 0x0F4, PRISD = 0x0F6;
// VDP1 registers
constexpr uint32_t TVMR = 0x00, FBCR = 0x02, PTMR = 0x04, EWDR = 0x06, EWLR = 0x08, EWRR = 0x0A;
// FBCR bits
constexpr uint16_t kFCT = 1, kFCM = 2, kDIL = 4, kDIE = 8, kEOS = 16;
// TVMD values
constexpr uint16_t kDisp = 0x8000, kDoubleDensity = 0x00C0;

// VDP1 VRAM layout of the scenes: the command table at 0, look-up tables, Gouraud tables, textures
constexpr uint32_t kLUTBase = 0x8000;
constexpr uint32_t kGouraudBase = 0x9000;
constexpr uint32_t kTextureBase = 0x10000;

uint32_t Rand(std::mt19937 &rng, uint32_t bound) {
    return std::uniform_int_distribution<uint32_t>(0, bound - 1)(rng);
}
int RandI(std::mt19937 &rng, int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}

// Display on, back colour, random CRAM, the sprite layer alone (mixed palette/RGB format or an 8-bit type), every
// sprite priority visible.
void Display(Vdp2SceneWriter &w, std::mt19937 &rng, uint16_t tvmd = kDisp, uint16_t spctl = 0x0020) {
    w.Reg(TVMD, tvmd);
    w.VRAM16(0x7FFF0, 0x1234);
    w.Reg(BKTAU, static_cast<uint16_t>((0x7FFF0 >> 17) & 7));
    w.Reg(BKTAL, static_cast<uint16_t>((0x7FFF0 >> 1) & 0xFFFF));
    for (uint32_t a = 0; a < 0x1000; a += 2) {
        w.CRAM16(a, static_cast<uint16_t>(rng()));
    }
    w.Reg(SPCTL, spctl);
    for (uint32_t reg = PRISA; reg <= PRISD; reg += 2) {
        w.Reg(reg, static_cast<uint16_t>((1 + Rand(rng, 7)) | ((1 + Rand(rng, 7)) << 8)));
    }
    w.Reg(BGON, 0);
}

// Textures (16 x 16 in each colour mode), a look-up table and Gouraud tables in VDP1 VRAM.
struct Assets {
    std::array<uint16_t, 6> srca{}; // per colour mode
    uint16_t lut = kLUTBase / 8;
    std::array<uint16_t, 8> gouraud{};
};

Assets WriteAssets(Vdp2SceneWriter &w, std::mt19937 &rng) {
    Assets a;
    uint32_t address = kTextureBase;
    static constexpr uint32_t kBytes[6] = {128, 128, 256, 256, 256, 512};
    for (uint32_t cm = 0; cm < 6; ++cm) {
        std::vector<uint8_t> bytes(kBytes[cm]);
        for (uint8_t &b : bytes) {
            const uint32_t r = Rand(rng, 100);
            b = r < 10 ? 0x00 : r < 14 ? 0xFF : static_cast<uint8_t>(rng());
        }
        if (cm == 5) {
            for (size_t i = 0; i < bytes.size(); i += 2) {
                if (bytes[i] != 0 || bytes[i + 1] != 0) {
                    bytes[i] |= 0x80; // RGB, except transparent words and the odd end code
                }
                if (Rand(rng, 50) == 0) {
                    bytes[i] = 0x7F, bytes[i + 1] = 0xFF;
                }
            }
        }
        w.VDP1VRAMBytes(address, bytes);
        a.srca[cm] = static_cast<uint16_t>(address / 8);
        address += kBytes[cm];
    }
    std::array<uint16_t, 16> lut{};
    for (uint16_t &c : lut) {
        c = static_cast<uint16_t>(Rand(rng, 4) == 0 ? Rand(rng, 0x800) : 0x8000 | Rand(rng, 0x8000));
    }
    w.VDP1LUT(kLUTBase, lut);
    for (uint32_t i = 0; i < a.gouraud.size(); ++i) {
        std::array<uint16_t, 4> g{};
        for (uint16_t &c : g) {
            c = static_cast<uint16_t>(Rand(rng, 0x8000));
        }
        w.VDP1Gouraud(kGouraudBase + i * 8, g);
        a.gouraud[i] = static_cast<uint16_t>((kGouraudBase + i * 8) / 8);
    }
    return a;
}

// A random draw command (every command type, colour mode and draw mode).
Vdp1Cmd RandomDraw(std::mt19937 &rng, const Assets &a, int maxX, int maxY) {
    const uint32_t cm = Rand(rng, 6);
    uint16_t pmod = static_cast<uint16_t>((cm << 3) | Rand(rng, 8));
    if (Rand(rng, 5) == 0) pmod |= 0x0040; // transparent pixels drawn
    if (Rand(rng, 5) == 0) pmod |= 0x0080; // end codes ignored
    if (Rand(rng, 5) == 0) pmod |= 0x0100; // mesh
    if (Rand(rng, 7) == 0) pmod |= static_cast<uint16_t>(0x0400 | (Rand(rng, 2) << 9)); // user clipping, either mode
    if (Rand(rng, 5) == 0) pmod |= 0x1000; // high-speed shrink
    if (Rand(rng, 10) == 0) pmod |= 0x8000; // MSB on
    const uint16_t bank = static_cast<uint16_t>(Rand(rng, 0x80) << 4);
    const uint16_t colr = cm == 1 ? a.lut : cm == 5 ? 0 : bank;
    const uint16_t rgb = static_cast<uint16_t>(Rand(rng, 2) ? 0x8000 | Rand(rng, 0x8000) : Rand(rng, 0x10000) & 0x7FFF);
    const auto x = [&] { return RandI(rng, -24, maxX + 8); };
    const auto y = [&] { return RandI(rng, -24, maxY + 8); };
    Vdp1Cmd cmd;
    switch (Rand(rng, 8)) {
    case 0: cmd = Vdp1Cmd::Normal(pmod, colr, a.srca[cm], Vdp1Size(16, 16), x(), y()); break;
    case 1: {
        const int ax = x(), ay = y();
        cmd = Vdp1Cmd::Scaled(pmod, colr, a.srca[cm], Vdp1Size(16, 16), ax, ay, ax + RandI(rng, -40, 60),
                              ay + RandI(rng, -40, 60));
        break;
    }
    case 2: {
        static constexpr uint16_t kZoom[] = {5, 6, 7, 9, 10, 11, 13, 14, 15};
        cmd = Vdp1Cmd::ScaledZoom(kZoom[Rand(rng, 9)], pmod, colr, a.srca[cm], Vdp1Size(16, 16), x(), y(),
                                  RandI(rng, -50, 50), RandI(rng, -50, 50));
        break;
    }
    case 3: {
        // Convex, concave, bowtie, triangle (C = D) or a point
        const int ax = x(), ay = y();
        int bx = ax + RandI(rng, 0, 60), by = ay + RandI(rng, -10, 10);
        int cx = bx + RandI(rng, -20, 20), cy = by + RandI(rng, 0, 60);
        int dx = ax + RandI(rng, -20, 20), dy = cy + RandI(rng, -10, 10);
        const uint32_t shape = Rand(rng, 6);
        if (shape == 0) std::swap(cx, dx);                // bowtie
        if (shape == 1) dx = cx, dy = cy;                 // triangle
        if (shape == 2) bx = cx = dx = ax, by = cy = dy = ay; // a point
        cmd = Vdp1Cmd::Distorted(pmod, colr, a.srca[cm], Vdp1Size(16, 16), ax, ay, bx, by, cx, cy, dx, dy);
        break;
    }
    case 4:
    case 5: {
        const int ax = x(), ay = y(), wd = RandI(rng, 4, 90), ht = RandI(rng, 4, 70);
        cmd = Vdp1Cmd::Polygon(static_cast<uint16_t>(pmod & ~0x0038), rgb, ax, ay, ax + wd, ay + RandI(rng, -8, 8),
                               ax + wd + RandI(rng, -8, 8), ay + ht, ax + RandI(rng, -8, 8), ay + ht);
        break;
    }
    case 6:
        cmd = Vdp1Cmd::Polyline(static_cast<uint16_t>(pmod & ~0x0038), rgb, x(), y(), x(), y(), x(), y(), x(), y());
        break;
    default: cmd = Vdp1Cmd::Line(static_cast<uint16_t>(pmod & ~0x0038), rgb, x(), y(), x(), y()); break;
    }
    if ((pmod & 0x0004) != 0) {
        cmd.Gouraud(a.gouraud[Rand(rng, static_cast<uint32_t>(a.gouraud.size()))]);
    }
    cmd.Flip(Rand(rng, 2) != 0, Rand(rng, 2) != 0);
    return cmd;
}

// A command table at 0: system clip, user clip, local coordinates, `count` random draws (with a few clip and local
// coordinate changes and a skipped command among them), an overlapping half-transparent stack, end.
void WriteTable(Vdp2SceneWriter &w, std::mt19937 &rng, const Assets &a, uint32_t count, int maxX = 320,
                int maxY = 224) {
    uint32_t slot = 0;
    const auto put = [&](const Vdp1Cmd &cmd) { w.VDP1Command(0x20 * slot++, cmd); };
    put(Vdp1Cmd::SystemClip(maxX - 1, maxY - 1));
    put(Vdp1Cmd::UserClip(RandI(rng, 0, 80), RandI(rng, 0, 60), RandI(rng, 160, 300), RandI(rng, 120, 200)));
    put(Vdp1Cmd::Local(RandI(rng, -8, 16), RandI(rng, -8, 16)));
    for (uint32_t i = 0; i < count; ++i) {
        switch (Rand(rng, 24)) {
        case 0: put(Vdp1Cmd::Local(RandI(rng, -16, 24), RandI(rng, -16, 24))); break;
        case 1: put(Vdp1Cmd::SystemClip(RandI(rng, 100, 0x3FF), RandI(rng, 80, 0x3FF))); break; // large raw values too
        case 2: put(RandomDraw(rng, a, maxX, maxY).Skip()); break;
        default: put(RandomDraw(rng, a, maxX, maxY)); break;
        }
    }
    // Overlapping half-transparent polygons (order matters on every covered word)
    const int sx = RandI(rng, 20, 200), sy = RandI(rng, 20, 120);
    for (uint32_t i = 0; i < 6; ++i) {
        const int o = static_cast<int>(i) * 3;
        put(Vdp1Cmd::Polygon(0x0003, static_cast<uint16_t>(0x8000 | Rand(rng, 0x8000)), sx + o, sy + o, sx + 60 + o,
                             sy + o, sx + 60 + o, sy + 40 + o, sx + o, sy + 40 + o));
    }
    put(Vdp1Cmd::End());
}

// The VDP's VDP1 frame control, per frame of a scene (vdp.cpp): at the frame's last line (before its line 0) the
// VBlank erase (when VBE was set at VBlank IN), then the erase/swap decision from FCM/FCT (1-cycle mode: erase and
// swap; manual: swap on FCT, else erase, once per FBCR write), then the draw; after the VDP2 frame's end (blanking)
// the display erase.
struct FrameControl {
    // Register writes the program makes for frame f, before its last line (FBCR, TVMR, erase registers)
    std::function<void(Vdp2SceneWriter &, std::mt19937 &, uint32_t f)> program;
    // The VDP1 drawing of frame f (plot trigger on swap, or the program's PTMR write), after the swap decision
    std::function<void(Vdp2SceneWriter &, std::mt19937 &, uint32_t f, bool swapped)> draw;
    // Writes before RenderLine(y) of frame f
    std::function<void(Vdp2SceneWriter &, std::mt19937 &, uint32_t f, uint32_t y)> line;
    uint64_t vblankEraseCycles = 4000; // the VBlank erase budget (ends mid-row)
};

Vdp2Scene Vdp1Scene(std::function<void(Vdp2SceneWriter &, std::mt19937 &)> setup, FrameControl control,
                    uint32_t frames, ymir::vdp::config::Enhancements enhancements = {}, uint32_t seed = 1) {
    struct Ctl {
        uint32_t frame = 0;
        bool doDisplayErase = false;
        bool doVBlankErase = false;
        std::mt19937 rng;
    };
    auto ctl = std::make_shared<Ctl>();
    Vdp2Scene scene;
    scene.frames = frames;
    scene.enhancements = enhancements;
    scene.setup = [=](Vdp2SceneWriter &w) {
        *ctl = Ctl{};
        ctl->rng.seed(seed);
        setup(w, ctl->rng);
    };
    scene.onLine = [=](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            const uint32_t f = ctl->frame++;
            if (control.program) {
                control.program(w, ctl->rng, f);
            }
            // vdp.cpp:864-901 (every state holds the same registers; each clears its trigger)
            bool swapMode = false, trigger = false, changed = false, first = true;
            w.ForEachState([&](VDPState &s) {
                if (first) {
                    swapMode = s.regs1.fbSwapMode;
                    trigger = s.regs1.fbSwapTrigger;
                    changed = s.regs1.fbParamsChanged;
                    first = false;
                }
                s.regs1.fbParamsChanged = false;
            });
            bool erase = false, swap = false;
            if (!swapMode) {
                erase = swap = true;
            } else if (changed) {
                (trigger ? swap : erase) = true;
            }
            if (ctl->doVBlankErase) {
                w.EraseFramebuffer(control.vblankEraseCycles);
            }
            if (erase) {
                ctl->doDisplayErase = true;
            }
            if (swap) {
                w.SwapFramebuffers();
            }
            if (control.draw) {
                control.draw(w, ctl->rng, f, swap);
            }
        }
        if (control.line) {
            control.line(w, ctl->rng, ctl->frame - 1, y);
        }
    };
    scene.blanking = [=](Vdp2SceneWriter &w) {
        // VBlank IN latches VBE (vdp.cpp:835); the display erase follows the frame's end (vdp.cpp:941-946)
        bool vbe = false, first = true;
        w.ForEachState([&](VDPState &s) {
            if (first) {
                vbe = s.regs1.tvmr.vblankErase;
                first = false;
            }
        });
        ctl->doVBlankErase = vbe;
        if (ctl->doDisplayErase) {
            ctl->doDisplayErase = false;
            w.EraseFramebuffer(0);
        }
    };
    return scene;
}

// Erase registers: a random region (sometimes empty or one row, sometimes past the 400/428 clamps) and value.
void RandomErase(Vdp2SceneWriter &w, std::mt19937 &rng) {
    w.VDP1Reg(EWDR, static_cast<uint16_t>(Rand(rng, 3) == 0 ? 0 : 0x8000 | Rand(rng, 0x8000)));
    const uint32_t x1 = Rand(rng, 20), y1 = Rand(rng, 100);
    uint32_t x3 = x1 + Rand(rng, 64), y3 = y1 + Rand(rng, 200);
    switch (Rand(rng, 6)) {
    case 0: x3 = x1; break; // x1 == x3: nothing
    case 1: y3 = y1; break; // one row
    case 2: x3 = 0x7F, y3 = 0x1FF; break; // past the clamps
    default: break;
    }
    w.VDP1Reg(EWLR, static_cast<uint16_t>((x1 << 9) | y1));
    w.VDP1Reg(EWRR, static_cast<uint16_t>(((x3 & 0x7F) << 9) | (y3 & 0x1FF)));
}

// Checks a scene: FBRAM after every frame and checkpoint, every frame's composite (skipFrame: one not compared).
void RunFbramScene(const Vdp2Scene &scene, RenderBothOptions options = {}, bool requireColors = true,
                   uint32_t skipFrame = UINT32_MAX) {
    auto device = OpenDevice();
    options.compareFrame = [skipFrame](uint32_t f) { return f != skipFrame; };
    const FbramResult r = RenderBothFBRAM(scene, *device, options);
    INFO(r.Describe());
    CHECK(r.error.empty());
    CHECK(r.frame.error.empty());
    if (requireColors) {
        CHECK(r.frame.distinctColors > 1);
    }
    CHECK(r.frame.identical);
    CHECK(r.identical);
    CHECK(r.maxNonZero > 0);
    CHECK(r.checkpoints >= scene.frames - (skipFrame < scene.frames ? 1 : 0));
    CloseAndCheckValidation(device);
}

// The 1-cycle mode (FCM = 0) with a full-screen erase and a fresh random table every frame.
FrameControl AutoSwap(uint32_t count, int maxX = 320, int maxY = 224, uint16_t fbcr = 0) {
    FrameControl c;
    c.program = [=](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t) {
        w.VDP1Reg(FBCR, fbcr);
        w.VDP1Reg(EWDR, static_cast<uint16_t>(Rand(rng, 2) ? 0 : 0x8000 | Rand(rng, 0x8000)));
        w.VDP1Reg(EWLR, 0);
        w.VDP1Reg(EWRR, static_cast<uint16_t>((50 << 9) | 0x1FF));
    };
    c.draw = [=](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool) {
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, count, maxX, maxY);
        w.DrawVDP1();
    };
    return c;
}

} // namespace

TEST_CASE("GPU VDP1 scene: every command type, colour mode and draw mode", "[gpu][vdp1][scene]") {
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, AutoSwap(60), 4));
}

TEST_CASE("GPU VDP1 scene: transparent meshes", "[gpu][vdp1][scene]") {
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, AutoSwap(60), 4,
                            {.transparentMeshes = true}, 2));
}

TEST_CASE("GPU VDP1 scene: double density with deinterlacing (doubleV, alternate fields)", "[gpu][vdp1][scene]") {
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); },
                            AutoSwap(50), 4, {.deinterlace = true, .transparentMeshes = true}, 3));
}

TEST_CASE("GPU VDP1 scene: double interlace (DIE, DIL per field)", "[gpu][vdp1][scene]") {
    for (const bool deinterlace : {false, true}) {
        DYNAMIC_SECTION("deinterlace " << deinterlace) {
            FrameControl c = AutoSwap(50, 320, 448);
            const auto program = c.program;
            c.program = [program](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f) {
                program(w, rng, f);
                w.VDP1Reg(FBCR, static_cast<uint16_t>(kDIE | ((f & 1) ? kDIL : 0)));
            };
            RunFbramScene(
                Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); }, c,
                          4, {.deinterlace = deinterlace}, 4));
        }
    }
}

TEST_CASE("GPU VDP1 scene: 8 bpp framebuffers (1024 wide, rotation 512 tall)", "[gpu][vdp1][scene]") {
    for (const uint16_t tvmr : {uint16_t{1}, uint16_t{3}}) {
        DYNAMIC_SECTION("TVMR " << tvmr) {
            FrameControl c = AutoSwap(50);
            const auto program = c.program;
            c.program = [program, tvmr](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f) {
                program(w, rng, f);
                w.VDP1Reg(TVMR, tvmr);
            };
            // A rotated framebuffer (TVMR 3) is not shown without a rotation background: FBRAM only then.
            RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp, 0x0008); }, c,
                                    4, {}, 5 + tvmr),
                          {}, tvmr == 1);
        }
    }
}

TEST_CASE("GPU VDP1 scene: erases with the VBlank cycle limit, empty regions and the clamps", "[gpu][vdp1][scene]") {
    // Manual erase and swap with VBE (TVMR.VBE, FCM, FCT = 1, 1, 1): the VBlank erase at the last line (ending mid-row
    // after 4,000 cycles), then the swap; a random erase region each frame, latched at the swap.
    FrameControl c;
    c.program = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f) {
        RandomErase(w, rng);
        w.VDP1Reg(TVMR, static_cast<uint16_t>(f & 1 ? 0x0008 : 0x0000));
        w.VDP1Reg(FBCR, kFCM | kFCT);
    };
    c.draw = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool) {
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 40);
        w.DrawVDP1();
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 6, {}, 9));
}

TEST_CASE("GPU VDP1 scene: manual swap (FCM = 1, FCT = 1) every other frame", "[gpu][vdp1][scene]") {
    FrameControl c;
    c.program = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f) {
        if ((f & 1) == 0) {
            RandomErase(w, rng);
            w.VDP1Reg(FBCR, kFCM | kFCT);
        }
    };
    c.draw = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool swapped) {
        if (swapped) {
            const Assets a = WriteAssets(w, rng);
            WriteTable(w, rng, a, 40);
            w.DrawVDP1();
        }
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 6, {}, 10));
}

TEST_CASE("GPU VDP1 scene: erase without swap (FCM = 1, FCT = 0)", "[gpu][vdp1][scene]") {
    // Frames 0-1 swap (1-cycle mode); from frame 2 the program erases without swapping every frame: the display
    // framebuffer is erased after each frame (a trailing op), while VDP1 keeps drawing into the draw framebuffer.
    FrameControl c;
    c.program = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f) {
        RandomErase(w, rng);
        w.VDP1Reg(FBCR, f < 2 ? 0 : kFCM);
    };
    c.draw = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool) {
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 30);
        w.DrawVDP1();
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 6, {}, 11));
}

TEST_CASE("GPU VDP1 scene: a game that stops swapping", "[gpu][vdp1][scene]") {
    // 1-cycle mode for three frames, then manual mode without any further FBCR write (no swap, no erase): the game draws
    // into the draw framebuffer (PTMR) and writes it with the CPU every frame, and VDP2 keeps showing the same one.
    FrameControl c;
    c.program = [](Vdp2SceneWriter &w, std::mt19937 &, uint32_t f) {
        if (f < 3) {
            w.VDP1Reg(FBCR, 0);
        } else if (f == 3) {
            w.VDP1Reg(FBCR, kFCM);
        }
    };
    c.draw = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, bool) {
        if (f >= 3) {
            w.VDP1Reg(PTMR, 1);
        }
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 30);
        w.DrawVDP1();
    };
    c.line = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, uint32_t y) {
        if (f >= 3 && y % 16 == 5) {
            for (uint32_t i = 0; i < 64; ++i) {
                w.FBRAM16(w.DrawFramebuffer(), (Rand(rng, 0x20000) * 2) & 0x3FFFE,
                          static_cast<uint16_t>(0x8000 | Rand(rng, 0x8000)));
            }
        }
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 7, {}, 12));
}

TEST_CASE("GPU VDP1 scene: register, VRAM and CPU writes between commands", "[gpu][vdp1][scene]") {
    // The frame's table is drawn in parts across the frame's lines; between them: TVMR 8 bpp toggles, FBCR DIE/DIL/EOS
    // changes, VRAM writes to a texture, the look-up table, a Gouraud table and a later command table entry, CPU
    // framebuffer writes, and FBRAM checkpoints.
    FrameControl c;
    c.program = [](Vdp2SceneWriter &w, std::mt19937 &, uint32_t) { w.VDP1Reg(FBCR, 0); };
    auto assets = std::make_shared<Assets>();
    c.draw = [assets](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool) {
        *assets = WriteAssets(w, rng);
        WriteTable(w, rng, *assets, 60);
    };
    c.line = [assets](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, uint32_t y) {
        if (y % 20 != 0 || y / 20 > 9) {
            return;
        }
        const uint32_t part = y / 20;
        if (part > 0) {
            switch ((part + f) % 6) {
            case 0: w.VDP1Reg(TVMR, static_cast<uint16_t>(part & 1)); break;
            case 1: w.VDP1Reg(FBCR, static_cast<uint16_t>(Rand(rng, 2) ? kEOS : 0) | (Rand(rng, 2) ? kDIE | kDIL : 0));
                break;
            case 2: w.VDP1VRAM16(assets->srca[Rand(rng, 6)] * 8u + Rand(rng, 64) * 2, static_cast<uint16_t>(rng())); break;
            case 3: w.VDP1VRAM16(kLUTBase + Rand(rng, 16) * 2, static_cast<uint16_t>(0x8000 | rng())); break;
            case 4: w.VDP1VRAM16(kGouraudBase + Rand(rng, 32) * 2, static_cast<uint16_t>(rng() & 0x7FFF)); break;
            default:
                // the next command's colour (a command table write between commands)
                w.VDP1VRAM16(0x20 * (3 + part * 7) + 6, static_cast<uint16_t>(0x8000 | rng()));
                break;
            }
            for (uint32_t i = 0; i < 8; ++i) {
                w.FBRAM16(w.DrawFramebuffer(), (Rand(rng, 0x20000) * 2) & 0x3FFFE, static_cast<uint16_t>(rng()));
            }
        }
        w.DrawVDP1Partial(part * 7, part == 9 ? 64 : 7);
        w.Checkpoint();
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 4, {}, 13));
}

TEST_CASE("GPU VDP1 scene: CPU writes in double density with deinterlacing and transparent meshes",
          "[gpu][vdp1][scene]") {
    // Writes mirror to the alternate field and leave the meshes alone (software's VDP1WriteFB), between mesh draws.
    FrameControl c = AutoSwap(40);
    c.line = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, uint32_t y) {
        if (y % 32 == 7) {
            for (uint32_t i = 0; i < 256; ++i) {
                w.FBRAM16(w.DrawFramebuffer(), (Rand(rng, 0x20000) * 2) & 0x3FFFE, static_cast<uint16_t>(rng()));
            }
            w.Checkpoint();
        }
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); }, c,
                            4, {.deinterlace = true, .transparentMeshes = true}, 14));
}

TEST_CASE("GPU VDP1 scene: a dropped command buffer's VDP1 ops are replayed with the next one", "[gpu][vdp1][scene]") {
    RenderBothOptions options;
    options.dropFrame = 2;
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, AutoSwap(40), 5, {}, 15),
                  options, true, 2);
}

TEST_CASE("GPU VDP1 scene: save and load between commands", "[gpu][vdp1][scene]") {
    // TODO(Task 6) replaces the full drain the save uses (one sync submit and wait) with the CPU shadow.
    FrameControl c = AutoSwap(40);
    c.draw = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, bool) {
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 40);
        w.DrawVDP1Partial(0, 20);
        if (f == 1 || f == 2) {
            w.SaveLoadMidFrame();
        }
        w.DrawVDP1Partial(20, 64);
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 4,
                            {.deinterlace = true, .transparentMeshes = true}, 16));
}

// ---------------------------------------------------------------------------------------------------------------------
// Seeded random command tables (vdp1_scenario.hpp) driven through the renderer

namespace {

namespace vs = brimir::test::vdp1scene;

// Software and the Vulkan renderer (VDP1 on the GPU) in lockstep over a scenario's ops; at every take and at the end
// the pending VDP1 ops are submitted (ops-only) and the GPU FBRAM is compared with software's. Returns the first
// difference (empty if none); counts the compares and the non-zero software bytes.
std::string RunScenarioThroughRenderer(gpu::VulkanDevice &device, const vs::Scenario &s, uint32_t &compares,
                                       uint64_t &nonZero) {
    using namespace ymir;
    auto sw = std::make_unique<vs::SoftwareRun>();
    sw->renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*sw->state, sw->debug, sw->access);
    auto vkState = std::make_unique<VDPState>();
    vdp::config::VDP2DebugRender vkDebug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    auto vulkan = std::make_unique<gpu::VulkanVDPRenderer>(*vkState, vkDebug, access);
    vs::SetupState(s, *sw->state);
    vs::SetupState(s, *vkState);
    // Attach after the setup: entering Gpu mode uploads the CPU's framebuffers (the scenario's fill)
    vulkan->SetVdp1Mode(gpu::Vdp1Mode::Gpu);
    vulkan->AttachDevice(device, 1);
    if (!vulkan->GpuVdp1()) {
        return "VDP1 on the GPU unavailable";
    }
    vulkan->SetVdp1BatchLimitsForTest(s.limits);
    const vdp::config::Enhancements enh{.deinterlace = s.deinterlace, .transparentMeshes = s.transparentMeshes};
    std::array<vdp::IVDPRenderer *, 2> renderers{sw->renderer.get(), vulkan.get()};
    std::array<VDPState *, 2> states{sw->state.get(), vkState.get()};
    for (vdp::IVDPRenderer *r : renderers) {
        r->ConfigureEnhancements(enh);
        r->VDP2SetResolution(320, s.vres, false);
        r->PostLoadStateSync();
    }

    std::string diff;
    const auto compare = [&](const std::string &where) {
        if (!diff.empty()) {
            return;
        }
        if (vulkan->HasPendingVdp1Work()) {
            bool recorded = false;
            if (gpu::vk_util::SubmitOneShot(device, [&](VkCommandBuffer cmd) {
                    recorded = vulkan->RecordPendingVdp1Work(0, cmd);
                }) != VK_SUCCESS ||
                !recorded) {
                diff = where + ": the ops-only submit failed";
                return;
            }
        }
        const std::vector<uint32_t> words =
            ReadbackBuffer(device, vulkan->Backend()->SpriteFBBuffer(), 8 * vdp::kVDP1FBRAMSize / 4);
        if (words.size() != 8 * vdp::kVDP1FBRAMSize / 4) {
            diff = where + ": readback failed";
            return;
        }
        auto fb = std::make_unique<vs::ref::Fbram>();
        const auto *bytes = reinterpret_cast<const uint8_t *>(words.data());
        std::array<vdp::SpriteFB *, 8> slices{&fb->main[0],    &fb->main[1],    &fb->alt[0],     &fb->alt[1],
                                              &fb->mesh[0][0], &fb->mesh[0][1], &fb->mesh[1][0], &fb->mesh[1][1]};
        for (uint32_t i = 0; i < 8; ++i) {
            std::memcpy(slices[i]->data(), bytes + size_t{i} * vdp::kVDP1FBRAMSize, vdp::kVDP1FBRAMSize);
        }
        ++compares;
        for (uint32_t i = 0; i < 2; ++i) {
            nonZero += static_cast<uint64_t>(
                std::count_if(sw->Main(i).begin(), sw->Main(i).end(), [](uint8_t b) { return b != 0; }));
        }
        const std::string d = vs::CompareFbram(*sw, *fb);
        if (!d.empty()) {
            diff = where + ": " + d;
        }
    };

    uint32_t opIndex = 0;
    for (const vs::Op &op : s.ops) {
        ++opIndex;
        bool stateOp = false;
        for (VDPState *st : states) {
            stateOp = vs::ApplyStateOp(op, *st);
        }
        if (stateOp) {
            continue;
        }
        for (size_t k = 0; k < 2; ++k) {
            vdp::IVDPRenderer &r = *renderers[k];
            VDPState &state = *states[k];
            switch (op.kind) {
            case vs::Op::Kind::BeginFrame: r.VDP1BeginFrame(); break;
            case vs::Op::Kind::Command: {
                const vdp::VDP1Command::Control control{.u16 = util::ReadBE<uint16>(&state.mem1.VRAM[op.address])};
                r.VDP1ExecuteCommand(op.address, control);
                break;
            }
            case vs::Op::Kind::EndFrame: r.VDP1EndFrame(); break;
            case vs::Op::Kind::Erase: r.VDP1EraseFramebuffer(op.cycles); break;
            case vs::Op::Kind::WriteVRAM:
                state.mem1.WriteVRAM<uint16>(op.address, op.value, [&](uint32 a, uint16 v) { r.VDP1WriteVRAM(a, v); });
                break;
            case vs::Op::Kind::WriteFB8:
                state.VDP1WriteFB<uint8>(op.address, static_cast<uint8>(op.value),
                                         [&](uint32 a, uint8 v) { r.VDP1WriteFB(a, v); });
                break;
            case vs::Op::Kind::WriteFB16:
                state.VDP1WriteFB<uint16>(op.address, op.value, [&](uint32 a, uint16 v) { r.VDP1WriteFB(a, v); });
                break;
            case vs::Op::Kind::SetEnhancements: r.ConfigureEnhancements(vs::EnhancementsOf(op.value)); break;
            default: break;
            }
        }
        if (op.kind == vs::Op::Kind::Take || op.kind == vs::Op::Kind::FrameBoundary) {
            compare("op " + std::to_string(opIndex));
        }
    }
    compare("the end");
    vulkan->DetachDevice();
    return diff;
}

uint32_t ScenarioSeeds(const gpu::VulkanDevice &device) {
    if (const char *env = std::getenv("BRIMIR_VDP1_GPU_SEEDS")) {
        return static_cast<uint32_t>(std::max(1, std::atoi(env)));
    }
    return device.Properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 10 : 40;
}

} // namespace

TEST_CASE("GPU VDP1: random command tables through the renderer, FBRAM after every take", "[gpu][vdp1][scene]") {
    auto device = OpenDevice();
    const uint32_t seeds = ScenarioSeeds(*device);
    uint32_t compares = 0;
    uint64_t nonZero = 0;
    for (uint32_t seed = 0; seed < seeds; ++seed) {
        vs::Coverage cov;
        const vs::Scenario s = vs::RandomScenario(1000 + seed, cov);
        if (!s.hazardTracking) {
            continue;
        }
        const std::string diff = RunScenarioThroughRenderer(*device, s, compares, nonZero);
        INFO("seed " << 1000 + seed);
        CHECK(diff.empty());
        if (!diff.empty()) {
            UNSCOPED_INFO(diff);
            FAIL_CHECK(diff);
            break;
        }
    }
    INFO(compares << " FBRAM compares");
    CHECK(compares >= seeds);
    CHECK(nonZero > 0);
    CloseAndCheckValidation(device);
}
