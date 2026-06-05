#include "../include/common.hlsli"

Texture2D<float> gDepth : register(t0);
SamplerState gLinearClamp : register(s0);

FullscreenVertexOutput VSMain(uint vertex_id : SV_VertexID)
{
    return FullscreenTriangleVertex(vertex_id);
}

float4 PSMain(FullscreenVertexOutput input) : SV_Target0
{
    const float depth = gDepth.SampleLevel(gLinearClamp, input.uv, 0);
    return float4(depth, depth, depth, 1.0);
}
