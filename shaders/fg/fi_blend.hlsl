// ─────────────────────────────────────────────────────────────────────────────
//  Chimera FI — Adaptive Blend + Inpaint (Compute Shader)
//  Blends two warped frames based on occlusion map and confidence,
//  with integrated spiral-search inpainting for remaining holes.
// ─────────────────────────────────────────────────────────────────────────────

cbuffer FiBlendConstants : register(b0)
{
    uint2 gSize;
    float2 gTexelSize;
    float gInterpolationT;
    float gPad0;
    float gPad1;
    float gPad2;
};

Texture2D<float4> gWarpedFromN     : register(t0);
Texture2D<float4> gWarpedFromN1    : register(t1);
Texture2D<float>  gOcclusionMap    : register(t2);
Texture2D<float4> gFrameN          : register(t3);
Texture2D<float4> gFrameN1         : register(t4);
Texture2D<float>  gFlowConfidence  : register(t5);

RWTexture2D<float4> gOutput : register(u0);

SamplerState gLinearClamp : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= gSize.x || dispatch_id.y >= gSize.y)
        return;

    float2 uv = (float2(dispatch_id.xy) + 0.5) * gTexelSize;
    float t = gInterpolationT;

    float4 warped_n  = gWarpedFromN.SampleLevel(gLinearClamp, uv, 0);
    float4 warped_n1 = gWarpedFromN1.SampleLevel(gLinearClamp, uv, 0);
    float  occlusion = gOcclusionMap.SampleLevel(gLinearClamp, uv, 0);
    float  confidence = gFlowConfidence.SampleLevel(gLinearClamp, uv, 0);

    // ── Adaptive blending ───────────────────────────────────────────────
    float4 blended;

    if (occlusion < 0.3)
    {
        // Both visible: smooth blend weighted by interpolation factor
        float weight_n1 = t;  // At t=0.5, equal blend
        blended = lerp(warped_n, warped_n1, weight_n1);
    }
    else if (occlusion < 0.7)
    {
        // Partial occlusion: favor the more confident warp
        float weight_n1 = t * (1.0 + (occlusion - 0.3) * 1.5);
        weight_n1 = saturate(weight_n1);
        blended = lerp(warped_n, warped_n1, weight_n1);

        // Reduce intensity to signal uncertainty
        blended.rgb *= lerp(1.0, 0.95, occlusion);
    }
    else
    {
        // Severe occlusion: fall back to nearest real frame
        // with simple inpainting from neighborhood
        float4 fallback_n  = gFrameN.SampleLevel(gLinearClamp, uv, 0);
        float4 fallback_n1 = gFrameN1.SampleLevel(gLinearClamp, uv, 0);
        blended = lerp(fallback_n, fallback_n1, t);

        // Spiral neighbor search: find nearest non-occluded pixel
        float best_occ = occlusion;
        float4 best_color = blended;

        [unroll]
        for (int radius = 1; radius <= 4; ++radius)
        {
            [unroll]
            for (int dy = -1; dy <= 1; ++dy)
            {
                [unroll]
                for (int dx = -1; dx <= 1; ++dx)
                {
                    if (dx == 0 && dy == 0) continue;
                    float2 neighbor_uv = uv + float2(dx * radius, dy * radius) * gTexelSize;
                    float neighbor_occ = gOcclusionMap.SampleLevel(gLinearClamp, neighbor_uv, 0);

                    if (neighbor_occ < best_occ)
                    {
                        best_occ = neighbor_occ;
                        float4 nw_n  = gWarpedFromN.SampleLevel(gLinearClamp, neighbor_uv, 0);
                        float4 nw_n1 = gWarpedFromN1.SampleLevel(gLinearClamp, neighbor_uv, 0);
                        best_color = lerp(nw_n, nw_n1, t);
                    }
                }
            }
        }

        blended = lerp(best_color, blended, saturate(occlusion * 0.5));
    }

    // ── Edge softening at warp seams ────────────────────────────────────
    // Detect gradient discontinuities between neighbors
    float4 left   = gWarpedFromN.SampleLevel(gLinearClamp, uv + float2(-gTexelSize.x, 0), 0);
    float4 right  = gWarpedFromN.SampleLevel(gLinearClamp, uv + float2( gTexelSize.x, 0), 0);
    float gradient_disc = length(left.rgb - right.rgb);
    float seam_strength = saturate(gradient_disc * 4.0 - 1.0) * 0.15;

    // Slight blur at seams to reduce visible edges
    if (seam_strength > 0.01)
    {
        float4 up    = gWarpedFromN.SampleLevel(gLinearClamp, uv + float2(0, -gTexelSize.y), 0);
        float4 down  = gWarpedFromN.SampleLevel(gLinearClamp, uv + float2(0,  gTexelSize.y), 0);
        float4 blur_avg = (left + right + up + down) * 0.25;
        blended = lerp(blended, lerp(blended, blur_avg, 0.3), seam_strength);
    }

    gOutput[dispatch_id.xy] = float4(max(blended.rgb, 0.0.xxx), 1.0);
}
