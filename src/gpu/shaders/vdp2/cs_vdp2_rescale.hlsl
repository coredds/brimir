// Brimir: one-shot nearest rescale of the VDP2 composite when the internal resolution changes (Vdp2VulkanBackend::
// SetScale, recorded by the next Record before anything else writes the new composite). The old composite holds a
// width x height native frame at srcScale (pitch srcScale * width), the new one gets it at dstScale (pitch dstScale *
// width): N x pixel X of native pixel x = X / dstScale reads x * srcScale + (X % dstScale) * srcScale / dstScale (and
// the same for rows), so the sample points (dstScale * x, dstScale * y) keep exactly the old sample points and a woven
// field survives the change. One thread per new composite pixel.
// Bindings (vdp2_bindings.hpp kRescaleBindings): t15 -> binding 15, u4 -> binding 20.

struct RescaleParams {
    uint width;    // native frame size the old composite holds
    uint height;
    uint srcScale; // internal resolution of the old composite
    uint dstScale; // ... and of the new one
};

[[vk::push_constant]] RescaleParams g_rescaleParams;

StructuredBuffer<uint> g_rescaleSource : register(t15);
RWStructuredBuffer<uint> g_compositeOut : register(u4);

[numthreads(32, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const RescaleParams p = g_rescaleParams;
    const uint dstWidth = p.width * p.dstScale;
    if (id.x >= dstWidth || id.y >= p.height * p.dstScale) {
        return;
    }
    const uint sx = (id.x / p.dstScale) * p.srcScale + (id.x % p.dstScale) * p.srcScale / p.dstScale;
    const uint sy = (id.y / p.dstScale) * p.srcScale + (id.y % p.dstScale) * p.srcScale / p.dstScale;
    g_compositeOut[id.y * dstWidth + id.x] = g_rescaleSource[sy * (p.width * p.srcScale) + sx];
}
