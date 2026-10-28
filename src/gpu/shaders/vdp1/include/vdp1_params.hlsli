// Brimir: VDP1 GPU structs, all uint (std430; no vec3, no bitfields). Brimir only.
// Host mirror: src/gpu/vdp1/vdp1_hw_params.hpp (Vdp1CommandParams, Vdp1Span, Vdp1Fragment, Vdp1FbWrite and the push
// constants; offsets are checked against the reflection of cs_vdp1_layout_probe.hlsl). The fields, flag bit positions and
// units are documented there; signed values (coordinates) are int32 stored as uint: use asint().
#ifndef BRIMIR_VDP1_PARAMS_HLSLI
#define BRIMIR_VDP1_PARAMS_HLSLI

// StructuredBuffer<CommandParams>, 80 bytes
struct CommandParams {
    uint vertex[8];
    uint gouraud[2];
    uint drawFlags;
    uint rasterFlags;
    uint color;
    uint colorBank;
    uint charAddr;
    uint charSize;
    uint sysClip;
    uint userClip0;
    uint userClip1;
    uint cmdAddress;
};

// StructuredBuffer<Span>, 32 bytes
struct Span {
    uint x0, y0, x1, y1;
    uint cmdIndex;
    uint fragBase;
    uint gouraudLR;
    uint stepTexV;
};

// RWStructuredBuffer<Fragment>, 8 bytes, indexed by the order key
struct Fragment {
    uint target;
    uint color;
};

// StructuredBuffer<FbWrite>, 16 bytes
struct FbWrite {
    uint address;
    uint value;
    uint mask;
    uint pad;
};

// Push constants of the raster, bucket, scatter and resolve passes
struct BatchParams {
    uint spanOffset;
    uint spanCount;
    uint cmdOffset;
    uint fragmentCount;
    uint drawFB;
    uint scale;
};

// Push constants of the erase pass
struct EraseParams {
    uint x1, x3;
    uint y1, y3;
    uint value;
    uint offsetShift;
    uint writeLimit;
    uint flags;
};

// Push constants of the CPU framebuffer write pass
struct FbWriteParams {
    uint offset;
    uint count;
};

#endif
