struct vs_output
{
  float4 position : SV_Position;
  float2 uv;
};

Texture2D    image_scene;
Texture2D    image_ui;
SamplerState image_sampler;

[shader("vertex")]
vs_output vertMain(uint vertex_id: SV_VertexID)
{
  vs_output output;
  output.uv       = float2((vertex_id << 1) & 2, vertex_id & 2);
  output.position = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
  return output;
}

// must in one line, will be replaced when USE_OCIO is true
float4 ocio_conversion(float4 in_pixel) { return in_pixel; }

[shader("fragment")]
float4 fragMain(vs_output input)
{
  float4 color_scene = ocio_conversion(image_scene.Sample(image_sampler, input.uv));
  float4 color_ui    = image_ui.Sample(image_sampler, input.uv);

  float3 color_final = lerp(color_scene.rgb, color_ui.rgb, color_ui.a);
  return float4(color_final, 1.f);
}
