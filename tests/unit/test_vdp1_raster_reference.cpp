// The CPU reference of the GPU VDP1 rasterizer (vdp1_raster_reference.hpp) against the steppers and against the
// unthreaded SoftwareVDPRenderer's FBRAM (main framebuffers, alternate fields, meshes), byte for byte.

#include "catch_amalgamated.hpp"

#include "vdp1_raster_reference.hpp"

#include <ymir/hw/vdp/renderer/common/vdp1_steppers.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ymir;
namespace ref = brimir::test::vdp1ref;
using vdp::CoordS32;

// splitmix64; deterministic on every platform
struct TestRng {
    uint64 s;
    explicit TestRng(uint64 seed)
        : s(seed) {}
    uint64 Next() {
        uint64 z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    sint32 Range(sint32 lo, sint32 hi) {
        return lo + static_cast<sint32>(Next() % static_cast<uint64>(static_cast<sint64>(hi) - lo + 1));
    }
    bool Chance(uint32 percent) {
        return Next() % 100 < percent;
    }
    template <typename T, size_t N>
    T Pick(const T (&values)[N]) {
        return values[Next() % N];
    }
};

uint16 U(sint32 v) {
    return static_cast<uint16>(v);
}

// -----------------------------------------------------------------------------
// Scenarios: registers, VRAM and an op list, run on both renderers

struct Op {
    enum class Kind { BeginFrame, Command, EndFrame, WriteTVMR, WriteFBCR, WriteTVMD, Erase, Swap };
    Kind kind;
    uint32 address = 0; // Command
    uint16 value = 0;   // register writes; Erase: the write value
    uint16 x1 = 0, y1 = 0, x3 = 0, y3 = 0;
    uint64 cycles = 0;
};

struct Scenario {
    static constexpr uint32 kVRAMSize = 0x20000; // the first 128 KiB of VDP1 VRAM; the rest is zero
    static constexpr uint32 kCommandBase = 0x0000;
    static constexpr uint32 kGouraudBase = 0x1000;
    static constexpr uint32 kTextureBase = 0x2000;

    uint16 tvmd = 0x8000;
    uint16 tvmr = 0x0000;
    uint16 fbcr = 0x0000;
    bool deinterlace = false;
    bool transparentMeshes = false;
    uint32 vres = 224;
    ref::ClipContext clip;
    std::vector<uint8> vram = std::vector<uint8>(kVRAMSize, 0);
    std::vector<uint8> fbFill; // tiled over both main framebuffers; empty = zero
    std::vector<Op> ops;
    uint32 maxBatchFragments = 0xFFFFFFFFu;

    uint32 nextCommand = kCommandBase;
    uint32 nextGouraud = kGouraudBase;
    uint32 nextTexture = kTextureBase;

    void Write16(uint32 address, uint16 value) {
        vram[address] = static_cast<uint8>(value >> 8);
        vram[address + 1] = static_cast<uint8>(value);
    }

    bool CanAddCommand() const {
        return nextCommand < kGouraudBase;
    }

    // CMDCTRL, CMDLINK, CMDPMOD, CMDCOLR, CMDSRCA, CMDSIZE, XA, YA, XB, YB, XC, YC, XD, YD, CMDGRDA, unused
    void AddCommand(const std::array<uint16, 16> &words) {
        if (!CanAddCommand()) {
            throw std::logic_error("Scenario: command table full");
        }
        for (uint32 i = 0; i < 16; ++i) {
            Write16(nextCommand + i * 2, words[i]);
        }
        ops.push_back({.kind = Op::Kind::Command, .address = nextCommand});
        nextCommand += 0x20;
    }

    // Returns CMDSRCA (address / 8), or 0 if it does not fit
    uint16 AddTexture(const std::vector<uint8> &bytes) {
        const uint32 address = nextTexture;
        if (address + bytes.size() > kVRAMSize) {
            return 0;
        }
        std::copy(bytes.begin(), bytes.end(), vram.begin() + address);
        nextTexture = (address + static_cast<uint32>(bytes.size()) + 31) & ~31u;
        return static_cast<uint16>(address / 8);
    }

    uint16 AddGouraud(std::array<uint16, 4> colors) {
        const uint32 address = nextGouraud;
        if (address >= kTextureBase) {
            return static_cast<uint16>(kGouraudBase / 8);
        }
        for (uint32 i = 0; i < 4; ++i) {
            Write16(address + i * 2, colors[i]);
        }
        nextGouraud += 8;
        return static_cast<uint16>(address / 8);
    }

    void Polygon(uint16 pmod, uint16 color, CoordS32 a, CoordS32 b, CoordS32 c, CoordS32 d, uint16 grda = 0) {
        AddCommand({0x0004, 0, pmod, color, 0, 0, U(a.x()), U(a.y()), U(b.x()), U(b.y()), U(c.x()), U(c.y()), U(d.x()),
                    U(d.y()), grda, 0});
    }

    void Distorted(uint16 ctrlFlags, uint16 pmod, uint16 color, uint16 srca, uint16 size, CoordS32 a, CoordS32 b,
                   CoordS32 c, CoordS32 d, uint16 grda = 0) {
        AddCommand({static_cast<uint16>(0x0002 | ctrlFlags), 0, pmod, color, srca, size, U(a.x()), U(a.y()), U(b.x()),
                    U(b.y()), U(c.x()), U(c.y()), U(d.x()), U(d.y()), grda, 0});
    }

    void Op0(Op::Kind kind, uint16 value = 0) {
        ops.push_back({.kind = kind, .value = value});
    }
};

uint16 LSMD(uint16 tvmd) {
    return (tvmd >> 6) & 3;
}

// The unthreaded software renderer after running a scenario
struct SoftwareRun {
    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<vdp::SoftwareVDPRenderer> renderer;

    const vdp::SpriteFB &Main(uint32 i) const {
        return state->mem1.FBRAM[i];
    }
    const vdp::SpriteFB &Alt(uint32 i) const {
        return renderer->GetAltFBRAM(static_cast<uint8>(i));
    }
    const vdp::SpriteFB &Mesh(uint32 a, uint32 i) const {
        return renderer->GetMeshFBRAM(a != 0, static_cast<uint8>(i));
    }
};

void FillMain(const Scenario &s, std::array<vdp::SpriteFB, 2> &fbs) {
    for (auto &fb : fbs) {
        if (s.fbFill.empty()) {
            fb.fill(0);
        } else {
            for (size_t i = 0; i < fb.size(); ++i) {
                fb[i] = s.fbFill[i % s.fbFill.size()];
            }
        }
    }
}

std::unique_ptr<SoftwareRun> RunSoftware(const Scenario &s) {
    auto run = std::make_unique<SoftwareRun>();
    auto &state = *run->state;
    run->renderer = std::make_unique<vdp::SoftwareVDPRenderer>(state, run->debug, run->access);
    auto &renderer = *run->renderer;
    renderer.ConfigureEnhancements({.deinterlace = s.deinterlace, .transparentMeshes = s.transparentMeshes});

    state.regs2.WriteTVMD(s.tvmd);
    state.regs2.LatchTVMD();
    state.regs1.WriteTVMR(s.tvmr);
    state.regs1.WriteFBCR(s.fbcr);
    auto &ctx = state.state1;
    ctx.sysClipH = s.clip.sysClipH;
    ctx.sysClipV = s.clip.sysClipV;
    ctx.userClipX0 = s.clip.userClipX0;
    ctx.userClipY0 = s.clip.userClipY0;
    ctx.userClipX1 = s.clip.userClipX1;
    ctx.userClipY1 = s.clip.userClipY1;
    ctx.localCoordX = s.clip.localX;
    ctx.localCoordY = s.clip.localY;
    state.state2.layerEnabled[0] = true;
    std::copy(s.vram.begin(), s.vram.end(), state.mem1.VRAM.begin());
    FillMain(s, state.mem1.FBRAM);
    renderer.VDP2SetResolution(320, s.vres, false);
    renderer.PostLoadStateSync();

    for (const Op &op : s.ops) {
        switch (op.kind) {
        case Op::Kind::BeginFrame: renderer.VDP1BeginFrame(); break;
        case Op::Kind::Command: {
            const vdp::VDP1Command::Control control{.u16 = util::ReadBE<uint16>(&state.mem1.VRAM[op.address])};
            renderer.VDP1ExecuteCommand(op.address, control);
            break;
        }
        case Op::Kind::EndFrame: renderer.VDP1EndFrame(); break;
        case Op::Kind::WriteTVMR: state.regs1.WriteTVMR(op.value); break;
        case Op::Kind::WriteFBCR: state.regs1.WriteFBCR(op.value); break;
        case Op::Kind::WriteTVMD:
            state.regs2.WriteTVMD(op.value);
            state.regs2.LatchTVMD();
            break;
        case Op::Kind::Erase:
            state.regs1.eraseWriteValueLatch = op.value;
            state.regs1.eraseX1Latch = op.x1;
            state.regs1.eraseY1Latch = op.y1;
            state.regs1.eraseX3Latch = op.x3;
            state.regs1.eraseY3Latch = op.y3;
            renderer.VDP1EraseFramebuffer(op.cycles);
            break;
        case Op::Kind::Swap: state.fbIndex.Flip(); break;
        }
    }
    return run;
}

struct ReferenceRun {
    std::unique_ptr<ref::Fbram> fb = std::make_unique<ref::Fbram>();
    ref::ReferenceStats stats;
};

ReferenceRun RunReference(const Scenario &s, ref::RasterOptions options = {}, uint64 shuffleSeed = 0) {
    ReferenceRun run;
    // VDP1Memory::Reset's pattern past the scenario's 128 KiB, as in the software renderer's VDPState
    static const std::vector<uint8> kResetVRAM = [] {
        std::vector<uint8> v(vdp::kVDP1VRAMSize);
        for (uint32 addr = 0; addr < v.size(); addr++) {
            v[addr] = (addr & 0x1F) == 0 ? 0x80 : (addr & 0x1F) == 1 ? 0x00 : (addr & 2) == 2 ? 0x55 : 0xAA;
        }
        return v;
    }();
    std::vector<uint8> vram = kResetVRAM;
    std::copy(s.vram.begin(), s.vram.end(), vram.begin());
    FillMain(s, run.fb->main);

    ref::ReferenceVDP1 reference{vram.data(), *run.fb, options, s.maxBatchFragments, shuffleSeed};
    reference.clip = s.clip;
    vdp::VDP1Regs regs1;
    regs1.WriteTVMR(s.tvmr);
    regs1.WriteFBCR(s.fbcr);
    uint16 tvmd = s.tvmd & 0x81F7;
    uint32 doubleV = 0;
    vdp::FBRAMIndex fbIndex;

    for (const Op &op : s.ops) {
        switch (op.kind) {
        case Op::Kind::BeginFrame:
            doubleV = s.deinterlace && LSMD(tvmd) == 3 && !regs1.dblInterlaceEnable;
            break;
        case Op::Kind::Command:
            reference.Command(op.address, ref::RasterState{
                                              .fbSizeH = regs1.tvmr.fbSizeH,
                                              .pixel8Bits = regs1.tvmr.pixel8Bits,
                                              .dblInterlaceEnable = regs1.dblInterlaceEnable,
                                              .dblInterlaceDrawLine = regs1.dblInterlaceDrawLine,
                                              .evenOddCoordSelect = regs1.evenOddCoordSelect,
                                              .doubleDensity = LSMD(tvmd) == 3,
                                              .deinterlace = s.deinterlace,
                                              .transparentMeshes = s.transparentMeshes,
                                              .doubleV = doubleV,
                                              .drawFB = fbIndex.draw,
                                          });
            break;
        case Op::Kind::EndFrame: break;
        case Op::Kind::WriteTVMR: regs1.WriteTVMR(op.value); break;
        case Op::Kind::WriteFBCR: regs1.WriteFBCR(op.value); break;
        case Op::Kind::WriteTVMD: tvmd = op.value & 0x81F7; break;
        case Op::Kind::Erase:
            reference.Erase({
                .fbIndex = fbIndex.display,
                .value = op.value,
                .x1 = op.x1,
                .y1 = op.y1,
                .x3 = op.x3,
                .y3 = op.y3,
                .offsetShift = regs1.tvmr.eraseOffsetShift,
                .doubleDensity = LSMD(tvmd) == 3,
                .hiResH = (tvmd & 1) != 0,
                .vres = s.vres,
                .deinterlace = s.deinterlace,
                .transparentMeshes = s.transparentMeshes,
                .cycles = op.cycles,
            });
            break;
        case Op::Kind::Swap:
            reference.Flush();
            fbIndex.Flip();
            break;
        }
    }
    reference.Flush();
    run.stats = reference.Stats();
    return run;
}

// Empty if equal; otherwise the first difference and the number of differing bytes
std::string CompareFbram(const SoftwareRun &sw, const ref::Fbram &fb) {
    std::string first;
    uint64 count = 0;
    auto cmp = [&](const char *name, const vdp::SpriteFB &a, const vdp::SpriteFB &b) {
        if (std::memcmp(a.data(), b.data(), a.size()) == 0) {
            return;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) {
                if (count++ == 0) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "%s byte 0x%05zX (x %zu, y %zu at 16 bpp/512): software %02X reference %02X", name, i,
                                  (i / 2) % 512, (i / 2) / 512, a[i], b[i]);
                    first = buf;
                }
            }
        }
    };
    cmp("main fb0", sw.Main(0), fb.main[0]);
    cmp("main fb1", sw.Main(1), fb.main[1]);
    cmp("alt fb0", sw.Alt(0), fb.alt[0]);
    cmp("alt fb1", sw.Alt(1), fb.alt[1]);
    cmp("mesh[0] fb0", sw.Mesh(0, 0), fb.mesh[0][0]);
    cmp("mesh[0] fb1", sw.Mesh(0, 1), fb.mesh[0][1]);
    cmp("mesh[1] fb0", sw.Mesh(1, 0), fb.mesh[1][0]);
    cmp("mesh[1] fb1", sw.Mesh(1, 1), fb.mesh[1][1]);
    if (count == 0) {
        return {};
    }
    return first + " (" + std::to_string(count) + " bytes differ)";
}

// -----------------------------------------------------------------------------
// Random scenarios

struct Coverage {
    std::array<uint64, 8> commandTypes{};
    std::array<uint64, 8> colorModes{};
    std::array<uint64, 4> colorCalc{};
    uint64 gouraud = 0, msbOn = 0, hss = 0, endCodeRows = 0, transparentRows = 0, userClip = 0, clipMode1 = 0;
    std::array<std::array<uint64, 2>, 2> meshByTransparentMeshes{};
    uint64 draws8bpp = 0, drawsDIEDoubleDensity = 0, drawsDeinterlacedDoubleDensity = 0, drawsDoubleV = 0;
    uint64 rawLargeClip = 0, localCoords = 0, wrappedCoords = 0, sysClipCmds = 0, userClipCmds = 0;
    uint64 erases = 0, partialErases = 0, swaps = 0, midFrameRegWrites = 0, batchLimits = 0, multiFrame = 0;
};

CoordS32 RandomPoint(TestRng &rng, sint32 w, sint32 h, bool nearOnly, Coverage &cov) {
    if (nearOnly) {
        return {rng.Range(-48, w + 48), rng.Range(-48, h + 48)};
    }
    if (rng.Chance(3)) {
        ++cov.wrappedCoords;
        return {static_cast<sint16>(rng.Next()), static_cast<sint16>(rng.Next())}; // raw 16-bit: 13-bit sign extension
    }
    auto axis = [&](sint32 limit) {
        const uint32 r = static_cast<uint32>(rng.Range(0, 999));
        if (r < 700) {
            return rng.Range(-48, limit + 48);
        } else if (r < 920) {
            return rng.Range(-400, limit + 400);
        } else if (r < 980) {
            return rng.Range(-1100, limit + 1100);
        }
        return rng.Range(-4096, 4095);
    };
    return {axis(w), axis(h)};
}

std::vector<uint8> RandomTexture(TestRng &rng, uint32 bytesPerRow, uint32 rows, uint32 colorMode, Coverage &cov) {
    std::vector<uint8> bytes(bytesPerRow * rows);
    for (uint32 y = 0; y < rows; ++y) {
        const uint32 kind = static_cast<uint32>(rng.Range(0, 99));
        cov.transparentRows += kind < 15;
        cov.endCodeRows += kind >= 15 && kind < 22;
        for (uint32 i = 0; i < bytesPerRow; ++i) {
            uint8 v;
            if (kind < 15) {
                v = 0x00; // transparent row
            } else if (kind < 22 && i < 4) {
                v = colorMode == 5 ? (i % 2 == 0 ? 0x7F : 0xFF) : 0xFF; // row starting with end codes
            } else {
                const uint32 r = static_cast<uint32>(rng.Range(0, 99));
                v = r < 25 ? 0x00 : r < 32 ? 0xFF : static_cast<uint8>(rng.Next());
            }
            bytes[y * bytesPerRow + i] = v;
        }
        if (colorMode == 5) {
            for (uint32 i = 0; i + 1 < bytesPerRow; i += 2) {
                if (rng.Chance(4)) {
                    bytes[y * bytesPerRow + i] = 0x7F;
                    bytes[y * bytesPerRow + i + 1] = 0xFF;
                }
            }
        }
    }
    return bytes;
}

void RandomSysClip(TestRng &rng, uint16 &h, uint16 &v, Coverage &cov) {
    const uint32 r = static_cast<uint32>(rng.Range(0, 99));
    if (r < 35) {
        h = 319;
        v = 223;
    } else if (r < 70) {
        h = static_cast<uint16>(rng.Range(0, 420));
        v = static_cast<uint16>(rng.Range(0, 300));
    } else if (r < 90) {
        h = static_cast<uint16>(rng.Range(0, 40));
        v = static_cast<uint16>(rng.Range(0, 40));
    } else {
        // raw 16-bit values beyond the 10/9 bits hardware registers hold; software keeps them
        h = static_cast<uint16>(rng.Range(0x400, 0xFFFF));
        v = static_cast<uint16>(rng.Range(0x200, 0xFFFF));
        ++cov.rawLargeClip;
    }
}

void RandomUserClip(TestRng &rng, uint16 &x0, uint16 &y0, uint16 &x1, uint16 &y1, Coverage &cov) {
    x0 = U(rng.Range(-8, 330));
    y0 = U(rng.Range(-8, 250));
    x1 = U(rng.Chance(80) ? rng.Range(static_cast<sint16>(x0), 340) : rng.Range(-8, 340));
    y1 = U(rng.Chance(80) ? rng.Range(static_cast<sint16>(y0), 260) : rng.Range(-8, 260));
    if (rng.Chance(15)) {
        x1 = static_cast<uint16>(rng.Range(0x400, 0xFFFF));
        y1 = static_cast<uint16>(rng.Range(0x200, 0xFFFF));
        ++cov.rawLargeClip;
    }
    if (rng.Chance(5)) {
        x0 = static_cast<uint16>(rng.Range(0x400, 0xFFFF));
    }
    if (rng.Chance(5)) {
        y0 = static_cast<uint16>(rng.Range(0x200, 0xFFFF));
    }
}

// Registers the generator tracks to count what the draws see
struct GenRegs {
    uint16 tvmd, tvmr, fbcr;
    uint16 sysClipH, sysClipV;
};

void AddRandomDraw(TestRng &rng, Scenario &s, const GenRegs &regs, Coverage &cov) {
    const sint32 w = std::min<sint32>(regs.sysClipH, 420);
    const sint32 h = std::min<sint32>(regs.sysClipV, 320);
    // Far vertices with a raw large system clip make huge shapes; keep them rare there
    const bool nearOnly = (regs.sysClipH > 1023 || regs.sysClipV > 1023) && !rng.Chance(5);

    uint16 pmod = static_cast<uint16>(rng.Next() & 0x9FFF);
    if (!rng.Chance(2)) {
        while (((pmod >> 3) & 7) > 5) {
            pmod = static_cast<uint16>((pmod & ~0x0038) | (rng.Range(0, 5) << 3));
        }
    }
    if (!rng.Chance(15)) {
        pmod &= ~0x1000; // high-speed shrink mostly off
    }
    if (!rng.Chance(10)) {
        pmod &= ~0x8000; // MSB on mostly off
    }
    const uint32 colorMode = (pmod >> 3) & 7;
    uint16 color = static_cast<uint16>(rng.Next());
    if (colorMode == 1 && rng.Chance(85)) {
        color = static_cast<uint16>(rng.Range(0x400, 0x3FFF)); // look-up table inside the random VRAM
    }
    const uint16 grda = s.AddGouraud({static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next()),
                                      static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next())});

    uint32 sizeH = rng.Chance(5) ? 0 : static_cast<uint32>(rng.Range(1, 8)); // x8 pixels
    uint32 sizeV = rng.Chance(5) ? 0 : static_cast<uint32>(rng.Range(1, 40));
    if (rng.Chance(3)) {
        sizeH = static_cast<uint32>(rng.Range(9, 63));
    }
    if (rng.Chance(3)) {
        sizeV = static_cast<uint32>(rng.Range(41, 255));
    }
    const uint32 bytesPerRow = colorMode <= 1 ? sizeH * 4 : colorMode == 5 ? sizeH * 16 : sizeH * 8;
    uint16 srca = s.AddTexture(RandomTexture(rng, std::max(bytesPerRow, 16u), std::max(sizeV, 1u), colorMode, cov));
    if (srca == 0) {
        srca = static_cast<uint16>(rng.Range(Scenario::kTextureBase / 8, Scenario::kVRAMSize / 8 - 1)); // noise
    }
    const uint16 size = static_cast<uint16>((sizeH << 8) | sizeV);
    const uint16 flags = static_cast<uint16>((rng.Range(0, 3) << 4) | (rng.Range(0, 15) << 8)); // flip, zoom point

    auto a = RandomPoint(rng, w, h, nearOnly, cov);
    auto b = RandomPoint(rng, w, h, nearOnly, cov);
    auto c = RandomPoint(rng, w, h, nearOnly, cov);
    auto d = RandomPoint(rng, w, h, nearOnly, cov);
    if (rng.Chance(55)) {
        // shrink towards a random point near the screen (keeps the fragment count, and the test time, down)
        const CoordS32 m{rng.Range(-20, w + 20), rng.Range(-20, h + 20)};
        const sint32 k = rng.Range(1, 3);
        for (CoordS32 *p : {&a, &b, &c, &d}) {
            *p = {m.x() + (p->x() - m.x()) * k / 8, m.y() + (p->y() - m.y()) * k / 8};
        }
    }
    const uint32 shape = static_cast<uint32>(rng.Range(0, 99));
    if (shape < 15) {
        switch (rng.Range(0, 3)) { // triangles; B = C stacks lines at the apex
        case 0: b = a; break;
        case 1: c = b; break;
        case 2: d = c; break;
        default: a = d; break;
        }
    } else if (shape < 30) {
        b = {c.x(), a.y()};
        d = {a.x(), c.y()};
    } else if (shape < 50) {
        const sint32 ux = rng.Range(-12, 12), uy = rng.Range(-12, 12);
        const sint32 ka = rng.Range(1, 30), kb = rng.Range(1, 30);
        b = {a.x() + ux * ka, a.y() + uy * ka};
        c = {b.x() - uy * kb, b.y() + ux * kb};
        d = {a.x() - uy * kb, a.y() + ux * kb};
    } else if (shape < 62) {
        const sint32 cx = rng.Chance(50) ? 0 : w, cy = rng.Chance(50) ? 0 : h;
        a = {cx + rng.Range(-20, 20), cy + rng.Range(-20, 20)};
        b = {cx + rng.Range(-20, 20), cy + rng.Range(-20, 20)};
        c = {cx + rng.Range(-20, 20), cy + rng.Range(-20, 20)};
        d = {cx + rng.Range(-20, 20), cy + rng.Range(-20, 20)};
    } else if (shape < 70) {
        // small overlapping shapes near the middle: deep per-pixel stacks
        const sint32 cx = w / 2, cy = h / 2;
        a = {cx + rng.Range(-30, 30), cy + rng.Range(-30, 30)};
        b = {cx + rng.Range(-30, 30), cy + rng.Range(-30, 30)};
        c = {cx + rng.Range(-30, 30), cy + rng.Range(-30, 30)};
        d = {cx + rng.Range(-30, 30), cy + rng.Range(-30, 30)};
    }

    const uint32 type = static_cast<uint32>(rng.Range(0, 99));
    uint32 command;
    if (type < 16) {
        command = 0;
        s.AddCommand({static_cast<uint16>(flags), 0, pmod, color, srca, size, U(a.x()), U(a.y()), 0, 0, 0, 0, 0, 0,
                      grda, 0});
    } else if (type < 34) {
        command = 1;
        // XB/YB: the zoom point width and height
        auto zoomSize = [&](sint32 small, sint32 large) {
            return nearOnly || rng.Chance(95) ? rng.Range(-60, rng.Chance(70) ? small : large)
                                              : static_cast<sint32>(rng.Next() & 0xFFFF);
        };
        const sint32 xb = zoomSize(100, 400);
        const sint32 yb = zoomSize(80, 300);
        s.AddCommand({static_cast<uint16>(0x0001 | flags), 0, pmod, color, srca, size, U(a.x()), U(a.y()), U(xb),
                      U(yb), U(c.x()), U(c.y()), 0, 0, grda, 0});
    } else if (type < 56) {
        command = rng.Chance(80) ? 2 : 3;
        s.AddCommand({static_cast<uint16>(command | flags), 0, pmod, color, srca, size, U(a.x()), U(a.y()), U(b.x()),
                      U(b.y()), U(c.x()), U(c.y()), U(d.x()), U(d.y()), grda, 0});
    } else if (type < 80) {
        command = 4;
        s.Polygon(pmod, color, a, b, c, d, grda);
    } else if (type < 90) {
        command = rng.Chance(80) ? 5 : 7;
        s.AddCommand({static_cast<uint16>(command), 0, pmod, color, 0, 0, U(a.x()), U(a.y()), U(b.x()), U(b.y()),
                      U(c.x()), U(c.y()), U(d.x()), U(d.y()), grda, 0});
    } else {
        command = 6;
        s.AddCommand({0x0006, 0, pmod, color, 0, 0, U(a.x()), U(a.y()), U(b.x()), U(b.y()), 0, 0, 0, 0, grda, 0});
    }

    const vdp::VDP1Command::DrawMode mode{.u16 = pmod};
    ++cov.commandTypes[command];
    if (command <= 3) {
        ++cov.colorModes[mode.colorMode];
        cov.hss += mode.highSpeedShrink;
    }
    ++cov.colorCalc[mode.colorCalcBits];
    cov.gouraud += mode.gouraudEnable;
    cov.msbOn += mode.msbOn;
    cov.userClip += mode.userClippingEnable;
    cov.clipMode1 += mode.clippingMode;
    ++cov.meshByTransparentMeshes[mode.meshEnable][s.transparentMeshes];
    const bool doubleDensity = LSMD(regs.tvmd) == 3;
    cov.draws8bpp += regs.tvmr & 1;
    cov.drawsDIEDoubleDensity += doubleDensity && (regs.fbcr & 0x8) && !s.deinterlace;
    cov.drawsDeinterlacedDoubleDensity += doubleDensity && s.deinterlace;
    cov.drawsDoubleV += doubleDensity && s.deinterlace && !(regs.fbcr & 0x8);
}

uint16 RandomTVMD(TestRng &rng) {
    static constexpr uint16 kTVMD[] = {0x8000, 0x8000, 0x8001, 0x80C0, 0x80C0, 0x80C1, 0x80D0, 0x8080, 0x8002, 0x8003};
    return rng.Pick(kTVMD);
}

uint16 RandomTVMR(TestRng &rng) {
    return static_cast<uint16>(rng.Chance(50) ? 0 : rng.Chance(60) ? 1 : rng.Chance(50) ? 3 : rng.Range(0, 7));
}

Scenario RandomScenario(uint64 seed, Coverage &cov) {
    TestRng rng{seed};
    Scenario s;
    for (uint32 i = Scenario::kTextureBase; i < Scenario::kVRAMSize; ++i) {
        s.vram[i] = static_cast<uint8>(rng.Next()); // noise for look-up tables and textures past the allocated ones
    }
    GenRegs regs{RandomTVMD(rng), RandomTVMR(rng), static_cast<uint16>(rng.Range(0, 7) << 2), 0, 0}; // DIL, DIE, EOS
    s.tvmd = regs.tvmd;
    s.tvmr = regs.tvmr;
    s.fbcr = regs.fbcr;
    s.deinterlace = rng.Chance(50);
    s.transparentMeshes = rng.Chance(50);
    static constexpr uint32 kVRes[] = {224, 240, 256, 448, 480, 512};
    s.vres = rng.Pick(kVRes);
    RandomSysClip(rng, s.clip.sysClipH, s.clip.sysClipV, cov);
    regs.sysClipH = s.clip.sysClipH;
    regs.sysClipV = s.clip.sysClipV;
    RandomUserClip(rng, s.clip.userClipX0, s.clip.userClipY0, s.clip.userClipX1, s.clip.userClipY1, cov);
    if (rng.Chance(30)) {
        s.clip.localX = rng.Range(-60, 60);
        s.clip.localY = rng.Range(-60, 60);
    }
    if (rng.Chance(40)) {
        s.fbFill.resize(static_cast<size_t>(rng.Chance(50) ? 4096 : rng.Range(1, 5000)));
        for (auto &b : s.fbFill) {
            b = static_cast<uint8>(rng.Next());
        }
    }
    if (rng.Chance(30)) {
        s.maxBatchFragments = static_cast<uint32>(rng.Chance(50) ? rng.Range(1, 64) : rng.Range(64, 20000));
        ++cov.batchLimits;
    }

    auto randomRegWrite = [&] {
        switch (rng.Range(0, 5)) {
        case 0:
        case 1:
            regs.tvmr = RandomTVMR(rng);
            s.Op0(Op::Kind::WriteTVMR, regs.tvmr);
            break;
        case 2:
        case 3:
        case 4:
            regs.fbcr = static_cast<uint16>(rng.Range(0, 7) << 2);
            s.Op0(Op::Kind::WriteFBCR, regs.fbcr);
            break;
        default:
            regs.tvmd = RandomTVMD(rng);
            s.Op0(Op::Kind::WriteTVMD, regs.tvmd);
            break;
        }
    };

    const uint32 frames = static_cast<uint32>(rng.Range(1, 3));
    cov.multiFrame += frames > 1;
    for (uint32 frame = 0; frame < frames; ++frame) {
        if ((frame > 0 || rng.Chance(20)) && rng.Chance(60)) {
            Op erase{.kind = Op::Kind::Erase, .value = static_cast<uint16>(rng.Next())};
            erase.x1 = static_cast<uint16>(rng.Range(0, 63) << 3);
            erase.x3 = static_cast<uint16>(rng.Range(0, 127) << 3);
            erase.y1 = static_cast<uint16>(rng.Range(0, 511));
            erase.y3 = static_cast<uint16>(rng.Range(0, 511));
            if (rng.Chance(50)) {
                erase.x1 = static_cast<uint16>(rng.Range(0, 20) << 3);
                erase.y1 = static_cast<uint16>(rng.Range(0, 60));
                erase.x3 = static_cast<uint16>(rng.Range(20, 60) << 3);
                erase.y3 = static_cast<uint16>(rng.Range(100, 300));
            }
            const uint32 r = static_cast<uint32>(rng.Range(0, 99));
            erase.cycles = r < 40   ? 0
                           : r < 70 ? static_cast<uint64>(rng.Range(1, 3000))
                                    : static_cast<uint64>(rng.Range(3000, 300000));
            cov.partialErases += erase.cycles != 0;
            ++cov.erases;
            s.ops.push_back(erase);
        }
        if (frame > 0 && rng.Chance(60)) {
            s.Op0(Op::Kind::Swap);
            ++cov.swaps;
        }
        if (rng.Chance(25)) {
            randomRegWrite();
        }
        s.Op0(Op::Kind::BeginFrame);
        const uint32 count = static_cast<uint32>(rng.Range(1, 11));
        for (uint32 n = 0; n < count && s.CanAddCommand(); ++n) {
            if (rng.Chance(4)) {
                randomRegWrite();
                ++cov.midFrameRegWrites;
            }
            const uint32 kind = static_cast<uint32>(rng.Range(0, 99));
            if (kind < 7) {
                uint16 ch, cv;
                RandomSysClip(rng, ch, cv, cov);
                regs.sysClipH = ch;
                regs.sysClipV = cv;
                s.AddCommand({0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, ch, cv, 0, 0, 0, 0});
                ++cov.sysClipCmds;
            } else if (kind < 14) {
                uint16 x0, y0, x1, y1;
                RandomUserClip(rng, x0, y0, x1, y1, cov);
                const uint16 type = rng.Chance(80) ? 0x0008 : 0x000B;
                s.AddCommand({type, 0, 0, 0, 0, 0, x0, y0, 0, 0, x1, y1, 0, 0, 0, 0});
                ++cov.userClipCmds;
            } else if (kind < 18) {
                const sint32 lx = rng.Chance(90) ? rng.Range(-80, 80) : static_cast<sint32>(rng.Next() & 0xFFFF);
                const sint32 ly = rng.Chance(90) ? rng.Range(-80, 80) : static_cast<sint32>(rng.Next() & 0xFFFF);
                s.AddCommand({0x000A, 0, 0, 0, 0, 0, U(lx), U(ly), 0, 0, 0, 0, 0, 0, 0, 0});
                ++cov.localCoords;
            } else if (kind < 19) {
                s.AddCommand({static_cast<uint16>(rng.Range(0xC, 0xF)), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
            } else {
                AddRandomDraw(rng, s, regs, cov);
            }
        }
        s.Op0(Op::Kind::EndFrame);
    }
    return s;
}

struct RandomRunResult {
    uint64 mismatches = 0;
    uint64 noBreakMismatches = 0;
    uint64 firstMismatch = ~0ull;
    uint32 maxPerUnit = 0;
    uint64 fragments = 0;
    uint64 batches = 0;
    uint64 spans = 0;
    uint64 undefinedScenarios = 0;
};

struct SeedResult {
    bool undefined = false;
    std::string diff, noBreakDiff;
    ref::ReferenceStats stats;
};

SeedResult RunRandomSeed(uint64 seed, Coverage &cov) {
    SeedResult out;
    const Scenario s = RandomScenario(seed, cov);
    // Every 4th scenario resolves its fragments from a shuffled (scatter) order; every 4th (others) also runs without
    // the early break
    ReferenceRun r;
    try {
        r = RunReference(s, {}, seed % 4 == 0 ? seed * 2654435761ull + 1 : 0);
    } catch (const ref::UndefinedLineError &) {
        out.undefined = true; // a line software may hang on; not run
        return out;
    }
    const auto sw = RunSoftware(s);
    out.diff = CompareFbram(*sw, *r.fb);
    out.stats = r.stats;
    if (seed % 4 == 1) {
        out.noBreakDiff = CompareFbram(*sw, *RunReference(s, {.earlyBreak = false}).fb);
    }
    return out;
}

void MergeCoverage(Coverage &into, const Coverage &from) {
    auto add = [](auto &a, const auto &b) {
        for (size_t i = 0; i < a.size(); ++i) {
            a[i] += b[i];
        }
    };
    add(into.commandTypes, from.commandTypes);
    add(into.colorModes, from.colorModes);
    add(into.colorCalc, from.colorCalc);
    add(into.meshByTransparentMeshes[0], from.meshByTransparentMeshes[0]);
    add(into.meshByTransparentMeshes[1], from.meshByTransparentMeshes[1]);
    for (auto field : {&Coverage::gouraud, &Coverage::msbOn, &Coverage::hss, &Coverage::endCodeRows,
                       &Coverage::transparentRows, &Coverage::userClip, &Coverage::clipMode1, &Coverage::draws8bpp,
                       &Coverage::drawsDIEDoubleDensity, &Coverage::drawsDeinterlacedDoubleDensity,
                       &Coverage::drawsDoubleV, &Coverage::rawLargeClip, &Coverage::localCoords,
                       &Coverage::wrappedCoords, &Coverage::sysClipCmds, &Coverage::userClipCmds, &Coverage::erases,
                       &Coverage::partialErases, &Coverage::swaps, &Coverage::midFrameRegWrites,
                       &Coverage::batchLimits, &Coverage::multiFrame}) {
        into.*field += from.*field;
    }
}

// Runs the seeds on worker threads (each scenario owns its software renderer, unthreaded); Catch assertions stay on
// the calling thread
RandomRunResult RunRandomScenarios(uint64 firstSeed, uint64 count, Coverage &cov) {
    const uint32 workers = std::clamp<uint32>(std::thread::hardware_concurrency(), 1, 16);
    std::vector<SeedResult> results(count);
    std::vector<Coverage> coverages(workers);
    std::atomic<uint64> next{0};
    std::vector<std::thread> threads;
    for (uint32 w = 0; w < workers; ++w) {
        threads.emplace_back([&, w] {
            for (uint64 i = next++; i < count; i = next++) {
                results[i] = RunRandomSeed(firstSeed + i, coverages[w]);
            }
        });
    }
    for (auto &t : threads) {
        t.join();
    }
    for (const Coverage &c : coverages) {
        MergeCoverage(cov, c);
    }

    RandomRunResult result;
    uint32 reported = 0;
    for (uint64 i = 0; i < count; ++i) {
        const SeedResult &r = results[i];
        const uint64 seed = firstSeed + i;
        if (r.undefined) {
            ++result.undefinedScenarios;
            continue;
        }
        if (!r.diff.empty()) {
            ++result.mismatches;
            result.firstMismatch = std::min(result.firstMismatch, seed);
            if (reported++ < 5) {
                UNSCOPED_INFO("seed " << seed << ": " << r.diff);
            }
        }
        if (!r.noBreakDiff.empty()) {
            ++result.noBreakMismatches;
            if (reported++ < 5) {
                UNSCOPED_INFO("seed " << seed << " without the early break: " << r.noBreakDiff);
            }
        }
        result.maxPerUnit = std::max(result.maxPerUnit, r.stats.maxPerUnit);
        result.fragments += r.stats.fragments;
        result.batches += r.stats.batches;
        result.spans += r.stats.spans;
    }
    return result;
}

void CheckCoverage(const Coverage &cov) {
    for (uint32 t = 0; t < 8; ++t) {
        CAPTURE(t);
        CHECK(cov.commandTypes[t] > 0);
    }
    for (uint32 m = 0; m < 8; ++m) {
        CAPTURE(m);
        CHECK(cov.colorModes[m] > 0);
    }
    for (uint32 c = 0; c < 4; ++c) {
        CAPTURE(c);
        CHECK(cov.colorCalc[c] > 0);
    }
    CHECK(cov.meshByTransparentMeshes[1][0] > 0);
    CHECK(cov.meshByTransparentMeshes[1][1] > 0);
    CHECK(cov.gouraud > 0);
    CHECK(cov.msbOn > 0);
    CHECK(cov.hss > 0);
    CHECK(cov.endCodeRows > 0);
    CHECK(cov.transparentRows > 0);
    CHECK(cov.userClip > 0);
    CHECK(cov.clipMode1 > 0);
    CHECK(cov.draws8bpp > 0);
    CHECK(cov.drawsDIEDoubleDensity > 0);
    CHECK(cov.drawsDeinterlacedDoubleDensity > 0);
    CHECK(cov.drawsDoubleV > 0);
    CHECK(cov.rawLargeClip > 0);
    CHECK(cov.localCoords > 0);
    CHECK(cov.wrappedCoords > 0);
    CHECK(cov.sysClipCmds > 0);
    CHECK(cov.userClipCmds > 0);
    CHECK(cov.erases > 0);
    CHECK(cov.partialErases > 0);
    CHECK(cov.swaps > 0);
    CHECK(cov.midFrameRegWrites > 0);
    CHECK(cov.batchLimits > 0);
    CHECK(cov.multiFrame > 0);
}

} // namespace

// =====================================================================================================================
// Steppers

TEST_CASE("VDP1 reference steppers step exactly like vdp1_steppers.hpp", "[vdp1][reference]") {
    TestRng rng{101};
    uint64 lineMismatches = 0;
    uint64 undefinedLines = 0;
    for (int i = 0; i < 100'000; ++i) {
        const bool longLine = i % 50 == 0;
        const sint32 range = longLine ? 20000 : 600;
        const CoordS32 a{rng.Range(-range, range), rng.Range(-range, range)};
        const CoordS32 b{rng.Range(-range, range), rng.Range(-range, range)};
        const bool aa = rng.Chance(50);
        vdp::LineStepper<> real{a, b, aa};
        ref::port::Line port{a, b, aa};
        bool same = real.DMajor() == port.dmaj && real.Length() == port.length;
        if (rng.Chance(60)) {
            const sint32 w = rng.Chance(80) ? rng.Range(0, 800) : rng.Range(0, 0xFFFF);
            const sint32 h = rng.Chance(80) ? rng.Range(0, 600) : rng.Range(0, 0x1FFFF);
            const uint32 portSkip = port.SystemClip(w, h);
            if (port.undefined) {
                ++undefinedLines; // software may hang here
                continue;
            }
            same = same && real.SystemClip(w, h) == portSkip;
        }
        same = same && real.Length() == port.length;
        bool realAA = false, portAA = false;
        real.Step();
        port.Step();
        for (; same && real.CanStep(); realAA = real.Step(), portAA = port.Step()) {
            same = port.CanStep() && real.Coord().elements == port.Coord().elements && realAA == portAA &&
                   (!realAA || real.AACoord().elements == port.AACoord().elements);
        }
        same = same && !port.CanStep();
        if (!same) {
            CAPTURE(a.x(), a.y(), b.x(), b.y(), aa);
            ++lineMismatches;
        }
    }
    CAPTURE(undefinedLines);
    CHECK(lineMismatches == 0);

    uint64 textureMismatches = 0;
    for (int i = 0; i < 20'000; ++i) {
        const uint32 len = static_cast<uint32>(rng.Chance(80) ? rng.Range(1, 600) : rng.Range(1, 8000));
        const sint32 start = rng.Range(0, 511), end = rng.Range(0, 511);
        const bool hss = rng.Chance(30);
        const sint32 sel = rng.Range(0, 1);
        vdp::TextureStepper real;
        real.Setup(len, start, end, hss, sel);
        ref::port::Texture port;
        port.Setup(len, start, end, hss, sel);
        const uint32 skip = rng.Chance(50) ? static_cast<uint32>(rng.Range(0, static_cast<sint32>(len))) : 0;
        real.SkipPixels(skip);
        port.SkipPixels(skip);
        bool same = real.Value() == port.Value();
        for (uint32 p = skip; same && p < len; ++p) {
            for (int guard = 0; real.ShouldStepTexel() && guard < 100000; ++guard) {
                same = same && port.ShouldStepTexel();
                real.StepTexel();
                port.StepTexel();
            }
            same = same && !port.ShouldStepTexel() && real.Value() == port.Value();
            real.StepPixel();
            port.StepPixel();
        }
        textureMismatches += !same;
    }
    CHECK(textureMismatches == 0);

    uint64 gouraudMismatches = 0;
    for (int i = 0; i < 20'000; ++i) {
        const uint32 len = static_cast<uint32>(rng.Chance(80) ? rng.Range(1, 600) : rng.Range(1, 8000));
        const vdp::Color555 c0{.u16 = static_cast<uint16>(rng.Next())}, c1{.u16 = static_cast<uint16>(rng.Next())};
        vdp::GouraudStepper real;
        real.Setup(len, c0, c1);
        ref::port::Gouraud port;
        port.Setup(len, c0, c1);
        const uint32 skip = rng.Chance(50) ? static_cast<uint32>(rng.Range(0, static_cast<sint32>(len))) : 0;
        real.Skip(skip);
        port.Skip(skip);
        bool same = true;
        for (uint32 p = skip; same && p < len; ++p) {
            const vdp::Color555 base{.u16 = static_cast<uint16>(rng.Next())};
            same = real.Blend(base).u16 == port.Blend(base).u16;
            real.Step();
            port.Step();
        }
        gouraudMismatches += !same;
    }
    CHECK(gouraudMismatches == 0);
}

TEST_CASE("VDP1 reference span slots equal LineStepper's step count", "[vdp1][reference]") {
    // fragBase is a prefix sum of 2 x Length() after SystemClip; a span's keys stay inside its slots
    TestRng rng{103};
    for (int i = 0; i < 50'000; ++i) {
        const sint32 range = i % 10 == 0 ? 3000 : 1000;
        const CoordS32 a{rng.Range(-range, range), rng.Range(-range, range)};
        const CoordS32 b{rng.Range(-range, range), rng.Range(-range, range)};
        ref::CommandParams cmd;
        cmd.antiAlias = rng.Chance(50);
        cmd.clip.sysClipH = static_cast<uint16>(rng.Range(0, 800));
        cmd.clip.sysClipV = static_cast<uint16>(rng.Range(0, 600));
        ref::RasterState state;
        state.doubleV = rng.Chance(20) ? 1 : 0;
        const ref::Span span{.a = a, .b = b};
        uint32 slots;
        try {
            slots = ref::SpanSlots(state, cmd, span);
        } catch (const ref::UndefinedLineError &) {
            continue; // software may hang here
        }
        vdp::LineStepper<> real{a, b, cmd.antiAlias};
        const sint32 clipV = (cmd.clip.sysClipV << state.doubleV) | state.doubleV;
        real.SystemClip(cmd.clip.sysClipH, clipV);
        const bool lineClipped = (a.x() < 0 && b.x() < 0) || (a.y() < 0 && b.y() < 0) ||
                                 (a.x() > cmd.clip.sysClipH && b.x() > cmd.clip.sysClipH) ||
                                 (a.y() > clipV && b.y() > clipV);
        CAPTURE(a.x(), a.y(), b.x(), b.y());
        REQUIRE(slots == (lineClipped ? 0 : 2 * real.Length()));
    }
}

TEST_CASE("VDP1 reference flags the lines whose SystemClip never ends", "[vdp1][reference]") {
    // LineStepper::SystemClip's minor-step loops never end, with wrapping arithmetic, for some lines without
    // antialiasing whose major delta is 2048 (mod 4096). The unthreaded software renderer (MSVC Release) hangs on a
    // Draw Line command (207, 2426)-(1684, 378) with system clip 565 x 383. The reference refuses such spans
    // (UndefinedLineError); the random tables skip them.
    {
        ref::port::Line line{{207, 2426}, {1684, 378}, false};
        line.SystemClip(565, 383);
        CHECK(line.undefined);
    }
    {
        ref::port::Line line{{207, 2426}, {1684, 378}, true};
        line.SystemClip(565, 383);
        CHECK_FALSE(line.undefined);
    }
    TestRng rng{113};
    uint64 undefined = 0, wrong = 0;
    for (int i = 0; i < 200'000; ++i) {
        const CoordS32 a{rng.Range(-3000, 3000), rng.Range(-3000, 3000)};
        CoordS32 b{rng.Range(-3000, 3000), rng.Range(-3000, 3000)};
        if (rng.Chance(50)) { // major delta 2048 or 4096 + 2048
            const sint32 d = rng.Chance(80) ? 2048 : 6144;
            if (rng.Chance(50)) {
                b = {a.x() + (rng.Chance(50) ? d : -d), a.y() + rng.Range(-d, d)};
            } else {
                b = {a.x() + rng.Range(-d, d), a.y() + (rng.Chance(50) ? d : -d)};
            }
        }
        const bool aa = rng.Chance(30);
        ref::port::Line line{a, b, aa};
        line.SystemClip(rng.Range(0, 704), rng.Range(0, 512));
        if (line.undefined) {
            ++undefined;
            wrong += aa || line.dmaj % 4096 != 2048;
        }
    }
    CAPTURE(undefined);
    CHECK(undefined > 0);
    CHECK(wrong == 0);
}

// =====================================================================================================================
// Pixel uniqueness and the early break

namespace {

// Steps a line as RasterSpan does and calls fn(main), fn(aa) for each pixel
template <typename Fn>
void ForEachLinePixel(ref::port::Line &line, Fn &&fn) {
    bool aa = false;
    for (line.Step(); line.CanStep(); aa = line.Step()) {
        fn(line.Coord(), line.step - 1, false);
        if (aa) {
            fn(line.AACoord(), line.step - 1, true);
        }
    }
}

} // namespace

TEST_CASE("VDP1 reference lines never repeat a pixel, main and antialias included", "[vdp1][reference]") {
    // Research 4.1: within one span no pixel repeats, so software's within-span order never matters per pixel.
    // Exhaustive over every delta within +-512 (the stepper depends on the deltas only), antialias on and off.
    constexpr sint32 kMax = 512;
    constexpr sint32 kGrid = 2 * kMax + 5;
    std::atomic<uint64> repeats{0}, outside{0}, lines{0}, pixels{0};
    std::atomic<sint32> nextDX{-kMax};
    std::vector<std::thread> threads;
    for (uint32 w = 0; w < std::clamp<uint32>(std::thread::hardware_concurrency(), 1, 16); ++w) {
        threads.emplace_back([&] {
            std::vector<uint32> stamp(static_cast<size_t>(kGrid) * kGrid, 0);
            uint32 generation = 0;
            uint64 rep = 0, out = 0, lin = 0, pix = 0;
            for (sint32 dx = nextDX++; dx <= kMax; dx = nextDX++) {
                for (sint32 dy = -kMax; dy <= kMax; ++dy) {
                    for (const bool aa : {false, true}) {
                        ++generation;
                        ++lin;
                        ref::port::Line line{{0, 0}, {dx, dy}, aa};
                        uint64 mains = 0;
                        ForEachLinePixel(line, [&](CoordS32 c, uint32, bool isAA) {
                            mains += !isAA;
                            ++pix;
                            const sint32 gx = c.x() + kMax + 2, gy = c.y() + kMax + 2;
                            if (gx < 0 || gy < 0 || gx >= kGrid || gy >= kGrid) {
                                ++out;
                                return;
                            }
                            uint32 &s = stamp[static_cast<size_t>(gy) * kGrid + gx];
                            rep += s == generation;
                            s = generation;
                        });
                        out += mains != static_cast<uint64>(std::max(std::abs(dx), std::abs(dy))) + 1;
                    }
                }
            }
            repeats += rep;
            outside += out;
            lines += lin;
            pixels += pix;
        });
    }
    for (auto &t : threads) {
        t.join();
    }
    CAPTURE(lines.load(), pixels.load());
    CHECK(repeats == 0);
    CHECK(outside == 0);

    // Random long lines (13-bit accumulator wraps beyond 1364), anywhere, with and without SystemClip
    TestRng rng{107};
    uint64 longRepeats = 0;
    std::vector<uint64> seen;
    for (int i = 0; i < 4000; ++i) {
        const CoordS32 a{rng.Range(-9000, 9000), rng.Range(-9000, 9000)};
        const CoordS32 b{a.x() + rng.Range(-12000, 12000), a.y() + rng.Range(-12000, 12000)};
        ref::port::Line line{a, b, rng.Chance(70)};
        if (rng.Chance(50)) {
            line.SystemClip(rng.Range(0, 1000), rng.Range(0, 1000));
            if (line.undefined) {
                continue;
            }
        }
        seen.clear();
        ForEachLinePixel(line, [&](CoordS32 c, uint32, bool) {
            seen.push_back((static_cast<uint64>(static_cast<uint32>(c.x())) << 32) | static_cast<uint32>(c.y()));
        });
        std::sort(seen.begin(), seen.end());
        longRepeats += std::adjacent_find(seen.begin(), seen.end()) != seen.end();
    }
    CHECK(longRepeats == 0);
}

TEST_CASE("VDP1 reference: the in-clip steps of a line are contiguous, so the early break is an optimisation",
          "[vdp1][reference]") {
    // Research 4.1: "plotted && clipped && clipping mode 0 -> break" never skips a writable pixel, because the steps
    // whose main or antialias pixel lies in the clip rectangle (system clip, or its intersection with the user clip)
    // form one run.
    TestRng rng{109};
    uint64 gaps = 0;
    for (int i = 0; i < 300'000; ++i) {
        const sint32 range = rng.Chance(90) ? 600 : 5000;
        const CoordS32 a{rng.Range(-range, range), rng.Range(-range, range)};
        const CoordS32 b{rng.Range(-range, range), rng.Range(-range, range)};
        const sint32 x0 = rng.Range(-20, 300), y0 = rng.Range(-20, 200);
        const sint32 x1 = x0 + rng.Range(0, 300), y1 = y0 + rng.Range(0, 300);
        const sint32 clipW = rng.Range(0, 400), clipH = rng.Range(0, 300);
        ref::port::Line line{a, b, rng.Chance(70)};
        line.SystemClip(clipW, clipH);
        if (line.undefined) {
            continue;
        }
        auto inside = [&](CoordS32 c) {
            return c.x() >= std::max(0, x0) && c.x() <= std::min(clipW, x1) && c.y() >= std::max(0, y0) &&
                   c.y() <= std::min(clipH, y1);
        };
        uint32 runs = 0;
        bool prev = false;
        uint32 lastStep = ~0u;
        bool stepInside = false;
        auto endStep = [&] {
            if (stepInside && !prev) {
                ++runs;
            }
            prev = stepInside;
            stepInside = false;
        };
        ForEachLinePixel(line, [&](CoordS32 c, uint32 step, bool) {
            if (step != lastStep) {
                if (lastStep != ~0u) {
                    endStep();
                }
                lastStep = step;
            }
            stepInside = stepInside || inside(c);
        });
        endStep();
        gaps += runs > 1;
    }
    CHECK(gaps == 0);
}

// =====================================================================================================================
// Reference vs software

TEST_CASE("VDP1 reference FBRAM equals software on random command tables", "[vdp1][reference]") {
    // 10,000 seeded tables: every command type and colour mode, draw modes with Gouraud, mesh with transparent meshes
    // on and off, MSB-on, 8 bpp, double interlace, end codes, HSS, clipping (raw large values, local coordinates,
    // 13-bit wraps), erases (partial), swaps, register writes between commands and fragment-limited batch splits.
    const auto t0 = std::chrono::steady_clock::now();
    Coverage cov;
    const RandomRunResult r = RunRandomScenarios(1, 10'000, cov);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CAPTURE(r.firstMismatch, r.maxPerUnit, r.fragments, r.batches, r.spans, r.undefinedScenarios, seconds);
    CHECK(r.mismatches == 0);
    CHECK(r.noBreakMismatches == 0);
    CHECK(r.undefinedScenarios < 100); // tables with a line software may hang on are skipped
    CheckCoverage(cov);
}

TEST_CASE("VDP1 reference FBRAM equals software on random command tables (long)", "[vdp1][reference][.]") {
    Coverage cov;
    const RandomRunResult r = RunRandomScenarios(1'000'001, 200'000, cov);
    CAPTURE(r.firstMismatch, r.maxPerUnit, r.fragments, r.undefinedScenarios);
    CHECK(r.mismatches == 0);
    CHECK(r.noBreakMismatches == 0);
    CheckCoverage(cov);
}

// =====================================================================================================================
// Targeted cases

namespace {

std::vector<uint8> RGBTexture(uint32 w, uint32 h, uint64 seed) {
    TestRng rng{seed};
    std::vector<uint8> bytes(w * h * 2);
    for (uint32 i = 0; i < w * h; ++i) {
        const uint16 texel = static_cast<uint16>(0x8000 | (rng.Next() & 0x7FFE));
        bytes[i * 2] = static_cast<uint8>(texel >> 8);
        bytes[i * 2 + 1] = static_cast<uint8>(texel);
    }
    return bytes;
}

// Runs both renderers; returns the reference's stats
ref::ReferenceStats CheckScenario(const Scenario &s, uint64 shuffle = 1, bool alsoWithoutBreak = true) {
    const auto sw = RunSoftware(s);
    const ReferenceRun r = RunReference(s, {}, shuffle);
    const std::string diff = CompareFbram(*sw, *r.fb);
    CHECK(diff == "");
    if (alsoWithoutBreak) {
        const ReferenceRun nb = RunReference(s, {.earlyBreak = false}, shuffle + 1);
        CHECK(CompareFbram(*sw, *nb.fb) == "");
    }
    return r.stats;
}

} // namespace

TEST_CASE("VDP1 reference matches software on apex stacks of more than 256 fragments per word", "[vdp1][reference]") {
    // Triangles with B = C: every line of the quad ends at the apex, so its word receives one fragment per line
    // (500+). Half-transparency with Gouraud makes the result depend on the whole order.
    uint32 maxPerUnit = 0;
    struct Variant {
        uint16 pmod;
        uint16 tvmr;
        uint16 tvmd;
        uint16 fbcr;
        bool deinterlace, transparentMeshes;
    };
    std::vector<Variant> variants;
    for (const uint16 calc : {uint16{0}, uint16{1}, uint16{2}, uint16{3}}) {
        for (const uint16 extra : {uint16{0x0004}, uint16{0x0100}, uint16{0x0104}, uint16{0x8000}}) {
            for (const bool tm : {false, true}) {
                variants.push_back({static_cast<uint16>(0x00C0 | calc | extra), 0, 0x8000, 0, false, tm});
            }
        }
    }
    variants.push_back({0x00C7, 1, 0x8000, 0, false, false});   // 8 bpp
    variants.push_back({0x80C0, 1, 0x8000, 0, false, true});    // 8 bpp MSB-on
    variants.push_back({0x01C3, 1, 0x8000, 0, false, true});    // 8 bpp mesh
    variants.push_back({0x00C7, 0, 0x80C0, 0, true, true});     // deinterlaced double density (doubleV, alt field)
    variants.push_back({0x01C7, 0, 0x80C0, 0, true, true});
    variants.push_back({0x00C7, 0, 0x80C0, 0x0008, false, false}); // double interlace, even lines
    variants.push_back({0x00C7, 0, 0x8000, 0x0008, false, false}); // DIE without double density: rows merge
    for (const Variant &v : variants) {
        for (const bool textured : {false, true}) {
            CAPTURE(v.pmod, v.tvmr, v.tvmd, v.fbcr, v.deinterlace, v.transparentMeshes, textured);
            Scenario s;
            s.tvmd = v.tvmd;
            s.tvmr = v.tvmr;
            s.fbcr = v.fbcr;
            s.deinterlace = v.deinterlace;
            s.transparentMeshes = v.transparentMeshes;
            s.vres = 448;
            s.clip.sysClipH = 352;
            s.clip.sysClipV = 255;
            s.fbFill = {0x80, 0x00, 0xC2, 0x10, 0x9F, 0xFF, 0x00, 0x01, 0xAB, 0xCD};
            const uint16 grda = s.AddGouraud({0x7FFF, 0x0000, 0x0000, 0x7C1F});
            const uint16 srca = s.AddTexture(RGBTexture(16, 64, 5));
            s.Op0(Op::Kind::BeginFrame);
            const uint16 pmod = textured ? static_cast<uint16>(v.pmod | 0x0028) : v.pmod;
            // apex inside the clip area, long left edge from far above to far below
            const CoordS32 apex{200, 120};
            if (textured) {
                s.Distorted(0, pmod, 0, srca, 0x0240, {20, -150}, apex, apex, {30, 400}, grda);
                s.Distorted(0x0010, pmod, 0, srca, 0x0240, {340, 380}, apex, apex, {320, -120}, grda);
            } else {
                s.Polygon(pmod, 0xC631, {20, -150}, apex, apex, {30, 400}, grda);
                s.Polygon(pmod, 0x83E0, {340, 380}, apex, apex, {320, -120}, grda);
            }
            s.Op0(Op::Kind::EndFrame);
            const ref::ReferenceStats stats = CheckScenario(s);
            maxPerUnit = std::max(maxPerUnit, stats.maxPerUnit);
        }
    }
    CAPTURE(maxPerUnit);
    CHECK(maxPerUnit > 256);
}

TEST_CASE("VDP1 reference matches software on overlapping half-transparency chains", "[vdp1][reference]") {
    // Many overlapping half-transparent, shadow and Gouraud shapes in one batch. Resolving in descending key order must
    // differ from software (the scenes are order-sensitive); ascending must equal it.
    for (uint64 seed = 1; seed <= 40; ++seed) {
        CAPTURE(seed);
        TestRng rng{seed * 977};
        Scenario s;
        s.transparentMeshes = rng.Chance(50);
        s.tvmr = rng.Chance(25) ? 1 : 0;
        s.fbFill.resize(1000);
        for (auto &b : s.fbFill) {
            b = static_cast<uint8>(rng.Next() | 0x80);
        }
        const uint16 srca = s.AddTexture(RGBTexture(32, 32, seed));
        s.Op0(Op::Kind::BeginFrame);
        for (int n = 0; n < 40; ++n) {
            static constexpr uint16 kCalc[] = {3, 3, 3, 1, 2, 0, 7, 7};
            uint16 pmod = static_cast<uint16>(0x00C0 | rng.Pick(kCalc));
            if (rng.Chance(20)) {
                pmod |= 0x0100; // mesh
            }
            const uint16 grda = s.AddGouraud({static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next()),
                                              static_cast<uint16>(rng.Next()), static_cast<uint16>(rng.Next())});
            const CoordS32 c{rng.Range(60, 260), rng.Range(40, 180)};
            const sint32 r = rng.Range(10, 60);
            const CoordS32 a{c.x() - r, c.y() - rng.Range(5, r)}, b{c.x() + rng.Range(5, r), c.y() - r};
            const CoordS32 cc{c.x() + r, c.y() + rng.Range(5, r)}, d{c.x() - rng.Range(5, r), c.y() + r};
            if (rng.Chance(30)) {
                s.Distorted(0, static_cast<uint16>(pmod | 0x0028), 0, srca, 0x0420, a, b, cc, d, grda);
            } else {
                s.Polygon(pmod, static_cast<uint16>(rng.Next() | 0x8000), a, b, cc, d, grda);
            }
        }
        s.Op0(Op::Kind::EndFrame);
        CheckScenario(s, seed);

        const auto sw = RunSoftware(s);
        const ReferenceRun reversed = RunReference(s, {.resolveOrder = ref::ResolveOrder::DescendingForTesting});
        CHECK(CompareFbram(*sw, *reversed.fb) != "");
    }
}

TEST_CASE("VDP1 reference erase matches software", "[vdp1][reference]") {
    // Regions with y1 == y3 (one row), x1 == x3 (nothing), y1 > y3, the 400/428 and VRes clamps, cycle-limited partial
    // erases ending mid-row, 8 bpp and every erase offset shift, double density (alt mirror) and transparent meshes
    // (mesh cleared). The display framebuffer first gets main, alt and mesh content from a mesh polygon drawn, then
    // swapped in.
    struct Region {
        uint16 x1, y1, x3, y3;
    };
    const Region regions[] = {{4, 10, 12, 10}, {4, 10, 4, 12}, {4, 13, 12, 10},
                              {0, 0, 1016, 511}, {8, 100, 600, 300}, {296, 200, 1016, 511}};
    const uint64 cycleCounts[] = {0, 1, 37, 5000};
    const uint16 tvmrs[] = {0, 1, 2, 3};
    const uint16 tvmds[] = {0x8000, 0x8001, 0x80C0, 0x80C1};
    uint64 scenarios = 0;
    for (const Region &rg : regions) {
        for (const uint64 cycles : cycleCounts) {
            for (const uint16 tvmr : tvmrs) {
                for (const uint16 tvmd : tvmds) {
                    for (const uint32 flags : {0u, 1u, 2u, 3u}) {
                        CAPTURE(rg.x1, rg.y1, rg.x3, rg.y3, cycles, tvmr, tvmd, flags);
                        Scenario s;
                        s.tvmd = tvmd;
                        s.tvmr = tvmr;
                        s.deinterlace = flags & 1;
                        s.transparentMeshes = flags & 2;
                        s.vres = LSMD(tvmd) == 3 ? 480 : 240;
                        s.clip.sysClipH = 511;
                        s.clip.sysClipV = 511;
                        s.fbFill = {0x12, 0x34, 0x56, 0x78, 0x9A};
                        s.Op0(Op::Kind::BeginFrame);
                        // mesh content around the region corners and edges (main, alt and mesh framebuffers)
                        s.Polygon(0x01C0, 0x8421, {0, 0}, {40, 0}, {40, 40}, {0, 40});
                        s.Polygon(0x01C0, 0x8421, {280, 8}, {440, 8}, {440, 30}, {280, 30});
                        s.Polygon(0x01C0, 0x8421, {380, 180}, {440, 180}, {440, 520}, {380, 520});
                        s.Polygon(0x00C0, 0xFC00, {100, 230}, {300, 230}, {300, 250}, {100, 250});
                        s.Op0(Op::Kind::EndFrame);
                        s.Op0(Op::Kind::Swap);
                        s.ops.push_back({.kind = Op::Kind::Erase,
                                         .value = 0xBEEF,
                                         .x1 = rg.x1,
                                         .y1 = rg.y1,
                                         .x3 = rg.x3,
                                         .y3 = rg.y3,
                                         .cycles = cycles});
                        CheckScenario(s, 1, false);
                        ++scenarios;
                    }
                }
            }
        }
    }
    CHECK(scenarios == 6 * 4 * 4 * 4 * 4);
}

TEST_CASE("VDP1 reference: one random table (BRIMIR_VDP1REF_SEED)", "[vdp1][reference][.debug]") {
    const char *env = std::getenv("BRIMIR_VDP1REF_SEED");
    const uint64 seed = env ? std::strtoull(env, nullptr, 10) : 1;
    Coverage cov;
    const Scenario s = RandomScenario(seed, cov);
    std::printf("seed %llu: tvmd %04X tvmr %04X fbcr %04X deint %d tm %d vres %u maxBatch %u ops %zu\n",
                static_cast<unsigned long long>(seed), s.tvmd, s.tvmr, s.fbcr, s.deinterlace, s.transparentMeshes,
                s.vres, s.maxBatchFragments, s.ops.size());
    std::printf("clip sys %u %u user %u %u %u %u local %d %d\n", s.clip.sysClipH, s.clip.sysClipV, s.clip.userClipX0,
                s.clip.userClipY0, s.clip.userClipX1, s.clip.userClipY1, s.clip.localX, s.clip.localY);
    for (const Op &op : s.ops) {
        if (op.kind == Op::Kind::Command) {
            std::printf("  cmd @%04X:", op.address);
            for (uint32 i = 0; i < 16; ++i) {
                std::printf(" %04X", util::ReadBE<uint16>(&s.vram[op.address + i * 2]));
            }
            std::printf("\n");
        } else {
            std::printf("  op %d value %04X x1 %u y1 %u x3 %u y3 %u cycles %llu\n", static_cast<int>(op.kind), op.value,
                        op.x1, op.y1, op.x3, op.y3, static_cast<unsigned long long>(op.cycles));
        }
    }
    const auto sw = RunSoftware(s);
    const ReferenceRun r = RunReference(s);
    std::printf("diff: %s\n", CompareFbram(*sw, *r.fb).c_str());
    for (size_t k = 1; k <= s.ops.size(); ++k) {
        Scenario p = s;
        p.ops.resize(k);
        const std::string d = CompareFbram(*RunSoftware(p), *RunReference(p).fb);
        if (!d.empty()) {
            std::printf("first differing op %zu: %s\n", k - 1, d.c_str());
            break;
        }
    }
}
