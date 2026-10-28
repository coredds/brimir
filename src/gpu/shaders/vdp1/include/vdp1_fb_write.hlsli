// Brimir: shared part of the CPU framebuffer write passes (cs_vdp1_fb_write_mark, cs_vdp1_fb_write,
// cs_vdp1_fb_write_clear). Brimir only.
//
// A run applies records [offset, offset + count) of g_fbWrites in order (reference ApplyFbWrites): each replaces the
// bits of its mask (0xFF00 the even byte, 0x00FF the odd byte, or both) of its word in main framebuffer fb, and of the
// alternate field when it mirrors; the meshes are left alone. In order, a byte ends with the value of the last record
// that writes it, so: mark (each record InterlockedMax(record + 1) into the winner slot of every byte it writes, main and
// alternate field separately), write (a record writes the bytes whose winner it is), clear (the winner slots back to
// 0). One thread per record of the run, ceil(count / 64) groups per pass.
// The including shader declares g_fbWrites (t3), g_fbWinners (u7) and g_params.
#ifndef BRIMIR_VDP1_FB_WRITE_HLSLI
#define BRIMIR_VDP1_FB_WRITE_HLSLI

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

// The record of this thread (false if none); its winner slots: [0] main even byte, [1] main odd byte, [2] [3] alt
bool FbWriteRecord(uint thread, out FbWrite w, out uint slots[4], out bool writes[4]) {
    uint records, recordStride, winners, winnerStride;
    g_fbWrites.GetDimensions(records, recordStride);
    g_fbWinners.GetDimensions(winners, winnerStride);
    const uint index = g_params.offset + thread;
    w.address = 0;
    w.value = 0;
    w.mask = 0;
    w.pad = 0;
    [unroll] for (uint k = 0; k < 4; ++k) {
        slots[k] = 0;
        writes[k] = false;
    }
    if (thread >= g_params.count || index < g_params.offset || index >= records) {
        return false;
    }
    w = g_fbWrites[index];
    const uint word = Vdp1FbWriteWord(w.address);
    const uint fb = Vdp1FbWriteFb(w.address);
    const bool mirror = Vdp1FbWriteMirrorAlt(w.address);
    [unroll] for (uint s = 0; s < 4; ++s) {
        const bool alt = s >= 2;
        const bool low = (s & 1) != 0;
        slots[s] = Vdp1FbWinnerIndex(alt, fb, word, low);
        writes[s] = (w.mask & (low ? 0x00FFu : 0xFF00u)) != 0 && (!alt || mirror) && slots[s] < winners;
    }
    return true;
}

#endif
