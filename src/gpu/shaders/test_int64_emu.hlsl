// Evaluates the int64_emu.hlsli helpers over an input buffer (tests/unit/test_gpu_vdp2_params.cpp compares the
// results with the C++ mirror, src/gpu/vdp2/int64_emu.hpp).
// Bindings (tools/shaders/build_shaders.cmake shifts t by 0, u by 16): t0 -> binding 0, u0 -> binding 16.

#include "int64_emu.hlsli"

struct Int64EmuCase {
    int a0, b0, a1, b1, a2, b2; // RotDotShr10 operands
    int k, scr, p;              // RotCoordShr operands
};

struct Int64EmuResult {
    int dot;
    uint coord;
};

StructuredBuffer<Int64EmuCase> gCases : register(t0);
RWStructuredBuffer<Int64EmuResult> gResults : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint count;
    uint stride;
    gResults.GetDimensions(count, stride);
    if (id.x >= count) {
        return;
    }
    const Int64EmuCase c = gCases[id.x];
    Int64EmuResult r;
    r.dot = RotDotShr10(c.a0, c.b0, c.a1, c.b1, c.a2, c.b2);
    r.coord = RotCoordShr(c.k, c.scr, c.p);
    gResults[id.x] = r;
}
