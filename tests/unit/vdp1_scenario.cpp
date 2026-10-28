#include "vdp1_scenario.hpp"

#include <ymir/util/data_ops.hpp>

#include <cstdio>
#include <cstring>

namespace brimir::test::vdp1scene {

using namespace ymir;
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

ReferenceRun RunReference(const Scenario &s, ref::RasterOptions options, uint64 shuffleSeed, const OpListHook &onOps) {
    ReferenceRun run;
    auto &state = *run.state;
    SetupState(s, state);
    FillMain(s, run.fb->main);

    gpu::Vdp1FrontEnd front{state};
    front.SetEnhancements({.deinterlace = s.deinterlace, .transparentMeshes = s.transparentMeshes});
    front.SetBatchLimitsForTesting(s.limits);
    front.SetHazardTrackingForTesting(s.hazardTracking);
    ref::ReferenceVDP1 reference{*run.fb, options, shuffleSeed};
    auto replay = [&](const gpu::Vdp1OpList &ops) {
        if (onOps) {
            onOps(ops);
        }
        reference.Replay(ops);
    };

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
        case Op::Kind::Take: replay(front.TakeOps()); break;
        default: break;
        }
    }
    replay(front.TakeOps());
    run.stats = reference.Stats();
    run.front = front.Stats();
    run.doubleV = front.DoubleV();
    return run;
}

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

namespace {

// The eight slices of `a` and `b` (main 0-1, alt 0-1, mesh[0] 0-1, mesh[1] 0-1)
std::string CompareSlices(const std::array<const vdp::SpriteFB *, 8> &a, const std::array<const vdp::SpriteFB *, 8> &b,
                          const char *aName, const char *bName) {
    static constexpr const char *kSlices[8] = {"main fb0",    "main fb1",    "alt fb0",     "alt fb1",
                                               "mesh[0] fb0", "mesh[0] fb1", "mesh[1] fb0", "mesh[1] fb1"};
    std::string first;
    uint64 count = 0;
    for (uint32 slice = 0; slice < 8; ++slice) {
        const vdp::SpriteFB &x = *a[slice];
        const vdp::SpriteFB &y = *b[slice];
        if (std::memcmp(x.data(), y.data(), x.size()) == 0) {
            continue;
        }
        for (size_t i = 0; i < x.size(); ++i) {
            if (x[i] != y[i] && count++ == 0) {
                char buf[192];
                std::snprintf(buf, sizeof(buf), "%s byte 0x%05zX (x %zu, y %zu at 16 bpp/512): %s %02X %s %02X",
                              kSlices[slice], i, (i / 2) % 512, (i / 2) / 512, aName, x[i], bName, y[i]);
                first = buf;
            }
        }
    }
    if (count == 0) {
        return {};
    }
    return first + " (" + std::to_string(count) + " bytes differ)";
}

std::array<const vdp::SpriteFB *, 8> Slices(const ref::Fbram &fb) {
    return {&fb.main[0], &fb.main[1], &fb.alt[0], &fb.alt[1], &fb.mesh[0][0], &fb.mesh[0][1], &fb.mesh[1][0],
            &fb.mesh[1][1]};
}

} // namespace

std::string CompareFbram(const SoftwareRun &sw, const ref::Fbram &fb) {
    const std::array<const vdp::SpriteFB *, 8> software{&sw.Main(0),    &sw.Main(1),    &sw.Alt(0),     &sw.Alt(1),
                                                        &sw.Mesh(0, 0), &sw.Mesh(0, 1), &sw.Mesh(1, 0), &sw.Mesh(1, 1)};
    return CompareSlices(software, Slices(fb), "software", "reference");
}

std::string CompareFbram(const ref::Fbram &a, const ref::Fbram &b, const char *aName, const char *bName) {
    return CompareSlices(Slices(a), Slices(b), aName, bName);
}

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

} // namespace brimir::test::vdp1scene
