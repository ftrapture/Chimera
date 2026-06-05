// ─────────────────────────────────────────────────────────────────────────────
//  Chimera Optical Flow — Weighted Median Filter (Compute Shader)
//  3×3 confidence-weighted median filter on the final flow field.
//  Removes flow outliers while preserving motion edges.
// ─────────────────────────────────────────────────────────────────────────────

cbuffer FlowMedianConstants : register(b0)
{
    uint2 gSize;
    float2 gTexelSize;
};

Texture2D<float2> gInputFlow       : register(t0);
Texture2D<float>  gInputConfidence : register(t1);
RWTexture2D<float2> gOutputFlow    : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= gSize.x || dispatch_id.y >= gSize.y)
        return;

    // Gather 3×3 neighborhood
    float2 flows[9];
    float  weights[9];
    uint count = 0;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 coord = int2(dispatch_id.xy) + int2(x, y);
            coord = clamp(coord, int2(0, 0), int2(gSize) - int2(1, 1));

            flows[count] = gInputFlow[coord];
            weights[count] = gInputConfidence[coord];
            count++;
        }
    }

    // Weighted median via sorting by flow magnitude
    // Simple bubble sort for 9 elements (unrolled by compiler)
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        [unroll]
        for (uint j = i + 1; j < 9; ++j)
        {
            float mag_i = dot(flows[i], flows[i]);
            float mag_j = dot(flows[j], flows[j]);
            if (mag_i > mag_j)
            {
                float2 tmp_f = flows[i];
                flows[i] = flows[j];
                flows[j] = tmp_f;

                float tmp_w = weights[i];
                weights[i] = weights[j];
                weights[j] = tmp_w;
            }
        }
    }

    // Find weighted median
    float total_weight = 0.0;
    [unroll]
    for (uint k = 0; k < 9; ++k)
        total_weight += weights[k];

    float half_weight = total_weight * 0.5;
    float accumulated = 0.0;
    float2 median_flow = flows[4]; // fallback to center

    [unroll]
    for (uint m = 0; m < 9; ++m)
    {
        accumulated += weights[m];
        if (accumulated >= half_weight)
        {
            median_flow = flows[m];
            break;
        }
    }

    gOutputFlow[dispatch_id.xy] = median_flow;
}
