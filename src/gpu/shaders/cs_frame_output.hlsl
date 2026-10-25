// Frame output pass (gpu/frame_presenter.cpp): native Ymir XBGR8888 frame -> R8G8B8A8 output image, with the
// bridge's overscan crop and rotation (CoreWrapper::OnFrameComplete). One thread per output pixel.
// Bindings (tools/shaders/build_shaders.cmake shifts t by 0, u by 16): t0 -> binding 0, u0 -> binding 16.

struct Params {
    uint srcWidth;  // native frame width (= pitch)
    uint cropLeft;  // first native column and row of the cropped frame
    uint cropTop;
    uint cropWidth; // cropped (pre-rotation) frame width
    uint outWidth;  // output (post-rotation) size
    uint outHeight;
    uint rotation;  // 0, 90 (clockwise), 180, 270 (counter-clockwise)
};

[[vk::push_constant]] Params gParams;

StructuredBuffer<uint> gSource : register(t0); // 0xAABBGGRR
[[vk::image_format("rgba8")]] RWTexture2D<float4> gOutput : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint x = id.x;
    const uint y = id.y;
    if (x >= gParams.outWidth || y >= gParams.outHeight) {
        return;
    }

    // Output pixel -> cropped-frame pixel, the inverse of the bridge's rotation loops.
    uint sx = x;
    uint sy = y;
    if (gParams.rotation == 90) {
        sx = y;
        sy = gParams.outWidth - 1 - x;
    } else if (gParams.rotation == 180) {
        sx = gParams.outWidth - 1 - x;
        sy = gParams.outHeight - 1 - y;
    } else if (gParams.rotation == 270) {
        sx = gParams.cropWidth - 1 - y;
        sy = x;
    }

    const uint pixel = gSource[(sy + gParams.cropTop) * gParams.srcWidth + sx + gParams.cropLeft];
    const float3 rgb = float3(pixel & 0xFF, (pixel >> 8) & 0xFF, (pixel >> 16) & 0xFF);
    gOutput[uint2(x, y)] = float4(rgb / 255.0, 1.0);
}
