cbuffer TsrConstants : register(b0)
{
    float2 gInputSize;
    float2 gOutputSize;
    float2 gCurrentJitterPixels;
    float2 gPreviousJitterPixels;
    float2 gInputTexelSize;
    float2 gOutputTexelSize;
    float gExposure;
    float gSharpenStrength;
    float gMotionVectorScale;
    float gHistoryValid;
    float gVarianceClipGamma;
    float gReactiveStrength;
    uint gFrameIndex;
    float gAntiFlickerStrength;
};
