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
// VDP1 upscaling (GPU stage 3, [nx]): the scene list again at N = 2, 3, 4 with the N x sprite surface compared with the
// N x reference at every checkpoint, and the consistency paths (CPU writes, a budgeted erase, resets, a state load, a
// detach and attach, internal resolution changes, dropped command buffers, an allocation failure, a full 4x batch)
// checked block by block.
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
#include <map>
#include <memory>
#include <random>
#include <span>
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

// Checks a scene: FBRAM after every frame and checkpoint, every frame's composite (skipFrame: one not compared). With
// options.vdp1Upscaling also the N x sprite surface against the N x reference at the internal resolution options.scale
// (compared at least once, at that scale, with sub-texel detail when `nxDetail`).
FbramResult RunFbramScene(const Vdp2Scene &scene, RenderBothOptions options = {}, bool requireColors = true,
                          uint32_t skipFrame = UINT32_MAX, bool nxDetail = true) {
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
    if (options.vdp1Upscaling && options.scale > 1) {
        CHECK(r.nxCompares > 0);
        CHECK(((r.nxScales >> options.scale) & 1u) != 0);
        CHECK(r.nxCount == 0);
        CHECK(r.nxNativeCount == 0);
        if (nxDetail) {
            CHECK(r.nxDetailBlocks > 0);
        }
    }
    CloseAndCheckValidation(device);
    return r;
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

// The stage 2 scene list (each a TEST_CASE below; the N x test runs them all at N = 2, 3, 4)
struct SceneCase {
    std::string name;
    Vdp2Scene scene;
    RenderBothOptions options;
    bool requireColors = true;
    uint32_t skipFrame = UINT32_MAX;
};

SceneCase SceneEveryCommand() {
    return {"every command type",
            Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, AutoSwap(60), 4)};
}

SceneCase SceneTransparentMeshes() {
    return {"transparent meshes", Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); },
                                            AutoSwap(60), 4, {.transparentMeshes = true}, 2)};
}

SceneCase SceneDoubleDensity() {
    return {"double density with deinterlacing",
            Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); },
                      AutoSwap(50), 4, {.deinterlace = true, .transparentMeshes = true}, 3)};
}

SceneCase SceneDoubleInterlace(bool deinterlace) {
    FrameControl c = AutoSwap(50, 320, 448);
    const auto program = c.program;
    c.program = [program](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f) {
        program(w, rng, f);
        w.VDP1Reg(FBCR, static_cast<uint16_t>(kDIE | ((f & 1) ? kDIL : 0)));
    };
    return {std::string("double interlace, deinterlace ") + (deinterlace ? "on" : "off"),
            Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); }, c, 4,
                      {.deinterlace = deinterlace}, 4)};
}

SceneCase Scene8bpp(uint16_t tvmr) {
    FrameControl c = AutoSwap(50);
    const auto program = c.program;
    c.program = [program, tvmr](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f) {
        program(w, rng, f);
        w.VDP1Reg(TVMR, tvmr);
    };
    // A rotated framebuffer (TVMR 3) is not shown without a rotation background: FBRAM only then.
    SceneCase s{"8 bpp, TVMR " + std::to_string(tvmr),
                Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp, 0x0008); }, c, 4, {},
                          5 + tvmr)};
    s.requireColors = tvmr == 1;
    return s;
}

SceneCase SceneErases() {
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
    return {"erases with the VBlank cycle limit",
            Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 6, {}, 9)};
}

SceneCase SceneManualSwap() {
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
    return {"manual swap", Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 6, {}, 10)};
}

SceneCase SceneEraseWithoutSwap() {
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
    return {"erase without swap",
            Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 6, {}, 11)};
}

SceneCase SceneStopsSwapping() {
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
    return {"a game that stops swapping",
            Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 7, {}, 12)};
}

SceneCase SceneWritesBetweenCommands() {
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
    return {"register, VRAM and CPU writes between commands",
            Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 4, {}, 13)};
}

SceneCase SceneCpuWritesDoubleDensity() {
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
    return {"CPU writes in double density with deinterlacing and transparent meshes",
            Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); }, c, 4,
                      {.deinterlace = true, .transparentMeshes = true}, 14)};
}

SceneCase SceneDroppedCommandBuffer() {
    SceneCase s{"a dropped command buffer",
                Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, AutoSwap(40), 5, {}, 15)};
    s.options.dropFrame = 2;
    s.skipFrame = 2;
    return s;
}

SceneCase SceneSaveLoad() {
    // The save inside the frame (the GPU holds newer data) makes one sync submit; the load uploads main and mesh.
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
    return {"save and load between commands",
            Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 4,
                      {.deinterlace = true, .transparentMeshes = true}, 16)};
}

FbramResult RunCase(const SceneCase &s, uint32_t nx = 0) {
    RenderBothOptions options = s.options;
    if (nx != 0) {
        options.vdp1Upscaling = true;
        options.scale = nx;
    }
    return RunFbramScene(s.scene, options, s.requireColors, s.skipFrame);
}

std::vector<SceneCase> Stage2Scenes() {
    std::vector<SceneCase> list;
    list.push_back(SceneEveryCommand());
    list.push_back(SceneTransparentMeshes());
    list.push_back(SceneDoubleDensity());
    list.push_back(SceneDoubleInterlace(false));
    list.push_back(SceneDoubleInterlace(true));
    list.push_back(Scene8bpp(1));
    list.push_back(Scene8bpp(3));
    list.push_back(SceneErases());
    list.push_back(SceneManualSwap());
    list.push_back(SceneEraseWithoutSwap());
    list.push_back(SceneStopsSwapping());
    list.push_back(SceneWritesBetweenCommands());
    list.push_back(SceneCpuWritesDoubleDensity());
    list.push_back(SceneDroppedCommandBuffer());
    list.push_back(SceneSaveLoad());
    return list;
}

} // namespace

TEST_CASE("GPU VDP1 scene: every command type, colour mode and draw mode", "[gpu][vdp1][scene]") {
    RunCase(SceneEveryCommand());
}

TEST_CASE("GPU VDP1 scene: transparent meshes", "[gpu][vdp1][scene]") {
    RunCase(SceneTransparentMeshes());
}

TEST_CASE("GPU VDP1 scene: double density with deinterlacing (doubleV, alternate fields)", "[gpu][vdp1][scene]") {
    RunCase(SceneDoubleDensity());
}

TEST_CASE("GPU VDP1 scene: double interlace (DIE, DIL per field)", "[gpu][vdp1][scene]") {
    for (const bool deinterlace : {false, true}) {
        DYNAMIC_SECTION("deinterlace " << deinterlace) {
            RunCase(SceneDoubleInterlace(deinterlace));
        }
    }
}

TEST_CASE("GPU VDP1 scene: 8 bpp framebuffers (1024 wide, rotation 512 tall)", "[gpu][vdp1][scene]") {
    for (const uint16_t tvmr : {uint16_t{1}, uint16_t{3}}) {
        DYNAMIC_SECTION("TVMR " << tvmr) {
            RunCase(Scene8bpp(tvmr));
        }
    }
}

TEST_CASE("GPU VDP1 scene: erases with the VBlank cycle limit, empty regions and the clamps", "[gpu][vdp1][scene]") {
    RunCase(SceneErases());
}

TEST_CASE("GPU VDP1 scene: manual swap (FCM = 1, FCT = 1) every other frame", "[gpu][vdp1][scene]") {
    RunCase(SceneManualSwap());
}

TEST_CASE("GPU VDP1 scene: erase without swap (FCM = 1, FCT = 0)", "[gpu][vdp1][scene]") {
    RunCase(SceneEraseWithoutSwap());
}

TEST_CASE("GPU VDP1 scene: a game that stops swapping", "[gpu][vdp1][scene]") {
    RunCase(SceneStopsSwapping());
}

TEST_CASE("GPU VDP1 scene: register, VRAM and CPU writes between commands", "[gpu][vdp1][scene]") {
    RunCase(SceneWritesBetweenCommands());
}

TEST_CASE("GPU VDP1 scene: CPU writes in double density with deinterlacing and transparent meshes",
          "[gpu][vdp1][scene]") {
    RunCase(SceneCpuWritesDoubleDensity());
}

TEST_CASE("GPU VDP1 scene: a dropped command buffer's VDP1 ops are replayed with the next one", "[gpu][vdp1][scene]") {
    RunCase(SceneDroppedCommandBuffer());
}

TEST_CASE("GPU VDP1 scene: save and load between commands", "[gpu][vdp1][scene]") {
    RunCase(SceneSaveLoad());
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
    brimir::test::InstallSyncSubmit(*vulkan, device);
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
            vulkan->FrameSubmitted(0, VK_NULL_HANDLE);
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

// ---------------------------------------------------------------------------------------------------------------------
// CPU framebuffer access, save states and the device lifecycle (GPU stage 2, Task 6): the CPU shadow of
// vulkan_vdp_renderer.hpp (rules 1-8 at its top). RenderBoth installs a sync submit (InstallSyncSubmit) and waits for
// every submit; AsyncRig below submits without waiting, as VulkanOutput does, so readbacks stay Pending on a fence.

namespace {

using brimir::test::SyncSubmitCounter;
using SyncKind = gpu::VulkanVDPRenderer::Vdp1SyncKind;

// Byte address of pixel (x, y) of a 16-bpp framebuffer 512 pixels wide (TVMR 0)
constexpr uint32_t Pixel16(uint32_t x, uint32_t y) {
    return (y * 512 + x) * 2;
}

// A command table at 0 that draws one polygon (x0, y0)-(x1, y1) in `color` (replace).
void WriteOverdraw(Vdp2SceneWriter &w, int x0, int y0, int x1, int y1, uint16_t color) {
    w.VDP1Command(0x00, Vdp1Cmd::SystemClip(319, 223));
    w.VDP1Command(0x20, Vdp1Cmd::UserClip(0, 0, 319, 223));
    w.VDP1Command(0x40, Vdp1Cmd::Local(0, 0));
    w.VDP1Command(0x60, Vdp1Cmd::Polygon(0x0000, color, x0, y0, x1, y0, x1, y1, x0, y1));
    w.VDP1Command(0x80, Vdp1Cmd::End());
}

// The write-then-read hazard (research section 3.3): after a draw (the draw framebuffer is GpuNewer), a CPU write and a
// read of it return the write; a draw over it and a read return the draw; word and byte variants. Reads of both pairs
// compared (software first). One sync submit per read after a draw.
FrameControl HazardControl(bool byteWrites, SyncSubmitCounter &counter) {
    FrameControl c = AutoSwap(30);
    const auto draw = c.draw;
    c.draw = [draw, byteWrites, &counter](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, bool swapped) {
        draw(w, rng, f, swapped);
        const uint32_t fb = w.DrawFramebuffer();
        const uint32_t a = Pixel16(40 + f, 30 + f);
        uint64_t before = counter.submits;
        if (byteWrites) {
            w.FBRAM8(fb, a + 1, static_cast<uint8_t>(0x5A + f));
            const auto low = w.ReadFB8(a + 1);
            CHECK(low[0] == low[1]);
            CHECK(low[1] == static_cast<uint8_t>(0x5A + f));
            const auto word = w.ReadFB16(a); // the draw's high byte, the CPU's low byte
            CHECK(word[0] == word[1]);
            CHECK((word[1] & 0xFF) == static_cast<uint8_t>(0x5A + f));
            w.FBRAM8(fb, a, 0x12);
            const auto high = w.ReadFB16(a);
            CHECK(high[0] == high[1]);
            CHECK(high[1] == static_cast<uint16_t>(0x1200 | static_cast<uint8_t>(0x5A + f)));
        } else {
            w.FBRAM16(fb, a, static_cast<uint16_t>(0x1234 + f));
            const auto v = w.ReadFB16(a);
            CHECK(v[0] == v[1]);
            CHECK(v[1] == static_cast<uint16_t>(0x1234 + f));
        }
        CHECK(counter.submits == before + 1); // the first read after the draw
        before = counter.submits;
        WriteOverdraw(w, 20, 20, 100, 80, 0x83E0);
        w.DrawVDP1();
        const auto v = w.ReadFB16(a);
        CHECK(v[0] == v[1]);
        CHECK(v[1] == 0x83E0);
        if (byteWrites) {
            const auto low = w.ReadFB8(a + 1);
            CHECK(low[0] == low[1]);
            CHECK(low[1] == 0xE0);
        }
        CHECK(counter.submits == before + 1);
    };
    return c;
}

} // namespace

TEST_CASE("GPU VDP1 scene: a CPU write read back after a draw, then a draw over it (write-then-read hazard)",
          "[gpu][vdp1][scene]") {
    for (const bool byteWrites : {false, true}) {
        for (const bool threaded : {false, true}) {
            DYNAMIC_SECTION((byteWrites ? "bytes" : "words") << (threaded ? ", threaded VDP1 on" : "")) {
                SyncSubmitCounter counter;
                RenderBothOptions options;
                options.syncCounter = &counter;
                options.threadedVDP1 = threaded; // the inner software renderer's thread (GPU mode does not use it)
                RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); },
                                        HazardControl(byteWrites, counter), 4, {}, 20),
                              options);
                CHECK(counter.submits == 8);
                CHECK(counter.refused == 0);
            }
        }
    }
}

TEST_CASE("GPU VDP1 scene: a burst of CPU reads between draws makes one sync submit", "[gpu][vdp1][scene]") {
    SyncSubmitCounter counter;
    FrameControl c;
    c.program = AutoSwap(30).program;
    uint32_t reads = 0;
    c.draw = [&counter, &reads](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool) {
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 30);
        w.DrawVDP1();
        for (uint32_t burst = 0; burst < 2; ++burst) {
            const uint64_t before = counter.submits;
            for (uint32_t i = 0; i < 64; ++i) {
                const uint32_t address = (Rand(rng, 0x20000) * 2) & 0x3FFFE;
                const auto v = w.ReadFB16(address);
                ++reads;
                CHECK(v[0] == v[1]);
                if (i % 8 == 3) {
                    // CPU writes between the reads do not make the GPU newer
                    w.FBRAM16(w.DrawFramebuffer(), address, static_cast<uint16_t>(rng()));
                    const auto written = w.ReadFB16(address);
                    CHECK(written[0] == written[1]);
                }
                CHECK(counter.submits == before + 1);
            }
            WriteOverdraw(w, RandI(rng, 0, 100), RandI(rng, 0, 60), RandI(rng, 150, 300), RandI(rng, 100, 200),
                          static_cast<uint16_t>(0x8000 | rng()));
            w.DrawVDP1();
        }
    };
    RenderBothOptions options;
    options.syncCounter = &counter;
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 4, {}, 21), options);
    INFO(reads << " reads");
    CHECK(reads == 4 * 2 * 64);
    CHECK(counter.submits == 4 * 2);
}

TEST_CASE("GPU VDP1 scene: CPU writes and reads in double-density interlace mirror to alt and leave the meshes",
          "[gpu][vdp1][scene]") {
    FrameControl c = AutoSwap(40);
    c.line = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, uint32_t y) {
        if (y % 40 != 9) {
            return;
        }
        for (uint32_t i = 0; i < 64; ++i) {
            const uint32_t address = (Rand(rng, 0x20000) * 2) & 0x3FFFE;
            const uint16_t value = static_cast<uint16_t>(rng());
            if (i % 2 == 0) {
                w.FBRAM16(w.DrawFramebuffer(), address, value);
            } else {
                w.FBRAM8(w.DrawFramebuffer(), address + (i & 2) / 2, static_cast<uint8_t>(value));
            }
            const auto v = w.ReadFB16(address);
            CHECK(v[0] == v[1]);
        }
        w.Checkpoint();
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); }, c,
                            4, {.deinterlace = true, .transparentMeshes = true}, 22));
}

TEST_CASE("GPU VDP1 scene: a read and a save in blanking with the trailing display erase pending",
          "[gpu][vdp1][scene]") {
    // After VDP2EndFrame (the frame is pending, not recorded) the BlankingAndSync display erase is a trailing op: the
    // sync submit records the frame, then the erase, then the readbacks (FramePolicy::Sync). Even frames read first,
    // odd frames save and load first; one sync submit either way.
    SyncSubmitCounter counter;
    Vdp2Scene scene = Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, AutoSwap(40), 6,
                                {.transparentMeshes = true}, 23);
    const auto blanking = scene.blanking;
    auto frame = std::make_shared<uint32_t>(0);
    scene.blanking = [blanking, frame, &counter](Vdp2SceneWriter &w) {
        blanking(w);
        const uint32_t f = (*frame)++;
        const uint64_t before = counter.submits;
        if (f % 2 == 1) {
            w.SaveLoadMidFrame();
        }
        std::mt19937 rng(f);
        for (uint32_t i = 0; i < 16; ++i) {
            const auto v = w.ReadFB16((Rand(rng, 0x20000) * 2) & 0x3FFFE);
            CHECK(v[0] == v[1]);
        }
        if (f % 2 == 0) {
            w.SaveLoadMidFrame();
        }
        CHECK(counter.submits == before + 1);
    };
    RenderBothOptions options;
    options.syncCounter = &counter;
    RunFbramScene(scene, options);
    CHECK(counter.submits == 6);
}

TEST_CASE("GPU VDP1 scene: a dropped VDP1-only command buffer after a frame a sync submit recorded",
          "[gpu][vdp1][scene][scale]") {
    // Double density without deinterlacing: each frame draws one field, the other is the previous frame's. In frame 3
    // the scale changes and a read in blanking makes a sync submit record the frame (with the rescale); a CPU write
    // after it is VDP1 work alone, whose command buffer is dropped. Only the VDP1 ops are queued again: the frame (its
    // rescale) was submitted, so frame 4 keeps frame 3's field and every frame but 3 equals software's.
    constexpr uint32_t kChange = 3;
    SyncSubmitCounter counter;
    Vdp2Scene scene = Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); },
                                AutoSwap(40), 6, {}, 30);
    const auto blanking = scene.blanking;
    auto frame = std::make_shared<uint32_t>(0);
    scene.blanking = [blanking, frame, &counter](Vdp2SceneWriter &w) {
        blanking(w);
        if ((*frame)++ != kChange) {
            return;
        }
        const uint64_t before = counter.submits;
        const auto v = w.ReadFB16(0x100);
        CHECK(v[0] == v[1]);
        CHECK(counter.submits == before + 1);
        w.FBRAM16(w.DrawFramebuffer(), 0x200, 0x8421);
    };
    bool rescalePending = true;
    RenderBothOptions options;
    options.syncCounter = &counter;
    options.scale = 1;
    options.dropFrame = kChange;
    options.beforeFrame = [&](uint32_t f, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (f == kChange) {
            REQUIRE(vulkan.SetInternalScale(2));
        } else if (f == kChange + 1) {
            rescalePending = vulkan.Backend()->RescalePendingForTest();
        }
    };
    RunFbramScene(scene, options, true, kChange);
    CHECK_FALSE(rescalePending);
}

TEST_CASE("GPU VDP1 scene: readbacks from invalidated memory; the readback memory logged once", "[gpu][vdp1][scene]") {
    // ReadbackData invalidates every slice it hands out (the non-coherent path, also on coherent memory, under
    // validation); the shadow still equals software's at every read and save.
    std::vector<std::string> logs;
    gpu::vdp1::Vdp1VulkanBackend::ReadbackMemory memory = gpu::vdp1::Vdp1VulkanBackend::ReadbackMemory::None;
    FrameControl c = AutoSwap(40);
    c.draw = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool) {
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 40);
        w.DrawVDP1Partial(0, 25);
        for (uint32_t i = 0; i < 8; ++i) {
            const auto v = w.ReadFB16((Rand(rng, 0x20000) * 2) & 0x3FFFE);
            CHECK(v[0] == v[1]);
        }
        const auto saved = w.SaveStates();
        REQUIRE(saved.size() == 2);
        CHECK(saved[0]->FBRAM == saved[1]->FBRAM);
        w.DrawVDP1Partial(25, 64);
    };
    RenderBothOptions options;
    options.beforeFrame = [&](uint32_t f, gpu::VulkanVDPRenderer &vulkan, gpu::VulkanDevice &) {
        if (f == 0) {
            vulkan.SetLog([&logs](const std::string &text) { logs.push_back(text); });
            REQUIRE(vulkan.Vdp1Backend() != nullptr);
            vulkan.Vdp1Backend()->SetInvalidateReadbacksForTest(true);
        } else {
            memory = vulkan.Vdp1Backend()->ReadbackMemoryForTest(vulkan.Vdp1Backend()->FrameContexts() - 1);
        }
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 3, {}, 32), options);
    CHECK(memory != gpu::vdp1::Vdp1VulkanBackend::ReadbackMemory::None);
    CHECK(std::count_if(logs.begin(), logs.end(), [](const std::string &text) {
              return text.find("framebuffer readbacks in") != std::string::npos;
          }) == 1);
}

TEST_CASE("GPU VDP1 back half: malformed ops are skipped, counted and logged once", "[gpu][vdp1]") {
    // The front half never produces one; a malformed op (data out of range) is skipped by Record, counted in the
    // stats and the total, and the first one is logged.
    auto device = OpenDevice();
    gpu::vk_util::Buffer fbram;
    REQUIRE(gpu::vk_util::CreateBuffer(*device, gpu::vdp1::kFbramBufferSize,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, fbram) == VK_SUCCESS);
    {
        gpu::vdp1::Vdp1VulkanBackend backend(*device, 1, fbram.buffer);
        REQUIRE(backend.Ok());
        std::vector<std::string> logs;
        backend.SetLog([&logs](const std::string &text) { logs.push_back(text); });
        gpu::vdp1::Vdp1OpList ops;
        ops.batches.resize(1);
        ops.batches[0].cmdCount = 1; // no commands in the list
        ops.ops = {{gpu::vdp1::Vdp1OpKind::Draw, 0},
                   {gpu::vdp1::Vdp1OpKind::Draw, 7},
                   {gpu::vdp1::Vdp1OpKind::Erase, 0},
                   {gpu::vdp1::Vdp1OpKind::FbWrites, 0}};
        for (int round = 0; round < 2; ++round) {
            REQUIRE(backend.BeginContext(0));
            bool recorded = false;
            REQUIRE(gpu::vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                        recorded = backend.Record(ops, {0, 4}, 0, cmd);
                    }) == VK_SUCCESS);
            CHECK(recorded);
            CHECK(backend.StatsForTest().malformed == 4);
            CHECK(backend.StatsForTest().batches == 0);
        }
        CHECK(backend.MalformedTotal() == 8);
        REQUIRE(logs.size() == 1);
        CHECK(logs[0] == "VDP1: skipped a malformed op (draw batch 0: data out of range); later ones are only counted");
    }
    gpu::vk_util::DestroyBuffer(*device, fbram);
    CloseAndCheckValidation(device);
}

TEST_CASE("GPU VDP1 scene: a save whose sync submit is refused reports the framebuffers behind",
          "[gpu][vdp1][scene]") {
    // Mid-frame the GPU holds newer data: with the sync submit refused PreSaveStateSync cannot bring main and mesh
    // current (SaveSyncFailed: CoreWrapper::SaveState fails); allowed again, the next save is current. Logged once.
    SyncSubmitCounter counter;
    gpu::VulkanVDPRenderer *vulkan = nullptr;
    std::vector<std::string> logs;
    uint32_t failed = 0, current = 0;
    FrameControl c = AutoSwap(40);
    c.draw = [&](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, bool) {
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 40);
        w.DrawVDP1Partial(0, 25);
        if (f == 1 || f == 2) {
            counter.allowed = false;
            (void)w.SaveStates();
            failed += vulkan->SaveSyncFailed() ? 1 : 0;
            counter.allowed = true;
            (void)w.SaveStates();
            current += vulkan->SaveSyncFailed() ? 0 : 1;
        }
        w.DrawVDP1Partial(25, 64);
    };
    RenderBothOptions options;
    options.syncCounter = &counter;
    options.beforeFrame = [&](uint32_t f, gpu::VulkanVDPRenderer &renderer, gpu::VulkanDevice &) {
        vulkan = &renderer;
        if (f == 0) {
            renderer.SetLog([&logs](const std::string &text) { logs.push_back(text); });
        }
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 4, {}, 31), options);
    CHECK(failed == 2);
    CHECK(current == 2);
    CHECK(counter.refused == 2);
    CHECK(std::count_if(logs.begin(), logs.end(), [](const std::string &text) {
              return text.find("a save state fails") != std::string::npos;
          }) == 1);
}

TEST_CASE("GPU VDP1 scene: save states equal software's byte for byte; cross-renderer loads both ways",
          "[gpu][vdp1][scene]") {
    // Mid-frame (between commands, the GPU holding newer data) the state of each pair is saved: the VDP part (main
    // FBRAM included) and the renderer's VDP1 part (doubleV, meshFBRAM) must be identical. In frames 1 and 3 each pair
    // then loads the other's state (software the GPU renderer's, the GPU renderer software's) and continues.
    FrameControl c = AutoSwap(40);
    uint32_t compared = 0;
    uint64_t meshBytes = 0;
    c.draw = [&compared, &meshBytes](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, bool) {
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 40);
        w.DrawVDP1Partial(0, 25);
        const auto saved = w.SaveStates();
        REQUIRE(saved.size() == 2);
        const ymir::savestate::VDPSaveState &sw = *saved[0];
        const ymir::savestate::VDPSaveState &gpu = *saved[1];
        // Field by field (the structs' padding is indeterminate: the serialized state, compared byte for byte by
        // brimir_state_diff in tests/vkhost_checks.cmake, holds the fields)
        CHECK(sw.VRAM1 == gpu.VRAM1);
        CHECK(sw.FBRAM == gpu.FBRAM);
        CHECK(sw.displayFB == gpu.displayFB);
        const auto &swR = sw.renderer.vdp1State;
        const auto &gpuR = gpu.renderer.vdp1State;
        CHECK(swR.doubleV == gpuR.doubleV);
        CHECK(swR.meshFBRAM == gpuR.meshFBRAM);
        // (The section's clip and local-coordinate fields are never written by either renderer's SaveState: always
        // zero, so not compared.)
        CHECK(sw.renderer.vcellScrollInc == gpu.renderer.vcellScrollInc);
        for (const auto &alt : sw.renderer.vdp1State.meshFBRAM) {
            for (const auto &fb : alt) {
                meshBytes += static_cast<uint64_t>(std::count_if(fb.begin(), fb.end(), [](uint8_t b) { return b != 0; }));
            }
        }
        ++compared;
        if (f == 1 || f == 3) {
            w.CrossLoad();
        }
        w.DrawVDP1Partial(25, 64);
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); }, c,
                            5, {.deinterlace = true, .transparentMeshes = true}, 24));
    CHECK(compared == 5);
    CHECK(meshBytes > 0);
}

TEST_CASE("GPU VDP1 scene: a save and load every frame for 300 frames continues identically", "[gpu][vdp1][scene]") {
    FrameControl c = AutoSwap(8);
    c.draw = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool) {
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 8);
        w.DrawVDP1Partial(0, 6);
        w.SaveLoadMidFrame();
        w.DrawVDP1Partial(6, 64);
    };
    c.line = [](Vdp2SceneWriter &w, std::mt19937 &, uint32_t, uint32_t y) {
        if (y == 100) {
            w.SaveLoadMidFrame();
        }
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 300,
                            {.transparentMeshes = true}, 25));
}

TEST_CASE("GPU VDP1 scene: soft and hard VDP resets mid-frame", "[gpu][vdp1][scene]") {
    // A reset replays the ops before it, then uploads the reset framebuffers (rule 7); the erase and draw after it use
    // the reset resolution, as software's.
    FrameControl c = AutoSwap(30);
    auto done = std::make_shared<uint32_t>(UINT32_MAX); // the frame last reset (the frame restarts at line 0)
    c.line = [done](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, uint32_t y) {
        if (y != 60 || (f != 2 && f != 4) || *done == f) {
            return;
        }
        *done = f;
        w.ResetVDP(f == 4);
        Display(w, rng);
        RandomErase(w, rng);
        w.SwapFramebuffers(); // latches the erase registers
        w.EraseFramebuffer(0);
        const Assets a = WriteAssets(w, rng);
        WriteTable(w, rng, a, 30);
        w.DrawVDP1();
        w.Checkpoint();
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 6,
                            {.transparentMeshes = true}, 26));
}

// ---------------------------------------------------------------------------------------------------------------------
// Unwaited submits (as VulkanOutput's), detach, attach and abandon

namespace {

// A software and a Vulkan renderer (VDP1 on the GPU) on separate states with the same writes; frames are recorded into
// two frame contexts round-robin and submitted without a wait (FrameSubmitted with the fence), each context's fence
// waited for only before the context is reused, as VulkanOutput does.
struct AsyncRig {
    struct Ctx {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false;
    };

    AsyncRig(gpu::VulkanDevice &dev, const ymir::vdp::config::Enhancements &enhancements, bool threaded)
        : device(dev), writer(enhancements) {
        swState = std::make_unique<VDPState>();
        vkState = std::make_unique<VDPState>();
        sw = std::make_unique<ymir::vdp::SoftwareVDPRenderer>(*swState, swDebug, access);
        vk = std::make_unique<gpu::VulkanVDPRenderer>(*vkState, vkDebug, access);
        vk->SetVdp1Mode(gpu::Vdp1Mode::Gpu);
        brimir::test::InstallSyncSubmit(*vk, device, &sync);
        vk->AttachDevice(device, 2);
        vk->EnableThreadedVDP1(threaded);
        writer.Attach(*swState, *sw);
        writer.Attach(*vkState, *vk);
        const auto &t = device.Table();
        for (Ctx &c : ctx) {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.queueFamilyIndex = device.QueueFamily();
            t.vkCreateCommandPool(device.Device(), &poolInfo, nullptr, &c.pool);
            VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocInfo.commandPool = c.pool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            t.vkAllocateCommandBuffers(device.Device(), &allocInfo, &c.cmd);
            const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            t.vkCreateFence(device.Device(), &fenceInfo, nullptr, &c.fence);
        }
    }
    ~AsyncRig() {
        WaitAll();
        vk.reset();
        const auto &t = device.Table();
        for (Ctx &c : ctx) {
            t.vkDestroyFence(device.Device(), c.fence, nullptr);
            t.vkDestroyCommandPool(device.Device(), c.pool, nullptr);
        }
    }
    AsyncRig(const AsyncRig &) = delete;
    AsyncRig &operator=(const AsyncRig &) = delete;

    bool Ready() const {
        return vk->Attached() && vk->GpuVdp1();
    }

    // Records the pending frame (or VDP1 work) into the next context and submits it without waiting.
    bool Submit() {
        if (!vk->Attached() || (!vk->HasPendingFrame() && !vk->HasPendingVdp1Work())) {
            return true;
        }
        const auto &t = device.Table();
        const bool frame = vk->HasPendingFrame();
        Ctx &c = ctx[next];
        if (c.pending) {
            t.vkWaitForFences(device.Device(), 1, &c.fence, VK_TRUE, UINT64_MAX);
            c.pending = false;
        }
        t.vkResetCommandPool(device.Device(), c.pool, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        t.vkBeginCommandBuffer(c.cmd, &begin);
        const bool recorded = frame ? vk->RecordPendingFrame(next, c.cmd) : vk->RecordPendingVdp1Work(next, c.cmd);
        t.vkEndCommandBuffer(c.cmd);
        if (!recorded && !vk->RecordedVdp1Work()) {
            return false;
        }
        t.vkResetFences(device.Device(), 1, &c.fence);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &c.cmd;
        if (device.Submit(submit, c.fence) != VK_SUCCESS) {
            vk->RecordedFrameDropped();
            return false;
        }
        vk->FrameSubmitted(next, c.fence);
        c.pending = true;
        last = next;
        next = (next + 1) % static_cast<uint32_t>(ctx.size());
        return true;
    }

    // The last submit has completed (a GPU that finished the previous retro_run's work before the next one starts).
    void WaitLast() {
        Ctx &c = ctx[last];
        if (c.pending) {
            device.Table().vkWaitForFences(device.Device(), 1, &c.fence, VK_TRUE, UINT64_MAX);
        }
    }

    void WaitAll() {
        for (Ctx &c : ctx) {
            if (c.pending) {
                device.Table().vkWaitForFences(device.Device(), 1, &c.fence, VK_TRUE, UINT64_MAX);
                c.pending = false;
            }
        }
        if (vk) {
            vk->OwnWorkCompleted();
        }
    }

    // The emulated FBRAM against software's: the GPU FBRAM (after submitting the pending work and waiting for it), or
    // the inner software renderer's (VDPState main, its alt and mesh) while VDP1 is not on the GPU.
    std::string CompareFbram(const std::string &where) {
        constexpr uint32_t kSlice = ymir::vdp::kVDP1FBRAMSize;
        std::vector<uint8_t> got(8 * size_t{kSlice});
        if (Ready()) {
            if (!Submit()) {
                return where + ": submit failed";
            }
            WaitAll();
            const std::vector<uint32_t> words = ReadbackBuffer(device, vk->Backend()->SpriteFBBuffer(), 8 * kSlice / 4);
            if (words.size() * 4 != got.size()) {
                return where + ": readback failed";
            }
            std::memcpy(got.data(), words.data(), got.size());
        } else {
            vk->PreSaveStateSync(); // a threaded inner VDP1 drained
            const ymir::vdp::SoftwareVDPRenderer &inner = vk->InnerVdp1ForTest();
            const std::array<const ymir::vdp::SpriteFB *, 8> slices{
                &vkState->mem1.FBRAM[0],       &vkState->mem1.FBRAM[1],       &inner.GetAltFBRAM(0),
                &inner.GetAltFBRAM(1),         &inner.GetMeshFBRAM(false, 0), &inner.GetMeshFBRAM(false, 1),
                &inner.GetMeshFBRAM(true, 0),  &inner.GetMeshFBRAM(true, 1)};
            for (uint32_t i = 0; i < 8; ++i) {
                std::memcpy(got.data() + size_t{i} * kSlice, slices[i]->data(), kSlice);
            }
        }
        sw->PreSaveStateSync();
        const std::array<const ymir::vdp::SpriteFB *, 8> want{
            &swState->mem1.FBRAM[0],    &swState->mem1.FBRAM[1],    &sw->GetAltFBRAM(0),
            &sw->GetAltFBRAM(1),        &sw->GetMeshFBRAM(false, 0), &sw->GetMeshFBRAM(false, 1),
            &sw->GetMeshFBRAM(true, 0), &sw->GetMeshFBRAM(true, 1)};
        uint64_t differ = 0;
        std::string first;
        for (uint32_t i = 0; i < 8; ++i) {
            for (uint32_t b = 0; b < kSlice; ++b) {
                if (got[size_t{i} * kSlice + b] != (*want[i])[b] && differ++ == 0) {
                    first = "slice " + std::to_string(i) + " byte " + std::to_string(b);
                }
            }
        }
        return differ == 0 ? std::string{} : where + ": " + std::to_string(differ) + " bytes differ, first " + first;
    }

    gpu::VulkanDevice &device;
    std::unique_ptr<VDPState> swState, vkState;
    ymir::vdp::config::VDP2DebugRender swDebug{}, vkDebug{};
    ymir::vdp::config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<ymir::vdp::SoftwareVDPRenderer> sw;
    std::unique_ptr<gpu::VulkanVDPRenderer> vk;
    Vdp2SceneWriter writer;
    SyncSubmitCounter sync;
    std::array<Ctx, 2> ctx{};
    uint32_t next = 0;
    uint32_t last = 0;
};

uint64_t Count(const std::array<uint64_t, gpu::VulkanVDPRenderer::kVdp1SyncKinds> &a, SyncKind kind) {
    return a[static_cast<uint32_t>(kind)];
}

// The validation errors so far (before the device is destroyed: an abandoned renderer leaks its objects on purpose).
void CloseAfterLeak(std::unique_ptr<gpu::VulkanDevice> &device) {
    const bool validation = device->ValidationEnabled();
    const uint32_t errors = device->ValidationErrors()->load();
    device.reset();
    if (validation) {
        CHECK(errors == 0);
    }
}

} // namespace

TEST_CASE("GPU VDP1: reads and saves after an unwaited submit wait for its readback (no sync submit)",
          "[gpu][vdp1][scene]") {
    auto device = OpenDevice();
    {
        AsyncRig rig(*device, {.transparentMeshes = true}, false);
        REQUIRE(rig.Ready());
        const Vdp2Scene scene = Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, AutoSwap(40),
                                          6, {.transparentMeshes = true}, 30);
        scene.setup(rig.writer);
        for (uint32_t f = 0; f < scene.frames; ++f) {
            INFO("frame " << f);
            rig.writer.RunFrame(scene.onLine, 0, UINT32_MAX, scene.blanking);
            REQUIRE(rig.Submit()); // the frame's readbacks are Pending on its fence (rule 2)
            const uint64_t syncs = rig.sync.submits;
            const auto waits = [&](SyncKind kind) { return Count(rig.vk->Vdp1Sync().fenceWaits, kind); };
            const uint64_t readWaits = waits(SyncKind::Read);
            const uint64_t saveWaits = waits(SyncKind::Save);
            // retro_serialize: fence waits only (rule 5). The frame's readbacks are all in one context: the first wait
            // merges them, so a save first (odd frames) waits once and the read after it not at all, and the other way
            // round in even frames.
            const auto save = [&] {
                const auto saved = rig.writer.SaveStates();
                CHECK(saved[0]->FBRAM == saved[1]->FBRAM);
                CHECK(saved[0]->renderer.vdp1State.meshFBRAM == saved[1]->renderer.vdp1State.meshFBRAM);
                CHECK(saved[0]->renderer.vdp1State.doubleV == saved[1]->renderer.vdp1State.doubleV);
            };
            if (f % 2 == 1) {
                save();
            }
            // A CPU byte write after the readback was committed is kept over it by the merge (the write mask)
            const uint32_t a = Pixel16(10 + f * 7, 20 + f * 5);
            rig.writer.FBRAM8(rig.writer.DrawFramebuffer(), a + 1, static_cast<uint8_t>(0xA5 ^ f));
            const auto word = rig.writer.ReadFB16(a);
            CHECK(word[0] == word[1]);
            CHECK((word[1] & 0xFF) == static_cast<uint8_t>(0xA5 ^ f));
            if (f % 2 == 0) {
                save();
            }
            CHECK(waits(SyncKind::Read) == readWaits + (f % 2 == 0 ? 1 : 0));
            CHECK(waits(SyncKind::Save) == saveWaits + (f % 2 == 1 ? 1 : 0));
            CHECK(rig.sync.submits == syncs);
            const std::string diff = rig.CompareFbram("frame " + std::to_string(f));
            CHECK(diff.empty());
            rig.vk->PollVdp1Readbacks();
        }
        CHECK(rig.vk->Vdp1SyncSubmits() == 0);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("GPU VDP1: detach mid-game syncs, VDP1 continues in software, attach uploads", "[gpu][vdp1][scene]") {
    auto device = OpenDevice();
    for (const bool threaded : {false, true}) {
        DYNAMIC_SECTION("threaded VDP1 " << threaded) {
            const ymir::vdp::config::Enhancements enhancements{.deinterlace = true, .transparentMeshes = true};
            AsyncRig rig(*device, enhancements, threaded);
            REQUIRE(rig.Ready());
            FrameControl c = AutoSwap(40);
            c.line = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, uint32_t y) {
                if (y % 50 == 20) {
                    for (uint32_t i = 0; i < 32; ++i) {
                        w.FBRAM16(w.DrawFramebuffer(), (Rand(rng, 0x20000) * 2) & 0x3FFFE,
                                  static_cast<uint16_t>(rng()));
                    }
                }
            };
            const Vdp2Scene scene = Vdp1Scene(
                [](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); }, c, 12,
                enhancements, 31);
            scene.setup(rig.writer);
            for (uint32_t f = 0; f < scene.frames; ++f) {
                INFO("frame " << f);
                if (f == 7) {
                    // Attach: VDP1 back on the GPU from the inner renderer's framebuffers (uploaded first)
                    rig.vk->AttachDevice(rig.device, 2);
                    REQUIRE(rig.Ready());
                }
                const auto onLine = [&](Vdp2SceneWriter &w, uint32_t y) {
                    scene.onLine(w, y);
                    if ((f == 3 || f == 9) && y == 100) {
                        // Detach mid-frame, the frame's draw and CPU writes pending: the device idle first, then the
                        // sync of everything (alt too) into VDPState and the inner renderer
                        rig.WaitAll();
                        rig.vk->DetachDevice();
                        CHECK(!rig.vk->GpuVdp1());
                    }
                };
                rig.writer.RunFrame(onLine, 0, UINT32_MAX, scene.blanking);
                REQUIRE(rig.Submit());
                const std::string diff = rig.CompareFbram("frame " + std::to_string(f));
                CHECK(diff.empty());
                if (rig.Ready()) {
                    rig.vk->PollVdp1Readbacks();
                }
            }
            CHECK(Count(rig.vk->Vdp1Sync().syncSubmits, SyncKind::Detach) == 2);
        }
    }
    CloseAndCheckValidation(device);
}

namespace {

// Abandon mid-frame (frame 4, line 100: the device is gone, no Vulkan call): VDP1 continues in software from the
// shadow as of the last merge (rule 8). With `poll` = AfterWait the GPU finished the previous frame before frame 4
// started (PollVdp1Readbacks merged it), so exactly frame 4's VDP1 work so far is lost: the framebuffers then equal
// software's at the start of frame 4. With NoWait or Never the GPU may still run the earlier frames when they are
// polled: the loss is bounded by the frame contexts (two here; a context is waited for before it is reused, and
// merged then), so the framebuffers equal software's at the start of frame 2, 3 or 4 (frame 2 when never polled: the
// worst case). With `deinterlace` (double density) alt is in the shadow and survives the same way. Attached again at
// frame 7. With `strict` every frame from 4 on is compared with software (the operational exception: frame 4
// differs); otherwise the confined loss is checked, software then takes over the renderer's state at the abandon, and
// frames 0 and 4 on must be identical (the continuation is exact).
enum class Poll { AfterWait, NoWait, Never };

void RunAbandonScene(bool strict, Poll poll, bool deinterlace) {
    const bool waitLast = poll == Poll::AfterWait;
    auto device = OpenDevice();
    {
        const ymir::vdp::config::Enhancements enhancements{.deinterlace = deinterlace, .transparentMeshes = true};
        AsyncRig rig(*device, enhancements, false);
        REQUIRE(rig.Ready());
        const uint16_t tvmd = deinterlace ? kDisp | kDoubleDensity : kDisp;
        const Vdp2Scene scene = Vdp1Scene([tvmd](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, tvmd); },
                                          AutoSwap(40), 10, enhancements, 32);
        scene.setup(rig.writer);
        struct Snapshot {
            std::array<ymir::vdp::SpriteFB, 2> main;
            std::array<std::array<ymir::vdp::SpriteFB, 2>, 2> mesh;
            std::array<ymir::vdp::SpriteFB, 2> alt;
        };
        std::array<std::unique_ptr<Snapshot>, 5> atStart{}; // software's framebuffers at the start of frames 0-4
        for (uint32_t f = 0; f < scene.frames; ++f) {
            INFO("frame " << f);
            if (f == 7) {
                rig.vk->AttachDevice(rig.device, 2);
                REQUIRE(rig.Ready());
            }
            if (rig.Ready() && poll != Poll::Never) {
                if (waitLast) {
                    rig.WaitLast();
                }
                rig.vk->PollVdp1Readbacks(); // VulkanOutput::BeginRun
            }
            const auto onLine = [&](Vdp2SceneWriter &w, uint32_t y) {
                if (f < atStart.size() && y == 0) {
                    auto snap = std::make_unique<Snapshot>();
                    snap->main = rig.swState->mem1.FBRAM;
                    for (uint8_t fb = 0; fb < 2; ++fb) {
                        snap->mesh[0][fb] = rig.sw->GetMeshFBRAM(false, fb);
                        snap->mesh[1][fb] = rig.sw->GetMeshFBRAM(true, fb);
                        snap->alt[fb] = rig.sw->GetAltFBRAM(fb);
                    }
                    atStart[f] = std::move(snap);
                }
                scene.onLine(w, y);
                if (f == 4 && y == 100) {
                    rig.vk->AbandonDevice();
                    CHECK(!rig.vk->Attached());
                    const ymir::vdp::SoftwareVDPRenderer &inner = rig.vk->InnerVdp1ForTest();
                    const auto matches = [&](const Snapshot &s) {
                        bool same = rig.vkState->mem1.FBRAM == s.main;
                        for (uint8_t fb = 0; fb < 2; ++fb) {
                            same = same && inner.GetMeshFBRAM(false, fb) == s.mesh[0][fb] &&
                                   inner.GetMeshFBRAM(true, fb) == s.mesh[1][fb];
                            same = same && (!deinterlace || inner.GetAltFBRAM(fb) == s.alt[fb]);
                        }
                        return same;
                    };
                    int lostFrom = -1; // the shadow is software's state at the start of this frame
                    for (int j = 4; j >= 2 && lostFrom < 0; --j) {
                        if (matches(*atStart[j])) {
                            lostFrom = j;
                        }
                    }
                    INFO("the shadow is software's framebuffers at the start of frame " << lostFrom);
                    CHECK(lostFrom >= (waitLast ? 4 : 2));
                    if (poll == Poll::Never) {
                        // the worst case: only the context reuses merged (frame 3's record merged frame 1's readback)
                        CHECK(lostFrom == 2);
                    }
                    // ... and it is a loss: software has frame 4's draw
                    CHECK(rig.swState->mem1.FBRAM != atStart[4]->main);
                    if (deinterlace) {
                        const uint32_t draw = rig.swState->fbIndex.draw;
                        CHECK(rig.sw->GetAltFBRAM(draw) != atStart[4]->alt[draw]);
                    }
                    if (!strict) {
                        // The confined loss checked: software takes over the state the renderer continues from (the
                        // VDP state, its renderer section, alt), so every later frame must be identical again.
                        auto saved = std::make_unique<ymir::savestate::VDPSaveState>();
                        rig.vk->PreSaveStateSync();
                        rig.vkState->SaveState(*saved);
                        rig.vk->SaveState(saved->renderer);
                        rig.swState->LoadState(*saved);
                        rig.sw->LoadState(saved->renderer);
                        for (uint8_t fb = 0; fb < 2; ++fb) {
                            rig.sw->SetAltFBRAM(fb, inner.GetAltFBRAM(fb));
                        }
                        rig.sw->PostLoadStateSync();
                    }
                }
            };
            rig.writer.RunFrame(onLine, 0, UINT32_MAX, scene.blanking);
            REQUIRE(rig.Submit());
            // (Not in frames 1-3 unless strict: the compare waits for the GPU, which merges every readback.)
            if (strict || f == 0 || f >= 4) {
                const std::string diff = rig.CompareFbram("frame " + std::to_string(f));
                CHECK(diff.empty());
            }
        }
    }
    CloseAfterLeak(device);
}

} // namespace

TEST_CASE("GPU VDP1: abandoning the device loses only the VDP1 work of the runs the GPU had not completed",
          "[gpu][vdp1][scene]") {
    for (const bool deinterlace : {false, true}) {
        for (const Poll poll : {Poll::AfterWait, Poll::NoWait, Poll::Never}) {
            DYNAMIC_SECTION((deinterlace ? "double density, deinterlace" : "progressive")
                            << (poll == Poll::AfterWait ? ", previous frame completed when polled"
                                : poll == Poll::NoWait ? ", polled without a wait"
                                                       : ", never polled (worst case)")) {
                RunAbandonScene(false, poll, deinterlace);
            }
        }
    }
}

TEST_CASE("GPU VDP1: a detach that discards the state (unload) makes no sync submit and logs no readback failure",
          "[gpu][vdp1][scene]") {
    // Mid-frame the GPU holds newer data and the sync submit is refused (outside retro_run, as at retro_unload_game
    // without context_destroy): a detach keeping the state tries the sync and logs that it failed; one discarding it
    // tries nothing and logs nothing about it.
    for (const bool discard : {false, true}) {
        DYNAMIC_SECTION((discard ? "discarding the state" : "keeping the state")) {
            auto device = OpenDevice();
            {
                AsyncRig rig(*device, {}, false);
                REQUIRE(rig.Ready());
                std::vector<std::string> logs;
                rig.vk->SetLog([&logs](const std::string &text) { logs.push_back(text); });
                const Vdp2Scene scene = Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); },
                                                  AutoSwap(40), 3, {}, 33);
                scene.setup(rig.writer);
                for (uint32_t f = 0; f < scene.frames; ++f) {
                    const bool last = f + 1 == scene.frames;
                    rig.writer.RunFrame(scene.onLine, 0, last ? 100 : UINT32_MAX, scene.blanking);
                    if (!last) {
                        REQUIRE(rig.Submit());
                    }
                }
                REQUIRE(rig.vk->HasPendingVdp1Work());
                rig.sync.allowed = false;
                rig.WaitAll();
                rig.vk->DetachDevice(discard);
                CHECK_FALSE(rig.vk->Attached());
                CHECK(rig.sync.refused == (discard ? 0u : 1u));
                const auto failed = std::count_if(logs.begin(), logs.end(), [](const std::string &text) {
                    return text.find("reading the GPU framebuffers back failed") != std::string::npos;
                });
                CHECK(failed == (discard ? 0 : 1));
            }
            CloseAndCheckValidation(device);
        }
    }
}

TEST_CASE("GPU VDP1 exception: abandon loses the frame's VDP1 work (operational exception; frame 4 differs)",
          "[gpu][vdp1][scene][!shouldfail]") {
    RunAbandonScene(true, Poll::AfterWait, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// VDP1 upscaling (GPU stage 3, Task 5): the N x sprite surface of the VDP1 back half (RenderBothOptions::vdp1Upscaling:
// at every checkpoint and frame end the GPU surface equals the N x reference replaying what the GPU replayed, and the
// native FBRAM equals software's), plus the consistency paths of research section 2 checked block by block: a native
// word changed by anything but a draw has its whole N x block equal to it, the other blocks keep their sub-texel detail.

namespace {

bool CpuDevice() {
    auto device = OpenDevice();
    return device->Properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
}

// The N x surface and the FBRAM as read back at one checkpoint
struct NxSnapshot {
    uint32_t scale = 0;
    std::vector<uint8_t> surface, fbram;
};

uint16_t NxWordAt(const NxSnapshot &s, uint32_t slice, uint32_t index) {
    const uint8_t *p = s.surface.data() + gpu::vdp1::Vdp1NxFbramByteAddress(slice, index, s.scale);
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint16_t NativeWordAt(const NxSnapshot &s, uint32_t slice, uint32_t word) {
    const uint8_t *p = s.fbram.data() + size_t{slice} * gpu::vdp1::kFbramSliceSize + 2 * size_t{word};
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

// Block by block from `before` to `after` over the slices of `mask`: in `rebuilt` slices every block must be its native
// word (a slice upload or a new surface); elsewhere a block whose native word changed must be its native word
// (touched) and every other block must be as it was (kept; keptDetail counts those with sub-texel detail).
struct NxConsistency {
    uint64_t touched = 0, kept = 0, keptDetail = 0, rebuilt = 0, rebuiltDetailLost = 0, violations = 0;
    std::string first;
};

NxConsistency CheckNxBlocks(const NxSnapshot &before, const NxSnapshot &after, uint32_t mask, uint32_t rebuilt) {
    NxConsistency c;
    const uint32_t n = after.scale, sub = n * n;
    const auto violation = [&](uint32_t slice, uint32_t w, const char *what) {
        if (c.violations++ == 0) {
            c.first = std::string(what) + ": slice " + std::to_string(slice) + " native word " + std::to_string(w) +
                      " (x " + std::to_string(w % 512) + ", y " + std::to_string(w / 512) + ")";
        }
    };
    for (uint32_t slice = 0; slice < 8; ++slice) {
        if (((mask >> slice) & 1u) == 0) {
            continue;
        }
        for (uint32_t w = 0; w < gpu::vdp1::kFbramSliceSize / 2; ++w) {
            const uint16_t native = NativeWordAt(after, slice, w);
            bool uniform = true, same = true, detailBefore = false;
            for (uint32_t s = 0; s < sub; ++s) {
                const uint16_t v = NxWordAt(after, slice, w * sub + s);
                uniform = uniform && v == native;
                if (before.scale == n) {
                    const uint16_t old = NxWordAt(before, slice, w * sub + s);
                    same = same && v == old;
                    detailBefore = detailBefore || old != NxWordAt(before, slice, w * sub);
                }
            }
            if ((rebuilt >> slice) & 1u) {
                ++c.rebuilt;
                c.rebuiltDetailLost += detailBefore ? 1 : 0;
                if (!uniform) {
                    violation(slice, w, "a rebuilt block is not its native word");
                }
                continue;
            }
            if (before.scale != n) {
                violation(slice, w, "compared across a scale change");
                continue;
            }
            if (NativeWordAt(before, slice, w) != native) {
                ++c.touched;
                if (!uniform) {
                    violation(slice, w, "a touched block is not its native word");
                }
            } else if (same) {
                ++c.kept;
                c.keptDetail += detailBefore ? 1 : 0;
            } else if (!uniform) {
                violation(slice, w, "an untouched block changed");
            } else {
                ++c.touched; // set to its unchanged native word (an erase or a write of the same value)
            }
        }
    }
    return c;
}

// Snapshots taken at the checkpoints the scene names (`next` is set right before the checkpoint)
struct NxSnapshots {
    std::string next;
    std::map<std::string, NxSnapshot> taken;
};

RenderBothOptions NxOptions(uint32_t nx, const std::shared_ptr<NxSnapshots> &snaps = nullptr) {
    RenderBothOptions options;
    options.vdp1Upscaling = true;
    options.scale = nx;
    if (snaps) {
        options.nxInspect = [snaps](const std::string &, uint32_t scale, std::span<const uint8_t> surface,
                                    std::span<const uint8_t> fbram) {
            if (snaps->next.empty()) {
                return;
            }
            NxSnapshot &s = snaps->taken[snaps->next];
            s.scale = scale;
            s.surface.assign(surface.begin(), surface.end());
            s.fbram.assign(fbram.begin(), fbram.end());
            snaps->next.clear();
        };
    }
    return options;
}

void Snap(Vdp2SceneWriter &w, const std::shared_ptr<NxSnapshots> &snaps, const std::string &name) {
    snaps->next = name;
    w.Checkpoint();
}

const NxSnapshot &Taken(const NxSnapshots &snaps, const std::string &name) {
    const auto it = snaps.taken.find(name);
    INFO("snapshot " << name);
    REQUIRE(it != snaps.taken.end());
    return it->second;
}

std::vector<uint32_t> NxScales(bool cpu) {
    return cpu ? std::vector<uint32_t>{2, 4} : std::vector<uint32_t>{2, 3, 4};
}

} // namespace

TEST_CASE("GPU VDP1 N x: the stage 2 scenes, the N x surface equal to the reference at N = 2, 3, 4",
          "[gpu][vdp1][scene][nx]") {
    const bool cpu = CpuDevice();
    for (const uint32_t n : NxScales(cpu)) {
        for (const SceneCase &s : Stage2Scenes()) {
            // A CPU device (lavapipe) runs every scene at 2x and three of them at 4x
            if (cpu && n == 4 && s.name != "every command type" && s.name != "transparent meshes" &&
                s.name != "a dropped command buffer") {
                continue;
            }
            DYNAMIC_SECTION(n << "x: " << s.name) {
                RunCase(s, n);
            }
        }
    }
}

TEST_CASE("GPU VDP1 N x: upscaling off or at 1x allocates and records nothing N x", "[gpu][vdp1][scene][nx]") {
    for (const bool upscaling : {false, true}) {
        const uint32_t scale = upscaling ? 1 : 2;
        DYNAMIC_SECTION("upscaling " << upscaling << ", " << scale << "x") {
            uint32_t frames = 0;
            RenderBothOptions options;
            options.vdp1Upscaling = upscaling;
            options.scale = scale;
            options.afterFrame = [&](uint32_t, gpu::VulkanVDPRenderer &vk, gpu::VulkanDevice &) {
                REQUIRE(vk.Vdp1Backend() != nullptr);
                CHECK(vk.Vdp1UpscaleScale() == 0);
                CHECK(vk.Vdp1Backend()->NxScale() == 0);
                CHECK(vk.Vdp1Backend()->NxSurfaceBuffer() == VK_NULL_HANDLE);
                CHECK(vk.Vdp1Backend()->NxMemoryBytes() == 0);
                const auto &stats = vk.Vdp1Backend()->StatsForTest();
                CHECK(stats.batches > 0);
                CHECK(stats.nxSubBatches + stats.nxErases + stats.nxFbWriteRuns + stats.nxRebuilds == 0);
                ++frames;
            };
            const FbramResult r = RunFbramScene(SceneEveryCommand().scene, options);
            CHECK(r.nxCompares == 0);
            CHECK(frames == 4);
        }
    }
}

TEST_CASE("GPU VDP1 N x consistency: CPU writes then a read; touched blocks become the native word",
          "[gpu][vdp1][scene][nx]") {
    // Each frame draws a table (sub-texel detail), then 300 CPU word writes (and a few byte writes) into the draw
    // framebuffer and a read (a sync submit): the written words' blocks equal the native word, every other block of
    // the framebuffer keeps its detail.
    for (const uint32_t n : NxScales(CpuDevice())) {
        DYNAMIC_SECTION(n << "x") {
            auto snaps = std::make_shared<NxSnapshots>();
            FrameControl c = AutoSwap(50);
            const auto draw = c.draw;
            c.draw = [draw, snaps](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, bool swapped) {
                draw(w, rng, f, swapped);
                Snap(w, snaps, "before " + std::to_string(f));
                const uint32_t fb = w.DrawFramebuffer();
                for (uint32_t i = 0; i < 300; ++i) {
                    const uint32_t x = Rand(rng, 320), y = Rand(rng, 224);
                    w.FBRAM16(fb, Pixel16(x, y), static_cast<uint16_t>(0x8000 | Rand(rng, 0x8000)));
                }
                const auto v = w.ReadFB16(Pixel16(Rand(rng, 320), Rand(rng, 224)));
                CHECK(v[0] == v[1]);
                Snap(w, snaps, "after " + std::to_string(f));
            };
            RenderBothOptions options = NxOptions(n, snaps);
            RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 3, {}, 40),
                          options);
            for (uint32_t f = 0; f < 3; ++f) {
                const NxSnapshot &before = Taken(*snaps, "before " + std::to_string(f));
                const NxSnapshot &after = Taken(*snaps, "after " + std::to_string(f));
                // Both main framebuffers (the display one is not written)
                const NxConsistency k = CheckNxBlocks(before, after, 0x03, 0);
                INFO("frame " << f << ": " << k.first);
                CHECK(k.violations == 0);
                CHECK(k.touched >= 250);
                CHECK(k.touched <= 300);
                CHECK(k.keptDetail > 100);
            }
        }
    }
}

TEST_CASE("GPU VDP1 N x consistency: an erase cut short by its cycle budget", "[gpu][vdp1][scene][nx]") {
    // In blanking, before the frame's own display erase, the display framebuffer still holds the previous frame's
    // drawing: an erase of the whole screen with a 3,000-cycle budget (the VBlank erase's) stops mid-row. The erased
    // words' blocks hold the erase value, the words after the stop and outside the region keep their detail.
    for (const uint32_t n : NxScales(CpuDevice())) {
        DYNAMIC_SECTION(n << "x") {
            auto snaps = std::make_shared<NxSnapshots>();
            FrameControl c;
            c.program = [](Vdp2SceneWriter &w, std::mt19937 &, uint32_t) {
                w.VDP1Reg(FBCR, 0);
                w.VDP1Reg(EWDR, 0x8421);
                w.VDP1Reg(EWLR, 0);
                w.VDP1Reg(EWRR, static_cast<uint16_t>((40 << 9) | 0x1FF));
            };
            c.draw = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool) {
                const Assets a = WriteAssets(w, rng);
                WriteTable(w, rng, a, 60);
                w.DrawVDP1();
            };
            Vdp2Scene scene = Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 3, {}, 41);
            auto frame = std::make_shared<uint32_t>(0);
            scene.blanking = [blanking = scene.blanking, snaps, frame](Vdp2SceneWriter &w) {
                const uint32_t f = (*frame)++;
                if (f >= 1) {
                    Snap(w, snaps, "before " + std::to_string(f));
                    w.EraseFramebuffer(3000);
                    Snap(w, snaps, "after " + std::to_string(f));
                }
                blanking(w);
            };
            RunFbramScene(scene, NxOptions(n, snaps));
            for (uint32_t f = 1; f < 3; ++f) {
                const NxSnapshot &before = Taken(*snaps, "before " + std::to_string(f));
                const NxSnapshot &after = Taken(*snaps, "after " + std::to_string(f));
                const NxConsistency k = CheckNxBlocks(before, after, 0x03, 0); // both main framebuffers
                INFO("frame " << f << ": " << k.first);
                CHECK(k.violations == 0);
                // (words the erase sets to the value they had are not counted: the background of the drawing)
                CHECK(k.touched > 100);
                CHECK(k.touched <= 3001); // the budget's writes
                CHECK(k.keptDetail > 100);
            }
        }
    }
}

TEST_CASE("GPU VDP1 N x consistency: resets, a state load, a detach and attach rebuild the uploaded slices",
          "[gpu][vdp1][scene][nx]") {
    // Soft reset (frame 1: alt and mesh uploaded, main kept), hard reset (frame 2: all slices uploaded), a state load
    // (frame 3: main and mesh uploaded), a detach and attach (frame 4: everything uploaded); transparent meshes and
    // deinterlacing on, so every slice is in use. Each upload's slices are their native words block for block; the
    // slices not uploaded keep their detail.
    for (const uint32_t n : NxScales(CpuDevice())) {
        DYNAMIC_SECTION(n << "x") {
            auto snaps = std::make_shared<NxSnapshots>();
            // Step k (0 soft reset, 1 hard reset, 2 state load) at line 40 of a frame after the first (a reset
            // restarts the frame at line 0: once per frame); the attach before RenderBoth's frame 4
            struct Steps {
                uint32_t next = 0, lastFrame = UINT32_MAX;
                bool attached = false;
            };
            auto steps = std::make_shared<Steps>();
            FrameControl c = AutoSwap(50);
            c.line = [snaps, steps](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f, uint32_t y) {
                if (y != 40 || f == 0 || steps->next > 2 || steps->lastFrame == f) {
                    return;
                }
                const uint32_t k = steps->next++;
                steps->lastFrame = f;
                Snap(w, snaps, "before " + std::to_string(k));
                if (k == 2) {
                    w.SaveLoadMidFrame();
                    Snap(w, snaps, "after " + std::to_string(k));
                } else {
                    // (the reset restarts the frame: the snapshot right after it, before line 0)
                    w.ResetVDP(k == 1);
                    Snap(w, snaps, "after " + std::to_string(k));
                    Display(w, rng, kDisp | kDoubleDensity);
                }
            };
            c.program = [program = c.program, snaps, steps](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f) {
                if (steps->attached) {
                    steps->attached = false;
                    Snap(w, snaps, "after attach");
                }
                program(w, rng, f);
            };
            RenderBothOptions options = NxOptions(n, snaps);
            options.beforeFrame = [steps](uint32_t f, gpu::VulkanVDPRenderer &vk, gpu::VulkanDevice &device) {
                if (f == 5) {
                    vk.DetachDevice();
                    CHECK(vk.Vdp1UpscaleScale() == 0);
                    vk.AttachDevice(device, 2);
                    REQUIRE(vk.GpuVdp1());
                    CHECK(vk.Vdp1UpscaleScale() == vk.InternalScale());
                    steps->attached = true;
                }
            };
            RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng, kDisp | kDoubleDensity); },
                                    c, 7, {.deinterlace = true, .transparentMeshes = true}, 42),
                          options);
            constexpr uint32_t kMain = 0x03, kAlt = 0x0C, kMesh = 0xF0;
            const struct {
                uint32_t step;
                uint32_t rebuilt;
            } cases[] = {{0, kAlt | kMesh}, {1, 0xFF}, {2, kMain | kMesh}};
            for (const auto &k : cases) {
                const NxSnapshot &before = Taken(*snaps, "before " + std::to_string(k.step));
                const NxSnapshot &after = Taken(*snaps, "after " + std::to_string(k.step));
                const NxConsistency r = CheckNxBlocks(before, after, 0xFF, k.rebuilt);
                INFO("step " << k.step << ": " << r.first);
                CHECK(r.violations == 0);
                CHECK(r.rebuiltDetailLost > 0); // the upload replaced detail (the accepted transient)
                if (k.rebuilt != 0xFF) {
                    CHECK(r.keptDetail > 0); // the slices not uploaded keep theirs
                }
            }
            const NxSnapshot &attached = Taken(*snaps, "after attach");
            const NxConsistency r = CheckNxBlocks(attached, attached, 0xFF, 0xFF);
            CHECK(r.violations == 0);
            CHECK(r.rebuilt == 8u * gpu::vdp1::kFbramSliceSize / 2);
        }
    }
}

TEST_CASE("GPU VDP1 N x consistency: internal resolution changes rebuild the surface at the new scale",
          "[gpu][vdp1][scene][nx]") {
    // 2x -> 4x -> 1x (no surface) -> 3x, one change before each frame: the first checkpoint of the frame (before its
    // erase and draw) finds every block of every slice equal to its native word at the new scale; the frames after it
    // draw detail again and equal the reference.
    auto snaps = std::make_shared<NxSnapshots>();
    const std::array<uint32_t, 5> scales{2, 4, 1, 3, 3};
    FrameControl c = AutoSwap(50);
    c.program = [program = c.program, snaps](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t f) {
        Snap(w, snaps, "start " + std::to_string(f));
        program(w, rng, f);
    };
    RenderBothOptions options = NxOptions(2, snaps);
    std::vector<uint32_t> effective;
    options.beforeFrame = [&](uint32_t f, gpu::VulkanVDPRenderer &vk, gpu::VulkanDevice &) {
        REQUIRE(vk.SetInternalScale(scales[f]));
        effective.push_back(vk.Vdp1UpscaleScale());
    };
    const FbramResult r =
        RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 5,
                                {.transparentMeshes = true}, 43),
                      options);
    CHECK(effective == std::vector<uint32_t>{2, 4, 0, 3, 3});
    CHECK(r.nxScales == ((1u << 2) | (1u << 4) | (1u << 3)));
    for (const uint32_t f : {1u, 3u}) {
        const NxSnapshot &s = Taken(*snaps, "start " + std::to_string(f));
        CHECK(s.scale == scales[f]);
        const NxConsistency k = CheckNxBlocks(s, s, 0xFF, 0xFF);
        INFO("frame " << f << ": " << k.first);
        CHECK(k.violations == 0);
    }
    CHECK(snaps->taken.count("start 2") == 0); // no surface at 1x
}

TEST_CASE("GPU VDP1 N x consistency: a dropped command buffer replays the surface's rebuild and the N x passes",
          "[gpu][vdp1][scene][nx]") {
    // Frame 2 changes 2x -> 4x and its command buffer (the new surface's rebuild, the frame's ops and their N x
    // passes) is dropped: the next one carries all of it again; frame 4's (no scale change) is dropped too.
    for (const uint32_t dropped : {2u, 4u}) {
        DYNAMIC_SECTION("dropped frame " << dropped) {
            RenderBothOptions options = NxOptions(2);
            options.dropFrame = dropped;
            options.beforeFrame = [](uint32_t f, gpu::VulkanVDPRenderer &vk, gpu::VulkanDevice &) {
                if (f == 2) {
                    REQUIRE(vk.SetInternalScale(4));
                }
            };
            RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, AutoSwap(40), 6,
                                    {}, 44),
                          options, true, dropped);
        }
    }
}

TEST_CASE("GPU VDP1 N x: a surface that cannot be allocated turns VDP1 upscaling off at that scale",
          "[gpu][vdp1][scene][nx]") {
    // N x resources above 20 MiB fail: 2x (16 MiB) works, 3x (36 MiB) is refused (logged; VDP1 stays on the GPU,
    // native and identical to software), back at 2x the surface is allocated again.
    std::vector<std::string> logs;
    std::vector<uint32_t> effective;
    RenderBothOptions options = NxOptions(2);
    options.beforeFrame = [&](uint32_t f, gpu::VulkanVDPRenderer &vk, gpu::VulkanDevice &) {
        if (f == 0) {
            vk.SetLog([&logs](const std::string &text) { logs.push_back(text); });
            vk.SetVdp1NxAllocationLimitForTest(20u * 1024 * 1024);
        } else if (f == 2) {
            REQUIRE(vk.SetInternalScale(3));
        } else if (f == 4) {
            REQUIRE(vk.SetInternalScale(2));
        }
        effective.push_back(vk.Vdp1UpscaleScale());
        CHECK(vk.GpuVdp1());
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, AutoSwap(40), 6, {}, 45),
                  options);
    CHECK(effective == std::vector<uint32_t>{2, 2, 0, 0, 2, 2});
    CHECK(std::count_if(logs.begin(), logs.end(), [](const std::string &text) {
              return text.find("3x sprite surface could not be allocated") != std::string::npos;
          }) == 1);
    CHECK(std::count_if(logs.begin(), logs.end(), [](const std::string &text) {
              return text.find("VDP1 upscaling: sprite surface at 2x (16 MiB)") != std::string::npos;
          }) == 1);
}

TEST_CASE("GPU VDP1 N x: a full batch at 4x splits into several sub-batches, resolved in order",
          "[gpu][vdp1][scene][nx]") {
    // One native batch of 14 half-transparent full-screen polygons (rect path: 1.15 M slots each at 4x) and 6 skewed
    // ones (quad path): far beyond one sub-batch's 4 Mi slots. Every sub-batch's blending depends on the previous ones.
    uint32_t maxSubBatches = 0, subBatches = 0;
    FrameControl c;
    c.program = [](Vdp2SceneWriter &w, std::mt19937 &, uint32_t) {
        w.VDP1Reg(FBCR, 0);
        w.VDP1Reg(EWDR, 0x8000);
        w.VDP1Reg(EWLR, 0);
        w.VDP1Reg(EWRR, static_cast<uint16_t>((50 << 9) | 0x1FF));
    };
    c.draw = [](Vdp2SceneWriter &w, std::mt19937 &rng, uint32_t, bool) {
        uint32_t slot = 0;
        const auto put = [&](const Vdp1Cmd &cmd) { w.VDP1Command(0x20 * slot++, cmd); };
        put(Vdp1Cmd::SystemClip(319, 223));
        put(Vdp1Cmd::UserClip(0, 0, 319, 223));
        put(Vdp1Cmd::Local(0, 0));
        put(Vdp1Cmd::Polygon(0x0000, static_cast<uint16_t>(0x8000 | Rand(rng, 0x8000)), 0, 0, 319, 0, 319, 223, 0, 223));
        for (uint32_t i = 0; i < 20; ++i) {
            const uint16_t color = static_cast<uint16_t>(0x8000 | Rand(rng, 0x8000));
            if (i % 3 == 2) {
                const int o = RandI(rng, 0, 30);
                put(Vdp1Cmd::Polygon(0x0003, color, o, 0, 319, o, 319 - o, 223, 0, 223 - o));
            } else {
                put(Vdp1Cmd::Polygon(0x0003, color, 0, 0, 319, 0, 319, 223, 0, 223));
            }
        }
        put(Vdp1Cmd::End());
        w.DrawVDP1();
    };
    RenderBothOptions options = NxOptions(4);
    options.afterFrame = [&](uint32_t, gpu::VulkanVDPRenderer &vk, gpu::VulkanDevice &) {
        const auto &stats = vk.Vdp1Backend()->StatsForTest();
        maxSubBatches = std::max(maxSubBatches, stats.nxMaxSubBatchesPerBatch);
        subBatches += stats.nxSubBatches;
    };
    RunFbramScene(Vdp1Scene([](Vdp2SceneWriter &w, std::mt19937 &rng) { Display(w, rng); }, c, 2, {}, 46), options,
                  true, UINT32_MAX, false);
    INFO(subBatches << " sub-batches, at most " << maxSubBatches << " per native batch");
    CHECK(maxSubBatches >= 4);
}
