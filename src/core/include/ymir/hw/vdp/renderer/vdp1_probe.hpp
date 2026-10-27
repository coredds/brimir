#pragma once

// Brimir: VDP1 workload probe for `brimir_vkhost --probe` (GPU stage 2 measurements). The counters exist only in
// builds configured with -DBRIMIR_VDP1_PROBE=ON; otherwise BRIMIR_VDP1_PROBE_COUNT and BRIMIR_VDP1_PROBE_DRAWN
// expand to nothing, so normal builds are unchanged. Counting never changes behaviour.
//
// Counters (all since the last VDP1ProbeTake):
//   commands  VDP1 commands executed (VDP1ExecuteCommand, any type)
//   lines     lines handed to the line plotters after the whole-line system clip reject (polygon and sprite quad
//             lines, polyline and line segments)
//   pixels    pixel writes into the framebuffer (VDP1PlotPixel calls that reach the write, MSB-on included; the
//             transparent-mesh and double-interlace skips are not writes)
//   syncs     VDP1SyncFB calls (CPU and DMA reads of the VDP1 framebuffer)
//   dirtySyncs  syncs preceded by a draw command or an erase since the previous sync (the reads a GPU renderer would
//             have to wait for)
//   fbWrites  CPU and DMA framebuffer writes (VDP1WriteFB calls, 8- or 16-bit)
//   erases    framebuffer erases (VDP1EraseFramebuffer calls)
//
// The line and pixel counters are bumped on the VDP1 render thread when VDP1 rendering is threaded, so per-frame
// values can lag the emulator thread by a little; totals are exact.

#if BRIMIR_VDP1_PROBE

    #include <atomic>
    #include <cstdint>

namespace ymir::vdp::probe {

struct VDP1Counters {
    std::atomic<uint64_t> commands{0};
    std::atomic<uint64_t> lines{0};
    std::atomic<uint64_t> pixels{0};
    std::atomic<uint64_t> syncs{0};
    std::atomic<uint64_t> dirtySyncs{0};
    std::atomic<uint64_t> fbWrites{0};
    std::atomic<uint64_t> erases{0};
    std::atomic<bool> drawnSinceSync{false};
};

inline VDP1Counters g_vdp1;

struct VDP1Snapshot {
    uint64_t commands, lines, pixels, syncs, dirtySyncs, fbWrites, erases;
};

// Returns the counters and resets them.
inline VDP1Snapshot VDP1ProbeTake() {
    auto take = [](std::atomic<uint64_t> &c) { return c.exchange(0, std::memory_order_relaxed); };
    return {take(g_vdp1.commands), take(g_vdp1.lines),    take(g_vdp1.pixels), take(g_vdp1.syncs),
            take(g_vdp1.dirtySyncs), take(g_vdp1.fbWrites), take(g_vdp1.erases)};
}

inline void VDP1ProbeSync() {
    g_vdp1.syncs.fetch_add(1, std::memory_order_relaxed);
    if (g_vdp1.drawnSinceSync.exchange(false, std::memory_order_relaxed)) {
        g_vdp1.dirtySyncs.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace ymir::vdp::probe

    #define BRIMIR_VDP1_PROBE_COUNT(field) (::ymir::vdp::probe::g_vdp1.field.fetch_add(1, std::memory_order_relaxed))
    #define BRIMIR_VDP1_PROBE_DRAWN() (::ymir::vdp::probe::g_vdp1.drawnSinceSync.store(true, std::memory_order_relaxed))
    #define BRIMIR_VDP1_PROBE_SYNC() (::ymir::vdp::probe::VDP1ProbeSync())

#else

    #define BRIMIR_VDP1_PROBE_COUNT(field) ((void)0)
    #define BRIMIR_VDP1_PROBE_DRAWN() ((void)0)
    #define BRIMIR_VDP1_PROBE_SYNC() ((void)0)

#endif
