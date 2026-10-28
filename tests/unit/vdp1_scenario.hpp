#pragma once

// Scenarios for the VDP1 GPU back half's tests (GPU stage 2): registers, VRAM and an op list (commands, register
// writes, erases, swaps, VRAM and CPU framebuffer writes, enhancement changes, frame boundaries, op takes), run through
// the unthreaded SoftwareVDPRenderer (RunSoftware) and through the front half into the CPU reference
// (RunReference, vdp1_raster_reference.hpp), plus the seeded random table generator. Shared by
// test_vdp1_raster_reference.cpp (reference against software) and test_gpu_vdp1_passes.cpp (GPU passes against the
// reference), which replays the same op lists on the GPU.

#include "vdp1_raster_reference.hpp"

#include <gpu/vdp1/vdp1_front.hpp>

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace brimir::test::vdp1scene {

namespace vdp = ymir::vdp;
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

inline uint16 U(sint32 v) {
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

inline uint16 LSMD(uint16 tvmd) {
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

struct ReferenceRun {
    std::unique_ptr<ref::Fbram> fb = std::make_unique<ref::Fbram>();
    ref::ReferenceStats stats;
    gpu::Vdp1FrontStats front;
    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>(); // the front half's
    bool doubleV = false;
};

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

// -----------------------------------------------------------------------------
// Runs

void FillMain(const Scenario &s, std::array<vdp::SpriteFB, 2> &fbs);
// The scenario's registers, clip state, VRAM and framebuffers in a fresh VDPState (both renderers start from this)
void SetupState(const Scenario &s, vdp::VDPState &state);
vdp::config::Enhancements EnhancementsOf(uint16 value);
// The ops every runner applies to VDPState the way the VDP does (register writes, erase latches, swaps, the memory
// writes before their renderer hooks); returns false for the ops it leaves to the runner
bool ApplyStateOp(const Op &op, vdp::VDPState &state);

std::unique_ptr<SoftwareRun> RunSoftware(const Scenario &s);

// Called with every op list the front half returns, before the reference replays it
using OpListHook = std::function<void(const gpu::Vdp1OpList &)>;

// The front half driven like a renderer, its op lists replayed into the reference
ReferenceRun RunReference(const Scenario &s, ref::RasterOptions options = {}, uint64 shuffleSeed = 0,
                          const OpListHook &onOps = {});

// The clip state and doubleV the two renderers end with (the front half keeps software's VDPState::state1 and doubleV)
std::string CompareState(const SoftwareRun &sw, const ReferenceRun &r);
// Empty if equal; otherwise the first difference and the number of differing bytes
std::string CompareFbram(const SoftwareRun &sw, const ref::Fbram &fb);
// The same between two FBRAM sets (`aName` and `bName` name them in the message)
std::string CompareFbram(const ref::Fbram &a, const ref::Fbram &b, const char *aName = "expected",
                         const char *bName = "actual");

// -----------------------------------------------------------------------------
// Generators

Scenario RandomScenario(uint64 seed, Coverage &cov);
// w x h colour mode 5 texels, all opaque
std::vector<uint8> RGBTexture(uint32 w, uint32 h, uint64 seed);

} // namespace brimir::test::vdp1scene