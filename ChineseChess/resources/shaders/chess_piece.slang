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
    float3 piece_color;
};
ConstantBuffer<UBO> ubo;

Sampler2D texture;

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    output.position = mul(ubo.projection, mul(ubo.view, mul(ubo.model, float4(input.position, 1.0))));
    output.color = input.normal; // todo
    output.color = ubo.piece_color;
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput output) : SV_Target
{
    float3 base_color = float3(251.0 / 255.0, 210.0 / 255.0, 148.0 / 255.0);
    float weight = texture.Sample(output.uv).x;
    return float4(lerp(base_color, output.color, weight), 1.0);
}
