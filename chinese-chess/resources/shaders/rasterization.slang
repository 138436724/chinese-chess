import common;
import scene_data;

// ============================================================================
// rasterization.slang — 光栅化主着色器（顶点 + 片元，前向 MSAA 路径）
//   vertMain : 间接绘制（SV_DrawIndex 即模型 SSBO 索引）→ 局部 → 世界 → 视图 → 投影
//   fragMain : bindless 材质解析（alpha 图权重在底色/前景色间 lerp）
// 绑定布局（与 scene_rasterization_render.cpp 一致）：
//   binding 0: models    (StructuredBuffer<model_data>)
//   binding 1: materials (StructuredBuffer<material_data>)
//   binding 2: samplers  (bindless Sampler2D[])
// 注意：SSBO 默认 std430 布局，UBO 为 std140；顶点输入仅 position + uv（见 raster_vertex）。
// ============================================================================

[[vk::push_constant]]
ConstantBuffer<raster_push_constant> pushConst;  // 相机投影/视图矩阵（scene_data.slang）

StructuredBuffer<model_data>    models;     // 模型运行时数据（与 scene_model_manager 的 SSBO 对齐）
StructuredBuffer<material_data> materials;  // 材质数据（与 scene_material_manager 的 SSBO 对齐）
Sampler2D[] samplers;                       // bindless 纹理数组（CombinedImageSampler，≤1024 槽）

// 顶点输入：与管线顶点属性绑定一致（location 0 = position，location 1 = uv）。
// 注意：与 scene_data.slang 的 Vertex 不同——光栅路径不需要法线（逐顶点数据更小）。
struct raster_vertex
{
  float3 position;
  float2 uv;
};

// 顶点 → 片元插值输出
struct VSOutput
{
  float4   position : SV_Position;
  float2   uv;              // 插值后的 UV（直接透传，片元采样用）
  uint32_t material_index;  // 该实例的材质索引（0xFFFFFFFF = 无材质，片元侧守卫）
};

[shader("vertex")]
VSOutput vertMain(raster_vertex input, uint draw_id: SV_DrawIndex)
{
  // 间接绘制：SV_DrawIndex 是 draw command 索引 == 模型 SSBO 索引
  // （与 TLAS 实例数组/RT 路径的 models[] 同序约定一致）
  model_data now_model = models[draw_id];

  // 变换链：局部 → 世界（model_matrix）→ 视图 → 投影（Vulkan NDC，深度 [0,1]）
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
  // 默认黑色（无材质实例时兜底）
  float4 color = float4(0.0, 0.0, 0.0, 1.0);

  // 材质守卫：material_index 合法才取材质（避免越界读取 SSBO）
  if(output.material_index != 0xFFFFFFFF)
  {
    material_data now_material = materials[output.material_index];
    if(now_material.texture_index != 0xFFFFFFFF)
    {
      // 有纹理：采样 R 通道作为"字形/图案权重"，
      // 在底色（background_color）与前景色（foreground_color）间插值
      // （棋盘"楚河汉界"、棋子刻字图集均为该机制）
      Sampler2D now_texture = samplers[now_material.texture_index];
      float     weight      = now_texture.Sample(output.uv).x;
      color                 = float4(lerp(now_material.background_color, now_material.foreground_color, weight), 1.0);
    }
    else
    {
      // 无纹理：直接用底色
      color = float4(now_material.background_color, 1.0);
    }
  }
  return color;
}
