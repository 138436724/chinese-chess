struct VSInput
{
    float3 position;
};

struct VSOutput
{
    float4 position : SV_Position;
};

struct UBO
{
    float4x4 model;
    float4x4 view;
    float4x4 projection;
    float3 camera_position;
};
ConstantBuffer<UBO> ubo;

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    output.position = mul(ubo.projection, mul(ubo.view, mul(ubo.model, float4(input.position, 1.0))));
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput output) : SV_Target
{
    return float4(0.0, 0.0, 0.0, 1.0);
}
