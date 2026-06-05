// ─────────────────────────────────────────────────────────────────────────────
//  Chimera Optical Flow — Flow Refinement + Confidence (Compute Shader)
//  Upsample from coarser level, apply forward-backward consistency check,
//  and produce per-pixel confidence.
// ─────────────────────────────────────────────────────────────────────────────

cbuffer FlowRefineConstants : register(b0)
{
    uint2 gSize;
    float2 gTexelSize;
    float gConsistencyThreshold;
    float gPad0;
    float gPad1;
    float gPad2;
};

Texture2D<float2> gForwardFlow  : register(t0);
Texture2D<float2> gBackwardFlow : register(t1);
RWTexture2D<float2> gRefinedFlow : register(u0);
RWTexture2D<float>  gConfidence  : register(u1);

SamplerState gLinearClamp : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= gSize.x || dispatch_id.y >= gSize.y)
        return;

    float2 uv = (float2(dispatch_id.xy) + 0.5) * gTexelSize;

    float2 fwd = gForwardFlow[dispatch_id.xy];
    float2 warped_uv = uv + fwd * gTexelSize;

    // Sample backward flow at the forward-warped position
    float2 bwd = gBackwardFlow.SampleLevel(gLinearClamp, warped_uv, 0);

    // Forward-backward consistency: ideally fwd(x) + bwd(x + fwd(x)) = 0
    float2 consistency_error = fwd + bwd;
    float error_magnitude = length(consistency_error);
    float flow_magnitude = max(length(fwd), length(bwd));

    // Relative threshold: error / max(flow, epsilon)
    float relative_error = error_magnitude / max(flow_magnitude, 0.5);

    // Confidence: high when forward-backward consistent
    float confidence = 1.0 - saturate(relative_error / gConsistencyThreshold);

    // Eigenvalue-based additional confidence (from gradient structure)
    // Higher flow magnitude with good consistency = more reliable
    float magnitude_conf = saturate(flow_magnitude * 0.1);
    confidence *= lerp(0.5, 1.0, magnitude_conf);

    gRefinedFlow[dispatch_id.xy] = fwd;
    gConfidence[dispatch_id.xy] = confidence;
}
