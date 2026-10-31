// Frame output pass (gpu/frame_presenter.cpp): native Ymir XBGR8888 frame -> R8G8B8A8 output image, with the
// bridge's overscan crop and rotation (CoreWrapper::OnFrameComplete). One thread per output pixel; the output image is
// exactly the output size (the frontend samples the whole image as the frame).
// Bindings (tools/shaders/build_shaders.cmake shifts t by 0, u by 16): t0 -> binding 0, u0 -> binding 16.

// At internal resolution N (device frames) every field is N x the native value: the host scales them (the crop guard
// and the crop / 2 rounding stay native), the shader is unchanged.
struct Params {
    uint srcWidth;  // source frame width (= pitch)
    uint cropLeft;  // first source column and row of the cropped frame
    uint cropTop;
    uint cropWidth; // cropped (pre-rotation) frame width
    uint outWidth;  // output (post-rotation) size
    uint outHeight;
    uint rotation;  // 0, 90 (clockwise), 180, 270 (counter-clockwise)
};

[[vk::push_constant]] Params gParams;

StructuredBuffer<uint> gSource : register(t0); // 0xAABBGGRR = bytes R, G, B, A
// An R32_UINT view of the R8G8B8A8_UNORM output image: the packed word is stored unchanged, so the RGB bytes are the
// source's exactly (no float -> UNORM rounding).
[[vk::image_format("r32ui")]] RWTexture2D<uint> gOutput : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gParams.outWidth || id.y >= gParams.outHeight) {
        return;
    }
    const uint x = id.x;
    const uint y = id.y;

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
    gOutput[id.xy] = pixel | 0xFF000000; // opaque alpha
}
