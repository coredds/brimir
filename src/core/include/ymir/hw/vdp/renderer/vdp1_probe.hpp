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
//   batches   draw batches the GPU VDP1 front half (src/gpu/vdp1/vdp1_front.hpp) closed, by end reason (Vdp1BatchEnd:
//             op kind, VRAM hazard, framebuffer swap, capacity, frame boundary, take); zero while no front half runs
//   nxSpans   N x spans the N x generator (src/gpu/vdp1/vdp1_nx_front.hpp) emitted, by kind (rect, quad, replicate)
//   nxSlots   their fragment slots
//   nxSubBatches  N x sub-batches
//   nxGenerateNanos  time spent in the N x generator (nanoseconds); all N x counters stay zero while VDP1 upscaling
//             is off
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
    std::atomic<uint64_t> batchEnds[6] = {};
    std::atomic<uint64_t> nxSpans[3] = {};
    std::atomic<uint64_t> nxSlots{0};
    std::atomic<uint64_t> nxSubBatches{0};
    std::atomic<uint64_t> nxGenerateNanos{0};
    std::atomic<bool> drawnSinceSync{false};
};

inline VDP1Counters g_vdp1;

struct VDP1Snapshot {
    uint64_t commands, lines, pixels, syncs, dirtySyncs, fbWrites, erases;
    uint64_t batchEnds[6];
    uint64_t nxSpans[3];
    uint64_t nxSlots, nxSubBatches, nxGenerateNanos;
};

// Returns the counters and resets them.
inline VDP1Snapshot VDP1ProbeTake() {
    auto take = [](std::atomic<uint64_t> &c) { return c.exchange(0, std::memory_order_relaxed); };
    VDP1Snapshot s{take(g_vdp1.commands),   take(g_vdp1.lines),    take(g_vdp1.pixels), take(g_vdp1.syncs),
                   take(g_vdp1.dirtySyncs), take(g_vdp1.fbWrites), take(g_vdp1.erases), {}, {}, 0, 0, 0};
    for (int i = 0; i < 6; ++i) {
        s.batchEnds[i] = take(g_vdp1.batchEnds[i]);
    }
    for (int i = 0; i < 3; ++i) {
        s.nxSpans[i] = take(g_vdp1.nxSpans[i]);
    }
    s.nxSlots = take(g_vdp1.nxSlots);
    s.nxSubBatches = take(g_vdp1.nxSubBatches);
    s.nxGenerateNanos = take(g_vdp1.nxGenerateNanos);
    return s;
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
    #define BRIMIR_VDP1_PROBE_BATCH(end) \
        (::ymir::vdp::probe::g_vdp1.batchEnds[(end) % 6].fetch_add(1, std::memory_order_relaxed))
    #define BRIMIR_VDP1_PROBE_ADD(field, value) \
        (::ymir::vdp::probe::g_vdp1.field.fetch_add((value), std::memory_order_relaxed))

#else

    #define BRIMIR_VDP1_PROBE_COUNT(field) ((void)0)
    #define BRIMIR_VDP1_PROBE_DRAWN() ((void)0)
    #define BRIMIR_VDP1_PROBE_SYNC() ((void)0)
    #define BRIMIR_VDP1_PROBE_BATCH(end) ((void)0)
    #define BRIMIR_VDP1_PROBE_ADD(field, value) ((void)0)

#endif
