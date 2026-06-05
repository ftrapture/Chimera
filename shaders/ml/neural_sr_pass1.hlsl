cbuffer NeuralSrConstants : register(b0)
{
    float2 uInputSize;
    float2 uOutputSize;
    float2 uInputTexelSize;
    float2 uOutputTexelSize;
    uint uFrameIndex;
    uint uInputWidth;
    uint uInputHeight;
    uint uOutputWidth;
    uint uOutputHeight;
};

Texture2D<float4> gColor : register(t0);
Texture2D<float> gDepth : register(t1);
Texture2D<float2> gMotion : register(t2);
StructuredBuffer<float> gWeights : register(t3);

RWTexture2DArray<float4> gOutFeatures : register(u0);

// Convert RGB to YCoCg inside the shader
float3 RGBToYCoCg(float3 rgb)
{
    float y  =  0.25 * rgb.r + 0.50 * rgb.g + 0.25 * rgb.b;
    float co =  0.50 * rgb.r                 - 0.50 * rgb.b;
    float cg = -0.25 * rgb.r + 0.50 * rgb.g - 0.25 * rgb.b;
    return float3(y, co, cg);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= uInputWidth || dispatch_id.y >= uInputHeight)
    {
        return;
    }

    // Layer 1 Convolution (6 inputs -> 32 outputs, 3x3 kernel)
    // Inputs:
    // 0..2: YCoCg color
    // 3: Depth
    // 4..5: Motion vector
    float outputs[32];
    for (int co = 0; co < 32; ++co)
    {
        // Start with bias
        float val = gWeights[1728 + co];

        for (int ky = -1; ky <= 1; ++ky)
        {
            for (int kx = -1; kx <= 1; ++kx)
            {
                int2 coord = clamp(int2(dispatch_id.xy) + int2(kx, ky), 0, int2(uInputWidth - 1, uInputHeight - 1));

                float4 color_val = gColor[coord];
                float depth_val = gDepth[coord].r;
                float2 motion_val = gMotion[coord].rg;

                float3 ycocg = RGBToYCoCg(color_val.rgb);

                float inputs_vec[6];
                inputs_vec[0] = ycocg.x;
                inputs_vec[1] = ycocg.y;
                inputs_vec[2] = ycocg.z;
                inputs_vec[3] = depth_val;
                inputs_vec[4] = motion_val.x;
                inputs_vec[5] = motion_val.y;

                int weight_start = co * 6 * 9 + ((ky + 1) * 3 + (kx + 1)) * 6;
                for (int ci = 0; ci < 6; ++ci)
                {
                    val += inputs_vec[ci] * gWeights[weight_start + ci];
                }
            }
        }

        // LeakyReLU activation (slope = 0.1)
        outputs[co] = max(val, 0.1 * val);
    }

    // Pixel Shuffle (rearrange 32 channels at WxH to 8 channels at 2Wx2H)
    for (int dy = 0; dy < 2; ++dy)
    {
        for (int dx = 0; dx < 2; ++dx)
        {
            uint2 hr_coord = dispatch_id.xy * 2 + uint2(dx, dy);
            if (hr_coord.x >= uOutputWidth || hr_coord.y >= uOutputHeight)
            {
                continue;
            }

            // Slice 0 (channels 0..3)
            float4 slice0;
            for (int c = 0; c < 4; ++c)
            {
                int low_res_ch = (0 * 4 + c) * 4 + (dy * 2 + dx);
                slice0[c] = outputs[low_res_ch];
            }
            gOutFeatures[uint3(hr_coord, 0)] = slice0;

            // Slice 1 (channels 4..7)
            float4 slice1;
            for (int c = 0; c < 4; ++c)
            {
                int low_res_ch = (1 * 4 + c) * 4 + (dy * 2 + dx);
                slice1[c] = outputs[low_res_ch];
            }
            gOutFeatures[uint3(hr_coord, 1)] = slice1;
        }
    }
}
