struct Vertex
{
    float3 position;
    // float3 normal;
    float2 uv;
};

struct push_constant
{
    float4x4 proj_matrix;
    float4x4 view_matrix;
};

struct model_data
{
    float4x4 model_matrix;
    uint32_t material_index;
    Vertex *vertex_address;
    uint32_t *index_address;
};

struct material_data
{
    float3 background_color;
    float3 foreground_color;
    uint32_t texture_index;
};

[[vk::push_constant]]
ConstantBuffer<push_constant> pushConst;   // camera info
StructuredBuffer<model_data> models;       // SSBO, has all model info
StructuredBuffer<material_data> materials; // SSBO, has all material info
Sampler2D[] samplers;                      // bindless descriptor, texture used in materials
// attention please, SSBO is std430 layout default, but UBO is std140

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv;
    uint32_t material_index;
};

[shader("vertex")]
VSOutput vertMain(Vertex input, uint draw_id: SV_DrawIndex)
{
    model_data now_model = models[draw_id];
    VSOutput output;
    output.position = mul(pushConst.proj_matrix,
                          mul(pushConst.view_matrix, mul(now_model.model_matrix, float4(input.position, 1.0))));
    output.uv = input.uv;
    output.material_index = now_model.material_index;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput output) : SV_Target
{
    float4 color = float4(0.0, 0.0, 0.0, 1.0);
    if (output.material_index != 0xFFFFFFFF)
    {
        material_data now_material = materials[output.material_index];
        Sampler2D now_texture = samplers[now_material.texture_index];
        float weight = now_texture.Sample(output.uv).x;
        color = float4(lerp(now_material.background_color, now_material.foreground_color, weight), 1.0);
    }
    return color;
}
