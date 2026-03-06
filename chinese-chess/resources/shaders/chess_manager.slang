struct VSInput
{
    float3 position;
    float2 uv;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 color;
    uint32_t sampler_layer;
    float2 uv;
};

struct UBO
{
    float4x4 model;
    float4x4 view;
    float4x4 projection;
    float3 camera_position;
    float3 piece_color;
    uint32_t piece_type;
};
struct UBOS
{
    UBO ubo[32];
};
ConstantBuffer<UBOS> ubos;

Sampler2DArray texture;

[shader("vertex")]
VSOutput vertMain(VSInput input, uint32_t instance_id: SV_InstanceID)
{
    VSOutput output;
    output.position =
        mul(ubos.ubo[instance_id].projection,
            mul(ubos.ubo[instance_id].view, mul(ubos.ubo[instance_id].model, float4(input.position, 1.0))));
    output.color = ubos.ubo[instance_id].piece_color;
    output.sampler_layer = ubos.ubo[instance_id].piece_type + uint32_t(abs(ubos.ubo[instance_id].piece_color.x - 1)) * 7;
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput output) : SV_Target
{
    float3 base_color = float3(251.0 / 255.0, 210.0 / 255.0, 148.0 / 255.0);
    float weight = texture.Sample(float3(output.uv, output.sampler_layer)).x;
    return float4(lerp(base_color, output.color, weight), 1.0);
}
