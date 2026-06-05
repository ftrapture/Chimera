// ─────────────────────────────────────────────────────────────────────────────
//  Chimera TSR — RCAS (Robust Contrast-Adaptive Sharpening)
//  Based on AMD FidelityFX CAS / RCAS algorithm.
//  Edge-aware: sharpens flat regions, preserves edges, no halo artifacts.
// ─────────────────────────────────────────────────────────────────────────────
#include "../include/common.hlsli"
#include "../include/constants.hlsli"

Texture2D<float4> gCurrentHistory : register(t0);
SamplerState      gLinearClamp    : register(s0);

FullscreenVertexOutput VSMain(uint vertex_id : SV_VertexID)
{
    return FullscreenTriangleVertex(vertex_id);
}

float4 PSMain(FullscreenVertexOutput input) : SV_Target0
{
    const float2 uv = input.uv;
    const float2 ts = gOutputTexelSize;

    // 5-tap cross pattern (center + NSEW)
    const float3 center = gCurrentHistory.SampleLevel(gLinearClamp, uv, 0).rgb;
    const float3 north  = gCurrentHistory.SampleLevel(gLinearClamp, uv + float2(0.0, -ts.y), 0).rgb;
    const float3 south  = gCurrentHistory.SampleLevel(gLinearClamp, uv + float2(0.0,  ts.y), 0).rgb;
    const float3 east   = gCurrentHistory.SampleLevel(gLinearClamp, uv + float2( ts.x, 0.0), 0).rgb;
    const float3 west   = gCurrentHistory.SampleLevel(gLinearClamp, uv + float2(-ts.x, 0.0), 0).rgb;

    // Per-channel min/max of the 5 samples
    float3 mn = min(min(north, south), min(east, west));
    mn = min(mn, center);
    float3 mx = max(max(north, south), max(east, west));
    mx = max(mx, center);

    // RCAS adaptive sharpening weight:
    // Higher contrast → lower sharpening (edge preservation)
    // Lower contrast  → higher sharpening (detail enhancement)
    float3 amp = saturate(min(mn, 2.0 - mx) / mx);

    // Apply user-configurable strength
    amp *= gSharpenStrength;

    // Weight for neighbors: negative = sharpen
    float3 w = -amp / (4.0 * amp + 1.0);

    // Weighted sum: center gets positive weight, neighbors get negative
    float3 result = center;
    result -= w * north;
    result -= w * south;
    result -= w * east;
    result -= w * west;
    result /= (1.0 - 4.0 * w);

    // Clamp to prevent negative values from extreme sharpening
    result = max(result, 0.0.xxx);

    return float4(result, 1.0);
}
