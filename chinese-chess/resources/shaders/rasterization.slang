import common;
import scene_data;

// ============================================================================
// rasterization.slang — 光栅化主着色器（顶点 + 片元，前向 MSAA 路径）
//   vertMain : 间接绘制（SV_DrawIndex 即模型 SSBO 索引）→ 局部 → 世界 → 视图 → 投影
//   fragMain : bindless 材质解析（alpha 图权重在底色/前景色间 lerp）
// 绑定布局（与 scene_rasterization_render.cpp 一致）：
//   binding 0: samplers  (bindless Sampler2D[])
//   binding 1: scene_params (ConstantBuffer<raster_scene_data>,场景 UBO:矩阵+models/materials 基址+计数)
// 说明：models/materials 不再单独绑定——经 scene_params.models/materials 指针(设备地址)直接解引用。
// 注意：UBO 为 std140；顶点输入仅 position + uv（见 raster_vertex）。
// ============================================================================

ConstantBuffer<raster_scene_data> scene_params;
Sampler2D[] samplers;                       // bindless 纹理数组(binding 0,CombinedImageSampler,≤1024 槽)
// 场景参数 UBO(binding 1,声明序自动分配):相机矩阵 + models/materials 数组基址 + 计数。
// push constant 已废弃(见 scene_data.slang / scene_rasterization_render.cpp)。

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
  uint32_t material_index;  // 该实例的材质索引（0xFFFFFFFF = 无材质；片元侧以 < material_count 守卫）
};

[shader("vertex")]
VSOutput vertMain(raster_vertex input, uint draw_id: SV_DrawIndex)
{
  // 间接绘制：SV_DrawIndex 是 draw command 索引 == 模型索引
  // （与 TLAS 实例数组/RT 路径同序约定一致；经场景 UBO 中的基址解引用）
  // 按需取字段：光栅顶点只读 model_matrix / material_index，避免整拷 96B model_data（每顶点一次）
  const float4x4 model_matrix  = scene_params.models[draw_id].model_matrix;
  const uint32_t material_index = scene_params.models[draw_id].material_index;

  // 变换链：局部 → 世界（model_matrix）→ 视图 → 投影（Vulkan NDC，深度 [0,1]）
  VSOutput output;
  output.position =
      mul(scene_params.proj_matrix, mul(scene_params.view_matrix, mul(model_matrix, float4(input.position, 1.0))));
  output.uv             = input.uv;
  output.material_index = material_index;
  return output;
}

[shader("fragment")]
float4 fragMain(VSOutput output)
    : SV_Target
{
  // 默认黑色（无材质实例时兜底）
  float4 color = float4(0.0, 0.0, 0.0, 1.0);

  // 材质守卫：material_index < material_count 才取材质（计数上界来自场景 UBO）。
  // 单条比较同时覆盖两情形：0xFFFFFFFF（无材质哨兵，恒 ≥ count）与越界/悬空索引
  // （如材质被删除后模型仍引用旧索引；若不守卫将按 0 基址/越界解引用 → 设备丢失）。
  if(output.material_index < scene_params.material_count)
  {
    // 按需取字段：只读底色/纹理槽/前景色（逐像素，避免整拷 48B material_data）
    const uint32_t material_slot = output.material_index;
    const float3   bg_color      = scene_params.materials[material_slot].background_color;
    const uint32_t texture_index = scene_params.materials[material_slot].texture_index;
    if(texture_index < scene_params.texture_count)
    {
      // 有纹理：采样 R 通道作为"字形/图案权重"，
      // 在底色（background_color）与前景色（foreground_color）间插值
      // （棋盘"楚河汉界"、棋子刻字图集均为该机制）
      Sampler2D now_texture = samplers[texture_index];
      float     weight      = now_texture.Sample(output.uv).x;
      color                 = float4(lerp(bg_color, scene_params.materials[material_slot].foreground_color, weight), 1.0);
    }
    else
    {
      // 无纹理：直接用底色
      color = float4(bg_color, 1.0);
    }
  }
  return color;
}
