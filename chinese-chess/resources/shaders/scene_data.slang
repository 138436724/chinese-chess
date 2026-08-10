// ============================================================================
// scene_data.slang — CPU 与 GPU 共享的场景数据结构（std430 风格布局）
// 与 C++ 侧逐字段对齐：
//   model_data    ↔ scene_model_manager.cpp
//   material_data ↔ scene_material_manager.cpp（含 opacity/ior/transmission）
//   light_data    ↔ scene_light_manager.cpp
// 注意：SSBO 默认 std430 布局，UBO 为 std140；vec3 按 16 字节对齐。
// ============================================================================

// GPU 侧顶点布局：position + normal + uv（与 C++ model_vertex 完全一致）
struct Vertex
{
  float3 position;
  float3 normal;
  float2 uv;
};

// 光栅化路径 push constant（相机矩阵）
struct raster_push_constant
{
  float4x4 proj_matrix;
  float4x4 view_matrix;
};

// 光追路径 push constant（逆矩阵 + 场景信息）
struct rt_push_constant
{
  float4x4 proj_inv_matrix;
  float4x4 view_inv_matrix;
  uint32_t skybox_index;  // bindless 纹理数组中的 HDR 天空索引，0xFFFFFFFF 表示无
  uint32_t light_count;   // 灯光数量
  uint32_t frame_index;   // 时域累积帧计数（也用于 RNG 种子）
};

// 模型运行时数据：每个实例一条
struct model_data
{
  float4x4  model_matrix;    // 模型变换（局部 → 世界）
  uint32_t  material_index;  // 材质索引，0xFFFFFFFF 表示无材质
  Vertex*   vertex_address;  // 顶点缓冲区设备地址
  uint32_t* index_address;   // 索引缓冲区设备地址
};

// 材质参数：光栅化只用前五项，光追额外使用 opacity/ior/transmission
struct material_data
{
  float3   background_color;  // 底色（alpha 图权重 = 0 时）
  uint32_t texture_index;     // bindless 纹理索引，0xFFFFFFFF 表示无纹理
  float3   foreground_color;  // 前景色（alpha 图权重 = 1 时）
  float    roughness;
  float    metallic;
  float    opacity;       // 不透明度：1 = 完全不透明（半透明阴影/透射用）
  float    ior;           // 折射率（介质透射用）
  float    transmission;  // 透射强度：0 = 无透射
};

// 光源参数：与 C++ scene_light_manager.cpp 的 light_data 对齐
// active_type: 0=方向光, 1=点光源, 2=聚光灯
struct light_data
{
  float3   color;             // 线性空间 RGB 颜色
  uint32_t active_type;       // 光源类型枚举
  float3   direction;         // 方向（方向光 & 聚光灯，单位向量）
  float    intensity;         // 强度倍率
  float3   position;          // 世界坐标（点光源 & 聚光灯）
  float    range;             // 最大照射距离（点光源 & 聚光灯）
  float    inner_cone_angle;  // 内锥角（弧度，聚光灯）
  float    outer_cone_angle;  // 外锥角（弧度，聚光灯）
};
