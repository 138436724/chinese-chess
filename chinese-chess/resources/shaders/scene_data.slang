// ============================================================================
// scene_data.slang — CPU 与 GPU 共享的场景数据结构（std430 风格布局）
// 与 C++ 侧逐字段对齐：
//   model_data    ↔ scene_model_manager.cpp
//   material_data ↔ scene_material_manager.cpp（含光谱消光系数与色散字段）
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
// 注意：结构体自然大小即 96B（float4x4 对齐 16 使 sizeof 收尾到 96，offset 88 起的 8B 尾部空洞
// 由编译器补齐），与 C++ 侧 sizeof(model_data) 一致，无需显式 _pad。
// 若将来改动成员顺序/类型，务必复核 Slang 对物理存储缓冲指针（model_data*，如 rt_scene_data.models）
// 的元素步长：它按结构体自然大小计算，不做 std430 的 16 字节对齐收尾；步长与 C++ sizeof 不一致会让
// models[i]（i ≥ 1）错位，materials[垃圾索引] 越界解引用 → GPU 故障（设备丢失）。
struct model_data
{
  float4x4  model_matrix;    // 模型变换（局部 → 世界）
  uint32_t  material_index;  // 材质索引，0xFFFFFFFF 表示无材质
  Vertex*   vertex_address;  // 顶点缓冲区设备地址
  uint32_t* index_address;   // 索引缓冲区设备地址
  //uint32_t  _pad[2];         // 显式尾部填充：88B → 96B（与 C++ sizeof 对齐）
};

// 材质参数(物理基础着色 / specular workflow)
//   颜色均为**线性空间**,不是 sRGB
//   background_color : 基底反射率。电介质 = 漫反射反照率;金属 = 垂直入射反射率 F0
//   foreground_color : 刻字/纹样层反射率(由 alpha 图覆盖率在二者之间混合)
//   absorption_coefficient : Beer-Lambert 消光系数 σ_t(逐波长),1/世界单位。
//                            玉石颜色由此物理涌现,不再靠颜色插值
//   engrave_absorption     : 刻字层附加消光(按覆盖率加权叠加)
//   dispersion_model/param : 色散模型(0=常数折射率 1=Cauchy);阶段 5 接真实波长
// 布局:自然大小 80B,与 C++ 侧 gpu_material_data 逐字节一致(见 scene_material_manager.cpp)。
// ⚠ 物理正确性以 ray_tracing.slang 为准;光栅化路径只消费其中的颜色/粗糙度/金属度。
// 材质变更记录(2026 PBR 改造):
//   材质颜色语义统一为**线性空间**;新增光谱消光 / 色散字段,并把物理相关的
//   着色全部收敛到 shading.slang。
//
// 贴图通道(阶段 1):
//   albedo_index : 基础色/覆盖率图。当前程序化资产是**单通道**(R8/BC4)字形图,
//                  采样 .x 作为"刻字覆盖率",在 background/foreground 之间混合。
//                  换成真实 RGB albedo 贴图时,采样方式改为 .rgb 直接作反射率即可
//                  (同样的槽位,不需要改结构体)。
//   normal_index : 切线空间法线图(NormalMap 约定:RGB → [0,1],采样后 ×2-1)
//   orm_index    : R=AO  G=roughness  B=metallic(glTF ORM 约定)
//   emissive_index : 预留(自发光需要进 NEE 作为显式光源,阶段 4)
// 任一索引为 0xFFFFFFFF 表示该通道无贴图。
// 布局:自然大小 112B,与 C++ 侧 gpu_material_data 逐字节一致。
// ⚠ 物理正确性以 ray_tracing.slang 为准;光栅化路径只消费 albedo/texture 相关字段。
struct material_data
{
  float3   background_color;         //   0:基底反射率(或 RGB albedo 的替换目标)
  uint32_t albedo_index;             //  12:覆盖率/albedo 图
  float3   foreground_color;         //  16:刻字层反射率
  float    roughness;                //  28
  float    metallic;                 //  32
  float    opacity;                  //  36:不透明度(阴影/透射用)
  float    ior;                      //  40:折射率(色散模型下为参考波长折射率)
  float    transmission;             //  44:透射强度
  float3   absorption_coefficient;   //  48:σ_t 光谱消光系数(逐波长)
  float3   engrave_absorption;       //  64:刻字层附加消光
  uint32_t dispersion_model;         //  76:0 = 常数 ior,1 = Cauchy
  float    dispersion_param;         //  80:Cauchy A / 常数折射率
  uint32_t normal_index;             //  84:切线空间法线图
  uint32_t orm_index;                //  88:R=AO G=roughness B=metallic
  uint32_t emissive_index;           //  92:预留(阶段 4)
  float    normal_scale;             //  96:法线强度缩放(1 = 贴图原强度)
  float    occlusion_strength;       // 100:AO 应用强度(1 = 完全应用)
  float    _pad0;                    // 104:占位,保持 8 字节对齐
  float    _pad1;                    // 108
};

// 光源参数：与 C++ scene_light_manager.cpp 的 light_data 对齐
// active_type: 0=方向光, 1=点光源, 2=聚光灯
// 同 model_data：自然大小即 64B（offset 56 起的 8B 尾部空洞由编译器补齐），无需显式 _pad。
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
//   rt_scene_data     ↔ scene_raytracing_render.cpp(192B,含调试通道字段)
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

// RT 路径场景参数(仅 ray_tracing.slang 使用;ray query 路径已退役)
// 尾部为调试/实验通道:能量守恒验证必须能关闭全部启发式钳制,
// 否则白炉测试测到的是钳制而不是物理(见 pbr-exploration-plan.md 阶段 0)。
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
  uint32_t       debug_flags;      // 176:DEBUG_* 位组合(见 common.slang)
  float          env_intensity;    // 180:环境光强度倍率(阶段 3;0 = 关闭环境光)
  float          force_metallic;   // 184:DEBUG_FORCE_METALLIC 的强制值
  float          force_roughness;  // 188:DEBUG_FORCE_ROUGHNESS 的强制值
  // 尾部 pad 至 16 对齐(192B)
};
