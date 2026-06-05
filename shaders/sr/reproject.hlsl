float2 ComputePreviousUv(
    float2 current_uv,
    float2 motion_pixels,
    float2 current_jitter_pixels,
    float2 previous_jitter_pixels,
    float2 output_size)
{
    const float2 jitter_delta = current_jitter_pixels - previous_jitter_pixels;
    return current_uv - ((motion_pixels + jitter_delta) / output_size);
}
