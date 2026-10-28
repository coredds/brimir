// Brimir: VDP1 erase pass, VDP1DoEraseFramebuffer (GPU stage 2; research section 4.2 #20). Written from the CPU
// reference's Erase (tests/unit/vdp1_raster_reference.cpp). Brimir only.
//
// Thread (x, y) of the dispatch writes word x1 + x of row y1 + y of the region x1 <= x < x3, y1 <= y <= y3 (the front
// half computed it as software does, clamped and shifted): FBRAM word ((y << offsetShift) + x) & 0x1FFFF of the main
// framebuffer `fb` gets the erase value, and with the flags the alternate field too, and 0 in the mesh framebuffer(s).
// A write happens when its row-major index in the region is below writeLimit (a VBlank erase that runs out of cycles
// stops mid-row). Writes of rows that overlap (offset shift 8 with x >= 256) store the same value, so their order does
// not matter; adjacent words share a buffer uint, hence the atomic masked writes.
// Dispatch: ceil((x3 - x1) / 8) x ceil((y3 - y1 + 1) / 8) groups of 8 x 8.
// Bindings (vdp1_bindings.hpp kEraseBindings): FBRAM (19). Push constants: EraseParams.

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] EraseParams g_erase;

RWByteAddressBuffer g_fbram : register(u3);

#include "vdp1_fbram.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (g_erase.x3 <= g_erase.x1 || g_erase.y3 < g_erase.y1) {
        return;
    }
    const uint width = g_erase.x3 - g_erase.x1;
    const uint rows = g_erase.y3 - g_erase.y1 + 1;
    if (id.x >= width || id.y >= rows) {
        return;
    }
    const uint index = id.y * width + id.x;
    if (index >= g_erase.writeLimit) {
        return;
    }
    const uint x = g_erase.x1 + id.x;
    const uint y = g_erase.y1 + id.y;
    const uint word = ((y << g_erase.offsetShift) + x) & kWordIndexMask;
    const uint fb = Vdp1EraseFb(g_erase.flags);
    const bool mirror = Vdp1EraseMirrorAlt(g_erase.flags);
    const uint value = g_erase.value & 0xFFFF;

    FbramWriteWordMasked(Vdp1MainSlice(fb), word, value, 0xFFFF);
    if (mirror) {
        FbramWriteWordMasked(Vdp1AltSlice(fb), word, value, 0xFFFF);
    }
    if (Vdp1EraseClearMesh(g_erase.flags)) {
        FbramWriteWordMasked(Vdp1MeshSlice(0, fb), word, 0, 0xFFFF);
        if (mirror) {
            FbramWriteWordMasked(Vdp1MeshSlice(1, fb), word, 0, 0xFFFF);
        }
    }
}
