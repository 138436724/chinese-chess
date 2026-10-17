import shader_utils;
import vulkan_data_type;
import pbr;
import lighting;
import path_tracing;

// ============================================================================
// scene_raytracing_render.slang — 硬件光线追踪管线主渲染器（蒙特卡洛无偏 PBR 路径追踪）
//
// 入口（与 scene_raytracing_render.cpp 的 shader_stages / shader_groups 一一对应）：
//   rayGenMain          : 主光线生成 + 子像素抖动 + 时域累积（严格样本均值）
//   rayClosestHitMain   : 直接光 NEE（光源 + 环境 MIS）+ BSDF 采样弹射 + 轮盘赌
//   rayMissMain         : 逃逸到环境（MIS 权重与 NEE 互补）
//   rayShadowMissMain   : 阴影光线未命中 → 光源可见
//   rayShadowAnyHitMain : 阴影光线命中 → 按 opacity 概率遮挡（随机透射，期望无偏）
//
// 估计量无偏性要点（详见各模块注释）：
//   · 不做 firefly 数值钳制、不做吞吐量钳制、不做「恒定环境光」补偿
//     —— 这三者都是把估计量的期望拉离真值的偏差源。
//   · 路径终止只由俄罗斯轮盘赌承担，且**始终按真实存活概率归一**；MAX_BOUNCES
//     仅是防止病态输入的安全网（深度 10 处吞吐量已被轮盘赌压到极小）。
//   · 时域累积用严格样本均值（收敛到真值），而非权重衰减的指数滑动平均。
//   · 所有连续波瓣的采样权重统一取 fr·cosθ / pdf，与 NEE / MIS 使用的
//     eval / pdf 同源，杜绝三者不一致引入的偏差。
//
// 绑定（与 scene_raytracing_render.cpp 的 bindings 序一致）：
//   b0 = top_level_acceleration_structure
//   b1 = render_output (RWTexture2D<float4>)
//   b2 = scene_params   (ConstantBuffer<vulkan_rt_scene_data>)
//   b3 = samplers       (bindless Sampler2D[])
// ============================================================================

RaytracingAccelerationStructure                        top_level_acceleration_structure;  // b0
RWTexture2D<float4>                                    render_output;                     // b1
ConstantBuffer<vulkan_rt_scene_data, ScalarDataLayout> scene_params;                      // b2
Sampler2D[] samplers;                                                                     // b3

// ---------------------------------------------------------------------------
// 阴影光线载荷
// ---------------------------------------------------------------------------
struct shadow_payload
{
  bool     is_shadowed;
  uint64_t seed;  // 与主路径 RNG 解耦：any-hit 用局部种子自行推进
};

// ---------------------------------------------------------------------------
// 入口文件侧的共享装配（bindless 数组是入口文件的全局绑定）
// ---------------------------------------------------------------------------
environment make_environment()
{
  environment env;
  env.environment_index     = scene_params.environment_index;
  env.texture_count         = scene_params.texture_count;
  env.has_hdr               = env.environment_index < env.texture_count;
  env.environment_intensity = scene_params.environment_intensity;
  return env;
}

// 环境辐亮度：HDR 等距矩形贴图优先，无贴图时程序化天空。
// bindless 取值必须在索引守卫成立的分支内（越界索引解引用是未定义行为）。
float3 environment_radiance(environment env, float3 direction)
{
  if(env.has_hdr)
  {
    return env.equirect_radiance(direction, samplers[env.environment_index]);
  }
  return env.procedural_sky(direction);
}

// 反照率贴图采样：同样受 texture_count 守卫
float3 sample_albedo_map(material_lookup lookup, float2 uv, out bool has_albedo_map)
{
  has_albedo_map = false;
  if(!lookup.valid || lookup.data.texture_index >= scene_params.texture_count)
  {
    return float3(0.0);
  }

  has_albedo_map = true;
  return samplers[lookup.data.texture_index].SampleLevel(uv, 0.0).rgb;
}

// ---------------------------------------------------------------------------
// 阴影光线：命中组 1（any-hit），miss 索引 1
//   RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH：首个被接受（未被 IgnoreHit 跳过）
//   的命中即结束遍历；配合 any-hit 里的概率穿透实现半透明遮挡。
//   不对背面做剔除：介质内部的光线需要能命中背面。
// ---------------------------------------------------------------------------
bool trace_shadow_ray(float3 origin, float3 direction, float tmax, inout rng r)
{
  const uint shadow_ray_flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER;

  RayDesc ray;
  ray.Origin    = origin;
  ray.Direction = direction;
  ray.TMin      = SHADOW_EPSILON;
  ray.TMax      = max(tmax - SHADOW_EPSILON, SHADOW_EPSILON);

  shadow_payload payload;
  payload.is_shadowed = false;
  payload.seed        = r.next_uint64();

  TraceRay(top_level_acceleration_structure, shadow_ray_flags, 0xFFu,
           1,  // RayContributionToHitGroupIndex：阴影命中组（命中区第 2 项）
           0,  // MultiplierForGeometryContributionToHitGroupIndex
           1,  // MissShaderIndex：阴影 miss（miss 区第 2 项）
           ray, payload);

  return payload.is_shadowed;
}

// ---------------------------------------------------------------------------
// 直接光照：光源 NEE + 环境光 NEE（与 BSDF 采样做 MIS）
// ---------------------------------------------------------------------------
float3 estimate_direct_lighting(shading_context ctx, resolved_surface surface, inout rng r)
{
  float3 direct        = float3(0.0);
  float3 shadow_origin = surface.position + surface.normal * SHADOW_EPSILON;

  for(uint i = 0; i < scene_params.light_count; ++i)
  {
    light_source light;
    light.data = scene_params.lights[i];
    if(!light.enabled())
    {
      continue;
    }

    light_sample ls = light.sample(surface.position);
    if(!ls.valid || all(ls.radiance <= 0.0))
    {
      continue;
    }

    float visibility = trace_shadow_ray(shadow_origin, ls.direction, ls.distance, r) ? 0.0 : 1.0;
    direct += nee_light_contribution(ctx, ls, visibility);
  }

  // 环境光 NEE：均匀球面采样（pdf = 1/4π），与 BSDF 采样用能量启发式 MIS 组合
  environment  env     = make_environment();
  light_sample es      = env.sample(r);
  float        env_vis = trace_shadow_ray(shadow_origin, es.direction, es.distance, r) ? 0.0 : 1.0;
  if(env_vis > 0.0)
  {
    float3 env_radiance = environment_radiance(env, es.direction);
    direct += nee_environment_contribution(ctx, es.direction, env_radiance, env_vis);
  }

  return direct;
}

// ============================================================================
// 主光线生成
// ============================================================================
[shader("raygeneration")]
void rayGenMain()
{
  uint2  launch_id   = DispatchRaysIndex().xy;
  float2 launch_size = float2(DispatchRaysDimensions().xy);

  // RNG 种子：像素 + 帧号 → 帧间独立同分布（时域累积无偏的前提）
  path_vertex path;
  path.radiance     = float3(0.0);
  path.throughput   = float3(1.0);
  path.bsdf_pdf     = 0.0;
  path.depth        = 0;
  path.ray_inside   = false;
  path.has_bsdf_pdf = false;  // 相机主光线没有 MIS 伙伴 → 环境权重取 1
  path.rng_state.reset(uint64_t(launch_id.x) * 1664525ull + uint64_t(launch_id.y) * 1013904223ull
                       + uint64_t(scene_params.frame_index) * 0x9e3779b9ull);

  // 子像素抖动：每帧在像素内均匀取一点 → 时域累积即为无偏的抗锯齿
  float2 clip = (float2(launch_id) + path.rng_state.next_float2()) / launch_size * 2.0 - 1.0;

  // 逆投影求近 / 远平面上的世界坐标（Vulkan NDC：深度 ∈ [0,1]）
  float4 near_point_v = mul(scene_params.proj_inv_matrix, float4(clip, 0.0, 1.0));
  float4 far_point_v  = mul(scene_params.proj_inv_matrix, float4(clip, 1.0, 1.0));
  near_point_v /= near_point_v.w;
  far_point_v /= far_point_v.w;

  float3 world_near = mul(scene_params.view_inv_matrix, near_point_v).xyz;
  float3 world_far  = mul(scene_params.view_inv_matrix, far_point_v).xyz;

  RayDesc ray;
  ray.Origin    = world_near;
  ray.Direction = safe_normalize(world_far - world_near);
  ray.TMin      = SURFACE_EPSILON;
  ray.TMax      = RAY_TMAX;

  // SBT：主命中组 0，主 miss 索引 0；不对背面剔除（介质需要命中背面）
  TraceRay(top_level_acceleration_structure, 0u, 0xFFu, 0, 0, 0, ray, path);

  // 时域累积：严格样本均值 + 相机曝光
  write_accumulated(render_output, int2(launch_id), path.radiance, scene_params.frame_index, scene_params.exposure);
}

// ============================================================================
// 最近命中：着色 + BSDF 弹射
// ============================================================================
[shader("closesthit")]
void rayClosestHitMain(inout path_vertex path, in BuiltInTriangleIntersectionAttributes attr)
{
  // ---- 几何与材质解析 ----
  // 实例索引守卫：越界解引用是未定义行为（TLAS 实例数组与模型数组等长只是"靠巧合成立"的不变量）
  if(InstanceIndex() >= scene_params.model_count)
  {
    return;
  }

  vulkan_model_data model = scene_params.models[InstanceIndex()];

  hit_geometry geom = interpolate_hit(model, PrimitiveIndex(), attr.barycentrics, WorldRayOrigin(), WorldRayDirection());

  material_lookup lookup = fetch_material(scene_params.materials, scene_params.material_count, model.material_index);

  bool   has_albedo_map = false;
  float3 albedo_map     = sample_albedo_map(lookup, geom.uv, has_albedo_map);
  lookup.has_albedo_map = has_albedo_map;
  lookup.albedo_map     = albedo_map;

  resolved_surface surface = make_surface(geom, lookup, WorldRayDirection());
  shading_context  ctx     = surface.make_context();

  // ---- 介质内 Beer-Lambert 衰减：先结算刚穿过的一段，再着色 ----
  path.apply_medium_absorption(surface);

  // ---- 自发光 ----
  path.add_emission(surface.material.emitted_radiance());

  // ---- 直接光照（NEE）----
  path.radiance += path.throughput * estimate_direct_lighting(ctx, surface, path.rng_state);

  // ---- 间接光照：深度上限（安全网）----
  if(path.depth >= MAX_BOUNCES)
  {
    return;
  }

  // ---- 俄罗斯轮盘赌：无偏终止（存活时按真实概率归一）----
  float rr_scale = russian_roulette_scale(path.rng_state, path.throughput, path.depth);
  if(rr_scale <= 0.0)
  {
    return;
  }

  // ---- BSDF 采样 ----
  bsdf_sample bs = ctx.sample(path.rng_state);
  if(!bs.valid)
  {
    return;  // 该方向 BRDF 值为 0（或 pdf 为 0），路径自然结束，无偏差
  }

  // ---- 介质状态推进 ----
  //   折射：穿越界面（正面入射 → 进入介质；背面入射 → 离开介质）
  //   界面镜面反射：仍停留在原侧，状态不变
  //   不透明波瓣（漫反射 / 粗糙镜面）：视为离开介质
  bool next_inside = false;
  if(bs.is_transmission)
  {
    next_inside = !surface.front_face;
  }
  else if(bs.is_delta)
  {
    next_inside = path.ray_inside;
  }

  // ---- 弹射光线 ----
  //   折射光线射向界面另一侧，因此偏移方向取反；反射 / 散射则沿朝向观察者的法线。
  float3 offset_normal = bs.is_transmission ? -surface.normal : surface.normal;

  RayDesc bounce_ray;
  bounce_ray.Origin    = surface.position + offset_normal * SURFACE_EPSILON;
  bounce_ray.Direction = bs.direction;
  bounce_ray.TMin      = SURFACE_EPSILON;
  bounce_ray.TMax      = RAY_TMAX;

  path_vertex next;
  next.radiance     = path.radiance;
  next.throughput   = path.throughput * bs.weight * rr_scale;
  next.bsdf_pdf     = bs.pdf;
  next.depth        = path.depth + 1;
  next.ray_inside   = next_inside;
  next.has_bsdf_pdf = !bs.is_delta;  // delta 波瓣无 MIS 伙伴
  next.rng_state    = path.rng_state;

  TraceRay(top_level_acceleration_structure, 0u, 0xFFu, 0, 0, 0, bounce_ray, next);

  // 回写：父级（raygen 或上一层 closest-hit）只关心累积辐射度与随机数状态
  path.radiance  = next.radiance;
  path.rng_state = next.rng_state;
}

// ============================================================================
// Miss：逃逸到环境
// ============================================================================
[shader("miss")]
void rayMissMain(inout path_vertex path)
{
  environment env          = make_environment();
  float3      env_radiance = environment_radiance(env, WorldRayDirection());

  path.radiance += environment_miss_contribution(path, env_radiance);
}

// ============================================================================
// 阴影 Miss：未命中 → 光源可见
// ============================================================================
[shader("miss")]
void rayShadowMissMain(inout shadow_payload payload)
{
  payload.is_shadowed = false;
}

// ============================================================================
// 阴影 AnyHit：按 opacity 概率遮挡
//   期望意义下等价于「按 opacity 透射」的薄介质，是随机透射的无偏估计；
//   同时避免了对不透明物体做完整遍历。
// ============================================================================
[shader("anyhit")]
void rayShadowAnyHitMain(inout shadow_payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  // 只按需取材质的不透明度，避免整拷 vulkan_model_data / vulkan_material_data
  // （any-hit 是调用最频繁的入口之一）
  float opacity        = 1.0;
  uint  instance_index = InstanceIndex();

  // 两层守卫：先实例索引（越界 / 空场景 ⇒ 按不透明处理），再材质索引
  // （0xFFFFFFFF = 无材质与越界 / 悬空索引都由这一条比较覆盖）
  if(instance_index < scene_params.model_count)
  {
    uint material_index = scene_params.models[instance_index].material_index;
    if(material_index < scene_params.material_count)
    {
      opacity = clamp(scene_params.materials[material_index].opacity, 0.0, 1.0);
    }
  }

  if(opacity >= 1.0)
  {
    payload.is_shadowed = true;
    AcceptHitAndEndSearch();
    return;
  }

  // 局部 RNG：不扰动主路径的随机序列
  rng local_rng;
  local_rng.state = payload.seed;
  local_rng.inc   = 0xda3e39cb94b95bdbull;  // any-hit 专用流（奇数）
  float u         = local_rng.next_float();
  payload.seed    = local_rng.state;

  if(u >= opacity)
  {
    IgnoreHit();  // 概率穿透：本表面不遮挡，继续遍历
    return;
  }

  payload.is_shadowed = true;
  AcceptHitAndEndSearch();
}
