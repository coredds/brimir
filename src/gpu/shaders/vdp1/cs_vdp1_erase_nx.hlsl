// Brimir: N x VDP1 erase pass (GPU stage 3; research section 2: an erase changes native words, so their N x blocks
// change the same way). Written from the CPU reference's N x Erase (tests/unit/vdp1_nx_reference.cpp). Brimir only.
//
// The native erase's region, word order and write limit (cs_vdp1_erase.hlsl): thread (x, y) of the dispatch takes word
// ((y << offsetShift) + x) & 0x1FFFF of row y1 + y, x1 + x when its row-major index in the region is below writeLimit
// (a block is erased exactly when its native word is), and writes the erase value to the word's N x N block in the main
// framebuffer `fb` (and the alternate field with the flags), 0 to its mesh blocks with clearMesh.
// Dispatch: ceil((x3 - x1) / 8) x ceil((y3 - y1 + 1) / 8) groups of 8 x 8.
// Bindings (vdp1_bindings.hpp kEraseNxBindings): N x surface (24). Push constants: EraseNxParams (scale = N, 2 to 4).

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] EraseNxParams g_params;

RWByteAddressBuffer g_fbramNx : register(u8);

#include "vdp1_fbram_nx.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const EraseParams e = g_params.erase;
    const uint scale = g_params.scale;
    if (e.x3 <= e.x1 || e.y3 < e.y1 || scale < 2 || scale > kMaxNxScale) {
        return;
    }
    const uint width = e.x3 - e.x1;
    const uint rows = e.y3 - e.y1 + 1;
    if (id.x >= width || id.y >= rows) {
        return;
    }
    const uint index = id.y * width + id.x;
    if (index >= e.writeLimit) {
        return;
    }
    const uint x = e.x1 + id.x;
    const uint y = e.y1 + id.y;
    const uint word = ((y << e.offsetShift) + x) & kWordIndexMask;
    const uint fb = Vdp1EraseFb(e.flags);
    const bool mirror = Vdp1EraseMirrorAlt(e.flags);
    const uint value = e.value & 0xFFFF;

    NxWriteBlock(Vdp1MainSlice(fb), word, value, 0xFFFF, scale, true);
    if (mirror) {
        NxWriteBlock(Vdp1AltSlice(fb), word, value, 0xFFFF, scale, true);
    }
    if (Vdp1EraseClearMesh(e.flags)) {
        NxWriteBlock(Vdp1MeshSlice(0, fb), word, 0, 0xFFFF, scale, true);
        if (mirror) {
            NxWriteBlock(Vdp1MeshSlice(1, fb), word, 0, 0xFFFF, scale, true);
        }
    }
}
