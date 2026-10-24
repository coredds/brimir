// Fills a buffer with element i = i * 3 + 1. Used to test the shader pipeline and the compute helpers.
// Bindings (tools/shaders/build_shaders.cmake shifts u registers by 16): u0 -> set 0, binding 16.

RWStructuredBuffer<uint> gOutput : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint count;
    uint stride;
    gOutput.GetDimensions(count, stride);
    if (id.x < count) {
        gOutput[id.x] = id.x * 3 + 1;
    }
}
