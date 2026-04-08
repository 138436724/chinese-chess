struct VSOutput
{
    float4 position : SV_Position;
    float2 uv;
};

Texture2D image_scene;
Texture2D image_ui;
SamplerState image_texture;

[shader("vertex")]
VSOutput vertMain(uint vertexId: SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

float4 ocio_conversion(float4 inPixel) { return inPixel; }

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target
{
    float4 color_scene = ocio_conversion(image_scene.Sample(image_texture, input.uv));
    float4 color_ui = image_ui.Sample(image_texture, input.uv);

    float3 color_final = lerp(color_scene.rgb, color_ui.rgb, color_ui.a);

    return float4(color_final, 1.f);
}
