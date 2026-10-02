import shader_utils;
import gpu_shared_types;
import pbr;
import lighting;

// ============================================================================
// path_tracing.slang — 路径追踪的共享上下文与几何 / 路径状态工具
//
// 本模块不触碰任何全局着色器参数（RT 的 RaytracingAccelerationStructure 与
// Ray Query 的候选遍历 API 不能共存于同一模块），因此它只提供纯函数与
// 纯数据结构；两个入口文件各自负责：
//   · 主光线与弹射光线的求交（TraceRay / RayQuery）
//   · 阴影光线的可见性测试
//   · bindless 纹理数组取值（索引守卫）
// 其余（几何插值、材质解析、BSDF、光照、路径状态、累积）全部共享，
// 从而保证 RT pipeline 与 Ray Query 两条路径的渲染行为逐像素一致。
// ============================================================================

// ---------------------------------------------------------------------------
// hit_geometry：命中点的几何属性（顶点属性插值结果）
// ---------------------------------------------------------------------------
struct hit_geometry
{
  float3 position;  // 世界空间位置
  float3 normal;    // 世界空间几何法线（未定向）
  float2 uv;        // 纹理坐标
  float  distance;  // 沿当前光线到该点的距离（方向已归一化 → 即世界距离）
};

// ---------------------------------------------------------------------------
// material_lookup：材质查找结果（含索引守卫与图案权重）
// ---------------------------------------------------------------------------
struct material_lookup
{
  bool              valid;  // material_index < material_count
  gpu_material_data data;
  bool              has_pattern;     // 图案贴图可用（texture_index < texture_count）
  float             pattern_weight;  // 图案权重（0 = 背景色，1 = 前景色）

  // 图案机制：反照率 = lerp(background_color, foreground_color, weight)
  // （解析实现与光栅化路径共用 pbr.slang 的 resolve_pattern_albedo）
  float3 albedo() { return resolve_pattern_albedo(data, has_pattern, pattern_weight); }

  // 转换成着色侧的材质形式（三套渲染路径共用 pbr.slang 的 make_material）
  pbr_material to_material() { return make_material(data, albedo()); }
};

// 默认材质（无材质实例时兜底：黑色不透明电介质）
gpu_material_data default_material_data()
{
  gpu_material_data md;
  md.background_color  = float3(0.0);
  md.texture_index     = 0xFFFFFFFFu;
  md.foreground_color  = float3(0.0);
  md.roughness         = 0.5;
  md.emission          = float3(0.0);
  md.emission_strength = 0.0;
  md.absorption        = float3(0.0);
  md.metallic          = 0.0;
  md.reflectance       = 0.5;
  md.ior               = 1.5;
  md.transmission      = 0.0;
  md.opacity           = 1.0;
  return md;
}

// 材质查找：单条比较同时覆盖 0xFFFFFFFF（无材质哨兵）与越界 / 悬空索引
// （材质被删除后模型仍引用旧索引；不守卫则按越界地址解引用 → 设备丢失）
material_lookup fetch_material(gpu_material_data* materials, uint material_count, uint material_index)
{
  material_lookup lookup;
  lookup.valid          = material_index < material_count;
  lookup.has_pattern    = false;
  lookup.pattern_weight = 0.0;
  lookup.data           = lookup.valid ? materials[material_index] : default_material_data();
  return lookup;
}

// ---------------------------------------------------------------------------
// 顶点属性插值 → 命中点几何
//   索引 / 顶点缓冲经模型结构体中的设备地址直接解引用（与 C++ 侧
//   scene_model_manager 的缓冲布局一致：索引为 uint32，顶点步长 32B）。
// ---------------------------------------------------------------------------
hit_geometry interpolate_hit(gpu_model_data model, uint primitive_id, float2 bary, float3 ray_origin, float3 ray_direction)
{
  uint i0 = model.index_address[primitive_id * 3u + 0u];
  uint i1 = model.index_address[primitive_id * 3u + 1u];
  uint i2 = model.index_address[primitive_id * 3u + 2u];

  gpu_model_vertex v0 = model.vertex_address[i0];
  gpu_model_vertex v1 = model.vertex_address[i1];
  gpu_model_vertex v2 = model.vertex_address[i2];

  float3 local_position = interpolate(bary, v0.position, v1.position, v2.position);
  float3 local_normal   = interpolate(bary, v0.normal, v1.normal, v2.normal);

  hit_geometry geom;
  geom.position = mul(model.model_matrix, float4(local_position, 1.0)).xyz;
  // 法线用逆转置才是严格正确的；本场景模型矩阵只有平移 / 旋转 / 等比缩放，
  // 乘法与逆转置等价（非等比缩放会引入方向误差，属已知限制）。
  geom.normal = normalize(mul(model.model_matrix, float4(local_normal, 0.0)).xyz);
  geom.uv     = interpolate(bary, v0.uv, v1.uv, v2.uv);

  float3 delta  = geom.position - ray_origin;
  geom.distance = max(dot(delta, ray_direction), 0.0);
  return geom;
}

// ---------------------------------------------------------------------------
// resolved_surface：一次表面交互的完整着色输入
// ---------------------------------------------------------------------------
struct resolved_surface
{
  float3       position;    // 世界空间位置
  float3       normal;      // 朝向观察者的宏观法线
  float3       wo;          // 出射方向（指向观察者，单位向量）
  float        distance;    // 到该点的距离（介质衰减结算用）
  bool         front_face;  // 是否从几何正面入射（介质界面 eta 方向）
  pbr_material material;

  // 构造 BSDF 着色上下文
  shading_context make_context()
  {
    shading_context ctx;
    ctx.material   = material;
    ctx.ggx.n      = normal;
    ctx.ggx.alpha  = material.alpha();
    ctx.n          = normal;
    ctx.wo         = wo;
    ctx.front_face = front_face;
    return ctx;
  }
};

// 法线定向：翻转几何法线使其朝向观察者。
// 本项目对**所有**光线都关闭背面剔除（不使用 RAY_FLAG_CULL_BACK_FACING_TRIANGLES）：
// 介质内部的光线必须能命中所属物体的背面才能正确穿出，
// 因此由着色阶段统一做定向，而不是依赖遍历阶段的剔除。
resolved_surface make_surface(hit_geometry geom, material_lookup lookup, float3 ray_direction)
{
  resolved_surface surface;
  surface.position   = geom.position;
  surface.distance   = geom.distance;
  surface.wo         = safe_normalize(-ray_direction);
  surface.front_face = dot(geom.normal, surface.wo) > 0.0;
  surface.normal     = surface.front_face ? geom.normal : -geom.normal;
  surface.material   = lookup.to_material();
  return surface;
}

// ---------------------------------------------------------------------------
// path_vertex：路径追踪状态（沿路径以 inout 传递；RT pipeline 中直接作为光线载荷）
// ---------------------------------------------------------------------------
struct path_vertex
{
  float3 radiance;      // 已累积辐射度
  float3 throughput;    // 路径吞吐量（BSDF·cosθ/pdf 连乘）
  float  bsdf_pdf;      // 生成当前光线的连续 BSDF 方向 pdf（环境光 MIS 用）
  int    depth;         // 已完成的弹射次数
  bool   ray_inside;    // 当前光线是否在介质内部传播
  bool   has_bsdf_pdf;  // bsdf_pdf 是否有效（相机主光线 / delta 波瓣 → false）
  rng    rng_state;     // 蒙特卡洛随机数状态

  // 介质内 Beer-Lambert 衰减：对「刚刚穿过的那一段」精确结算。
  // 均匀介质中 T = exp(-σ_a · d) 是解析精确解，因此对任意厚度都无偏
  // （替代旧实现的「固定厚度 × 进入时一次性吸收」近似）。
  [mutating]
  void apply_medium_absorption(resolved_surface surface)
  {
    if(ray_inside)
    {
      throughput *= exp(-max(surface.material.absorption, float3(0.0)) * surface.distance);
    }
  }

  // 自发光：命中自发光表面时把辐亮度直接计入（无偏，不需要额外采样）
  [mutating]
  void add_emission(float3 emitted) { radiance += throughput * emitted; }
};

// ---------------------------------------------------------------------------
// 路径状态工具
// ---------------------------------------------------------------------------

// 俄罗斯轮盘赌的存活缩放因子（0 表示路径终止）。
//   p = clamp(max(throughput), RR_MIN_PROBABILITY, 1)，
//   存活时吞吐量除以 p → 期望不变；下限只是避免极低存活率造成的病态长路径，
//   由于「始终按真实概率归一」而不是直接截断，估计量保持无偏。
//   返回 1/p，由调用方乘到吞吐量上（避免 inout 参数与 payload 成员的别名问题）。
float russian_roulette_scale(inout rng r, float3 throughput, int depth)
{
  if(depth < RR_START_DEPTH)
  {
    return 1.0;
  }

  float p = clamp(max_component(throughput), RR_MIN_PROBABILITY, 1.0);
  if(r.next_float() >= p)
  {
    return 0.0;
  }

  return 1.0 / p;
}

// 光线逃逸到环境时的 MIS 贡献（与 NEE 权重互补，和为 1）
float3 environment_miss_contribution(path_vertex pv, float3 env_radiance)
{
  return pv.throughput * env_radiance * environment_miss_weight(pv.has_bsdf_pdf, pv.bsdf_pdf);
}

// ---------------------------------------------------------------------------
// 时域累积：把本次采样写入输出图像（严格样本均值，见 shader_utils.slang）
//   frame_index 由 C++ 侧逐帧递增并在场景 / 相机变化时复位；
//   本函数同时施加相机曝光（线性缩放，不改变无偏性）。
// ---------------------------------------------------------------------------
void write_accumulated(RWTexture2D<float4> image, int2 pixel, float3 sample_color, uint frame_index, float exposure)
{
  float4 previous = image[pixel];
  float3 mean     = accumulate_sample(previous.rgb, sample_color, frame_index, exposure);
  image[pixel]    = float4(mean, 1.0);
}
