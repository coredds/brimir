// Threaded VDP1 event order (performance round, Task 6): with threaded VDP1 rendering the emulator thread batches VDP1
// VRAM, framebuffer and register writes for the VDP1 render thread (VDP1RenderContext::EnqueueEvent, 64 events per
// bulk enqueue). The render thread must end up with exactly the VRAM and framebuffers the unthreaded renderer has,
// whatever the mix of writes (bytes, words, longs, misaligned, ascending, descending, repeated, wrapping at the end of
// VRAM, SCU DMA) and whatever comes between them (register writes, draws, CPU framebuffer writes, swaps, erases, resets,
// save and load syncs). Task 6 measured coalescing consecutive VRAM writes into range events (no gain, dropped) and
// moved the render thread's fence off the emulator thread's cache line; these cases guard both.

#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/hw/vdp/vdp_state.hpp>
#include <ymir/savestate/savestate.hpp>
#include <ymir/sys/saturn.hpp>

#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace {

using namespace ymir;

// A VDP state and a software renderer, threaded or not; every write goes through VDPState as the VDP does it (vdp.cpp
// VDP1WriteVRAM, VDP1WriteFB, VDP1WriteReg), so the renderer sees the masked address.
struct Rig {
    std::unique_ptr<vdp::VDPState> state = std::make_unique<vdp::VDPState>();
    vdp::config::VDP2DebugRender debug{};
    vdp::config::VDP2AccessPatternsConfig access{};
    std::unique_ptr<vdp::SoftwareVDPRenderer> renderer;
    bool threaded;

    explicit Rig(bool threaded)
        : threaded(threaded) {
        state->Reset(true);
        renderer = std::make_unique<vdp::SoftwareVDPRenderer>(*state, debug, access);
        renderer->EnableThreadedVDP1(threaded);
    }

    ~Rig() {
        renderer->EnableThreadedVDP1(false);
    }

    template <mem_primitive_16 T>
    void WriteVRAM(uint32 address, T value) {
        state->mem1.WriteVRAM<T>(address, value, [&](uint32 a, T v) { renderer->VDP1WriteVRAM(a, v); });
    }

    // A 32-bit bus write: two word writes, as VDP::MapMemory's 32-bit VDP1 VRAM handler does them
    void WriteVRAM32(uint32 address, uint32 value) {
        WriteVRAM<uint16>(address + 0, static_cast<uint16>(value >> 16u));
        WriteVRAM<uint16>(address + 2, static_cast<uint16>(value >> 0u));
    }

    template <mem_primitive_16 T>
    void WriteFB(uint32 address, T value) {
        state->VDP1WriteFB<T>(address, value, [&](uint32 a, T v) { renderer->VDP1WriteFB(a, v); });
    }

    void WriteReg(uint32 address, uint16 value) {
        state->VDP1WriteReg<false>(address, value, [&](uint32 a, uint16 v) { renderer->VDP1WriteReg(a, v); });
    }

    void Execute(uint32 cmdAddress) {
        const vdp::VDP1Command::Control control{.u16 = state->mem1.ReadVRAM<uint16>(cmdAddress)};
        renderer->VDP1ExecuteCommand(cmdAddress, control);
    }

    void Swap() {
        // VDP::VDP1SwapFramebuffer
        state->regs1.LatchEraseParameters();
        renderer->VDP1SwapFramebuffer();
        state->fbIndex.Flip();
    }

    // The draw framebuffer as drawn so far: the render thread's copy while threaded (the state's catches up at the end
    // of the draw), the state's otherwise. Call after PreSaveStateSync.
    const vdp::SpriteFB &DrawFB() const {
        const auto draw = state->fbIndex.draw;
        return threaded ? renderer->GetVDP1RendererMemoryForTesting().FBRAM[draw] : state->mem1.FBRAM[draw];
    }
};

// Both rigs agree, and the threaded render thread holds the emulator thread's VRAM.
void CheckSame(Rig &thr, Rig &unthr) {
    thr.renderer->PreSaveStateSync();
    unthr.renderer->PreSaveStateSync();
    const auto &rendererMem = thr.renderer->GetVDP1RendererMemoryForTesting();
    REQUIRE(rendererMem.VRAM == thr.state->mem1.VRAM);
    REQUIRE(thr.state->mem1.VRAM == unthr.state->mem1.VRAM);
    REQUIRE(thr.state->fbIndex.draw == unthr.state->fbIndex.draw);
    REQUIRE(thr.DrawFB() == unthr.DrawFB());
    const auto display = thr.state->fbIndex.display;
    REQUIRE(thr.state->mem1.FBRAM[display] == unthr.state->mem1.FBRAM[display]);
}

constexpr uint32 kVRAMSize = 0x80000;
constexpr uint32 kCmdBase = 0x00000; // command tables
constexpr uint32 kTexBase = 0x10000; // textures the draws read

// Command words (CMDCTRL .. CMDGRDA) written with a mix of widths.
void WriteCommand(Rig &rig, uint32 address, const std::array<uint16, 16> &words, int width) {
    for (uint32 i = 0; i < 16; ++i) {
        const uint32 a = address + i * 2;
        switch (width) {
        case 1:
            rig.WriteVRAM<uint8>(a + 0, static_cast<uint8>(words[i] >> 8));
            rig.WriteVRAM<uint8>(a + 1, static_cast<uint8>(words[i]));
            break;
        case 4:
            if (i % 2 == 0) {
                rig.WriteVRAM32(a, (uint32(words[i]) << 16) | words[i + 1]);
            }
            break;
        default: rig.WriteVRAM<uint16>(a, words[i]); break;
        }
    }
}

// The same randomized sequence of operations on both rigs, checked at random points and at the end.
void RunSequence(uint32 seed, int ops) {
    Rig thr(true);
    Rig unthr(false);
    Rig *rigs[2] = {&thr, &unthr};
    std::mt19937 rng(seed);
    auto rnd = [&](uint32 n) { return static_cast<uint32>(rng() % n); };

    // System clipping to the 512x256 framebuffer and local coordinates (0, 0)
    auto setup = [](Rig &r) {
        WriteCommand(r, kCmdBase + 0x00, {0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 511, 255, 0, 0, 0, 0}, 2);
        WriteCommand(r, kCmdBase + 0x20, {0x000A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 2);
        r.Execute(kCmdBase + 0x00);
        r.Execute(kCmdBase + 0x20);
        r.renderer->VDP1BeginFrame();
    };
    for (Rig *r : rigs) {
        setup(*r);
    }

    for (int op = 0; op < ops; ++op) {
        const uint32 kind = rnd(16);
        const uint32 start = rnd(kVRAMSize);
        const uint32 length = 1 + rnd(80);
        const uint32 seedValue = rng();
        CAPTURE(seed, op, kind, start, length);
        for (Rig *r : rigs) {
            std::mt19937 vals(seedValue); // same values on both rigs
            switch (kind) {
            case 0: // ascending words, up to and across the end of VRAM
            case 1:
                for (uint32 i = 0; i < length; ++i) {
                    r->WriteVRAM<uint16>(start + i * 2, static_cast<uint16>(vals()));
                }
                break;
            case 2: // ascending longs (bus 32-bit stores, SCU DMA)
                for (uint32 i = 0; i < length; ++i) {
                    r->WriteVRAM32(start + i * 4, static_cast<uint32>(vals()));
                }
                break;
            case 3: // ascending bytes from any address
                for (uint32 i = 0; i < length; ++i) {
                    r->WriteVRAM<uint8>(start + i, static_cast<uint8>(vals()));
                }
                break;
            case 4: { // mixed widths, contiguous
                uint32 a = start;
                for (uint32 i = 0; i < length; ++i) {
                    const uint32 w = vals() % 3;
                    if (w == 0) {
                        r->WriteVRAM<uint8>(a, static_cast<uint8>(vals()));
                        a += 1;
                    } else if (w == 1) {
                        a = (a + 1) & ~1u;
                        r->WriteVRAM<uint16>(a, static_cast<uint16>(vals()));
                        a += 2;
                    } else {
                        a = (a + 1) & ~1u;
                        r->WriteVRAM32(a, static_cast<uint32>(vals()));
                        a += 4;
                    }
                }
                break;
            }
            case 5: // descending words
                for (uint32 i = 0; i < length; ++i) {
                    r->WriteVRAM<uint16>(start - i * 2, static_cast<uint16>(vals()));
                }
                break;
            case 6: // the same word again and again, and bytes over it
                for (uint32 i = 0; i < length; ++i) {
                    if (vals() % 2) {
                        r->WriteVRAM<uint16>(start, static_cast<uint16>(vals()));
                    } else {
                        r->WriteVRAM<uint8>(start + (vals() % 2), static_cast<uint8>(vals()));
                    }
                }
                break;
            case 7: // misaligned words and longs (the VDP masks the address) at random places
                for (uint32 i = 0; i < length; ++i) {
                    const uint32 a = (start + vals() % 64) | 1u;
                    if (vals() % 2) {
                        r->WriteVRAM<uint16>(a, static_cast<uint16>(vals()));
                    } else {
                        r->WriteVRAM32(a & ~1u, static_cast<uint32>(vals()));
                    }
                }
                break;
            case 8: // erase registers between writes
                r->WriteVRAM<uint16>(start, static_cast<uint16>(vals()));
                r->WriteReg(0x06, static_cast<uint16>(vals()));                   // EWDR
                r->WriteVRAM<uint16>(start + 2, static_cast<uint16>(vals()));     // contiguous, after the reg write
                r->WriteReg(0x08, static_cast<uint16>(vals() & 0x7FFF));          // EWLR
                r->WriteReg(0x0A, static_cast<uint16>(vals() & 0xFFFF));          // EWRR
                r->WriteVRAM<uint16>(start + 4, static_cast<uint16>(vals()));
                break;
            case 9:
            case 10: { // a draw reading a texture and a command just written (still in the pending batch)
                const uint32 slot = kCmdBase + 0x40 + (vals() % 64) * 0x20;
                const uint32 tex = kTexBase + (vals() % 0x1000) * 8;
                const uint16 w8 = static_cast<uint16>(1 + vals() % 8);
                const uint16 h = static_cast<uint16>(1 + vals() % 48);
                for (uint32 i = 0; i < w8 * 8u * h; i += 2) { // RGB texels
                    r->WriteVRAM<uint16>(tex + i * 2, static_cast<uint16>(vals() | 0x8000));
                    r->WriteVRAM<uint16>(tex + i * 2 + 2, static_cast<uint16>(vals()));
                }
                const uint16 x = static_cast<uint16>(vals() % 480);
                const uint16 y = static_cast<uint16>(vals() % 240);
                const uint32 type = vals() % 4;
                std::array<uint16, 16> cmd{};
                cmd[2] = 0x00E8; // PMOD: MSB off, SPD, ECD off, RGB colour mode
                cmd[3] = static_cast<uint16>(0x8000 | vals());
                cmd[4] = static_cast<uint16>(tex >> 3);
                cmd[5] = static_cast<uint16>((w8 << 8) | h);
                cmd[6] = x;
                cmd[7] = y;
                if (type == 0) {
                    cmd[0] = 0x0000; // normal sprite
                } else if (type == 1) {
                    cmd[0] = 0x0004; // polygon
                    cmd[2] = 0x00C0;
                } else {
                    cmd[0] = 0x0002; // distorted sprite
                }
                if (type != 0) {
                    cmd[8] = static_cast<uint16>(x + vals() % 64);
                    cmd[9] = static_cast<uint16>(y + vals() % 16);
                    cmd[10] = static_cast<uint16>(x + vals() % 64);
                    cmd[11] = static_cast<uint16>(y + vals() % 64);
                    cmd[12] = static_cast<uint16>(x - vals() % 16);
                    cmd[13] = static_cast<uint16>(y + vals() % 64);
                }
                WriteCommand(*r, slot, cmd, static_cast<int>(vals() % 3 == 0 ? 1 : (vals() % 2 ? 2 : 4)));
                r->Execute(slot);
                r->WriteVRAM<uint16>(tex, static_cast<uint16>(vals())); // texture changed right after the draw
                break;
            }
            case 11: // CPU framebuffer writes between VRAM writes
                for (uint32 i = 0; i < length; ++i) {
                    r->WriteVRAM<uint16>(start + i * 2, static_cast<uint16>(vals()));
                    if (vals() % 2) {
                        r->WriteFB<uint16>(vals() % 0x40000, static_cast<uint16>(vals()));
                    } else {
                        r->WriteFB<uint8>(vals() % 0x40000, static_cast<uint8>(vals()));
                    }
                }
                break;
            case 12: // end of the draw, swap, erase, next frame
                r->WriteVRAM<uint16>(start, static_cast<uint16>(vals()));
                r->renderer->VDP1EndFrame();
                r->WriteVRAM<uint16>(start + 2, static_cast<uint16>(vals()));
                r->Swap();
                r->WriteVRAM<uint16>(start + 4, static_cast<uint16>(vals()));
                r->renderer->VDP1EraseFramebuffer(0);
                r->WriteVRAM<uint16>(start + 6, static_cast<uint16>(vals()));
                r->renderer->VDP1BeginFrame();
                break;
            case 13: // save in the middle of a run of writes
                for (uint32 i = 0; i < length; ++i) {
                    r->WriteVRAM<uint16>(start + i * 2, static_cast<uint16>(vals()));
                }
                r->renderer->PreSaveStateSync();
                for (uint32 i = length; i < length * 2; ++i) {
                    r->WriteVRAM<uint16>(start + i * 2, static_cast<uint16>(vals()));
                }
                break;
            case 14: { // load in the middle of a run of writes: the loaded VRAM replaces what was written before
                for (uint32 i = 0; i < length; ++i) {
                    r->WriteVRAM<uint16>(start + i * 2, static_cast<uint16>(vals()));
                }
                // VDP::LoadState: the renderer syncs, VDPState takes the state's VRAM, the renderer the state's draw
                // framebuffer (here the one it had: the state was saved at this point)
                r->renderer->PreLoadState();
                auto ext = std::make_unique<savestate::BrimirSaveState::VDPRenderer>();
                r->renderer->SaveBrimirState(*ext);
                for (uint32 i = 0; i < 256; ++i) {
                    r->state->mem1.VRAM[(start + i) % kVRAMSize] = static_cast<uint8>(vals());
                }
                r->renderer->LoadBrimirState(*ext);
                r->renderer->PostLoadStateSync();
                for (uint32 i = 0; i < length; ++i) {
                    r->WriteVRAM<uint16>(start + 512 + i * 2, static_cast<uint16>(vals()));
                }
                break;
            }
            case 15: // a VRAM reset (hard reset) between writes
                r->WriteVRAM<uint16>(start, static_cast<uint16>(vals()));
                if (vals() % 4 == 0) {
                    // VDP::Reset: the state, then the renderer (drained first: the queued draws read fbIndex)
                    r->renderer->PreSaveStateSync();
                    r->state->Reset(true);
                    r->renderer->Reset(true);
                    setup(*r);
                }
                r->WriteVRAM<uint16>(start + 2, static_cast<uint16>(vals()));
                break;
            }
        }
        if (rnd(8) == 0) {
            CheckSame(thr, unthr);
        }
    }
    for (Rig *r : rigs) {
        r->renderer->VDP1EndFrame();
    }
    CheckSame(thr, unthr);
}

} // namespace

TEST_CASE("Threaded VDP1 sees the same VRAM and framebuffers as unthreaded over write-heavy sequences",
          "[vdp][vdp1][threaded][perf]") {
    for (uint32 seed = 1; seed <= 12; ++seed) {
        RunSequence(seed, 400);
    }
}

namespace {

constexpr uint32 kBusVDP1VRAM = 0x5C0'0000;
constexpr uint32 kBusVDP1FB = 0x5C8'0000;
constexpr uint32 kBusVDP1Regs = 0x5D0'0000;
constexpr uint32 kBusVDP2TVMD = 0x5F8'0000;
constexpr uint32 kBusSCU = 0x5FE'0000;
constexpr uint32 kWRAMH = 0x0600'0000;

void Step(Saturn &saturn, uint64 cycles) {
    uint64 done = 0;
    while (done < cycles) {
        done += saturn.StepMasterSH2();
    }
}

// SCU DMA level 0 from work RAM to VDP1 VRAM (as the "SCU DMA ignores writes to VDP1 registers" backport test).
void SCUDMAToVDP1(Saturn &saturn, uint32 src, uint32 dstOffset, uint32 bytes) {
    saturn.mainBus.Write<uint32>(kBusSCU + 0x00, src);
    saturn.mainBus.Write<uint32>(kBusSCU + 0x04, kBusVDP1VRAM + dstOffset);
    saturn.mainBus.Write<uint32>(kBusSCU + 0x08, bytes);
    saturn.mainBus.Write<uint32>(kBusSCU + 0x0C, 0x101); // read and write address add
    saturn.mainBus.Write<uint32>(kBusSCU + 0x14, 7);     // start factor: DMA enable bit
    saturn.mainBus.Write<uint32>(kBusSCU + 0x10, 0x101); // enable, start
}

struct SystemView {
    std::vector<uint8> vram;
    std::vector<uint8> displayFB;
    std::vector<uint8> drawFB;
};

SystemView View(Saturn &saturn) {
    auto state = std::make_unique<savestate::SaveState>();
    saturn.SaveState(*state);
    SystemView view;
    view.vram.assign(state->vdp.VRAM1.begin(), state->vdp.VRAM1.end());
    const auto &display = state->vdp.FBRAM[state->vdp.displayFB];
    view.displayFB.assign(display.begin(), display.end());
    // The draw framebuffer as drawn so far (a threaded VDP1's state FBRAM catches up only at the end of the draw)
    view.drawFB.assign(state->brimir.vdpRenderer.vdp1DrawFB.begin(), state->brimir.vdpRenderer.vdp1DrawFB.end());
    return view;
}

void RoundTrip(Saturn &saturn) {
    auto state = std::make_unique<savestate::SaveState>();
    saturn.SaveState(*state);
    REQUIRE(saturn.LoadState(*state, true));
}

} // namespace

TEST_CASE("Threaded VDP1 matches unthreaded on a running system (SH-2 stores, SCU DMA, save and load)",
          "[vdp][vdp1][threaded][perf][savestate]") {
    auto thr = std::make_unique<Saturn>();
    thr->configuration.video.threadedVDP1 = true;
    auto unthr = std::make_unique<Saturn>();
    unthr->configuration.video.threadedVDP1 = false;
    Saturn *systems[2] = {thr.get(), unthr.get()};

    std::mt19937 rng(0x7A5C6);
    int drawsSeen = 0;
    for (int round = 0; round < 24; ++round) {
        const uint32 seedValue = rng();
        CAPTURE(round);
        for (Saturn *s : systems) {
            auto &bus = s->mainBus;
            std::mt19937 vals(seedValue);
            if (round == 0) {
                bus.Write<uint16>(kBusVDP2TVMD, 0x8000); // display on
            }

            // Command table through the bus with every store width, some misaligned (the VDP masks the address)
            auto put16 = [&](uint32 offset, uint16 value) {
                switch (vals() % 4) {
                case 0:
                    bus.Write<uint8>(kBusVDP1VRAM + offset, static_cast<uint8>(value >> 8));
                    bus.Write<uint8>(kBusVDP1VRAM + offset + 1, static_cast<uint8>(value));
                    break;
                case 1: bus.Write<uint16>(kBusVDP1VRAM + offset + (vals() % 2), value); break;
                default: bus.Write<uint16>(kBusVDP1VRAM + offset, value); break;
                }
            };
            auto put32 = [&](uint32 offset, uint16 hi, uint16 lo) {
                bus.Write<uint32>(kBusVDP1VRAM + offset, (uint32(hi) << 16) | lo);
            };
            uint32 addr = 0;
            put32(addr + 0x00, 0x0009, 0);
            put32(addr + 0x14, 319, 223);
            addr += 0x20;
            put32(addr + 0x00, 0x000A, 0);
            put32(addr + 0x0C, 0, 0);
            addr += 0x20;
            const int sprites = 4 + static_cast<int>(vals() % 12);
            for (int i = 0; i < sprites; ++i) {
                const uint16 x = static_cast<uint16>(vals() % 300);
                const uint16 y = static_cast<uint16>(vals() % 200);
                put16(addr + 0x00, (i % 2) ? 0x0002 : 0x0000); // distorted / normal sprite
                put16(addr + 0x02, 0);
                put16(addr + 0x04, 0x00E8); // RGB, SPD
                put16(addr + 0x06, 0);
                put16(addr + 0x08, static_cast<uint16>((0x10000 + (vals() % 64) * 0x400) >> 3));
                put16(addr + 0x0A, static_cast<uint16>((4 << 8) | 32)); // 32x32
                put32(addr + 0x0C, x, y);
                put32(addr + 0x10, static_cast<uint16>(x + 40), static_cast<uint16>(y + 4));
                put32(addr + 0x14, static_cast<uint16>(x + 36), static_cast<uint16>(y + 40));
                put32(addr + 0x18, static_cast<uint16>(x - 4), static_cast<uint16>(y + 30));
                addr += 0x20;
            }
            put16(addr, 0x8000); // end

            // Textures: CPU stores to work RAM, then SCU DMA into VDP1 VRAM; some longs written directly too
            const uint32 src = kWRAMH + 0x10000;
            const uint32 bytes = 0x400 * (1 + vals() % 8);
            for (uint32 i = 0; i < bytes; i += 4) {
                bus.Write<uint32>(src + i, static_cast<uint32>(vals()) | 0x80008000u);
            }
            SCUDMAToVDP1(*s, src, 0x10000 + (vals() % 32) * 0x400, bytes);
            for (uint32 i = 0; i < 64; ++i) {
                bus.Write<uint32>(kBusVDP1VRAM + 0x30000 + i * 4, static_cast<uint32>(vals()));
            }

            // Erase registers, then draw now, interleaved with more VRAM stores
            bus.Write<uint16>(kBusVDP1Regs + 0x06, static_cast<uint16>(vals()));
            bus.Write<uint16>(kBusVDP1VRAM + 0x40000, static_cast<uint16>(vals()));
            bus.Write<uint16>(kBusVDP1Regs + 0x04, 0x0001); // PTMR: draw now
            Step(*s, 2000 + vals() % 4000);
            for (uint32 i = 0; i < 32; ++i) {
                bus.Write<uint16>(kBusVDP1VRAM + 0x40002 + i * 2, static_cast<uint16>(vals()));
            }
            bus.Write<uint16>(kBusVDP1FB + (vals() % 0x20000) * 2, static_cast<uint16>(vals())); // CPU FB store
            bus.Write<uint8>(kBusVDP1FB + (vals() % 0x40000), static_cast<uint8>(vals()));
            if (round % 3 == 2) {
                RoundTrip(*s); // save and load right after a run of writes (mid-batch)
            }
            Step(*s, 20000 + vals() % 200000);
            for (uint32 i = 0; i < 16; ++i) {
                bus.Write<uint32>(kBusVDP1VRAM + 0x50000 + i * 4, static_cast<uint32>(vals()));
            }
        }
        const SystemView a = View(*thr);
        const SystemView b = View(*unthr);
        REQUIRE(a.vram == b.vram);
        REQUIRE(a.displayFB == b.displayFB);
        REQUIRE(a.drawFB == b.drawFB);
        if (a.drawFB != std::vector<uint8>(a.drawFB.size(), 0) ||
            a.displayFB != std::vector<uint8>(a.displayFB.size(), 0)) {
            ++drawsSeen;
        }
    }
    CHECK(drawsSeen > 0); // the VDP1 drew
}
