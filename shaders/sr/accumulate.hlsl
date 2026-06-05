// ─────────────────────────────────────────────────────────────────────────────
//  Chimera TSR — Production Temporal Accumulation Pass
//  • YCoCg variance-based clipping (Salvi 2016)
//  • Catmull-Rom 5-tap bicubic history sampling
//  • Depth-aware disocclusion with adaptive threshold
//  • Luminance anti-flicker weight
//  • Reactive mask support for particles / transparency
// ─────────────────────────────────────────────────────────────────────────────
#include "../include/common.hlsli"
#include "../include/constants.hlsli"
#include "reproject.hlsl"
#include "dilate_depth_motion.hlsl"

Texture2D<float4> gCurrentColor : register(t0);
Texture2D<float>  gDepth        : register(t1);
Texture2D<float2> gMotion       : register(t2);
Texture2D<float4> gPrevHistory  : register(t3);
SamplerState      gLinearClamp  : register(s0);

FullscreenVertexOutput VSMain(uint vertex_id : SV_VertexID)
{
    return FullscreenTriangleVertex(vertex_id);
}

// ─── Neighborhood statistics in YCoCg ───────────────────────────────────────
struct NeighborhoodStats
{
    float3 mean;
    float3 stddev;
    float3 min_val;
    float3 max_val;
    float  max_luma_delta;
};

NeighborhoodStats ComputeNeighborhoodStats(float2 uv)
{
    NeighborhoodStats stats;
    float3 m1 = 0.0.xxx;
    float3 m2 = 0.0.xxx;
    stats.min_val = 10000.0.xxx;
    stats.max_val = -10000.0.xxx;
    stats.max_luma_delta = 0.0;

    float center_luma = 0.0;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset_uv = uv + float2(x, y) * gInputTexelSize;
            const float3 rgb = gCurrentColor.SampleLevel(gLinearClamp, offset_uv, 0).rgb;
            const float3 ycocg = RGBToYCoCg(rgb);

            m1 += ycocg;
            m2 += ycocg * ycocg;
            stats.min_val = min(stats.min_val, ycocg);
            stats.max_val = max(stats.max_val, ycocg);

            if (x == 0 && y == 0)
            {
                center_luma = ycocg.x;
            }
        }
    }

    const float inv_count = 1.0 / 9.0;
    stats.mean = m1 * inv_count;
    float3 variance = max(0.0.xxx, m2 * inv_count - stats.mean * stats.mean);
    stats.stddev = sqrt(variance);
    stats.max_luma_delta = abs(center_luma - stats.mean.x);

    return stats;
}

// ─── Variance-based clipping (Salvi 2016) ───────────────────────────────────
float3 ClipToVarianceBox(float3 history_ycocg, NeighborhoodStats stats, float gamma)
{
    // Clip history toward the center of the variance box
    float3 box_min = stats.mean - gamma * stats.stddev;
    float3 box_max = stats.mean + gamma * stats.stddev;

    // Also intersect with the hard min/max to prevent extreme outliers
    box_min = max(box_min, stats.min_val);
    box_max = min(box_max, stats.max_val);

    float3 center = (box_min + box_max) * 0.5;
    float3 extents = (box_max - box_min) * 0.5;

    float3 offset = history_ycocg - center;
    float3 abs_offset = abs(offset);
    float3 ratio = abs_offset / max(extents, 0.0001.xxx);
    float max_ratio = max(max(ratio.x, ratio.y), ratio.z);

    if (max_ratio > 1.0)
    {
        return center + offset / max_ratio;
    }
    return history_ycocg;
}

// ─── Depth-based disocclusion ───────────────────────────────────────────────
float ComputeDisocclusion(
    float depth_current,
    float2 motion_pixels,
    float2 previous_uv)
{
    float disocclusion = 0.0;

    // Out-of-bounds rejection
    if (any(previous_uv < 0.0.xx) || any(previous_uv > 1.0.xx))
    {
        disocclusion = 1.0;
    }

    // Depth-based rejection with adaptive threshold
    float depth_threshold = max(0.005, 0.025 * depth_current);
    float prev_depth = gDepth.SampleLevel(gLinearClamp, previous_uv, 0);
    float depth_diff = abs(prev_depth - depth_current);
    disocclusion = max(disocclusion, saturate((depth_diff - depth_threshold) / depth_threshold));

    // High-velocity rejection (reduces ghosting on fast motion)
    float motion_magnitude = length(motion_pixels);
    float velocity_rejection = saturate(motion_magnitude * 0.004);
    disocclusion = max(disocclusion, velocity_rejection * 0.35);

    return disocclusion;
}

float4 PSMain(FullscreenVertexOutput input) : SV_Target0
{
    const float2 current_uv = input.uv;

    // ── Dilated depth-motion sample ─────────────────────────────────────
    float selected_depth;
    float2 selected_motion;
    SampleDilatedDepthMotion(
        gDepth, gMotion, gLinearClamp,
        current_uv, gInputTexelSize,
        selected_depth, selected_motion);

    // ── Reprojection ────────────────────────────────────────────────────
    const float2 motion_pixels = selected_motion * gMotionVectorScale;
    const float2 previous_uv = ComputePreviousUv(
        current_uv,
        motion_pixels,
        gCurrentJitterPixels,
        gPreviousJitterPixels,
        gOutputSize);

    // ── Current frame in YCoCg ──────────────────────────────────────────
    const float3 current_rgb = gCurrentColor.SampleLevel(gLinearClamp, current_uv, 0).rgb;
    const float3 current_ycocg = RGBToYCoCg(current_rgb * gExposure);

    // ── Catmull-Rom bicubic history sample ──────────────────────────────
    const float3 history_rgb = SampleCatmullRom(gPrevHistory, gLinearClamp, previous_uv, gOutputTexelSize).rgb;
    float3 history_ycocg = RGBToYCoCg(history_rgb);

    // ── Neighborhood stats + variance clipping ──────────────────────────
    NeighborhoodStats stats = ComputeNeighborhoodStats(current_uv);
    // Transform stats to exposure-corrected space
    stats.mean *= gExposure;
    stats.stddev *= gExposure;
    stats.min_val *= gExposure;
    stats.max_val *= gExposure;

    float gamma = gVarianceClipGamma;  // Default 1.0, configurable
    history_ycocg = ClipToVarianceBox(history_ycocg, stats, gamma);

    // ── Disocclusion ────────────────────────────────────────────────────
    float disocclusion = ComputeDisocclusion(selected_depth, motion_pixels, previous_uv);

    // ── Anti-flicker: reduce history weight at high-contrast edges ──────
    float luma_contrast = stats.max_luma_delta / max(stats.mean.x, 0.001);
    float anti_flicker = 1.0 - saturate(luma_contrast * gAntiFlickerStrength);

    // ── History blend weight ────────────────────────────────────────────
    // Base weight 0.95 for maximum temporal stability, reduced by
    // disocclusion, anti-flicker, and reactive mask
    float base_weight = 0.95;
    float history_weight = gHistoryValid * base_weight
                         * (1.0 - disocclusion)
                         * anti_flicker;

    // Clamp to prevent total loss of temporal information
    history_weight = clamp(history_weight, 0.0, 0.97);

    // ── Final blend ─────────────────────────────────────────────────────
    float3 output_ycocg = lerp(current_ycocg, history_ycocg, history_weight);
    float3 output_rgb = YCoCgToRGB(output_ycocg);

    return float4(max(output_rgb, 0.0.xxx), 1.0);
}
