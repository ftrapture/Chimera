void SampleDilatedDepthMotion(
    Texture2D<float> depth_texture,
    Texture2D<float2> motion_texture,
    SamplerState linear_clamp,
    float2 uv,
    float2 input_texel_size,
    out float selected_depth,
    out float2 selected_motion)
{
    selected_depth = 1.0;
    selected_motion = 0.0.xx;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 sample_uv = uv + float2(x, y) * input_texel_size;
            const float depth_sample = depth_texture.SampleLevel(linear_clamp, sample_uv, 0);
            if (depth_sample < selected_depth)
            {
                selected_depth = depth_sample;
                selected_motion = motion_texture.SampleLevel(linear_clamp, sample_uv, 0);
            }
        }
    }
}
