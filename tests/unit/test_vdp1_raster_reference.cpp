// The CPU reference of the GPU VDP1 back half (vdp1_raster_reference.hpp), fed by the real front half
// (gpu/vdp1/vdp1_front.hpp: commands, spans, VRAM uploads, erases, CPU framebuffer writes), against the steppers and
// against the unthreaded SoftwareVDPRenderer's FBRAM (main framebuffers, alternate fields, meshes), byte for byte.

#include "catch_amalgamated.hpp"

#include "vdp1_raster_reference.hpp"

#include <gpu/vdp1/vdp1_front.hpp>

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
namespace gpu = brimir::gpu::vdp1;
using vdp::CoordS32;

struct TestRng {
    uint64 s;
    explicit TestRng(uint64 seed)
        : s(seed) {}
    uint64 Next() {
        return ref::SplitMix64(s);
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
    enum class Kind {
        BeginFrame, Command, EndFrame, WriteTVMR, WriteFBCR, WriteTVMD, Erase, Swap,
        WriteVRAM,       // a 16-bit VDP1 VRAM write (address, value)
        WriteFB8,        // a CPU write to the draw framebuffer (address, value)
        WriteFB16,
        SetEnhancements, // value bit 0 deinterlace, bit 1 transparent meshes
        FrameBoundary,   // the front half's MarkFrameBoundary (VDP2EndFrame); software has nothing to do
        Take,            // the front half's TakeOps, replayed into the reference
    };
    Kind kind;
    uint32 address = 0; // Command, WriteVRAM, WriteFB*
    uint16 value = 0;   // register writes, WriteVRAM, WriteFB*, SetEnhancements; Erase: the write value
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
    gpu::Vdp1BatchLimits limits; // the front half's batch capacities (lowered in some random tables)
    bool hazardTracking = true;  // off only to show that the tests notice

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

// The scenario's registers, clip state, VRAM and framebuffers in a fresh VDPState (both renderers start from this)
void SetupState(const Scenario &s, vdp::VDPState &state) {
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
}

vdp::config::Enhancements EnhancementsOf(uint16 value) {
    return {.deinterlace = (value & 1) != 0, .transparentMeshes = (value & 2) != 0};
}

// The ops every runner applies to VDPState the way the VDP does (register writes, erase latches, swaps, the memory
// writes before their renderer hooks); returns false for the ops it leaves to the runner
bool ApplyStateOp(const Op &op, vdp::VDPState &state) {
    switch (op.kind) {
    case Op::Kind::WriteTVMR: state.regs1.WriteTVMR(op.value); return true;
    case Op::Kind::WriteFBCR: state.regs1.WriteFBCR(op.value); return true;
    case Op::Kind::WriteTVMD:
        state.regs2.WriteTVMD(op.value);
        state.regs2.LatchTVMD();
        return true;
    case Op::Kind::Swap: state.fbIndex.Flip(); return true;
    case Op::Kind::Erase:
        state.regs1.eraseWriteValueLatch = op.value;
        state.regs1.eraseX1Latch = op.x1;
        state.regs1.eraseY1Latch = op.y1;
        state.regs1.eraseX3Latch = op.x3;
        state.regs1.eraseY3Latch = op.y3;
        return false;
    default: return false;
    }
}

std::unique_ptr<SoftwareRun> RunSoftware(const Scenario &s) {
    auto run = std::make_unique<SoftwareRun>();
    auto &state = *run->state;
    run->renderer = std::make_unique<vdp::SoftwareVDPRenderer>(state, run->debug, run->access);
    auto &renderer = *run->renderer;
    renderer.ConfigureEnhancements({.deinterlace = s.deinterlace, .transparentMeshes = s.transparentMeshes});
    SetupState(s, state);
    renderer.VDP2SetResolution(320, s.vres, false);
    renderer.PostLoadStateSync();

    for (const Op &op : s.ops) {
        if (ApplyStateOp(op, state)) {
            continue;
        }
        switch (op.kind) {
        case Op::Kind::BeginFrame: renderer.VDP1BeginFrame(); break;
        case Op::Kind::Command: {
            const vdp::VDP1Command::Control control{.u16 = util::ReadBE<uint16>(&state.mem1.VRAM[op.address])};
            renderer.VDP1ExecuteCommand(op.address, control);
            break;
        }
        case Op::Kind::EndFrame: renderer.VDP1EndFrame(); break;
        case Op::Kind::Erase: renderer.VDP1EraseFramebuffer(op.cycles); break;
        case Op::Kind::WriteVRAM:
            state.mem1.WriteVRAM<uint16>(op.address, op.value,
                                         [&](uint32 a, uint16 v) { renderer.VDP1WriteVRAM(a, v); });
            break;
        case Op::Kind::WriteFB8:
            state.VDP1WriteFB<uint8>(op.address, static_cast<uint8>(op.value),
                                     [&](uint32 a, uint8 v) { renderer.VDP1WriteFB(a, v); });
            break;
        case Op::Kind::WriteFB16:
            state.VDP1WriteFB<uint16>(op.address, op.value, [&](uint32 a, uint16 v) { renderer.VDP1WriteFB(a, v); });
            break;
        case Op::Kind::SetEnhancements: renderer.ConfigureEnhancements(EnhancementsOf(op.value)); break;
        default: break;
        }
    }
    return run;
}

struct ReferenceRun {
    std::unique_ptr<ref::Fbram> fb = std::make_unique<ref::Fbram>();
    ref::ReferenceStats stats;
    gpu::Vdp1FrontStats front;
    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>(); // the front half's
    bool doubleV = false;
};

// The front half driven like a renderer, its op lists replayed into the reference
ReferenceRun RunReference(const Scenario &s, ref::RasterOptions options = {}, uint64 shuffleSeed = 0) {
    ReferenceRun run;
    auto &state = *run.state;
    SetupState(s, state);
    FillMain(s, run.fb->main);

    gpu::Vdp1FrontEnd front{state};
    front.SetEnhancements({.deinterlace = s.deinterlace, .transparentMeshes = s.transparentMeshes});
    front.SetBatchLimitsForTesting(s.limits);
    front.SetHazardTrackingForTesting(s.hazardTracking);
    ref::ReferenceVDP1 reference{*run.fb, options, shuffleSeed};

    for (const Op &op : s.ops) {
        if (ApplyStateOp(op, state)) {
            continue;
        }
        switch (op.kind) {
        case Op::Kind::BeginFrame: front.BeginFrame(); break;
        case Op::Kind::Command: {
            const vdp::VDP1Command::Control control{.u16 = util::ReadBE<uint16>(&state.mem1.VRAM[op.address])};
            front.ExecuteCommand(op.address, control);
            break;
        }
        case Op::Kind::Erase: front.Erase(op.cycles, s.vres); break;
        case Op::Kind::WriteVRAM:
            state.mem1.WriteVRAM<uint16>(op.address, op.value, [&](uint32 a, uint16) { front.WriteVRAM(a); });
            break;
        case Op::Kind::WriteFB8:
            state.VDP1WriteFB<uint8>(op.address, static_cast<uint8>(op.value),
                                     [&](uint32 a, uint8 v) { front.WriteFB8(a, v); });
            break;
        case Op::Kind::WriteFB16:
            state.VDP1WriteFB<uint16>(op.address, op.value, [&](uint32 a, uint16 v) { front.WriteFB16(a, v); });
            break;
        case Op::Kind::SetEnhancements: front.SetEnhancements(EnhancementsOf(op.value)); break;
        case Op::Kind::FrameBoundary: front.MarkFrameBoundary(); break;
        case Op::Kind::Take: reference.Replay(front.TakeOps()); break;
        default: break;
        }
    }
    reference.Replay(front.TakeOps());
    run.stats = reference.Stats();
    run.front = front.Stats();
    run.doubleV = front.DoubleV();
    return run;
}

// The clip state and doubleV the two renderers end with (the front half keeps software's VDPState::state1 and doubleV)
std::string CompareState(const SoftwareRun &sw, const ReferenceRun &r) {
    const auto &a = sw.state->state1;
    const auto &b = r.state->state1;
    if (a.sysClipH != b.sysClipH || a.sysClipV != b.sysClipV || a.userClipX0 != b.userClipX0 ||
        a.userClipY0 != b.userClipY0 || a.userClipX1 != b.userClipX1 || a.userClipY1 != b.userClipY1 ||
        a.localCoordX != b.localCoordX || a.localCoordY != b.localCoordY) {
        return "clip state differs";
    }
    auto saved = std::make_unique<savestate::VDPSaveState::VDPRendererSaveState>();
    sw.renderer->SaveState(*saved);
    if ((saved->vdp1State.doubleV != 0) != r.doubleV) {
        return "doubleV differs";
    }
    return {};
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

// Scenario-structure features the generator produced (what is drawn is counted by the reference: DrawnCoverage)
struct Coverage {
    enum Counter : uint32 {
        WrappedCoords, TransparentRows, EndCodeRows, RawLargeClip, UserClip, ClipMode1, LocalCoords, SysClipCmds,
        UserClipCmds, Erases, PartialErases, Swaps, MidFrameRegWrites, BatchLimits, MultiFrame, VramWrites, FbWrites,
        EnhancementChanges, FrameBoundaries, Takes, Count
    };
    static constexpr const char *kNames[Count] = {
        "wrapped coordinates", "transparent texture rows", "end-code texture rows", "raw large clip values",
        "user clipping", "clipping mode 1", "local coordinate commands", "system clip commands", "user clip commands",
        "erases", "partial erases", "swaps", "mid-frame register writes", "batch limits", "multi-frame tables",
        "mid-frame VRAM writes", "CPU framebuffer writes", "enhancement changes", "frame boundaries", "op takes"};
    std::array<uint64, Count> counts{};

    uint64 &operator[](uint32 counter) {
        return counts[counter];
    }
    void Merge(const Coverage &other) {
        for (uint32 i = 0; i < Count; ++i) {
            counts[i] += other.counts[i];
        }
    }
};

CoordS32 RandomPoint(TestRng &rng, sint32 w, sint32 h, bool nearOnly, Coverage &cov) {
    if (nearOnly) {
        return {rng.Range(-48, w + 48), rng.Range(-48, h + 48)};
    }
    if (rng.Chance(3)) {
        ++cov[Coverage::WrappedCoords];
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
        cov[Coverage::TransparentRows] += kind < 15;
        cov[Coverage::EndCodeRows] += kind >= 15 && kind < 22;
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
        ++cov[Coverage::RawLargeClip];
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
        ++cov[Coverage::RawLargeClip];
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
    cov[Coverage::UserClip] += mode.userClippingEnable;
    cov[Coverage::ClipMode1] += mode.clippingMode;
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
        s.limits.fragments = static_cast<uint32>(rng.Chance(50) ? rng.Range(1, 64) : rng.Range(64, 20000));
        ++cov[Coverage::BatchLimits];
    }

    // The ops Task 1's tables did not have (VRAM and framebuffer writes between commands, enhancement changes, frame
    // boundaries, takes, the other batch limits) come from a second generator, so the tables keep Task 1's commands
    TestRng rng2{seed * 0x9E3779B97F4A7C15ull + 0x5EED};
    if (rng2.Chance(10)) {
        s.limits.spans = static_cast<uint32>(rng2.Range(1, 40));
        ++cov[Coverage::BatchLimits];
    }
    if (rng2.Chance(10)) {
        s.limits.commands = static_cast<uint32>(rng2.Range(1, 4));
        ++cov[Coverage::BatchLimits];
    }
    if (rng2.Chance(10)) {
        s.limits.fbWrites = static_cast<uint32>(rng2.Range(1, 3));
        ++cov[Coverage::BatchLimits];
    }
    auto extraOps = [&] {
        if (rng2.Chance(8)) {
            // VRAM writes, mostly into what earlier commands read: textures, Gouraud tables, command tables and the
            // look-up tables in the noise area
            const uint32 count = static_cast<uint32>(rng2.Range(1, 4));
            for (uint32 i = 0; i < count; ++i) {
                uint32 address;
                const uint32 r = static_cast<uint32>(rng2.Range(0, 99));
                if (r < 40 && s.nextTexture > Scenario::kTextureBase) {
                    address = static_cast<uint32>(rng2.Range(Scenario::kTextureBase, s.nextTexture - 1));
                } else if (r < 60 && s.nextGouraud > Scenario::kGouraudBase) {
                    address = static_cast<uint32>(rng2.Range(Scenario::kGouraudBase, s.nextGouraud - 1));
                } else if (r < 75 && s.nextCommand > Scenario::kCommandBase) {
                    address = static_cast<uint32>(rng2.Range(Scenario::kCommandBase, s.nextCommand - 1));
                } else {
                    address = static_cast<uint32>(rng2.Range(Scenario::kTextureBase, Scenario::kVRAMSize - 2));
                }
                s.ops.push_back({.kind = Op::Kind::WriteVRAM,
                                 .address = address & ~1u,
                                 .value = static_cast<uint16>(rng2.Next())});
                ++cov[Coverage::VramWrites];
            }
        }
        if (rng2.Chance(5)) {
            const uint32 count = static_cast<uint32>(rng2.Range(1, 8));
            for (uint32 i = 0; i < count; ++i) {
                const uint32 address = rng2.Chance(80)
                                           ? static_cast<uint32>((rng2.Range(0, 260) * 512 + rng2.Range(0, 400)) * 2)
                                           : static_cast<uint32>(rng2.Range(0, 0x3FFFF));
                const bool byte = rng2.Chance(40);
                s.ops.push_back({.kind = byte ? Op::Kind::WriteFB8 : Op::Kind::WriteFB16,
                                 .address = (byte ? address + rng2.Range(0, 1) : address) & 0x3FFFF,
                                 .value = static_cast<uint16>(rng2.Next())});
                ++cov[Coverage::FbWrites];
            }
        }
        if (rng2.Chance(2)) {
            s.Op0(Op::Kind::SetEnhancements, static_cast<uint16>(rng2.Range(0, 3)));
            ++cov[Coverage::EnhancementChanges];
        }
        if (rng2.Chance(4)) {
            s.Op0(Op::Kind::FrameBoundary);
            ++cov[Coverage::FrameBoundaries];
        }
        if (rng2.Chance(4)) {
            s.Op0(Op::Kind::Take);
            ++cov[Coverage::Takes];
        }
    };

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
    cov[Coverage::MultiFrame] += frames > 1;
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
            cov[Coverage::PartialErases] += erase.cycles != 0;
            ++cov[Coverage::Erases];
            s.ops.push_back(erase);
        }
        if (frame > 0 && rng.Chance(60)) {
            s.Op0(Op::Kind::Swap);
            ++cov[Coverage::Swaps];
        }
        if (rng.Chance(25)) {
            randomRegWrite();
        }
        s.Op0(Op::Kind::BeginFrame);
        const uint32 count = static_cast<uint32>(rng.Range(1, 11));
        for (uint32 n = 0; n < count && s.CanAddCommand(); ++n) {
            if (rng.Chance(4)) {
                randomRegWrite();
                ++cov[Coverage::MidFrameRegWrites];
            }
            extraOps();
            const uint32 kind = static_cast<uint32>(rng.Range(0, 99));
            if (kind < 7) {
                uint16 ch, cv;
                RandomSysClip(rng, ch, cv, cov);
                regs.sysClipH = ch;
                regs.sysClipV = cv;
                s.AddCommand({0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, ch, cv, 0, 0, 0, 0});
                ++cov[Coverage::SysClipCmds];
            } else if (kind < 14) {
                uint16 x0, y0, x1, y1;
                RandomUserClip(rng, x0, y0, x1, y1, cov);
                const uint16 type = rng.Chance(80) ? 0x0008 : 0x000B;
                s.AddCommand({type, 0, 0, 0, 0, 0, x0, y0, 0, 0, x1, y1, 0, 0, 0, 0});
                ++cov[Coverage::UserClipCmds];
            } else if (kind < 18) {
                const sint32 lx = rng.Chance(90) ? rng.Range(-80, 80) : static_cast<sint32>(rng.Next() & 0xFFFF);
                const sint32 ly = rng.Chance(90) ? rng.Range(-80, 80) : static_cast<sint32>(rng.Next() & 0xFFFF);
                s.AddCommand({0x000A, 0, 0, 0, 0, 0, U(lx), U(ly), 0, 0, 0, 0, 0, 0, 0, 0});
                ++cov[Coverage::LocalCoords];
            } else if (kind < 19) {
                s.AddCommand({static_cast<uint16>(rng.Range(0xC, 0xF)), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
            } else {
                AddRandomDraw(rng, s, regs, cov);
            }
        }
        extraOps();
        s.Op0(Op::Kind::EndFrame);
    }
    return s;
}

struct RandomRunResult {
    uint64 mismatches = 0;
    uint64 noBreakMismatches = 0;
    uint64 failures = 0; // exceptions
    uint64 firstMismatch = ~0ull;
    uint32 maxPerUnit = 0;
    uint64 fragments = 0;
    uint64 batches = 0;
    uint64 spans = 0;
    uint64 tablesWithAlt = 0;  // tables ending with a non-zero alternate-field slice
    uint64 tablesWithMesh = 0; // ... mesh slice
    ref::DrawnCoverage drawn;
    std::array<uint64, 6> batchEnds{}; // the front half's batch closes, by gpu::Vdp1BatchEnd
    uint64 fbWrites = 0, vramUploadBytes = 0;
};

struct SeedResult {
    std::string diff, noBreakDiff, failure;
    ref::ReferenceStats stats;
    gpu::Vdp1FrontStats front;
    bool alt = false, mesh = false;
};

bool AnyNonZero(const vdp::SpriteFB &fb) {
    return std::any_of(fb.begin(), fb.end(), [](uint8 b) { return b != 0; });
}

SeedResult RunRandomSeed(uint64 seed, Coverage &cov) {
    SeedResult out;
    try {
        const Scenario s = RandomScenario(seed, cov);
        // Every 4th scenario resolves its fragments from a shuffled (scatter) order; every 4th (others) also runs
        // without the early break
        const ReferenceRun r = RunReference(s, {}, seed % 4 == 0 ? seed * 2654435761ull + 1 : 0);
        const auto sw = RunSoftware(s);
        out.diff = CompareFbram(*sw, *r.fb);
        if (out.diff.empty()) {
            out.diff = CompareState(*sw, r);
        }
        out.stats = r.stats;
        out.front = r.front;
        out.alt = AnyNonZero(r.fb->alt[0]) || AnyNonZero(r.fb->alt[1]);
        out.mesh = AnyNonZero(r.fb->mesh[0][0]) || AnyNonZero(r.fb->mesh[0][1]) || AnyNonZero(r.fb->mesh[1][0]) ||
                   AnyNonZero(r.fb->mesh[1][1]);
        if (seed % 4 == 1) {
            out.noBreakDiff = CompareFbram(*sw, *RunReference(s, {.earlyBreak = false}).fb);
        }
    } catch (const std::exception &e) {
        out.failure = e.what();
    }
    return out;
}

uint32 WorkerCount() {
    return std::clamp<uint32>(std::thread::hardware_concurrency() / 2, 1, 8);
}

// Runs the seeds on worker threads (each scenario owns its software renderer, unthreaded); Catch assertions stay on
// the calling thread
RandomRunResult RunRandomScenarios(uint64 firstSeed, uint64 count, Coverage &cov) {
    const uint32 workers = WorkerCount();
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
        cov.Merge(c);
    }

    RandomRunResult result;
    uint32 reported = 0;
    auto report = [&](uint64 seed, const std::string &what) {
        result.firstMismatch = std::min(result.firstMismatch, seed);
        if (reported++ < 5) {
            UNSCOPED_INFO("seed " << seed << ": " << what);
        }
    };
    for (uint64 i = 0; i < count; ++i) {
        const SeedResult &r = results[i];
        const uint64 seed = firstSeed + i;
        if (!r.failure.empty()) {
            ++result.failures;
            report(seed, "exception: " + r.failure);
            continue;
        }
        if (!r.diff.empty()) {
            ++result.mismatches;
            report(seed, r.diff);
        }
        if (!r.noBreakDiff.empty()) {
            ++result.noBreakMismatches;
            report(seed, "without the early break: " + r.noBreakDiff);
        }
        result.maxPerUnit = std::max(result.maxPerUnit, r.stats.maxPerUnit);
        result.fragments += r.stats.fragments;
        result.batches += r.stats.batches;
        result.spans += r.stats.spans;
        result.tablesWithAlt += r.alt;
        result.tablesWithMesh += r.mesh;
        result.drawn.Merge(r.stats.drawn);
        for (size_t e = 0; e < result.batchEnds.size(); ++e) {
            result.batchEnds[e] += r.front.batchEnds[e];
        }
        result.fbWrites += r.stats.fbWrites;
        result.vramUploadBytes += r.stats.vramUploadBytes;
    }
    return result;
}

void CheckRandomRun(const RandomRunResult &r, const Coverage &cov) {
    CAPTURE(r.firstMismatch, r.maxPerUnit, r.fragments, r.batches, r.spans, r.fbWrites, r.vramUploadBytes);
    static constexpr const char *kEndNames[] = {"op kind", "VRAM hazard", "framebuffer swap", "capacity",
                                                "frame boundary", "take"};
    for (size_t e = 0; e < r.batchEnds.size(); ++e) {
        INFO("batches closed by " << kEndNames[e] << ": " << r.batchEnds[e]);
        CHECK(r.batchEnds[e] > 0);
    }
    CHECK(r.fbWrites > 0);
    CHECK(r.failures == 0);
    CHECK(r.mismatches == 0);
    CHECK(r.noBreakMismatches == 0);
    CHECK(r.fragments > 0);
    CHECK(r.tablesWithAlt > 0);
    CHECK(r.tablesWithMesh > 0);
    for (uint32 i = 0; i < ref::DrawnCoverage::Count; ++i) {
        INFO("drawn: " << ref::DrawnCoverage::Name(i));
        CHECK(r.drawn[i] > 0);
    }
    for (uint32 i = 0; i < Coverage::Count; ++i) {
        INFO("generated: " << Coverage::kNames[i]);
        CHECK(cov.counts[i] > 0);
    }
}
} // namespace

// =====================================================================================================================
// Steppers

TEST_CASE("VDP1 reference steppers step exactly like vdp1_steppers.hpp", "[vdp1][reference]") {
    TestRng rng{101};
    uint64 lineMismatches = 0;
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
            same = same && real.SystemClip(w, h) == port.SystemClip(w, h);
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
        const uint32 slots = ref::SpanSlots(state, cmd, span);
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

namespace {

// A line of the clip-loop hang class (master 1a769b1): major delta 2048 (+ 4096 k), towards negative (or positive)
// coordinates on its major axis
std::pair<CoordS32, CoordS32> HangClassLine(TestRng &rng, bool negative, bool xMajor) {
    const sint32 d = (rng.Chance(80) ? 2048 : 6144) * (negative ? -1 : 1);
    const CoordS32 a{rng.Chance(70) ? rng.Range(-300, 700) : rng.Range(-9000, 9000),
                     rng.Chance(70) ? rng.Range(-300, 600) : rng.Range(-9000, 9000)};
    const sint32 minor = rng.Range(-std::abs(d) + 1, std::abs(d) - 1);
    const CoordS32 b = xMajor ? CoordS32{a.x() + d, a.y() + minor} : CoordS32{a.x() + minor, a.y() + d};
    return {a, b};
}

} // namespace

TEST_CASE("VDP1 reference clips the hang-class lines like LineStepper", "[vdp1][reference]") {
    // LineStepper::SystemClip's minor-step loops could never end for lines without antialiasing whose major delta is
    // 2048 (mod 4096) running towards negative coordinates on the major axis; master now clips those by stepping.
    // The reference's port follows the same rule (written from its description): equal to LineStepper on the whole
    // family, both directions and axes, AA on and off; the fallback is taken only in the hang class.
    TestRng rng{113};
    uint64 mismatches = 0, fallbacks = 0, wrongFallbacks = 0;
    for (int i = 0; i < 40'000; ++i) {
        const bool negative = rng.Chance(50), xMajor = rng.Chance(50), aa = rng.Chance(50);
        const auto [a, b] = HangClassLine(rng, negative, xMajor);
        const sint32 w = rng.Chance(80) ? rng.Range(0, 704) : rng.Range(0, 0xFFFF);
        const sint32 h = rng.Chance(80) ? rng.Range(0, 512) : rng.Range(0, 0x1FFFF);
        vdp::LineStepper<> real{a, b, aa};
        ref::port::Line port{a, b, aa};
        bool same = real.SystemClip(w, h) == port.SystemClip(w, h) && real.Length() == port.length;
        if (port.clippedByStepping) {
            ++fallbacks;
            wrongFallbacks += aa || !negative;
        }
        bool realAA = false, portAA = false;
        real.Step();
        port.Step();
        for (; same && real.CanStep(); realAA = real.Step(), portAA = port.Step()) {
            same = port.CanStep() && real.Coord().elements == port.Coord().elements && realAA == portAA &&
                   (!realAA || real.AACoord().elements == port.AACoord().elements);
        }
        same = same && !port.CanStep();
        mismatches += !same;
    }
    CAPTURE(fallbacks);
    CHECK(mismatches == 0);
    CHECK(fallbacks > 0);
    CHECK(wrongFallbacks == 0);
}

TEST_CASE("VDP1 reference matches software on hang-class lines", "[vdp1][reference]") {
    // Draw Line and polylines (no antialiasing) and polygons and distorted sprites (antialiased spans) whose lines
    // have a major delta of 2048 in both directions, through both renderers
    TestRng rng{127};
    uint64 drawn = 0, visibleFallbackLines = 0;
    const std::vector<uint8> texture = [] {
        std::vector<uint8> bytes(16 * 16 * 2);
        for (uint32 i = 0; i < 16 * 16; ++i) {
            bytes[i * 2] = static_cast<uint8>(0x80 | (i & 0x7F));
            bytes[i * 2 + 1] = static_cast<uint8>(i * 7);
        }
        return bytes;
    }();
    for (int n = 0; n < 300; ++n) {
        CAPTURE(n);
        Scenario s;
        s.clip.sysClipH = static_cast<uint16>(rng.Chance(50) ? 319 : rng.Range(100, 704));
        s.clip.sysClipV = static_cast<uint16>(rng.Chance(50) ? 255 : rng.Range(100, 512));
        if (rng.Chance(25)) {
            s.tvmd = 0x80C0;
            s.deinterlace = true;
            s.vres = 448;
        }
        const uint16 srca = s.AddTexture(texture);
        s.Op0(Op::Kind::BeginFrame);
        for (int k = 0; k < 4; ++k) {
            const bool negative = rng.Chance(50), xMajor = rng.Chance(50);
            const auto [a, b] = HangClassLine(rng, negative, xMajor);
            const uint16 color = static_cast<uint16>(rng.Next() | 0x8000);
            const sint32 kind = rng.Range(0, 3);
            if (kind == 0 && !s.deinterlace) {
                // a Draw Line that takes the stepping fallback and still shows pixels
                ref::port::Line line{a, b, false};
                line.SystemClip(s.clip.sysClipH, s.clip.sysClipV);
                visibleFallbackLines += line.clippedByStepping && line.length > 0;
            }
            switch (kind) {
            case 0:
                s.AddCommand({0x0006, 0, 0x00C0, color, 0, 0, U(a.x()), U(a.y()), U(b.x()), U(b.y()), 0, 0, 0, 0, 0, 0});
                break;
            case 1: {
                const CoordS32 c{b.x() + rng.Range(-20, 20), b.y() + rng.Range(-20, 20)};
                s.AddCommand({0x0005, 0, 0x00C0, color, 0, 0, U(a.x()), U(a.y()), U(b.x()), U(b.y()), U(c.x()),
                              U(c.y()), U(a.x() + 3), U(a.y() + 3), 0, 0});
                break;
            }
            case 2: {
                // L-R lines of the quad run from edge A-D to edge B-C with the 2048 major delta
                const CoordS32 off = xMajor ? CoordS32{0, rng.Range(1, 40)} : CoordS32{rng.Range(1, 40), 0};
                s.Polygon(0x00C0, color, a, b, {b.x() + off.x(), b.y() + off.y()}, {a.x() + off.x(), a.y() + off.y()});
                break;
            }
            default: {
                const CoordS32 off = xMajor ? CoordS32{0, rng.Range(1, 40)} : CoordS32{rng.Range(1, 40), 0};
                s.Distorted(0, 0x00E8, 0, srca, 0x0210, a, b, {b.x() + off.x(), b.y() + off.y()},
                            {a.x() + off.x(), a.y() + off.y()});
                break;
            }
            }
        }
        s.Op0(Op::Kind::EndFrame);
        const auto sw = RunSoftware(s);
        const ReferenceRun r = RunReference(s);
        CHECK(CompareFbram(*sw, *r.fb) == "");
        drawn += r.stats.fragments > 0;
    }
    CAPTURE(drawn, visibleFallbackLines);
    CHECK(drawn > 50);
    CHECK(visibleFallbackLines > 0);
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
    std::atomic<uint64> repeats{0}, outsideGrid{0}, wrongMainCount{0}, lines{0}, pixels{0};
    std::atomic<sint32> nextDX{-kMax};
    std::vector<std::thread> threads;
    for (uint32 w = 0; w < WorkerCount(); ++w) {
        threads.emplace_back([&] {
            std::vector<uint32> stamp(static_cast<size_t>(kGrid) * kGrid, 0);
            uint32 generation = 0;
            uint64 rep = 0, out = 0, wrong = 0, lin = 0, pix = 0;
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
                        wrong += mains != static_cast<uint64>(std::max(std::abs(dx), std::abs(dy))) + 1;
                    }
                }
            }
            repeats += rep;
            outsideGrid += out;
            wrongMainCount += wrong;
            lines += lin;
            pixels += pix;
        });
    }
    for (auto &t : threads) {
        t.join();
    }
    CAPTURE(lines.load(), pixels.load());
    CHECK(repeats == 0);
    CHECK(outsideGrid == 0);
    CHECK(wrongMainCount == 0);

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
    CAPTURE(seconds);
    CheckRandomRun(r, cov);
}

TEST_CASE("VDP1 reference FBRAM equals software on random command tables (long)", "[vdp1][reference][.]") {
    Coverage cov;
    const RandomRunResult r = RunRandomScenarios(1'000'001, 200'000, cov);
    CheckRandomRun(r, cov);
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

namespace {

// Full grid: 1,536 scenes; otherwise the subset with cycles {0, 37}, TVMD {normal, double density + HRES bit 0} and
// neither or both of deinterlace and transparent meshes (192 scenes)
uint64 RunEraseGrid(bool full) {
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
                        if (!full && ((cycles != 0 && cycles != 37) || (tvmd != 0x8000 && tvmd != 0x80C1) ||
                                      (flags != 0 && flags != 3))) {
                            continue;
                        }
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
    return scenarios;
}

} // namespace

TEST_CASE("VDP1 reference erase matches software", "[vdp1][reference]") {
    // Regions with y1 == y3 (one row), x1 == x3 (nothing), y1 > y3, the 400/428 and VRes clamps, cycle-limited partial
    // erases ending mid-row, 8 bpp and every erase offset shift, double density (alt mirror) and transparent meshes
    // (mesh cleared). The display framebuffer first gets main, alt and mesh content from mesh polygons, then swapped in.
    CHECK(RunEraseGrid(false) == 6 * 2 * 4 * 2 * 2);
}

TEST_CASE("VDP1 reference erase matches software (full grid)", "[vdp1][reference][.]") {
    CHECK(RunEraseGrid(true) == 6 * 4 * 4 * 4 * 4);
}

TEST_CASE("VDP1 reference through the front half: VRAM rewrites between commands", "[vdp1][reference][front]") {
    // Sprites read a texture (colour mode 5) or a look-up table (colour mode 1) that the CPU rewrites after each draw,
    // before the next draw reads it again: software draws each sprite with the data of its time. The front half closes
    // the batch at each rewrite and uploads the old data with it. With hazard tracking off the GPU would draw the
    // earlier sprites with the later data, and the comparison notices.
    for (const uint32_t colorMode : {5u, 1u}) {
        CAPTURE(colorMode);
        Scenario s;
        s.fbFill = {0x80, 0x00};
        const uint16_t srca = s.AddTexture(colorMode == 5 ? RGBTexture(16, 16, 9) : std::vector<uint8>(128, 0x5A));
        const uint32_t textureAddress = srca * 8u;
        const uint32_t lut = 0x6000;
        for (uint32 i = 0; i < 16; ++i) {
            s.Write16(lut + i * 2, static_cast<uint16>(0x8000 | (i * 0x0421)));
        }
        s.Op0(Op::Kind::BeginFrame);
        for (int k = 0; k < 12; ++k) {
            const CoordS32 a{10 + k * 24, 20 + (k % 3) * 30};
            s.Distorted(0, static_cast<uint16>(0x00C0 | (colorMode << 3)), static_cast<uint16>(lut / 8), srca, 0x0210, a,
                        {a.x() + 15, a.y()}, {a.x() + 15, a.y() + 15}, {a.x(), a.y() + 15});
            // rewrite a texel row (or a look-up table entry) the next sprite reads
            for (uint32 w = 0; w < 4; ++w) {
                const uint32_t address = colorMode == 5 ? textureAddress + ((k * 37 + w * 8) % 512 & ~1u)
                                                        : lut + ((k + w) % 16) * 2;
                s.ops.push_back({.kind = Op::Kind::WriteVRAM,
                                 .address = address,
                                 .value = static_cast<uint16>(0x8000 | ((k * 7919 + w * 31) & 0x7FFF))});
            }
        }
        s.Op0(Op::Kind::EndFrame);
        const auto sw = RunSoftware(s);
        const ReferenceRun r = RunReference(s);
        CHECK(CompareFbram(*sw, *r.fb) == "");
        CHECK(r.front.batchEnds[static_cast<size_t>(gpu::Vdp1BatchEnd::VramHazard)] == 12);
        CHECK(r.stats.batches == 12);

        Scenario off = s;
        off.hazardTracking = false;
        const ReferenceRun broken = RunReference(off);
        CHECK(broken.stats.batches == 1);
        CHECK(CompareFbram(*sw, *broken.fb) != "");
    }
}

TEST_CASE("VDP1 reference through the front half: one batch mixing 8 bpp, 16 bpp and transparent meshes",
          "[vdp1][reference][front]") {
    // Overlapping draws in one batch while TVMR switches between 16 and 8 bpp and the transparent-meshes enhancement
    // switches on and off: each fragment carries its command's modes, so the batch does not split and the per-word
    // resolve applies each write as software did (16-bit words, 8-bit bytes, mesh writes and mesh clears).
    for (const bool startMeshes : {false, true}) {
        CAPTURE(startMeshes);
        Scenario s;
        s.transparentMeshes = startMeshes;
        s.fbFill = {0x9C, 0xE7, 0x80, 0x01, 0x5A};
        const uint16 grda = s.AddGouraud({0x7FFF, 0x0000, 0x03E0, 0x7C00});
        s.Op0(Op::Kind::BeginFrame);
        const CoordS32 a{40, 30}, b{140, 40}, c{150, 120}, d{30, 110};
        s.Polygon(0x01C0, 0x83E0, a, b, c, d);                 // mesh, 16 bpp
        s.Op0(Op::Kind::WriteTVMR, 1);                         // 8 bpp
        s.Polygon(0x00C0, 0x00A5, {60, 20}, {170, 50}, {120, 140}, {50, 90});
        s.Polygon(0x01C0, 0x003C, {20, 60}, {160, 60}, {160, 80}, {20, 80}); // mesh, 8 bpp
        s.Op0(Op::Kind::SetEnhancements, startMeshes ? 0 : 2); // transparent meshes toggled
        s.Polygon(0x01C0, 0x00F0, {50, 50}, {130, 50}, {130, 130}, {50, 130});
        s.Polygon(0x80C0, 0x0000, {70, 40}, {90, 40}, {90, 140}, {70, 140}); // MSB-on in 8 bpp
        s.Op0(Op::Kind::WriteTVMR, 0);                                         // 16 bpp again
        s.Polygon(0x01C7, 0x801F, {35, 35}, {145, 45}, {145, 125}, {35, 115}, grda); // mesh, half-transparent, Gouraud
        s.Op0(Op::Kind::SetEnhancements, startMeshes ? 2 : 0);
        s.Polygon(0x00C3, 0xFC00, {45, 25}, {155, 65}, {125, 135}, {25, 95}); // half-transparent
        s.Polygon(0x00C1, 0x8000, {80, 30}, {100, 30}, {100, 150}, {80, 150}); // shadow
        s.Op0(Op::Kind::EndFrame);
        const auto sw = RunSoftware(s);
        const ReferenceRun r = RunReference(s, {}, 77);
        CHECK(CompareFbram(*sw, *r.fb) == "");
        CHECK(r.stats.batches == 1);
        CHECK(r.stats.drawn[ref::DrawnCoverage::MixedModeBatches] == 1);
        CHECK(r.stats.drawn[ref::DrawnCoverage::Mesh] > 0);
        CHECK(r.stats.drawn[ref::DrawnCoverage::ByteEven] > 0);
        CHECK(r.stats.drawn[ref::DrawnCoverage::ColorCalc0 + 3] > 0);
    }
}

TEST_CASE("VDP1 reference through the front half: swaps, CPU writes and erases between draws",
          "[vdp1][reference][front]") {
    // A manual-swap pattern: draws into one framebuffer, a swap (no erase), draws into the other, CPU writes into the
    // draw framebuffer between draws (deinterlaced double density: mirrored to the alternate field), an erase of the
    // display framebuffer and a frame boundary in the middle
    Scenario s;
    s.tvmd = 0x80C0;
    s.deinterlace = true;
    s.transparentMeshes = true;
    s.vres = 448;
    s.fbFill = {0x12, 0x34};
    s.Op0(Op::Kind::BeginFrame);
    s.Polygon(0x01C0, 0x83E0, {10, 10}, {60, 10}, {60, 60}, {10, 60});
    s.ops.push_back({.kind = Op::Kind::WriteFB16, .address = (20 * 512 + 20) * 2, .value = 0xFFFF});
    s.ops.push_back({.kind = Op::Kind::WriteFB8, .address = (21 * 512 + 20) * 2 + 1, .value = 0xAB});
    s.Polygon(0x00C3, 0x801F, {15, 15}, {50, 15}, {50, 50}, {15, 50});
    s.Op0(Op::Kind::Swap);
    s.Polygon(0x00C0, 0xFC00, {30, 30}, {90, 30}, {90, 90}, {30, 90});
    s.Op0(Op::Kind::FrameBoundary);
    s.ops.push_back({.kind = Op::Kind::Erase, .value = 0x8421, .x1 = 0, .y1 = 0, .x3 = 40, .y3 = 40});
    s.Polygon(0x01C0, 0x801F, {35, 35}, {85, 35}, {85, 85}, {35, 85});
    s.Op0(Op::Kind::EndFrame);
    const auto sw = RunSoftware(s);
    const ReferenceRun r = RunReference(s, {}, 5);
    CHECK(CompareFbram(*sw, *r.fb) == "");
    CHECK(CompareState(*sw, r) == "");
    CHECK(r.stats.batches == 4);
    CHECK(r.front.batchEnds[static_cast<size_t>(gpu::Vdp1BatchEnd::FbSwap)] == 1);
    CHECK(r.stats.fbWrites == 2);
    CHECK(r.stats.erases == 1);
}

TEST_CASE("VDP1 reference: one random table (BRIMIR_VDP1REF_SEED)", "[vdp1][reference][.debug]") {
    const char *env = std::getenv("BRIMIR_VDP1REF_SEED");
    const uint64 seed = env ? std::strtoull(env, nullptr, 10) : 1;
    Coverage cov;
    const Scenario s = RandomScenario(seed, cov);
    std::printf("seed %llu: tvmd %04X tvmr %04X fbcr %04X deint %d tm %d vres %u maxBatch %u ops %zu\n",
                static_cast<unsigned long long>(seed), s.tvmd, s.tvmr, s.fbcr, s.deinterlace, s.transparentMeshes,
                s.vres, s.limits.fragments, s.ops.size());
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
