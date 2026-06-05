// ─────────────────────────────────────────────────────────────────────────────
//  Chimera TSR — Lanczos2 Upscale Pre-filter
//  Used when render scale < 0.6 for better initial reconstruction.
// ─────────────────────────────────────────────────────────────────────────────
#include "../include/common.hlsli"
#include "../include/constants.hlsli"

Texture2D<float4> gInputColor  : register(t0);
SamplerState      gLinearClamp : register(s0);

FullscreenVertexOutput VSMain(uint vertex_id : SV_VertexID)
{
    return FullscreenTriangleVertex(vertex_id);
}

static const float PI = 3.14159265358979323846;

float Lanczos2(float x)
{
    if (abs(x) < 0.0001)
        return 1.0;
    if (abs(x) >= 2.0)
        return 0.0;
    float px = PI * x;
    return (sin(px) / px) * (sin(px * 0.5) / (px * 0.5));
}

float4 PSMain(FullscreenVertexOutput input) : SV_Target0
{
    const float2 uv = input.uv;

    // Source texel position in input texture space
    float2 src_pos = uv * gInputSize;
    float2 src_center = floor(src_pos - 0.5) + 0.5;

    float4 result = 0.0;
    float  weight_sum = 0.0;

    // 4x4 Lanczos2 tap pattern
    [unroll]
    for (int y = -1; y <= 2; ++y)
    {
        [unroll]
        for (int x = -1; x <= 2; ++x)
        {
            float2 tap_pos = src_center + float2(x, y);
            float2 tap_uv = tap_pos * gInputTexelSize;

            float2 delta = src_pos - tap_pos;
            float w = Lanczos2(delta.x) * Lanczos2(delta.y);

            result += gInputColor.SampleLevel(gLinearClamp, tap_uv, 0) * w;
            weight_sum += w;
        }
    }

    result /= max(weight_sum, 0.0001);
    return float4(max(result.rgb, 0.0.xxx), 1.0);
}
