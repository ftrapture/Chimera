cbuffer OverlayConstants : register(b0)
{
    float2 gViewportSize;
    float2 gPadding;
};

struct VSInput
{
    float2 position : POSITION;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    const float2 ndc = float2(
        (input.position.x / gViewportSize.x) * 2.0 - 1.0,
        1.0 - (input.position.y / gViewportSize.y) * 2.0);
    output.position = float4(ndc, 0.0, 1.0);
    output.color = input.color;
    return output;
}
