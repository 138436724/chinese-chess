struct VSInput
{
    float3 position;
    float3 normal;
    float2 uv;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color;
    float2 uv;
};

struct UBO
{
    float4x4 model;
    float4x4 view;
    float4x4 projection;
    float3 camera_position;
};
ConstantBuffer<UBO> ubo;

Sampler2D texture;

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    output.position = mul(ubo.projection, mul(ubo.view, mul(ubo.model, float4(input.position, 1.0))));
    output.color = input.normal;
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput output) : SV_Target
{
    float3 base_color = float3(224.0 / 255.0, 176.0 / 255.0, 124.0 / 255.0);
    float3 font_color = float3(0.0, 0.0, 0.0);
    float weight = texture.Sample(output.uv).x;
    return float4(lerp(base_color, font_color, weight), 1.0);
}
