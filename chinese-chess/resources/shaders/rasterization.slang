import common;
import scene_data;

// ============================================================================
// rasterization.slang — 光栅化主着色器（顶点 + 片元）
//   vertMain : 间接绘制（SV_DrawIndex）取模型矩阵 → 世界 → 视图 → 投影
//   fragMain : bindless 材质采样（alpha 图权重 lerp 底色/前景色）
// 绑定布局（与 scene_rasterization_render.cpp 一致）：
//   binding 0: models    (StructuredBuffer<model_data>)
//   binding 1: materials (StructuredBuffer<material_data>)
//   binding 2: samplers  (bindless Sampler2D[])
// 注意：SSBO 默认 std430 布局，UBO 为 std140。
// ============================================================================

[[vk::push_constant]]
ConstantBuffer<raster_push_constant> pushConst;  // 相机投影/视图矩阵

StructuredBuffer<model_data>    models;     // 模型运行时数据
StructuredBuffer<material_data> materials;  // 材质数据
Sampler2D[] samplers;                       // bindless 纹理数组

// 顶点输入：与管线顶点属性（location 0 = position, location 1 = uv）一致
struct raster_vertex
{
  float3 position;
  float2 uv;
};

struct VSOutput
{
  float4   position : SV_Position;
  float2   uv;
  uint32_t material_index;
};

[shader("vertex")]
VSOutput vertMain(raster_vertex input, uint draw_id: SV_DrawIndex)
{
  model_data now_model = models[draw_id];

  VSOutput output;
  output.position =
      mul(pushConst.proj_matrix, mul(pushConst.view_matrix, mul(now_model.model_matrix, float4(input.position, 1.0))));
  output.uv             = input.uv;
  output.material_index = now_model.material_index;
  return output;
}

[shader("fragment")]
float4 fragMain(VSOutput output)
    : SV_Target
{
  float4 color = float4(0.0, 0.0, 0.0, 1.0);

  if(output.material_index != 0xFFFFFFFF)
  {
    material_data now_material = materials[output.material_index];
    if(now_material.texture_index != 0xFFFFFFFF)
    {
      Sampler2D now_texture = samplers[now_material.texture_index];
      float     weight      = now_texture.Sample(output.uv).x;
      color                 = float4(lerp(now_material.background_color, now_material.foreground_color, weight), 1.0);
    }
    else
    {
      color = float4(now_material.background_color, 1.0);
    }
  }
  return color;
}
