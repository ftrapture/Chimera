// ─────────────────────────────────────────────────────────────────────────────
//  Chimera FI — Bidirectional Motion-Compensated Warp (Compute Shader)
//  Warps frame_n forward and frame_n1 backward to the interpolation time t.
//  Uses scatter-free backward warping for stable results.
// ─────────────────────────────────────────────────────────────────────────────

cbuffer FiWarpConstants : register(b0)
{
    uint2 gSize;
    float2 gTexelSize;
    float gInterpolationT;  // 0.0 = frame_n, 1.0 = frame_n1, typically 0.5
    float gPad0;
    float gPad1;
    float gPad2;
};

Texture2D<float4> gFrameN         : register(t0);
Texture2D<float4> gFrameN1        : register(t1);
Texture2D<float2> gForwardFlow    : register(t2);
Texture2D<float2> gBackwardFlow   : register(t3);
Texture2D<float>  gFlowConfidence : register(t4);

RWTexture2D<float4> gWarpedFromN  : register(u0);
RWTexture2D<float4> gWarpedFromN1 : register(u1);
RWTexture2D<float>  gOcclusionMap : register(u2);

SamplerState gLinearClamp : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= gSize.x || dispatch_id.y >= gSize.y)
        return;

    float2 uv = (float2(dispatch_id.xy) + 0.5) * gTexelSize;
    float t = gInterpolationT;

    // Read flow at current pixel
    float2 fwd_flow = gForwardFlow.SampleLevel(gLinearClamp, uv, 0);
    float2 bwd_flow = gBackwardFlow.SampleLevel(gLinearClamp, uv, 0);
    float confidence = gFlowConfidence.SampleLevel(gLinearClamp, uv, 0);

    // ── Backward warp from frame N ──────────────────────────────────────
    // To sample frame_n at the interpolated time t:
    // We need to go backwards from interpolation point to frame N
    // motion = fwd_flow * t (fractional motion from N to interpolation point)
    float2 sample_uv_n = uv - fwd_flow * t * gTexelSize;
    float4 warped_n = gFrameN.SampleLevel(gLinearClamp, sample_uv_n, 0);

    // ── Backward warp from frame N+1 ────────────────────────────────────
    // motion = bwd_flow * (1 - t) (fractional motion from N+1 back to interpolation)
    float2 sample_uv_n1 = uv - bwd_flow * (1.0 - t) * gTexelSize;
    float4 warped_n1 = gFrameN1.SampleLevel(gLinearClamp, sample_uv_n1, 0);

    // ── Occlusion detection ─────────────────────────────────────────────
    // Forward-backward consistency at warped positions
    float2 fwd_at_n  = gForwardFlow.SampleLevel(gLinearClamp, sample_uv_n, 0);
    float2 bwd_at_n1 = gBackwardFlow.SampleLevel(gLinearClamp, sample_uv_n1, 0);

    float consistency_n  = length(fwd_at_n + bwd_flow) / max(length(fwd_at_n) + length(bwd_flow), 0.5);
    float consistency_n1 = length(bwd_at_n1 + fwd_flow) / max(length(bwd_at_n1) + length(fwd_flow), 0.5);

    // Out-of-bounds check
    float oob_n  = (any(sample_uv_n < 0.0.xx)  || any(sample_uv_n > 1.0.xx))  ? 1.0 : 0.0;
    float oob_n1 = (any(sample_uv_n1 < 0.0.xx) || any(sample_uv_n1 > 1.0.xx)) ? 1.0 : 0.0;

    // Occlusion: 0 = both visible, 0.5 = one occluded, 1.0 = both occluded
    float occ_n  = saturate(consistency_n * 2.0 + oob_n);
    float occ_n1 = saturate(consistency_n1 * 2.0 + oob_n1);
    float occlusion = (occ_n + occ_n1) * 0.5;

    // Reduce confidence for low-flow-confidence pixels
    occlusion = max(occlusion, 1.0 - confidence);

    gWarpedFromN[dispatch_id.xy] = warped_n;
    gWarpedFromN1[dispatch_id.xy] = warped_n1;
    gOcclusionMap[dispatch_id.xy] = occlusion;
}
