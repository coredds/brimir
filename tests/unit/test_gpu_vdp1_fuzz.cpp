// GPU VDP1 parity fuzzer (GPU stage 2, Task 7): seeded random programs run through the software renderer and the
// Vulkan renderer with VDP1 on the GPU (vdp2_scene.hpp RenderBothFBRAM), comparing all eight FBRAM slices (main
// fb0/fb1, alt fb0/fb1, mesh[alt][fb]) after every op (half the programs) or after about one op in N (the others: a
// checkpoint submits the pending VDP1 work and so ends the batch, and only sparse checkpoints let draws, VRAM and CPU
// writes share a batch), at every frame's end, and every frame's composite.
//
// A program is a list of ops, each at a frame and a phase (before the frame's VDP1 frame control, before a line, or in
// blanking after the VDP2 frame's end): VDP1 command tables and the textures, look-up tables and Gouraud tables they
// read (VRAM writes, also between the commands that read them), draws of parts of the table, CPU framebuffer writes
// (byte and word, draw and display framebuffer; mirrored to alt in deinterlaced double density) and reads (compared
// between the renderers), erases (with and without the cycle limit), swaps (1-cycle and manual frame control, plus
// explicit ones), TVMR, FBCR, erase-register and TVMD writes, enhancement changes, save/load points (each renderer its
// own state; software and GPU loading each other's state; SaveState fields compared), soft and hard resets. Frame
// control follows vdp.cpp (the VBlank erase with a cycle budget, the 1-cycle or manual erase/swap, the display erase
// after the frame's end).
//
// A failing program is minimised (ops removed while it still fails the same way) and printed as C++ for a reproducer
// scene. [gpu][vdp1][fuzz]: a few seeds in the suite (24 on a GPU, 12 on a CPU device such as lavapipe;
// every op kind, fragments and a mid-frame interlace switch required)
// and a check that the fuzzer notices a broken renderer. The long run is hidden:
//   BRIMIR_VDP1_FUZZ_SEEDS=N (default 1000) BRIMIR_VDP1_FUZZ_FIRST=S brimir_tests "[vdp1-fuzz-long]"
// (BRIMIR_VDP1_FUZZ_MINIMIZE=1 minimises and prints the first failures; BRIMIR_VDP1_FUZZ_VALIDATION=1 keeps the
// validation layer on), and one seed replays with
// BRIMIR_VDP1_FUZZ_SEED=S brimir_tests "[vdp1-fuzz-one]". Several long runs can share the device (one process each).
// VDP1 upscaling (GPU stage 3): BRIMIR_VDP1_FUZZ_NX=N (2-4) runs both at internal resolution N with the N x sprite
// surface compared with the N x reference (vdp1_nx_reference.hpp) at every BRIMIR_VDP1_FUZZ_NX_EVERY-th checkpoint
// (default 1) and every frame's end; [gpu][vdp1][fuzz][nx] runs a few seeds at 2x and 4x.
#include "catch_amalgamated.hpp"

#include "gpu_test_device.hpp"
#include "vdp2_scene.hpp"

#include <gpu/vdp/vulkan_vdp_renderer.hpp>

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif !defined(_WIN32)
#include <unistd.h>
#endif

using brimir::test::FbramResult;
using brimir::test::OpenDevice;
using brimir::test::RenderBothFBRAM;
using brimir::test::RenderBothOptions;
using brimir::test::Vdp2Scene;
using brimir::test::Vdp2SceneWriter;

namespace {

namespace gpu = brimir::gpu;
using ymir::vdp::VDPState;

// VDP2 registers
constexpr uint32_t TVMD = 0x000, BGON = 0x020, BKTAU = 0x0AC, BKTAL = 0x0AE, SPCTL = 0x0E0, PRISA = 0x0F0,
                   PRISD = 0x0F6;
// VDP1 registers
constexpr uint32_t TVMR = 0x00, FBCR = 0x02, EWDR = 0x06, EWLR = 0x08, EWRR = 0x0A;

// VDP1 VRAM layout: the command table at 0, look-up tables, Gouraud tables, textures
constexpr uint32_t kTableSlots = 64;
constexpr uint32_t kLUTBase = 0x8000, kLUTCount = 32;
constexpr uint32_t kGouraudBase = 0x9000, kGouraudCount = 64;
constexpr uint32_t kTextureBase = 0x10000, kTextureEnd = 0x40000;

struct Rng {
    uint64_t s;
    uint64_t Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int Range(int lo, int hi) {
        return lo + static_cast<int>(Next() % static_cast<uint64_t>(static_cast<int64_t>(hi) - lo + 1));
    }
    bool Chance(uint32_t percent) {
        return Next() % 100 < percent;
    }
    template <typename T, size_t N>
    T Pick(const T (&values)[N]) {
        return values[Next() % N];
    }
};

uint16_t U(int v) {
    return static_cast<uint16_t>(v);
}

// ---------------------------------------------------------------------------------------------------------------------
// Programs

enum class K : uint8_t {
    VDP1Reg,     // a register, b value
    VDP2Reg,     // a register, b value
    VRAM,        // VDP1 VRAM words w at address a
    Draw,        // DrawVDP1Partial(a, b)
    FB16,        // a 0 draw / 1 display framebuffer, byte address b, value w[0]
    FB8,         // the same with a byte
    Read16,      // a CPU read of the draw framebuffer at b (the renderers' values compared)
    Read8,
    Erase,       // EraseFramebuffer(a cycles)
    Swap,        // an explicit framebuffer swap
    SaveLoad,    // each renderer saves and loads its own state
    CrossLoad,   // software loads the GPU renderer's state and the GPU renderer software's
    SaveCompare, // both save; the SaveState fields are compared
    Reset,       // ResetVDP(a != 0)
    Display,     // the display setup: CRAM and priorities from seed a, SPCTL b, TVMD w[0]
    Enhance,     // enhancements: a bit 0 deinterlace, bit 1 transparent meshes
};

constexpr const char *kKindNames[] = {"VDP1Reg", "VDP2Reg", "VRAM",        "Draw",  "FB16",    "FB8",
                                      "Read16",  "Read8",   "Erase",       "Swap",  "SaveLoad", "CrossLoad",
                                      "SaveCompare", "Reset", "Display", "Enhance"};
constexpr uint32_t kKinds = sizeof(kKindNames) / sizeof(kKindNames[0]);

// Phases: 0 before the frame's VDP1 frame control (at its last line), 1 + y before RenderLine(y) (y = 0 after the
// frame control), kBlank after the VDP2 frame's end and the display erase. Frame 0 is the setup (before frame 1).
constexpr uint16_t kPre = 0, kBlank = 0xFFFF;

struct FuzzOp {
    K kind;
    uint16_t frame;
    uint16_t phase;
    uint32_t a = 0;
    uint32_t b = 0;
    std::vector<uint16_t> w;
};

struct FuzzProgram {
    uint64_t seed = 0;
    uint32_t frames = 2;
    uint32_t enhancements = 0;    // bit 0 deinterlace, bit 1 transparent meshes
    uint32_t vblankCycles = 4000; // the VBlank erase budget
    gpu::vdp1::Vdp1BatchLimits limits;
    // 0: an FBRAM checkpoint after every op. N > 0: after about one op in N (and at every frame's end): a checkpoint
    // submits the pending VDP1 work, which ends the front half's batch, so only programs with few checkpoints keep
    // draws, VRAM and CPU writes in one batch (the VRAM hazard splits, the write masks).
    uint32_t checkpointEvery = 0;
    std::vector<FuzzOp> ops; // sorted by (frame, phase), stable
};

ymir::vdp::config::Enhancements EnhancementsOf(uint32_t bits) {
    return {.deinterlace = (bits & 1) != 0, .transparentMeshes = (bits & 2) != 0};
}

void Sort(FuzzProgram &p) {
    std::stable_sort(p.ops.begin(), p.ops.end(), [](const FuzzOp &x, const FuzzOp &y) {
        return x.frame != y.frame ? x.frame < y.frame : x.phase < y.phase;
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// The generator

struct Texture {
    uint16_t srca;
    uint32_t bytes;
};

struct Gen {
    Rng rng;
    FuzzProgram p;
    std::vector<Texture> textures;
    uint16_t frame = 0;
    uint16_t phase = 0;
    uint16_t tvmd = 0x8000;
    uint32_t tableCount = 0; // commands in the current table (End at tableCount)
    std::array<std::vector<uint16_t>, kTableSlots> table; // the table's commands as written

    void Add(K kind, uint32_t a = 0, uint32_t b = 0, std::vector<uint16_t> w = {}) {
        p.ops.push_back({kind, frame, phase, a, b, std::move(w)});
    }

    uint16_t RandomTVMD() {
        static constexpr uint16_t kTVMD[] = {0x8000, 0x8000, 0x8000, 0x8001, 0x8002, 0x8003, 0x8010,
                                             0x80C0, 0x80C0, 0x80C1, 0x80D0, 0x8080, 0x8090, 0x80C3};
        return rng.Pick(kTVMD);
    }
    // (The RNG is called once per full expression or in braced lists, which are sequenced, so a seed is the same
    // program with every compiler.)
    uint16_t RandomTVMR() {
        const int tvm = rng.Chance(45) ? 0 : rng.Chance(55) ? 1 : rng.Chance(50) ? 3 : rng.Range(0, 7);
        const int vbe = rng.Chance(25) ? 8 : 0;
        return static_cast<uint16_t>(tvm | vbe);
    }
    uint16_t RandomFBCR() {
        // FCT, FCM (mostly the 1-cycle mode), DIL, DIE, EOS
        uint16_t v = static_cast<uint16_t>(rng.Range(0, 7) << 2);
        if (rng.Chance(25)) {
            v |= static_cast<uint16_t>(2 | (rng.Chance(50) ? 1 : 0));
        }
        return v;
    }

    // A CPU framebuffer address, mostly in the drawn area
    uint32_t FbAddress() {
        if (rng.Chance(80)) {
            const int y = rng.Range(0, 260);
            const int x = rng.Range(0, 420);
            return static_cast<uint32_t>((y * 512 + x) * 2);
        }
        return static_cast<uint32_t>(rng.Range(0, 0x3FFFF));
    }

    void Display() {
        static constexpr uint16_t kSPCTL[] = {0x0020, 0x0020, 0x0000, 0x0025, 0x0008, 0x000C, 0x002F};
        const uint32_t seed = static_cast<uint32_t>(rng.Next());
        Add(K::Display, seed, rng.Pick(kSPCTL), {tvmd});
    }

    void EraseRegs() {
        Add(K::VDP1Reg, EWDR, static_cast<uint16_t>(rng.Chance(30) ? 0 : rng.Next()));
        uint32_t x1 = static_cast<uint32_t>(rng.Range(0, 20)), y1 = static_cast<uint32_t>(rng.Range(0, 100));
        uint32_t x3 = x1 + static_cast<uint32_t>(rng.Range(0, 64)), y3 = y1 + static_cast<uint32_t>(rng.Range(0, 300));
        switch (rng.Range(0, 7)) {
        case 0: x3 = x1; break;
        case 1: y3 = y1; break;
        case 2: x3 = 0x7F, y3 = 0x1FF; break; // past the 400/428 clamps
        case 3: x1 = 0, y1 = 0, x3 = 0x7F, y3 = 0x1FF; break;
        case 4: x1 = static_cast<uint32_t>(rng.Range(0, 0x7F)), y1 = static_cast<uint32_t>(rng.Range(0, 0x1FF));
            x3 = static_cast<uint32_t>(rng.Range(0, 0x7F)), y3 = static_cast<uint32_t>(rng.Range(0, 0x1FF));
            break;
        default: break;
        }
        Add(K::VDP1Reg, EWLR, ((x1 & 0x3F) << 9) | (y1 & 0x1FF));
        Add(K::VDP1Reg, EWRR, ((x3 & 0x7F) << 9) | (y3 & 0x1FF));
    }

    std::vector<uint16_t> TextureWords(uint32_t bytes) {
        std::vector<uint8_t> b(bytes);
        const uint32_t row = std::max<uint32_t>(8, static_cast<uint32_t>(rng.Range(1, 8)) * 8);
        for (uint32_t y = 0; y * row < bytes; ++y) {
            const int kind = rng.Range(0, 99);
            for (uint32_t i = y * row; i < std::min(bytes, (y + 1) * row); ++i) {
                uint8_t v;
                if (kind < 12) {
                    v = 0x00; // a transparent row
                } else if (kind < 18 && i - y * row < 4) {
                    v = 0xFF; // a row starting with end codes
                } else {
                    const int r = rng.Range(0, 99);
                    v = r < 22 ? 0x00 : r < 30 ? 0xFF : static_cast<uint8_t>(rng.Next());
                }
                b[i] = v;
            }
        }
        for (uint32_t i = 0; i + 1 < bytes; i += 2) {
            if (rng.Chance(3)) {
                b[i] = 0x7F, b[i + 1] = 0xFF; // the RGB end code
            } else if (rng.Chance(50) && (b[i] | b[i + 1]) != 0) {
                b[i] |= 0x80; // RGB texels
            }
        }
        std::vector<uint16_t> w(bytes / 2);
        for (size_t i = 0; i < w.size(); ++i) {
            w[i] = static_cast<uint16_t>((b[i * 2] << 8) | b[i * 2 + 1]);
        }
        return w;
    }

    void Assets() {
        uint32_t address = kTextureBase;
        const uint32_t count = static_cast<uint32_t>(rng.Range(4, 14));
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t bytes = static_cast<uint32_t>(rng.Range(1, 64)) * 64;
            if (address + bytes > kTextureEnd) {
                break;
            }
            Add(K::VRAM, address, 0, TextureWords(bytes));
            textures.push_back({static_cast<uint16_t>(address / 8), bytes});
            address += (bytes + 31) & ~31u;
        }
        for (uint32_t i = 0; i < kLUTCount; ++i) {
            std::vector<uint16_t> lut(16);
            for (uint16_t &c : lut) {
                c = static_cast<uint16_t>(rng.Chance(25) ? rng.Range(0, 0x7FF) : 0x8000 | rng.Range(0, 0x7FFF));
            }
            Add(K::VRAM, kLUTBase + i * 32, 0, lut);
        }
        std::vector<uint16_t> g(kGouraudCount * 4);
        for (uint16_t &c : g) {
            c = static_cast<uint16_t>(rng.Next());
        }
        Add(K::VRAM, kGouraudBase, 0, g);
    }

    int Axis(int limit) {
        const int r = rng.Range(0, 999);
        if (r < 720) {
            return rng.Range(-40, limit + 40);
        } else if (r < 930) {
            return rng.Range(-300, limit + 300);
        } else if (r < 985) {
            return rng.Range(-1100, limit + 1100);
        }
        return rng.Range(-4096, 4095);
    }

    std::array<int, 2> Point(int w, int h, bool nearOnly) {
        if (nearOnly) {
            return {rng.Range(-40, w + 40), rng.Range(-40, h + 40)};
        }
        if (rng.Chance(2)) {
            return {static_cast<int16_t>(rng.Next()), static_cast<int16_t>(rng.Next())}; // raw 16-bit (13-bit wrap)
        }
        return {Axis(w), Axis(h)};
    }

    std::vector<uint16_t> RandomCommand(bool largeClip) {
        const int w = 352, h = 240;
        const int kind = rng.Range(0, 99);
        if (kind < 4) {
            // system clip: mostly the screen, sometimes small, sometimes raw values beyond the registers' 10/9 bits
            const int r = rng.Range(0, 99);
            int ch = 319, cv = 223;
            if (r < 40) {
                ch = rng.Range(0, 420), cv = rng.Range(0, 300);
            } else if (r < 60) {
                ch = rng.Range(0, 40), cv = rng.Range(0, 40);
            } else if (r < 66) {
                ch = rng.Range(0x400, 0xFFFF), cv = rng.Range(0x200, 0xFFFF);
            } else if (r < 80) {
                ch = 703, cv = 511;
            }
            return {0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, U(ch), U(cv), 0, 0, 0, 0};
        }
        if (kind < 9) {
            int x0 = rng.Range(-8, 330), y0 = rng.Range(-8, 250);
            int x1 = rng.Chance(80) ? rng.Range(x0, 340) : rng.Range(-8, 340);
            int y1 = rng.Chance(80) ? rng.Range(y0, 260) : rng.Range(-8, 260);
            if (rng.Chance(10)) {
                x1 = rng.Range(0x400, 0xFFFF), y1 = rng.Range(0x200, 0xFFFF);
            }
            return {static_cast<uint16_t>(rng.Chance(80) ? 0x0008 : 0x000B), 0, 0, 0, 0, 0, U(x0), U(y0), 0, 0, U(x1),
                    U(y1), 0, 0, 0, 0};
        }
        if (kind < 13) {
            const int lx = rng.Chance(90) ? rng.Range(-80, 80) : static_cast<int>(rng.Next() & 0xFFFF);
            const int ly = rng.Chance(90) ? rng.Range(-80, 80) : static_cast<int>(rng.Next() & 0xFFFF);
            return {0x000A, 0, 0, 0, 0, 0, U(lx), U(ly), 0, 0, 0, 0, 0, 0, 0, 0};
        }
        if (kind < 14) {
            return {static_cast<uint16_t>(rng.Range(0xC, 0xF)), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        }

        const bool nearOnly = largeClip && !rng.Chance(5);
        uint16_t pmod = static_cast<uint16_t>(rng.Next() & 0x9FFF);
        if (!rng.Chance(2)) {
            while (((pmod >> 3) & 7) > 5) {
                pmod = static_cast<uint16_t>((pmod & ~0x0038) | (rng.Range(0, 5) << 3));
            }
        }
        if (!rng.Chance(15)) {
            pmod &= ~0x1000; // high-speed shrink mostly off
        }
        if (!rng.Chance(12)) {
            pmod &= ~0x8000; // MSB on mostly off
        }
        const uint32_t cm = (pmod >> 3) & 7;
        uint16_t colr = static_cast<uint16_t>(rng.Next());
        if (cm == 1 && rng.Chance(85)) {
            colr = static_cast<uint16_t>((kLUTBase + static_cast<uint32_t>(rng.Range(0, kLUTCount - 1)) * 32) / 8);
        }
        const uint16_t grda = static_cast<uint16_t>(
            rng.Chance(90) ? (kGouraudBase + static_cast<uint32_t>(rng.Range(0, kGouraudCount - 1)) * 8) / 8
                           : rng.Next());
        uint32_t sizeH = rng.Chance(4) ? 0 : static_cast<uint32_t>(rng.Range(1, 8));
        uint32_t sizeV = rng.Chance(4) ? 0 : static_cast<uint32_t>(rng.Range(1, 40));
        if (rng.Chance(4)) {
            sizeH = static_cast<uint32_t>(rng.Range(9, 63));
        }
        if (rng.Chance(4)) {
            sizeV = static_cast<uint32_t>(rng.Range(41, 255));
        }
        const uint16_t srca = !textures.empty() && rng.Chance(90)
                                  ? textures[rng.Next() % textures.size()].srca
                                  : static_cast<uint16_t>(rng.Range(kTextureBase / 8, 0xFFFF));
        const uint16_t size = static_cast<uint16_t>((sizeH << 8) | sizeV);
        const int flip = rng.Range(0, 3), zoomPoint = rng.Range(0, 15), skip = rng.Chance(4) ? 0x4000 : 0;
        const uint16_t flags = static_cast<uint16_t>((flip << 4) | (zoomPoint << 8) | skip);

        auto a = Point(w, h, nearOnly), b = Point(w, h, nearOnly), c = Point(w, h, nearOnly),
             d = Point(w, h, nearOnly);
        if (rng.Chance(55)) {
            // shrink towards a point near the screen (keeps the fragment count, and the run time, down)
            const int mx = rng.Range(-20, w + 20), my = rng.Range(-20, h + 20);
            const int k = rng.Range(1, 3);
            for (auto *q : {&a, &b, &c, &d}) {
                *q = {mx + ((*q)[0] - mx) * k / 8, my + ((*q)[1] - my) * k / 8};
            }
        }
        const int shape = rng.Range(0, 99);
        if (shape < 15) {
            switch (rng.Range(0, 3)) { // triangles; B = C stacks lines at the apex
            case 0: b = a; break;
            case 1: c = b; break;
            case 2: d = c; break;
            default: a = d; break;
            }
        } else if (shape < 30) {
            b = {c[0], a[1]};
            d = {a[0], c[1]};
        } else if (shape < 48) {
            const int ux = rng.Range(-12, 12), uy = rng.Range(-12, 12);
            const int ka = rng.Range(1, 30), kb = rng.Range(1, 30);
            b = {a[0] + ux * ka, a[1] + uy * ka};
            c = {b[0] - uy * kb, b[1] + ux * kb};
            d = {a[0] - uy * kb, a[1] + ux * kb};
        } else if (shape < 58) {
            const int cx = rng.Chance(50) ? 0 : w, cy = rng.Chance(50) ? 0 : h;
            for (auto *q : {&a, &b, &c, &d}) {
                *q = {cx + rng.Range(-20, 20), cy + rng.Range(-20, 20)};
            }
        } else if (shape < 70) {
            // small overlapping shapes near the middle: deep per-word stacks
            for (auto *q : {&a, &b, &c, &d}) {
                *q = {w / 2 + rng.Range(-30, 30), h / 2 + rng.Range(-30, 30)};
            }
        } else if (shape < 73) {
            a = b = c = d; // a point
        }

        const int type = rng.Range(0, 99);
        if (type < 15) {
            return {flags, 0, pmod, colr, srca, size, U(a[0]), U(a[1]), 0, 0, 0, 0, 0, 0, grda, 0};
        }
        if (type < 33) {
            // XB/YB: the zoom point's width and height
            const auto zoom = [&](int small, int large) {
                return nearOnly || rng.Chance(95) ? rng.Range(-60, rng.Chance(70) ? small : large)
                                                  : static_cast<int>(rng.Next() & 0xFFFF);
            };
            const int xb = zoom(100, 400), yb = zoom(80, 300);
            return {static_cast<uint16_t>(0x0001 | flags), 0, pmod, colr, srca, size, U(a[0]), U(a[1]), U(xb), U(yb),
                    U(c[0]), U(c[1]), 0, 0, grda, 0};
        }
        uint16_t command;
        if (type < 55) {
            command = rng.Chance(85) ? 2 : 3;
        } else if (type < 80) {
            command = 4;
        } else if (type < 90) {
            command = rng.Chance(85) ? 5 : 7;
        } else {
            command = 6;
        }
        if (command >= 4) {
            pmod &= static_cast<uint16_t>(rng.Chance(90) ? ~0x0038 : 0xFFFF); // colour mode bits: ignored, mostly 0
        }
        return {static_cast<uint16_t>(command | (command >= 4 ? flags & 0x4000 : flags)), 0, pmod, colr, srca, size,
                U(a[0]), U(a[1]), U(b[0]), U(b[1]), U(c[0]), U(c[1]), U(d[0]), U(d[1]), grda, 0};
    }

    // A VRAM write into what one of the first `drawn` commands of the table read (its texture's first texels, its
    // look-up table, its Gouraud table): with no checkpoint between, the next draw shares the batch, and the GPU must
    // not see the new data for the earlier commands (the front half's hazard split).
    void HazardWrite(uint32_t drawn) {
        const uint32_t slot = static_cast<uint32_t>(rng.Range(0, static_cast<int>(std::min(drawn, tableCount)) - 1));
        const std::vector<uint16_t> &cmd = table[slot];
        if (cmd.size() < 16) {
            ExtraOp(false);
            return;
        }
        const uint32_t type = cmd[0] & 0xF;
        const uint32_t cm = (cmd[2] >> 3) & 7;
        const uint16_t value = static_cast<uint16_t>(rng.Chance(20) ? 0 : rng.Next());
        const int r = rng.Range(0, 99);
        if (type <= 3 && r < 55) {
            Add(K::VRAM, cmd[4] * 8u + static_cast<uint32_t>(rng.Range(0, 63)) * 2, 0, {value, value});
        } else if (type <= 3 && cm == 1 && r < 80) {
            Add(K::VRAM, (cmd[3] * 8u + static_cast<uint32_t>(rng.Range(0, 15)) * 2) & 0x7FFFE, 0, {value});
        } else if ((cmd[2] & 4) != 0) {
            Add(K::VRAM, (cmd[14] * 8u + static_cast<uint32_t>(rng.Range(0, 3)) * 2) & 0x7FFFE, 0, {value});
        } else {
            ExtraOp(false);
        }
    }

    // An op between draws. The display framebuffer changes (CPU writes to it, explicit swaps and erases) only where the
    // VDP can change it: outside the active display (`outside`: before the frame's first line, in blanking). During
    // the active display the bus reaches only the draw framebuffer and VDP1 swaps and erases happen at the last line
    // and in blanking, so the display framebuffer is the same for every line of a frame (the GPU renderer composes
    // the frame from it at the frame's end).
    void ExtraOp(bool outside) {
        const int r = rng.Range(0, 99);
        if (r < 22) {
            // VRAM: a texture, a look-up table, a Gouraud table or a table entry (also one not drawn yet)
            const int t = rng.Range(0, 99);
            if (t < 40 && !textures.empty()) {
                const Texture &tex = textures[rng.Next() % textures.size()];
                const uint32_t off = static_cast<uint32_t>(rng.Range(0, static_cast<int>(tex.bytes / 2) - 1)) * 2;
                std::vector<uint16_t> w(static_cast<size_t>(rng.Range(1, 16)));
                for (uint16_t &v : w) {
                    v = static_cast<uint16_t>(rng.Chance(20) ? 0 : rng.Next());
                }
                Add(K::VRAM, tex.srca * 8u + off, 0, w);
            } else if (t < 55) {
                const uint32_t address = kLUTBase + static_cast<uint32_t>(rng.Range(0, kLUTCount * 16 - 1)) * 2;
                Add(K::VRAM, address, 0, {static_cast<uint16_t>(rng.Next())});
            } else if (t < 70) {
                const uint32_t address = kGouraudBase + static_cast<uint32_t>(rng.Range(0, kGouraudCount * 4 - 1)) * 2;
                Add(K::VRAM, address, 0, {static_cast<uint16_t>(rng.Next())});
            } else if (t < 85 && tableCount > 0) {
                // a command's colour, draw mode or a vertex word
                static constexpr uint32_t kWords[] = {2, 3, 6, 7, 8, 9, 10, 11, 12, 13};
                const uint32_t slot = static_cast<uint32_t>(rng.Range(0, static_cast<int>(tableCount) - 1));
                const uint32_t word = rng.Pick(kWords);
                const uint16_t value = static_cast<uint16_t>(rng.Range(0, 0x13F));
                if (table[slot].size() == 16) {
                    table[slot][word] = value;
                }
                Add(K::VRAM, slot * 0x20 + word * 2, 0, {value});
            } else if (tableCount > 0) {
                const uint32_t slot = static_cast<uint32_t>(rng.Range(0, static_cast<int>(tableCount) - 1));
                table[slot] = RandomCommand(false);
                Add(K::VRAM, slot * 0x20, 0, table[slot]);
            }
        } else if (r < 44) {
            const uint32_t count = static_cast<uint32_t>(rng.Range(1, 6));
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t address = FbAddress();
                const uint32_t fb = outside && rng.Chance(40) ? 1 : 0;
                if (rng.Chance(35)) {
                    const uint32_t odd = static_cast<uint32_t>(rng.Range(0, 1));
                    Add(K::FB8, fb, (address + odd) & 0x3FFFF, {static_cast<uint16_t>(rng.Next() & 0xFF)});
                } else {
                    Add(K::FB16, fb, address & 0x3FFFE, {static_cast<uint16_t>(rng.Next())});
                }
            }
        } else if (r < 60) {
            const uint32_t count = static_cast<uint32_t>(rng.Range(1, 4));
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t address = FbAddress();
                if (rng.Chance(35)) {
                    Add(K::Read8, 0, (address + static_cast<uint32_t>(rng.Range(0, 1))) & 0x3FFFF);
                } else {
                    Add(K::Read16, 0, address & 0x3FFFE);
                }
            }
        } else if (r < 70) {
            if (rng.Chance(50)) {
                Add(K::VDP1Reg, TVMR, RandomTVMR());
            } else {
                Add(K::VDP1Reg, FBCR, RandomFBCR());
            }
        } else if (r < 74) {
            EraseRegs();
        } else if (r < 78 && outside) {
            const int c = rng.Range(0, 99);
            Add(K::Erase, c < 40 ? 0 : c < 75 ? static_cast<uint32_t>(rng.Range(1, 3000))
                                              : static_cast<uint32_t>(rng.Range(3000, 300000)));
        } else if (r < 80 && outside) {
            Add(K::Swap);
        } else if (r < 80) {
            Add(K::Read16, 0, static_cast<uint32_t>(rng.Range(0, 0x1FFFF)) * 2);
        } else if (r < 86) {
            Add(K::SaveLoad);
        } else if (r < 91) {
            Add(K::CrossLoad);
        } else if (r < 96) {
            Add(K::SaveCompare);
        } else if (r < 97 && phase != kBlank) {
            Add(K::Reset, rng.Chance(50) ? 1 : 0);
            tvmd = 0; // the reset TVMD (and resolution)
            if (rng.Chance(80)) {
                tvmd = RandomTVMD();
                Display();
                EraseRegs();
                Add(K::VDP1Reg, TVMR, RandomTVMR());
                Add(K::VDP1Reg, FBCR, RandomFBCR());
            }
        } else if (r < 98) {
            tvmd = RandomTVMD();
            Add(K::VDP2Reg, TVMD, tvmd);
        } else {
            Add(K::VDP2Reg, SPCTL, static_cast<uint16_t>(rng.Range(0, 0x3F)));
        }
    }

    void Generate(uint64_t seed) {
        rng.s = seed * 0x2545F4914F6CDD1Dull + 0x1234567;
        p.seed = seed;
        p.frames = static_cast<uint32_t>(rng.Chance(8) ? rng.Range(5, 6) : rng.Range(2, 4));
        p.enhancements = static_cast<uint32_t>(rng.Range(0, 3));
        p.vblankCycles = static_cast<uint32_t>(rng.Chance(20) ? 200000 : rng.Range(1, 8000));
        if (rng.Chance(25)) {
            p.limits.fragments = static_cast<uint32_t>(rng.Chance(50) ? rng.Range(1, 64) : rng.Range(64, 20000));
        }
        if (rng.Chance(8)) {
            p.limits.spans = static_cast<uint32_t>(rng.Range(1, 40));
        }
        if (rng.Chance(8)) {
            p.limits.commands = static_cast<uint32_t>(rng.Range(1, 4));
        }
        if (rng.Chance(8)) {
            p.limits.fbWrites = static_cast<uint32_t>(rng.Range(1, 3));
        }
        p.checkpointEvery = static_cast<uint32_t>(rng.Chance(50) ? 0 : rng.Range(3, 40));

        // Setup (frame 0)
        frame = 0;
        phase = kPre;
        tvmd = RandomTVMD();
        Display();
        Assets();
        Add(K::VDP1Reg, TVMR, RandomTVMR());
        Add(K::VDP1Reg, FBCR, RandomFBCR());
        EraseRegs();
        if (rng.Chance(30)) {
            // framebuffer content from the CPU before anything is drawn
            const uint32_t count = static_cast<uint32_t>(rng.Range(1, 40));
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t fb = static_cast<uint32_t>(rng.Range(0, 1));
                const uint32_t address = static_cast<uint32_t>(rng.Range(0, 0x1FFFF)) * 2;
                Add(K::FB16, fb, address, {static_cast<uint16_t>(rng.Next())});
            }
        }

        for (uint32_t f = 1; f <= p.frames; ++f) {
            frame = static_cast<uint16_t>(f);
            phase = kPre;
            if (rng.Chance(30)) {
                Add(K::VDP1Reg, TVMR, RandomTVMR());
            }
            if (rng.Chance(35)) {
                Add(K::VDP1Reg, FBCR, RandomFBCR());
            }
            if (rng.Chance(30)) {
                EraseRegs();
            }
            if (rng.Chance(10)) {
                tvmd = RandomTVMD();
                Add(K::VDP2Reg, TVMD, tvmd);
            }
            if (rng.Chance(6)) {
                Add(K::Enhance, static_cast<uint32_t>(rng.Range(0, 3)));
            }
            const uint32_t pre = static_cast<uint32_t>(rng.Chance(30) ? rng.Range(1, 3) : 0);
            for (uint32_t e = 0; e < pre; ++e) {
                ExtraOp(true);
            }

            // The frame's command table, drawn in parts at increasing lines with other ops between them
            const bool newTable = f == 1 || rng.Chance(85);
            std::vector<uint32_t> lines;
            const uint32_t parts = static_cast<uint32_t>(rng.Chance(30) ? rng.Range(6, 16) : rng.Range(1, 6));
            for (uint32_t i = 0; i < parts; ++i) {
                lines.push_back(static_cast<uint32_t>(rng.Range(0, 223)));
            }
            std::sort(lines.begin(), lines.end());
            phase = static_cast<uint16_t>(1 + lines[0]);
            bool largeClip = false;
            if (newTable) {
                tableCount = static_cast<uint32_t>(rng.Chance(10) ? rng.Range(40, kTableSlots - 2) : rng.Range(1, 30));
                for (uint32_t slot = 0; slot < tableCount; ++slot) {
                    std::vector<uint16_t> cmd = RandomCommand(largeClip);
                    if ((cmd[0] & 0x800F) == 0x0009) {
                        largeClip = cmd[10] > 1023 || cmd[11] > 1023;
                    }
                    table[slot] = cmd;
                    Add(K::VRAM, slot * 0x20, 0, std::move(cmd));
                }
                Add(K::VRAM, tableCount * 0x20, 0, {0x8000});
            }
            const uint32_t slots = tableCount + 1;
            uint32_t next = 0;
            for (uint32_t i = 0; i < parts; ++i) {
                phase = static_cast<uint16_t>(1 + lines[i]);
                const uint32_t extras = static_cast<uint32_t>(rng.Range(0, i == 0 ? 1 : 4));
                for (uint32_t e = 0; e < extras; ++e) {
                    if (next > 0 && rng.Chance(40)) {
                        HazardWrite(next);
                    } else {
                        ExtraOp(false);
                    }
                }
                // The last part mostly reaches the end code; otherwise the frame's drawing stays unfinished (as when
                // a frame's commands outlast the frame). Commands are never executed past the end code.
                const uint32_t left = slots - next;
                const uint32_t count = i + 1 == parts && rng.Chance(85)
                                           ? left
                                           : static_cast<uint32_t>(rng.Range(1, static_cast<int>(left)));
                Add(K::Draw, next, count);
                next += count;
                if (next >= slots) {
                    break;
                }
            }
            phase = static_cast<uint16_t>(1 + rng.Range(static_cast<int>(lines.back()), 223));
            const uint32_t late = static_cast<uint32_t>(rng.Range(0, 3));
            for (uint32_t e = 0; e < late; ++e) {
                ExtraOp(false);
            }
            phase = kBlank;
            const uint32_t blank = static_cast<uint32_t>(rng.Chance(40) ? rng.Range(1, 3) : 0);
            for (uint32_t e = 0; e < blank; ++e) {
                ExtraOp(true);
            }
        }
        Sort(p);
    }
};

FuzzProgram Generate(uint64_t seed) {
    Gen g;
    g.Generate(seed);
    return g.p;
}

// ---------------------------------------------------------------------------------------------------------------------
// The interpreter: a Vdp2Scene that applies the ops (an FBRAM checkpoint after each) and the VDP's frame control

struct FuzzLog {
    std::string diff; // the first difference the ops found (CPU reads, SaveState fields)
    std::array<uint64_t, kKinds> ops{};
    uint64_t checkpoints = 0;
    uint64_t lsmdSwitches = 0; // TVMD writes that switched the interlace mode inside a frame (not in blanking)
};

ymir::vdp::InterlaceMode CurrentLSMD(Vdp2SceneWriter &w) {
    ymir::vdp::InterlaceMode lsmd{};
    bool first = true;
    w.ForEachState([&](VDPState &s) {
        if (first) {
            lsmd = s.regs2.TVMD.LSMDn;
            first = false;
        }
    });
    return lsmd;
}

void ApplyDisplay(Vdp2SceneWriter &w, uint32_t seed, uint16_t spctl, uint16_t tvmd) {
    Rng r{seed};
    w.Reg(TVMD, tvmd);
    w.VRAM16(0x7FFF0, static_cast<uint16_t>(r.Next()));
    w.Reg(BKTAU, static_cast<uint16_t>((0x7FFF0 >> 17) & 7));
    w.Reg(BKTAL, static_cast<uint16_t>((0x7FFF0 >> 1) & 0xFFFF));
    for (uint32_t a = 0; a < 0x1000; a += 2) {
        w.CRAM16(a, static_cast<uint16_t>(r.Next()));
    }
    w.Reg(SPCTL, spctl);
    for (uint32_t reg = PRISA; reg <= PRISD; reg += 2) {
        const uint32_t lo = 1 + r.Next() % 7;
        const uint32_t hi = 1 + r.Next() % 7;
        w.Reg(reg, static_cast<uint16_t>(lo | (hi << 8)));
    }
    w.Reg(BGON, 0);
}

std::string CompareSaves(Vdp2SceneWriter &w) {
    const auto saved = w.SaveStates();
    if (saved.size() != 2) {
        return "SaveStates: " + std::to_string(saved.size()) + " states";
    }
    const ymir::savestate::VDPSaveState &sw = *saved[0];
    const ymir::savestate::VDPSaveState &vk = *saved[1];
    std::string d;
    if (sw.VRAM1 != vk.VRAM1) d += " VRAM1";
    if (sw.FBRAM != vk.FBRAM) d += " FBRAM";
    if (sw.displayFB != vk.displayFB) d += " displayFB";
    if (sw.renderer.vdp1State.doubleV != vk.renderer.vdp1State.doubleV) {
        d += " doubleV (software " + std::to_string(sw.renderer.vdp1State.doubleV) + ", gpu " +
             std::to_string(vk.renderer.vdp1State.doubleV) + ")";
    }
    if (sw.renderer.vdp1State.meshFBRAM != vk.renderer.vdp1State.meshFBRAM) d += " meshFBRAM";
    if (sw.renderer.vcellScrollInc != vk.renderer.vcellScrollInc) {
        d += " vcellScrollInc (software " + std::to_string(sw.renderer.vcellScrollInc) + ", gpu " +
             std::to_string(vk.renderer.vcellScrollInc) + ")";
    }
    return d.empty() ? d : "SaveState fields differ:" + d;
}

void Exec(Vdp2SceneWriter &w, const FuzzOp &op, FuzzLog &log, const std::string &where) {
    ++log.ops[static_cast<uint32_t>(op.kind)];
    const auto fb = [&] { return op.a == 0 ? w.DrawFramebuffer() : w.DrawFramebuffer() ^ 1; };
    const auto note = [&](const std::string &d) {
        if (log.diff.empty() && !d.empty()) {
            log.diff = where + ": " + d;
        }
    };
    // Mid-frame interlace mode switches (coverage)
    const bool tvmdWrite = op.frame != 0 && op.phase != kBlank &&
                           ((op.kind == K::VDP2Reg && op.a == TVMD) || op.kind == K::Display);
    const ymir::vdp::InterlaceMode lsmdBefore = tvmdWrite ? CurrentLSMD(w) : ymir::vdp::InterlaceMode{};
    switch (op.kind) {
    case K::VDP1Reg: w.VDP1Reg(op.a, static_cast<uint16_t>(op.b)); break;
    case K::VDP2Reg: w.Reg(op.a, static_cast<uint16_t>(op.b)); break;
    case K::VRAM:
        for (size_t i = 0; i < op.w.size(); ++i) {
            w.VDP1VRAM16((op.a + static_cast<uint32_t>(i) * 2) & 0x7FFFE, op.w[i]);
        }
        break;
    case K::Draw: w.DrawVDP1Partial(op.a, op.b); break;
    case K::FB16: w.FBRAM16(fb(), op.b & 0x3FFFE, op.w.empty() ? 0 : op.w[0]); break;
    case K::FB8: w.FBRAM8(fb(), op.b & 0x3FFFF, static_cast<uint8_t>(op.w.empty() ? 0 : op.w[0])); break;
    case K::Read16: {
        const auto v = w.ReadFB16(op.b & 0x3FFFE);
        if (v.size() != 2) {
            note("CPU read16: " + std::to_string(v.size()) + " values, not 2");
        } else if (v[0] != v[1]) {
            std::ostringstream s;
            s << "CPU read16 0x" << std::hex << (op.b & 0x3FFFE) << ": software 0x" << v[0] << ", gpu 0x" << v[1];
            note(s.str());
        }
        break;
    }
    case K::Read8: {
        const auto v = w.ReadFB8(op.b & 0x3FFFF);
        if (v.size() != 2) {
            note("CPU read8: " + std::to_string(v.size()) + " values, not 2");
        } else if (v[0] != v[1]) {
            std::ostringstream s;
            s << "CPU read8 0x" << std::hex << (op.b & 0x3FFFF) << ": software 0x" << +v[0] << ", gpu 0x" << +v[1];
            note(s.str());
        }
        break;
    }
    case K::Erase: w.EraseFramebuffer(op.a); break;
    case K::Swap: w.SwapFramebuffers(); break;
    case K::SaveLoad: w.SaveLoadMidFrame(); break;
    case K::CrossLoad: w.CrossLoad(); break;
    case K::SaveCompare: note(CompareSaves(w)); break;
    case K::Reset: w.ResetVDP(op.a != 0); break;
    case K::Display:
        ApplyDisplay(w, op.a, static_cast<uint16_t>(op.b), op.w.empty() ? uint16_t{0x8000} : op.w[0]);
        break;
    case K::Enhance: w.ConfigureEnhancements(EnhancementsOf(op.a)); break;
    }
    if (tvmdWrite && CurrentLSMD(w) != lsmdBefore) {
        ++log.lsmdSwitches;
    }
}

Vdp2Scene MakeScene(const FuzzProgram &p, std::shared_ptr<FuzzLog> log) {
    struct Ctl {
        uint32_t frame = 0; // the frame being run (1-based; 0 = setup)
        size_t cursor = 0;
        bool doDisplayErase = false;
        bool doVBlankErase = false;
    };
    auto ctl = std::make_shared<Ctl>();
    auto prog = std::make_shared<FuzzProgram>(p);
    // Runs the ops of the current frame with phase <= `upTo`, each followed by a checkpoint (or about one in
    // checkpointEvery)
    const auto run = [ctl, prog, log](Vdp2SceneWriter &w, uint32_t upTo) {
        while (ctl->cursor < prog->ops.size()) {
            const FuzzOp &op = prog->ops[ctl->cursor];
            if (op.frame != ctl->frame || op.phase > upTo) {
                break;
            }
            const size_t index = ctl->cursor++;
            Exec(w, op, *log, "op " + std::to_string(index) + " (" + kKindNames[static_cast<uint32_t>(op.kind)] + ")");
            Rng pick{prog->seed * 1000003u + index};
            if (prog->checkpointEvery == 0 || pick.Next() % prog->checkpointEvery == 0) {
                w.Checkpoint();
                ++log->checkpoints;
            }
        }
    };
    Vdp2Scene scene;
    scene.frames = p.frames;
    scene.enhancements = EnhancementsOf(p.enhancements);
    scene.setup = [ctl, run](Vdp2SceneWriter &w) {
        *ctl = Ctl{};
        run(w, kBlank);
    };
    const uint64_t vblankCycles = p.vblankCycles;
    const bool everyOp = p.checkpointEvery == 0;
    scene.onLine = [ctl, run, vblankCycles, everyOp](Vdp2SceneWriter &w, uint32_t y) {
        if (y == 0) {
            ++ctl->frame;
            run(w, kPre);
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
                w.EraseFramebuffer(vblankCycles);
            }
            if (erase) {
                ctl->doDisplayErase = true;
            }
            if (swap) {
                w.SwapFramebuffers();
            }
            if (everyOp) {
                w.Checkpoint();
            }
        }
        run(w, 1 + y);
    };
    scene.blanking = [ctl, run, everyOp](Vdp2SceneWriter &w) {
        run(w, kBlank - 1); // line ops past the field's last line
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
            if (everyOp) {
                w.Checkpoint();
            }
        }
        run(w, kBlank);
    };
    return scene;
}

struct FuzzResult {
    bool failed = false;
    std::string kind; // of the first difference: fbram, frame, the op's (CPU read, SaveState) or harness
    std::string what;
    uint64_t checkpoints = 0;
    uint64_t maxNonZero = 0;
    std::array<uint64_t, kKinds> ops{};
    uint64_t fragmentSlots = 0, spans = 0, batches = 0;
    uint64_t lsmdSwitches = 0;
    uint64_t nxCompares = 0, nxDetailBlocks = 0;
};

uint64_t EnvU64(const char *name, uint64_t fallback) {
    const char *v = std::getenv(name);
    return v != nullptr ? std::strtoull(v, nullptr, 10) : fallback;
}

// `hazardTracking` false breaks the GPU renderer on purpose (the front half's VRAM hazard batch splits off): the
// sensitivity check. `nx` > 1: VDP1 upscaling at internal resolution nx, the N x surface compared with the N x
// reference at every nxEvery-th checkpoint and every frame's end (vdp2_scene.hpp RenderBothOptions::vdp1Upscaling);
// frames are compared at the sample points.
FuzzResult RunProgram(gpu::VulkanDevice &device, const FuzzProgram &p, bool hazardTracking = true, uint32_t nx = 0,
                      uint32_t nxEvery = 1) {
    auto log = std::make_shared<FuzzLog>();
    RenderBothOptions options;
    if (nx > 1) {
        options.vdp1Upscaling = true;
        options.scale = nx;
        options.nxCompareEvery = nxEvery;
    }
    options.compareFrame = [](uint32_t) { return true; };
    const gpu::vdp1::Vdp1BatchLimits limits = p.limits;
    FuzzResult result;
    options.beforeFrame = [limits, hazardTracking](uint32_t frame, gpu::VulkanVDPRenderer &vk, gpu::VulkanDevice &) {
        if (frame == 0) {
            vk.SetVdp1BatchLimitsForTest(limits);
            vk.SetVdp1HazardTrackingForTest(hazardTracking);
        }
    };
    options.afterFrame = [&result](uint32_t, gpu::VulkanVDPRenderer &vk, gpu::VulkanDevice &) {
        const auto &s = vk.Vdp1Front().Stats();
        result.fragmentSlots = s.fragmentSlots;
        result.spans = s.spans;
        result.batches = s.batches;
    };
    const FbramResult r = RenderBothFBRAM(MakeScene(p, log), device, options);
    result.checkpoints = r.checkpoints;
    result.maxNonZero = r.maxNonZero;
    result.ops = log->ops;
    result.lsmdSwitches = log->lsmdSwitches;
    result.nxCompares = r.nxCompares;
    result.nxDetailBlocks = r.nxDetailBlocks;
    std::string what;
    if (!r.error.empty() || !r.frame.error.empty() || !r.identical || !r.frame.identical) {
        what = r.Describe();
    }
    if (nx > 1 && r.error.empty() && ((r.nxScales >> nx) & 1u) == 0) {
        what += (what.empty() ? "" : "; ") + std::string("the N x surface was never compared at ") +
                std::to_string(nx) + "x";
    }
    if (!log->diff.empty()) {
        what += (what.empty() ? "" : "; ") + log->diff;
    }
    result.failed = !what.empty();
    result.what = what;
    // The kind of the first difference (the minimiser keeps it)
    if (!r.error.empty() || !r.frame.error.empty()) {
        result.kind = "harness";
    } else if (r.count > 0) {
        result.kind = "fbram";
    } else if (r.nxCount > 0 || r.nxNativeCount > 0) {
        result.kind = "nx";
    } else if (!log->diff.empty()) {
        result.kind = log->diff.substr(log->diff.find(':') + 1, 24);
    } else if (!r.frame.identical) {
        result.kind = "frame";
    }
    return result;
}

// Removes ops (and trailing frames) while the program still fails the same way: ddmin over the op list.
FuzzProgram Minimize(gpu::VulkanDevice &device, FuzzProgram p, uint32_t maxRuns = 3000, uint32_t nx = 0) {
    uint32_t runs = 0;
    const std::string kind = RunProgram(device, p, true, nx).kind;
    const auto fails = [&](const FuzzProgram &q) {
        ++runs;
        const FuzzResult r = RunProgram(device, q, true, nx);
        return r.failed && r.kind == kind;
    };
    while (p.frames > 1) {
        FuzzProgram q = p;
        --q.frames;
        std::erase_if(q.ops, [&](const FuzzOp &op) { return op.frame > q.frames; });
        if (!fails(q)) {
            break;
        }
        p = std::move(q);
    }
    size_t chunk = std::max<size_t>(1, p.ops.size() / 2);
    while (runs < maxRuns) {
        bool removed = false;
        for (size_t start = 0; start < p.ops.size() && runs < maxRuns;) {
            FuzzProgram q = p;
            q.ops.erase(q.ops.begin() + static_cast<std::ptrdiff_t>(start),
                        q.ops.begin() + static_cast<std::ptrdiff_t>(std::min(p.ops.size(), start + chunk)));
            if (fails(q)) {
                p = std::move(q);
                removed = true;
            } else {
                start += chunk;
            }
        }
        if (chunk == 1 && !removed) {
            break;
        }
        if (!removed) {
            chunk = std::max<size_t>(1, chunk / 2);
        }
    }
    // Shorten VRAM writes (textures, tables) word by word from the end
    for (size_t i = 0; i < p.ops.size() && runs < maxRuns; ++i) {
        while (p.ops[i].kind == K::VRAM && p.ops[i].w.size() > 1 && runs < maxRuns) {
            FuzzProgram q = p;
            q.ops[i].w.resize(q.ops[i].w.size() / 2);
            if (!fails(q)) {
                break;
            }
            p = std::move(q);
        }
    }
    return p;
}

std::string ToCpp(const FuzzProgram &p) {
    std::ostringstream s;
    s << "// seed " << p.seed << "\n";
    s << "FuzzProgram{" << p.seed << ", " << p.frames << ", " << p.enhancements << ", " << p.vblankCycles << ", {"
      << p.limits.fragments << ", " << p.limits.spans << ", " << p.limits.commands << ", " << p.limits.fbWrites
      << "}, " << p.checkpointEvery << ", {\n";
    for (const FuzzOp &op : p.ops) {
        s << "    {K::" << kKindNames[static_cast<uint32_t>(op.kind)] << ", " << op.frame << ", " << op.phase << ", 0x"
          << std::hex << op.a << ", 0x" << op.b << std::dec << ", {";
        for (size_t i = 0; i < op.w.size(); ++i) {
            s << (i ? ", " : "") << "0x" << std::hex << op.w[i] << std::dec;
        }
        s << "}},\n";
    }
    s << "}}";
    return s.str();
}

} // namespace

TEST_CASE("GPU VDP1 fuzz: seeded random programs, FBRAM after every op and frames equal software",
          "[gpu][vdp1][fuzz]") {
    auto device = OpenDevice();
    const uint64_t seeds = device->Properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 12 : 24;
    uint64_t checkpoints = 0, nonZero = 0, fragmentSlots = 0, lsmdSwitches = 0;
    std::array<uint64_t, kKinds> ops{};
    for (uint64_t seed = 1; seed <= seeds; ++seed) {
        const FuzzResult r = RunProgram(*device, Generate(seed));
        INFO("seed " << seed << ": " << r.what);
        CHECK_FALSE(r.failed);
        checkpoints += r.checkpoints;
        nonZero = std::max(nonZero, r.maxNonZero);
        fragmentSlots += r.fragmentSlots;
        lsmdSwitches += r.lsmdSwitches;
        for (uint32_t k = 0; k < kKinds; ++k) {
            ops[k] += r.ops[k];
        }
    }
    CHECK(checkpoints > seeds * 10);
    CHECK(nonZero > 0);
    // Not vacuous: fragments drawn, every op kind run and the interlace mode switched mid-frame
    CHECK(fragmentSlots > 0);
    CHECK(lsmdSwitches > 0);
    for (uint32_t k = 0; k < kKinds; ++k) {
        INFO(kKindNames[k]);
        CHECK(ops[k] > 0);
    }
    brimir::test::CloseAndCheckValidation(device);
}

// Minimised fuzzer finds (Task 7 fix round 1): a VDP reset in double density with deinterlacing clears doubleV, as
// software's Reset does (the front half kept it): the save state, and the draws that continue
// after the reset without a VDP1 frame start.
TEST_CASE("GPU VDP1 fuzz reproducers: a reset in double density clears doubleV as software", "[gpu][vdp1][fuzz]") {
    const FuzzProgram programs[] = {
        {190, 3, 3, 1252, {4194304, 262144, 16384, 262144}, 0, {
            {K::Display, 0, 0, 0xa59ad3b9, 0x0, {0x80c3}},
            {K::Draw, 3, 30, 0x0, 0x3, {}},
            {K::Reset, 3, 110, 0x1, 0x0, {}},
            {K::SaveCompare, 3, 142, 0x0, 0x0, {}},
        }},
        {520, 3, 1, 5640, {4194304, 262144, 16384, 262144}, 26, {
            {K::Display, 0, 0, 0xf15a72d5, 0xc, {0x80c1}},
            {K::VRAM, 3, 52, 0x0, 0x0, {0x4300}},
            {K::VRAM, 3, 52, 0x20, 0x0, {0xb}},
            {K::VRAM, 3, 52, 0x3a0, 0x0, {0xe12, 0x0, 0x64e, 0x704f, 0x2148, 0x328, 0x53, 0x56}},
            {K::Draw, 3, 52, 0x0, 0x1a, {}},
            {K::Reset, 3, 216, 0x0, 0x0, {}},
            {K::Draw, 3, 216, 0x1d, 0x2, {}},
        }},
        {312, 1, 3, 4861, {48, 262144, 16384, 262144}, 0, {
            {K::Display, 0, 0, 0xef3e3e9c, 0x2f, {0x80d0}},
            {K::VRAM, 1, 45, 0xa0, 0x0, {0x5}},
            {K::VRAM, 1, 45, 0xc0, 0x0, {0x4, 0x0, 0xd86, 0x1078, 0xa621, 0x615, 0x13f, 0x46}},
            {K::Draw, 1, 45, 0x0, 0x5, {}},
            {K::Reset, 1, 64, 0x0, 0x0, {}},
            {K::Draw, 1, 64, 0x5, 0x2, {}},
        }},
    };
    auto device = OpenDevice();
    for (const FuzzProgram &p : programs) {
        const FuzzResult r = RunProgram(*device, p);
        INFO("program " << p.seed << ": " << r.what);
        CHECK_FALSE(r.failed);
        CHECK(r.checkpoints > 0);
    }
    brimir::test::CloseAndCheckValidation(device);
}

TEST_CASE("GPU VDP1 fuzz: VDP1 upscaling, the N x surface equal to the N x reference at 2x and 4x",
          "[gpu][vdp1][fuzz][nx]") {
    // The same programs with the N x sprite surface: native FBRAM, CPU reads and saves still equal software's, the
    // frames at the sample points, and the surface equals the reference at every checkpoint (the long run:
    // BRIMIR_VDP1_FUZZ_NX=N)
    auto device = OpenDevice();
    const uint64_t seeds = device->Properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 2 : 8;
    for (const uint32_t n : {2u, 4u}) {
        uint64_t compares = 0, detail = 0, fragmentSlots = 0;
        for (uint64_t seed = 1; seed <= seeds; ++seed) {
            const FuzzResult r = RunProgram(*device, Generate(seed), true, n);
            INFO(n << "x, seed " << seed << ": " << r.what);
            CHECK_FALSE(r.failed);
            compares += r.nxCompares;
            detail = std::max(detail, r.nxDetailBlocks);
            fragmentSlots += r.fragmentSlots;
        }
        INFO(n << "x");
        CHECK(compares > seeds * 5);
        CHECK(detail > 0);
        CHECK(fragmentSlots > 0);
    }
    brimir::test::CloseAndCheckValidation(device);
}

TEST_CASE("GPU VDP1 fuzz: the fuzzer notices a broken GPU renderer (VRAM hazard tracking off)", "[gpu][vdp1][fuzz]") {
    auto device = OpenDevice();
    uint32_t failing = 0, seeds = 0;
    for (uint64_t seed = 1; seed <= 60 && failing < 2; ++seed, ++seeds) {
        failing += RunProgram(*device, Generate(seed), false).failed ? 1 : 0;
    }
    INFO(failing << " of " << seeds << " seeds failing");
    CHECK(failing == 2);
    brimir::test::CloseAndCheckValidation(device);
}

namespace {

// The first line of `command`'s output (stderr included), or "?" when it cannot run.
std::string CommandLine(const std::string &command) {
#ifdef _WIN32
    FILE *pipe = _popen(command.c_str(), "r");
#else
    FILE *pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return "?";
    }
    char line[512] = {};
    const bool got = std::fgets(line, sizeof(line), pipe) != nullptr;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    std::string text = got ? line : "?";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

std::string ExecutablePath() {
#ifdef _WIN32
    char *path = nullptr;
    return _get_pgmptr(&path) == 0 && path != nullptr ? std::string(path) : "?";
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    return _NSGetExecutablePath(buffer, &size) == 0 ? std::string(buffer) : "?";
#else
    char buffer[4096];
    const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    return n > 0 ? std::string(buffer, static_cast<size_t>(n)) : "?";
#endif
}

// The long run's log header, so a log says what produced it: the source tree's commit (git at run time; "-dirty" with
// local changes) and the one the build was configured at, the binary, the device and the environment that steers the
// run.
void PrintFuzzHeader(const VkPhysicalDeviceProperties &device) {
    std::printf("[vdp1-fuzz-long] commit %s (git at run time in %s), %s at configure time\n",
                CommandLine(std::string("git -C \"") + BRIMIR_SOURCE_DIR + "\" describe --always --dirty --abbrev=12 2>&1")
                    .c_str(),
                BRIMIR_SOURCE_DIR, BRIMIR_GIT_DESCRIBE);
    std::printf("[vdp1-fuzz-long] binary %s\n", ExecutablePath().c_str());
    std::printf("[vdp1-fuzz-long] device %s (API %u.%u.%u, driver 0x%x)\n", device.deviceName,
                VK_API_VERSION_MAJOR(device.apiVersion), VK_API_VERSION_MINOR(device.apiVersion),
                VK_API_VERSION_PATCH(device.apiVersion), device.driverVersion);
    std::string env;
    for (const char *name : {"BRIMIR_VDP1_FUZZ_SEEDS", "BRIMIR_VDP1_FUZZ_FIRST", "BRIMIR_VDP1_FUZZ_MINIMIZE",
                             "BRIMIR_VDP1_FUZZ_VALIDATION", "BRIMIR_VDP1_FUZZ_SEED_LIMIT", "BRIMIR_VDP1_FUZZ_NX",
                             "BRIMIR_VDP1_FUZZ_NX_EVERY", "BRIMIR_VK_DEVICE",
                             "VK_DRIVER_FILES", "VK_ICD_FILENAMES"}) {
        const char *value = std::getenv(name);
        env += std::string(" ") + name + "=" + (value != nullptr ? value : "(unset)");
    }
    std::printf("[vdp1-fuzz-long] env%s\n", env.c_str());
    std::fflush(stdout);
}

} // namespace

TEST_CASE("GPU VDP1 fuzz: the long run", "[gpu][vdp1][fuzz][vdp1-fuzz-long][.]") {
    const bool validation = EnvU64("BRIMIR_VDP1_FUZZ_VALIDATION", 0) != 0;
    auto device = OpenDevice({.validation = validation});
    PrintFuzzHeader(device->Properties());
    const uint64_t seeds = EnvU64("BRIMIR_VDP1_FUZZ_SEEDS", 1000);
    const uint64_t first = EnvU64("BRIMIR_VDP1_FUZZ_FIRST", 1);
    const bool minimize = EnvU64("BRIMIR_VDP1_FUZZ_MINIMIZE", 0) != 0;
    // VDP1 upscaling at N (2-4) with the N x surface compared at every BRIMIR_VDP1_FUZZ_NX_EVERY-th checkpoint
    const uint32_t nx = static_cast<uint32_t>(EnvU64("BRIMIR_VDP1_FUZZ_NX", 0));
    const uint32_t nxEvery = static_cast<uint32_t>(EnvU64("BRIMIR_VDP1_FUZZ_NX_EVERY", 1));
    uint64_t nxCompares = 0, nxDetail = 0;
    const auto start = std::chrono::steady_clock::now();
    uint64_t checkpoints = 0, nonZero = 0, fragmentSlots = 0, spans = 0, batches = 0, lsmdSwitches = 0;
    std::array<uint64_t, kKinds> ops{};
    std::vector<uint64_t> failing;
    // A watchdog: a seed that runs longer than BRIMIR_VDP1_FUZZ_SEED_LIMIT seconds (default 600) is reported and the
    // process exits with an error (a hung submit or wait cannot be interrupted from here).
    const uint64_t seedLimit = EnvU64("BRIMIR_VDP1_FUZZ_SEED_LIMIT", 600);
    std::atomic<uint64_t> currentSeed{first};
    std::atomic<int64_t> seedStartMs{0};
    std::atomic<bool> done{false};
    const auto nowMs = [start] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    };
    std::thread watchdog([&] {
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const int64_t running = nowMs() - seedStartMs.load();
            if (!done.load() && running > static_cast<int64_t>(seedLimit) * 1000) {
                std::printf("STUCK seed %llu: running for %lld s (limit %llu s); exiting\n",
                            static_cast<unsigned long long>(currentSeed.load()), static_cast<long long>(running / 1000),
                            static_cast<unsigned long long>(seedLimit));
                std::fflush(stdout);
                std::_Exit(3);
            }
        }
    });
    // An exception leaving the loop (a failed REQUIRE in RunProgram, bad_alloc) must not destroy a joinable thread
    // (std::terminate): stop and join the watchdog on every exit from this scope.
    struct StopWatchdog {
        std::atomic<bool> &done;
        std::thread &thread;
        ~StopWatchdog() {
            done = true;
            if (thread.joinable()) {
                thread.join();
            }
        }
    } stopWatchdog{done, watchdog};
    for (uint64_t seed = first; seed < first + seeds; ++seed) {
        currentSeed = seed;
        seedStartMs = nowMs();
        const FuzzProgram p = Generate(seed);
        const FuzzResult r = RunProgram(*device, p, true, nx, nxEvery);
        nxCompares += r.nxCompares;
        nxDetail = std::max(nxDetail, r.nxDetailBlocks);
        checkpoints += r.checkpoints;
        nonZero = std::max(nonZero, r.maxNonZero);
        fragmentSlots += r.fragmentSlots;
        spans += r.spans;
        batches += r.batches;
        lsmdSwitches += r.lsmdSwitches;
        for (uint32_t k = 0; k < kKinds; ++k) {
            ops[k] += r.ops[k];
        }
        if (r.failed) {
            failing.push_back(seed);
            std::printf("FAIL seed %llu: %s\n", static_cast<unsigned long long>(seed), r.what.c_str());
            if (minimize && failing.size() <= 3) {
                seedStartMs = INT64_MAX / 2; // no limit while minimising
                const FuzzProgram m = Minimize(*device, p, 3000, nx);
                const FuzzResult mr = RunProgram(*device, m, true, nx);
                std::printf("minimised (%zu ops): %s\n%s\n", m.ops.size(), mr.what.c_str(), ToCpp(m).c_str());
            }
            std::fflush(stdout);
        }
        if ((seed - first + 1) % 25 == 0) {
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("... seed %llu (%llu done), %.0f s, %zu failing\n", static_cast<unsigned long long>(seed),
                        static_cast<unsigned long long>(seed - first + 1), s, failing.size());
            std::fflush(stdout);
        }
    }
    done = true;
    if (watchdog.joinable()) {
        watchdog.join();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("VDP1 fuzz (%s): seeds %llu-%llu, %.1f s, %llu FBRAM checkpoints, %llu fragment slots, %llu spans, "
                "%llu batches, %llu mid-frame interlace switches, max %llu non-zero bytes; %zu failing\n",
                device->Properties().deviceName, static_cast<unsigned long long>(first),
                static_cast<unsigned long long>(first + seeds - 1), seconds,
                static_cast<unsigned long long>(checkpoints), static_cast<unsigned long long>(fragmentSlots),
                static_cast<unsigned long long>(spans), static_cast<unsigned long long>(batches),
                static_cast<unsigned long long>(lsmdSwitches), static_cast<unsigned long long>(nonZero),
                failing.size());
    for (uint32_t k = 0; k < kKinds; ++k) {
        std::printf("  %-12s %llu\n", kKindNames[k], static_cast<unsigned long long>(ops[k]));
    }
    if (nx > 1) {
        std::printf("  N x surface at %ux: %llu compares with the reference (every %u checkpoints and frame ends), up to "
                    "%llu blocks with detail\n",
                    nx, static_cast<unsigned long long>(nxCompares), nxEvery, static_cast<unsigned long long>(nxDetail));
        CHECK(nxCompares > seeds * 5);
    }
    std::fflush(stdout);
    CHECK(failing.empty());
    CHECK(checkpoints > seeds * 10);
    if (validation) {
        brimir::test::CloseAndCheckValidation(device);
    }
}

TEST_CASE("GPU VDP1 fuzz: replay one seed (BRIMIR_VDP1_FUZZ_SEED)", "[gpu][vdp1][fuzz][vdp1-fuzz-one][.]") {
    auto device = OpenDevice();
    const FuzzProgram p = Generate(EnvU64("BRIMIR_VDP1_FUZZ_SEED", 1));
    const uint32_t nx = static_cast<uint32_t>(EnvU64("BRIMIR_VDP1_FUZZ_NX", 0));
    const FuzzResult r = RunProgram(*device, p, true, nx);
    uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a of the printed program (the same with every compiler)
    for (const char ch : ToCpp(p)) {
        hash = (hash ^ static_cast<uint8_t>(ch)) * 0x100000001B3ull;
    }
    std::printf("seed %llu: %zu ops (program %016llx), %llu checkpoints: %s\n", static_cast<unsigned long long>(p.seed),
                p.ops.size(), static_cast<unsigned long long>(hash), static_cast<unsigned long long>(r.checkpoints),
                r.failed ? r.what.c_str() : "identical");
    if (r.failed && EnvU64("BRIMIR_VDP1_FUZZ_MINIMIZE", 0) != 0) {
        const FuzzProgram m = Minimize(*device, p, 3000, nx);
        const FuzzResult mr = RunProgram(*device, m, true, nx);
        std::printf("minimised (%zu ops): %s\n%s\n", m.ops.size(), mr.what.c_str(), ToCpp(m).c_str());
    }
    CHECK_FALSE(r.failed);
    brimir::test::CloseAndCheckValidation(device);
}
