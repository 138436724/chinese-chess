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

// 模型运行时数据：每个实例一条
// 注意：必须显式补足尾部填充（offset 88 → 96）。Slang 对物理存储缓冲指针
// （model_data*，如 rt_scene_data.models）的元素步长按结构体自然大小计算，
// 不做 std430 的 16 字节对齐收尾；而 C++ 侧 sizeof(model_data) = 96（mat4 使结构体
// 对齐到 16）。若不补 pad，models[i]（i ≥ 1）会按 88 字节步长错位读取，
// material_index 等字段读到垃圾 → materials[垃圾索引] 越界解引用 → GPU 故障（设备丢失）。
// 描述符路径（StructuredBuffer）本就按 16 对齐得 96，此 pad 不影响其布局。
struct model_data
{
  float4x4  model_matrix;    // 模型变换（局部 → 世界）
  uint32_t  material_index;  // 材质索引，0xFFFFFFFF 表示无材质
  Vertex*   vertex_address;  // 顶点缓冲区设备地址
  uint32_t* index_address;   // 索引缓冲区设备地址
  //uint32_t  _pad[2];         // 显式尾部填充：88B → 96B（与 C++ sizeof 对齐）
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
// 同 model_data：显式补 8B 尾部填充（56 → 64），保证物理指针步长与 C++ sizeof 一致。
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
  //uint32_t _pad[2];           // 显式尾部填充：56B → 64B（与 C++ sizeof 对齐）
};

// ============================================================================
// 场景参数 UBO 结构（std140;替代原 push constant;CPU 每帧把三数组设备地址写入此处,
// shader 经指针成员直接解引用 → 描述符中不再需要 models/materials/lights 三个 SSBO 绑定)
//   raster_scene_data ↔ scene_rasterization_render.cpp 的 raster_scene_uniform_data(160B)
//   rt_scene_data     ↔ scene_raytracing_render.cpp / scene_rayquery_render.cpp(176B)
// 注意:指针成员在 std140 中占 8 字节、8 对齐,与 C++ 侧 uint64 地址字段同偏移。
// ============================================================================

// 光栅化路径场景参数
struct raster_scene_data
{
  float4x4       proj_matrix;      //   0:模型 → 裁剪
  float4x4       view_matrix;      //  64:模型 → 视图
  model_data*    models;           // 128:物体数组基址(设备地址)
  material_data* materials;        // 136:材质数组基址
  uint32_t       model_count;      // 144
  uint32_t       material_count;   // 148
  uint32_t       texture_count;    // 152(bindless 纹理槽数,samplers[] 上界)
  // 尾部 pad 至 16 对齐(160B)
};

// RT/RayQuery 路径场景参数
struct rt_scene_data
{
  float4x4       proj_inv_matrix;  //   0
  float4x4       view_inv_matrix;  //  64
  model_data*    models;           // 128:物体数组基址(设备地址)
  material_data* materials;        // 136:材质数组基址
  light_data*    lights;           // 144:灯光数组基址
  uint32_t       model_count;      // 152
  uint32_t       material_count;   // 156
  uint32_t       light_count;      // 160
  uint32_t       texture_count;    // 164(bindless 纹理槽数,samplers[] 上界)
  uint32_t       skybox_index;     // 168
  uint32_t       frame_index;      // 172
  // 尾部 pad 至 16 对齐(176B)
};
