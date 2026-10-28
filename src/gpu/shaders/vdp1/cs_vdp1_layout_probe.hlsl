// Brimir: layout probe of the VDP1 GPU structs. It declares every VDP1 buffer and push-constant struct, so
// tools/shaders/build_shaders.cmake reflects their std430 layouts into generated/vdp1_layout_probe.layout.h, which
// tests/unit/test_gpu_vdp1_params.cpp compares with the host structs of src/gpu/vdp1/vdp1_hw_params.hpp. It also
// evaluates every accessor of vdp1_defs.hlsli, so that test can run them against the C++ copies on random words.
//
// Thread i (i < batch.spanCount) reads element i of the command, span, fragment, FbWrite, VRAM, bucket, FBRAM and scan
// sum buffers and writes kVdp1ProbeWords words of results into g_unitCounts at i * kVdp1ProbeWords: the words of the
// structs, the accessor results (see the table in the test, ProbeExpected), and the push-constant accessors.
// Bindings (src/gpu/vdp1/vdp1_bindings.hpp; t -> binding 0+n, u -> binding 16+n): all nine VDP1 buffers.
// Push constants: ProbePush, the three push-constant structs one after the other.

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

static const uint kVdp1ProbeWords = 94;

struct ProbePush {
    BatchParams batch;
    EraseParams erase;
    FbWriteParams fbWrite;
};
[[vk::push_constant]] ProbePush g_probe;

StructuredBuffer<CommandParams> g_commands : register(t0);
StructuredBuffer<Span> g_spans : register(t1);
ByteAddressBuffer g_vram : register(t2);
StructuredBuffer<FbWrite> g_fbWrites : register(t3);
RWStructuredBuffer<Fragment> g_fragments : register(u0);
RWStructuredBuffer<uint> g_unitCounts : register(u1);
RWStructuredBuffer<uint> g_bucket : register(u2);
RWByteAddressBuffer g_fbram : register(u3);
RWStructuredBuffer<uint> g_scanSums : register(u4);

#define EMIT(value) g_unitCounts[o + k++] = (value)

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_probe.batch.spanCount) {
        return;
    }
    const uint i = id.x;
    const uint o = i * kVdp1ProbeWords;
    uint k = 0;

    const CommandParams c = g_commands[i];
    const Span s = g_spans[i];
    const Fragment f = g_fragments[i];
    const FbWrite w = g_fbWrites[i];
    const uint df = c.drawFlags;
    const uint rf = c.rasterFlags;

    // 0-9: raw words
    for (uint v = 0; v < 8; ++v) {
        EMIT(c.vertex[v]);
    }
    EMIT(c.gouraud[0]);
    EMIT(c.gouraud[1]);
    // 10-24: drawFlags
    EMIT(Vdp1CmdColorCalc(df));
    EMIT(Vdp1CmdGouraud(df));
    EMIT(Vdp1CmdColorMode(df));
    EMIT(Vdp1CmdTransparentPixelDisable(df));
    EMIT(Vdp1CmdEndCodeDisable(df));
    EMIT(Vdp1CmdMesh(df));
    EMIT(Vdp1CmdClippingMode(df));
    EMIT(Vdp1CmdUserClip(df));
    EMIT(Vdp1CmdHighSpeedShrink(df));
    EMIT(Vdp1CmdMsbOn(df));
    EMIT(Vdp1CmdTextured(df));
    EMIT(Vdp1CmdAntiAlias(df));
    EMIT(Vdp1CmdFlipH(df));
    EMIT(Vdp1CmdFlipV(df));
    EMIT(Vdp1CmdType(df));
    // 25-33: rasterFlags
    EMIT(Vdp1RasPixel8(rf));
    EMIT(Vdp1RasFbSizeH(rf));
    EMIT(Vdp1RasDblInterlace(rf));
    EMIT(Vdp1RasDblInterlaceDrawLine(rf));
    EMIT(Vdp1RasEvenOddCoordSelect(rf));
    EMIT(Vdp1RasDoubleDensity(rf));
    EMIT(Vdp1RasDeinterlace(rf));
    EMIT(Vdp1RasTransparentMeshes(rf));
    EMIT(Vdp1RasDoubleV(rf));
    // 34-41: halves of the packed words
    EMIT(Vdp1Lo16(c.charSize));
    EMIT(Vdp1Hi16(c.charSize));
    EMIT(Vdp1Lo16(c.sysClip));
    EMIT(Vdp1Hi16(c.sysClip));
    EMIT(Vdp1Lo16(c.userClip0));
    EMIT(Vdp1Hi16(c.userClip0));
    EMIT(Vdp1Lo16(c.userClip1));
    EMIT(Vdp1Hi16(c.userClip1));
    // 42-45: raw words
    EMIT(c.color);
    EMIT(c.colorBank);
    EMIT(c.charAddr);
    EMIT(c.cmdAddress);
    // 46-55: span
    EMIT(s.x0);
    EMIT(s.y0);
    EMIT(s.x1);
    EMIT(s.y1);
    EMIT(s.cmdIndex);
    EMIT(s.fragBase);
    EMIT(Vdp1SpanQuadStep(s.stepTexV));
    EMIT(Vdp1SpanTexV(s.stepTexV));
    EMIT(Vdp1Lo16(s.gouraudLR));
    EMIT(Vdp1Hi16(s.gouraudLR));
    // 56-67: fragment
    EMIT(Vdp1FragWord(f.target));
    EMIT(Vdp1FragAlt(f.target));
    EMIT(Vdp1FragOddByte(f.target));
    EMIT(Vdp1FragColorCalc(f.target));
    EMIT(Vdp1FragMesh(f.target));
    EMIT(Vdp1FragMsbOn(f.target));
    EMIT(Vdp1FragPixel8(f.target));
    EMIT(Vdp1FragTransparentMeshes(f.target));
    EMIT(Vdp1FragLive(f.target));
    EMIT(Vdp1FragUnit(f.target));
    EMIT(f.color);
    EMIT(Vdp1MakeFragmentTarget(Vdp1FragWord(f.target), Vdp1FragAlt(f.target), Vdp1FragOddByte(f.target),
                                Vdp1FragColorCalc(f.target), Vdp1FragMesh(f.target), Vdp1FragMsbOn(f.target),
                                Vdp1FragPixel8(f.target), Vdp1FragTransparentMeshes(f.target)));
    // 68-72: FbWrite
    EMIT(Vdp1FbWriteWord(w.address));
    EMIT(Vdp1FbWriteFb(w.address));
    EMIT(Vdp1FbWriteMirrorAlt(w.address));
    EMIT(w.value);
    EMIT(w.mask);
    // 73-75: erase flags
    EMIT(Vdp1EraseFb(g_probe.erase.flags));
    EMIT(Vdp1EraseMirrorAlt(g_probe.erase.flags));
    EMIT(Vdp1EraseClearMesh(g_probe.erase.flags));
    // 76-80: FBRAM addressing
    const uint fb = Vdp1FbWriteFb(w.address);
    EMIT(Vdp1FbramByteAddress(c.cmdAddress, Vdp1FragWord(f.target)));
    EMIT(Vdp1FbWordShift(Vdp1FragWord(f.target)));
    EMIT(Vdp1MainSlice(fb));
    EMIT(Vdp1AltSlice(fb));
    EMIT(Vdp1MeshSlice(Vdp1FragAlt(f.target), fb));
    // 81-84: the other buffers, element i (masked to the buffer)
    EMIT(g_vram.Load((i * 4) & (kVRAMBufferSize - 4)));
    EMIT(g_bucket[i]);
    EMIT(g_fbram.Load((i * 4) & (kFbramBufferSize - 4)));
    EMIT(g_scanSums[i]);
    // 85-93: the pack functions on the unpacked flags (their round trips)
    EMIT(Vdp1MakeDrawFlags(df & kCmdpmodMask, Vdp1CmdType(df) | (Vdp1CmdFlipH(df) ? 16u : 0u) | (Vdp1CmdFlipV(df) ? 32u : 0u),
                           Vdp1CmdTextured(df), Vdp1CmdAntiAlias(df)));
    EMIT(Vdp1MakeRasterFlags(Vdp1RasPixel8(rf), Vdp1RasFbSizeH(rf), Vdp1RasDblInterlace(rf),
                             Vdp1RasDblInterlaceDrawLine(rf), Vdp1RasEvenOddCoordSelect(rf),
                             Vdp1RasDoubleDensity(rf), Vdp1RasDeinterlace(rf), Vdp1RasTransparentMeshes(rf),
                             Vdp1RasDoubleV(rf)));
    EMIT(Vdp1MakeFbWriteAddress(Vdp1FbWriteWord(w.address), Vdp1FbWriteFb(w.address), Vdp1FbWriteMirrorAlt(w.address)));
    EMIT(Vdp1MakeEraseFlags(Vdp1EraseFb(g_probe.erase.flags), Vdp1EraseMirrorAlt(g_probe.erase.flags),
                            Vdp1EraseClearMesh(g_probe.erase.flags)));
    EMIT(Vdp1Pack16(s.gouraudLR, s.stepTexV));
    EMIT(g_probe.erase.x1 + g_probe.erase.x3 + g_probe.erase.y1 + g_probe.erase.y3);
    EMIT(g_probe.erase.value ^ g_probe.erase.offsetShift ^ g_probe.erase.writeLimit);
    EMIT(g_probe.batch.spanOffset + g_probe.batch.cmdOffset + g_probe.batch.fragmentCount + g_probe.batch.drawFB +
         g_probe.batch.scale);
    EMIT(g_probe.fbWrite.offset + g_probe.fbWrite.count + w.pad);
}
