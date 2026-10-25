// Declares every VDP2 parameter block the way the VDP2 shaders use it, so the shader build reflects their layouts
// into generated/cs_vdp2_layout_probe.layout.h (tests/unit/test_gpu_vdp2_params.cpp checks the host structs in
// src/gpu/vdp2/vdp2_hw_params.hpp against it). Never dispatched.
// Bindings (tools/shaders/build_shaders.cmake shifts t by 0, u by 16): t0-t3 -> bindings 0-3, u0 -> binding 16.

#include "vdp2_common_params.hlsli"
#include "vdp2_compose_params.hlsli"
#include "vdp2_nbg_line.hlsli"
#include "vdp2_render_params_layer.hlsli"
#include "vdp2_render_params_rotation.hlsli"

[[vk::push_constant]] CommonRenderParams g_commonParams;

StructuredBuffer<LayerRenderParams> g_layerRenderParams : register(t0);
StructuredBuffer<RotParamBase> g_rotParamBases : register(t1);
StructuredBuffer<ComposeParams> g_composeParams : register(t2);
StructuredBuffer<NBGLine> g_nbgLines : register(t3);
RWStructuredBuffer<uint> g_out : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    // Touch every block so none is optimised away.
    g_out[0] = g_commonParams.startY + g_layerRenderParams[0].specialFunctionCodes + g_rotParamBases[0].KA +
               g_composeParams[0].colorCalcEnable + g_nbgLines[0].mosaicSrcLine;
}
