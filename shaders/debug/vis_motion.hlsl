#include "../include/common.hlsli"

Texture2D<float2> gMotion : register(t0);
SamplerState gLinearClamp : register(s0);

FullscreenVertexOutput VSMain(uint vertex_id : SV_VertexID)
{
    return FullscreenTriangleVertex(vertex_id);
}

float4 PSMain(FullscreenVertexOutput input) : SV_Target0
{
    const float2 motion = gMotion.SampleLevel(gLinearClamp, input.uv, 0);
    return float4(abs(motion.x), abs(motion.y), 0.0, 1.0);
}
