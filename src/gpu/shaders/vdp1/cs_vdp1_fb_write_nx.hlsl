// Brimir: N x VDP1 CPU framebuffer write pass (GPU stage 3; research section 2), recorded after the native run's
// fb_write_clear. Brimir only.
//
// One thread per record of the run [offset, offset + count): the bits of the record's mask of every N x word of its
// word's block, in main framebuffer fb and in the alternate field when the record mirrors, become the native word's
// (the meshes are left alone, as the native write leaves them). The native word is final (the native run is applied),
// and in order a byte ends with the run's last record that writes it, so the blocks equal the reference's in-order
// replay (tests/unit/vdp1_nx_reference.cpp ApplyFbWrites); records of one word write the same values, atomically.
// Bindings (vdp1_bindings.hpp kFbWriteNxBindings): FbWrite records (3), FBRAM (19, read), N x surface (24). Push
// constants: FbWriteNxParams (scale = N, 2 to 4).

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

[[vk::push_constant]] FbWriteNxParams g_params;

StructuredBuffer<FbWrite> g_fbWrites : register(t3);
RWByteAddressBuffer g_fbram : register(u3);
RWByteAddressBuffer g_fbramNx : register(u8);

#include "vdp1_fbram_nx.hlsli"

// The native word (the native passes have finished writing FBRAM)
uint NativeWord(uint slice, uint word) {
    const uint address = Vdp1FbramByteAddress(slice, word);
    return ByteSwap16(g_fbram.Load(address & ~3u) >> ((address & 2u) * 8u));
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint scale = g_params.scale;
    uint records, stride;
    g_fbWrites.GetDimensions(records, stride);
    const uint index = g_params.run.offset + id.x;
    if (id.x >= g_params.run.count || index < g_params.run.offset || index >= records || scale < 2 ||
        scale > kMaxNxScale) {
        return;
    }
    const FbWrite w = g_fbWrites[index];
    const uint word = Vdp1FbWriteWord(w.address);
    const uint fb = Vdp1FbWriteFb(w.address);
    const uint mask = w.mask & 0xFFFF;
    if (mask == 0) {
        return;
    }
    NxWriteBlock(Vdp1MainSlice(fb), word, NativeWord(Vdp1MainSlice(fb), word), mask, scale, false);
    if (Vdp1FbWriteMirrorAlt(w.address)) {
        NxWriteBlock(Vdp1AltSlice(fb), word, NativeWord(Vdp1AltSlice(fb), word), mask, scale, false);
    }
}
