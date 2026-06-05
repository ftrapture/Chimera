// ─────────────────────────────────────────────────────────────────────────────
//  Chimera Optical Flow — Luminance Pyramid Pre-filter (Compute Shader)
//  Converts RGB to grayscale luminance and builds a Gaussian pyramid via
//  separable 5-tap blur + 2× downsample for coarse-to-fine flow estimation.
// ─────────────────────────────────────────────────────────────────────────────

cbuffer FlowConstants : register(b0)
{
    uint2 gSrcSize;
    uint2 gDstSize;
    float2 gSrcTexelSize;
    float2 gDstTexelSize;
    uint gPyramidLevel;
    uint gPad0;
    uint gPad1;
    uint gPad2;
};

Texture2D<float4> gInputColor : register(t0);
RWTexture2D<float> gOutputLuma : register(u0);
SamplerState gLinearClamp : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= gDstSize.x || dispatch_id.y >= gDstSize.y)
        return;

    float2 uv = (float2(dispatch_id.xy) + 0.5) * gDstTexelSize;

    if (gPyramidLevel == 0)
    {
        // Level 0: convert RGB to luminance
        float3 rgb = gInputColor.SampleLevel(gLinearClamp, uv, 0).rgb;
        gOutputLuma[dispatch_id.xy] = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    }
    else
    {
        // Higher levels: 5-tap Gaussian blur of parent level + 2× downsample
        // The input texture here is the previous pyramid level (bound as SRV).
        // We sample at the corresponding 2× position with a 5-tap separable kernel.
        float2 src_uv = uv;
        float2 ts = gSrcTexelSize;

        // Separable Gaussian [1,4,6,4,1]/16
        float sum = 0.0;
        sum += gInputColor.SampleLevel(gLinearClamp, src_uv + float2(-2.0 * ts.x, 0), 0).r * (1.0 / 16.0);
        sum += gInputColor.SampleLevel(gLinearClamp, src_uv + float2(-1.0 * ts.x, 0), 0).r * (4.0 / 16.0);
        sum += gInputColor.SampleLevel(gLinearClamp, src_uv,                          0).r * (6.0 / 16.0);
        sum += gInputColor.SampleLevel(gLinearClamp, src_uv + float2( 1.0 * ts.x, 0), 0).r * (4.0 / 16.0);
        sum += gInputColor.SampleLevel(gLinearClamp, src_uv + float2( 2.0 * ts.x, 0), 0).r * (1.0 / 16.0);

        gOutputLuma[dispatch_id.xy] = sum;
    }
}
