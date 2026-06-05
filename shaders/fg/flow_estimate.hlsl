// ─────────────────────────────────────────────────────────────────────────────
//  Chimera Optical Flow — Lucas-Kanade Estimation (Compute Shader)
//  Per-pixel 5×5 Lucas-Kanade with Scharr gradient operator.
//  Iterative refinement: 3 iterations per pyramid level.
//  Uses shared memory for gradient caching within 8×8 thread groups.
// ─────────────────────────────────────────────────────────────────────────────

cbuffer FlowEstimateConstants : register(b0)
{
    uint2 gSize;
    float2 gTexelSize;
    uint gIterationIndex;
    uint gPyramidLevel;
    uint gPad0;
    uint gPad1;
};

Texture2D<float>  gCurrentLuma  : register(t0);
Texture2D<float>  gPreviousLuma : register(t1);
Texture2D<float2> gCoarserFlow  : register(t2);  // Flow from coarser level (or zero at coarsest)
RWTexture2D<float2> gOutputFlow : register(u0);

SamplerState gLinearClamp : register(s0);

// Scharr gradient operator (more rotationally symmetric than Sobel)
float SampleGradientX(Texture2D<float> tex, float2 uv, float2 ts)
{
    float tl = tex.SampleLevel(gLinearClamp, uv + float2(-ts.x, -ts.y), 0);
    float tr = tex.SampleLevel(gLinearClamp, uv + float2( ts.x, -ts.y), 0);
    float ml = tex.SampleLevel(gLinearClamp, uv + float2(-ts.x,  0.0),  0);
    float mr = tex.SampleLevel(gLinearClamp, uv + float2( ts.x,  0.0),  0);
    float bl = tex.SampleLevel(gLinearClamp, uv + float2(-ts.x,  ts.y), 0);
    float br = tex.SampleLevel(gLinearClamp, uv + float2( ts.x,  ts.y), 0);
    // Scharr: [-3, 0, 3; -10, 0, 10; -3, 0, 3] / 32
    return (-3.0 * tl + 3.0 * tr - 10.0 * ml + 10.0 * mr - 3.0 * bl + 3.0 * br) / 32.0;
}

float SampleGradientY(Texture2D<float> tex, float2 uv, float2 ts)
{
    float tl = tex.SampleLevel(gLinearClamp, uv + float2(-ts.x, -ts.y), 0);
    float tr = tex.SampleLevel(gLinearClamp, uv + float2( ts.x, -ts.y), 0);
    float ml = tex.SampleLevel(gLinearClamp, uv + float2( 0.0,  -ts.y), 0);
    float mr = tex.SampleLevel(gLinearClamp, uv + float2( 0.0,   ts.y), 0);
    float bl = tex.SampleLevel(gLinearClamp, uv + float2(-ts.x,  ts.y), 0);
    float br = tex.SampleLevel(gLinearClamp, uv + float2( ts.x,  ts.y), 0);
    // Scharr: [-3, -10, -3; 0, 0, 0; 3, 10, 3] / 32
    return (-3.0 * tl - 10.0 * ml - 3.0 * tr + 3.0 * bl + 10.0 * mr + 3.0 * br) / 32.0;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= gSize.x || dispatch_id.y >= gSize.y)
        return;

    float2 uv = (float2(dispatch_id.xy) + 0.5) * gTexelSize;

    // Get initial flow from coarser level (upsampled × 2)
    float2 initial_flow = 0.0.xx;
    if (gPyramidLevel > 0 || gIterationIndex > 0)
    {
        if (gIterationIndex == 0)
        {
            // First iteration at this level: bilinear upsample from coarser
            initial_flow = gCoarserFlow.SampleLevel(gLinearClamp, uv, 0) * 2.0;
        }
        else
        {
            // Subsequent iterations: refine previous estimate
            initial_flow = gOutputFlow[dispatch_id.xy];
        }
    }

    // Warp the previous image by current flow estimate
    float2 warped_uv = uv + initial_flow * gTexelSize;

    // Compute spatial gradients of current image
    float Ix = SampleGradientX(gCurrentLuma, uv, gTexelSize);
    float Iy = SampleGradientY(gCurrentLuma, uv, gTexelSize);

    // Temporal gradient: difference between current and warped-previous
    float Ic = gCurrentLuma.SampleLevel(gLinearClamp, uv, 0);
    float Ip = gPreviousLuma.SampleLevel(gLinearClamp, warped_uv, 0);
    float It = Ic - Ip;

    // 5×5 Lucas-Kanade: accumulate structure tensor over the window
    float sum_Ix2  = 0.0;
    float sum_Iy2  = 0.0;
    float sum_IxIy = 0.0;
    float sum_IxIt = 0.0;
    float sum_IyIt = 0.0;

    [unroll]
    for (int wy = -2; wy <= 2; ++wy)
    {
        [unroll]
        for (int wx = -2; wx <= 2; ++wx)
        {
            float2 offset_uv = uv + float2(wx, wy) * gTexelSize;
            float2 offset_warped = offset_uv + initial_flow * gTexelSize;

            float local_Ix = SampleGradientX(gCurrentLuma, offset_uv, gTexelSize);
            float local_Iy = SampleGradientY(gCurrentLuma, offset_uv, gTexelSize);
            float local_Ic = gCurrentLuma.SampleLevel(gLinearClamp, offset_uv, 0);
            float local_Ip = gPreviousLuma.SampleLevel(gLinearClamp, offset_warped, 0);
            float local_It = local_Ic - local_Ip;

            // Gaussian weighting (approximate)
            float dist2 = float(wx * wx + wy * wy);
            float w = exp(-dist2 * 0.18);

            sum_Ix2  += w * local_Ix * local_Ix;
            sum_Iy2  += w * local_Iy * local_Iy;
            sum_IxIy += w * local_Ix * local_Iy;
            sum_IxIt += w * local_Ix * local_It;
            sum_IyIt += w * local_Iy * local_It;
        }
    }

    // Solve 2×2 system: A * [du, dv]^T = -b
    // A = [[sum_Ix2, sum_IxIy], [sum_IxIy, sum_Iy2]]
    // b = [sum_IxIt, sum_IyIt]
    float det = sum_Ix2 * sum_Iy2 - sum_IxIy * sum_IxIy;
    float2 delta_flow = 0.0.xx;

    // Only solve if determinant is large enough (well-conditioned)
    if (abs(det) > 1e-6)
    {
        delta_flow.x = -(sum_Iy2  * sum_IxIt - sum_IxIy * sum_IyIt) / det;
        delta_flow.y = -(sum_Ix2  * sum_IyIt - sum_IxIy * sum_IxIt) / det;

        // Clamp per-iteration displacement to prevent divergence
        float max_disp = 4.0;
        delta_flow = clamp(delta_flow, -max_disp.xx, max_disp.xx);
    }

    gOutputFlow[dispatch_id.xy] = initial_flow + delta_flow;
}
