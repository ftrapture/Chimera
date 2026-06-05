// ─── Fullscreen triangle ────────────────────────────────────────────────────
struct FullscreenVertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

FullscreenVertexOutput FullscreenTriangleVertex(uint vertex_id)
{
    FullscreenVertexOutput output;
    output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// ─── YCoCg conversions (better perceptual clamping than RGB) ────────────────
float3 RGBToYCoCg(float3 rgb)
{
    float y  =  0.25 * rgb.r + 0.50 * rgb.g + 0.25 * rgb.b;
    float co =  0.50 * rgb.r                 - 0.50 * rgb.b;
    float cg = -0.25 * rgb.r + 0.50 * rgb.g - 0.25 * rgb.b;
    return float3(y, co, cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
    float y  = ycocg.x;
    float co = ycocg.y;
    float cg = ycocg.z;
    float r = y + co - cg;
    float g = y + cg;
    float b = y - co - cg;
    return float3(r, g, b);
}

float Luminance(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

// ─── Catmull-Rom 5-tap bicubic filter (4 bilinear taps via weight folding) ──
float4 SampleCatmullRom(Texture2D<float4> tex, SamplerState samp, float2 uv, float2 texel_size)
{
    float2 position = uv / texel_size;
    float2 center   = floor(position - 0.5) + 0.5;
    float2 f        = position - center;
    float2 f2       = f * f;
    float2 f3       = f2 * f;

    // Catmull-Rom weights
    float2 w0 = f2 - 0.5 * (f3 + f);
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    float2 w3 = 0.5 * (f3 - f2);

    // Fold into 4 bilinear taps
    float2 w12  = w1 + w2;
    float2 tc12 = (center + w2 / w12) * texel_size;
    float2 tc0  = (center - 1.0) * texel_size;
    float2 tc3  = (center + 2.0) * texel_size;

    float4 result =
        (w12.x * w0.y)  * tex.SampleLevel(samp, float2(tc12.x, tc0.y),  0) +
        (w0.x  * w12.y) * tex.SampleLevel(samp, float2(tc0.x,  tc12.y), 0) +
        (w12.x * w12.y) * tex.SampleLevel(samp, float2(tc12.x, tc12.y), 0) +
        (w3.x  * w12.y) * tex.SampleLevel(samp, float2(tc3.x,  tc12.y), 0) +
        (w12.x * w3.y)  * tex.SampleLevel(samp, float2(tc12.x, tc3.y),  0);

    return max(result, 0.0);
}
