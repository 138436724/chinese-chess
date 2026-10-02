import shader_utils;
import gpu_shared_types;
import pbr;
import lighting;
import path_tracing;

// ============================================================================
// scene_rayquery_render.slang — compute + ray query 路径追踪（与 RT pipeline 行为一致）
//
// 与 scene_raytracing_render.slang 的差异只在「求交 API」本身：
//   · 主光线与弹射光线用「手动最近命中」（遍历候选取最小 t）替代硬件 committed
//     命中——Slang 2026.7.1 的 RayQuery 未暴露候选确认 API，且 CommittedStatus()
//     恒返回 None（详见下方实验记录）。
//   · 阴影光线用 ray query + opacity 概率穿透，统计等价于 RT 的 any-hit。
//   · 弹射用循环而非递归（路径追踪本就等价）。
// 估计量、材质、BSDF、光照与累积数学 100% 复用 pbr.slang / lighting.slang /
// path_tracing.slang，因此两条路径的输出逐像素同分布。
//
// 绑定（与 scene_rayquery_render.cpp 的 bindings 序一致）：
//   b0 = top_level_acceleration_structure
//   b1 = render_output (RWTexture2D<float4>)
//   b2 = scene_params   (ConstantBuffer<rt_scene_data>)
//   b3 = samplers       (bindless Sampler2D[])
// ============================================================================

RaytracingAccelerationStructure top_level_acceleration_structure;  // b0
RWTexture2D<float4>             render_output;                     // b1
ConstantBuffer<rt_scene_data, ScalarDataLayout>   scene_params;                      // b2
Sampler2D[]                     samplers;                          // b3

// ============================================================================
// 实验记录（Slang 2026.7.1，slangc + SPIR-V 反汇编 + 运行时诊断证实）：
//   a) 编译期：RayQuery 未暴露候选确认 API（CandidateProceed / CommittedProceed /
//      ConfirmIntersection / Terminate / GetIntersectionType 均不存在）。
//   b) 运行时：CommittedStatus() 恒返回 None → 依赖提交状态的判定全部走 miss，
//      因此必须「手动最近命中」。
//   c) CandidateInstanceID() 被降级为 InstanceCustomIndex（实例自定义索引，常量）→
//      必须用 CandidateInstanceIndex()（InstanceIdKHR）。
//   d) TraceRayInline(accel, runtimeFlags, instanceMask, ray)：第一个 uint 是附加
//      运行时 rayFlags（与模板 OR），第二个才是 instanceMask。
//   e) spvRayQueryKHR 在任何 SPIR-V 版本（含 1.6）都不是 profile 内置能力，需显式声明
//      （见 shader_compiler.cpp 的 Capability 选项）。
// ============================================================================

// ---------------------------------------------------------------------------
// 手动最近命中：遍历所有候选，跟踪最小 t
//   本项目对所有光线都不做背面剔除（介质内部必须能命中背面），
//   因此 RayQuery 模板旗标恒为 0。
// ---------------------------------------------------------------------------
bool trace_closest_hit(RayDesc ray, out uint model_index, out uint primitive_id, out float2 bary)
{
  var query = RayQuery<0>();
  query.TraceRayInline(top_level_acceleration_structure, 0u, 0xFFu, ray);

  bool  found  = false;
  float best_t = 1e30;

  while (query.Proceed())
  {
    float t = query.CandidateTriangleRayT();
    if (t < best_t)
    {
      best_t       = t;
      model_index  = query.CandidateInstanceIndex();
      primitive_id = query.CandidatePrimitiveIndex();
      bary         = query.CandidateTriangleBarycentrics();
      found        = true;
    }
  }

  return found;
}

// ---------------------------------------------------------------------------
// 阴影光线（与 RT 的 any-hit 统计等价）：沿光线遍历候选，逐个按 opacity
// 概率决定是否遮挡，未被遮挡则继续深入 —— 等价于 rayShadowAnyHitMain 的 IgnoreHit()。
// ---------------------------------------------------------------------------
bool trace_shadow_ray(float3 origin, float3 direction, float tmax, inout rng r)
{
  RayDesc ray;
  ray.Origin    = origin;
  ray.Direction = direction;
  ray.TMin      = SHADOW_EPSILON;
  ray.TMax      = max(tmax - SHADOW_EPSILON, SHADOW_EPSILON);

  var query = RayQuery<RAY_FLAG_SKIP_CLOSEST_HIT_SHADER>();
  query.TraceRayInline(top_level_acceleration_structure, 0u, 0xFFu, ray);

  while (query.Proceed())
  {
    // 计数上界守卫：0xFFFFFFFF（无材质）与越界 / 悬空索引都按不透明处理
    float opacity = 1.0;
    uint  model_index = query.CandidateInstanceIndex();
    if (model_index < scene_params.model_count)
    {
      uint material_index = scene_params.models[model_index].material_index;
      if (material_index < scene_params.material_count)
      {
        opacity = clamp(scene_params.materials[material_index].opacity, 0.0, 1.0);
      }
    }

    // 不透明 → 立即遮挡；半透明 → 按 opacity 概率遮挡，否则视为穿过并继续遍历
    if (opacity >= 1.0 || r.next_float() < opacity)
    {
      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// 入口文件侧的共享装配（bindless 数组是入口文件的全局绑定）
// ---------------------------------------------------------------------------
environment make_environment()
{
  environment env;
  env.skybox_index  = scene_params.skybox_index;
  env.texture_count = scene_params.texture_count;
  env.has_hdr       = env.skybox_index < env.texture_count;
  return env;
}

float3 environment_radiance(environment env, float3 direction)
{
  if (env.has_hdr)
  {
    return env.equirect_radiance(direction, samplers[env.skybox_index]);
  }
  return env.procedural_sky(direction);
}

float sample_pattern_weight(material_lookup lookup, float2 uv, out bool has_pattern)
{
  has_pattern = false;
  if (!lookup.valid || lookup.data.texture_index >= scene_params.texture_count)
  {
    return 0.0;
  }

  has_pattern = true;
  return samplers[lookup.data.texture_index].SampleLevel(uv, 0.0).x;
}

// ---------------------------------------------------------------------------
// 直接光照：光源 NEE + 环境光 NEE（与 BSDF 采样做 MIS）
// ---------------------------------------------------------------------------
float3 estimate_direct_lighting(shading_context ctx, resolved_surface surface, inout rng r)
{
  float3 direct        = float3(0.0);
  float3 shadow_origin = surface.position + surface.normal * SHADOW_EPSILON;

  for (uint i = 0; i < scene_params.light_count; ++i)
  {
    light_source light;
    light.data = scene_params.lights[i];
    if (!light.enabled())
    {
      continue;
    }

    light_sample ls = light.sample(surface.position);
    if (!ls.valid || all(ls.radiance <= 0.0))
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
  if (env_vis > 0.0)
  {
    float3 env_radiance = environment_radiance(env, es.direction);
    direct += nee_environment_contribution(ctx, es.direction, env_radiance, env_vis);
  }

  return direct;
}

// ============================================================================
// 主入口
// ============================================================================
[numthreads(8, 8, 1)]
[shader("compute")]
void rayQueryComputeMain(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
  uint image_width;
  uint image_height;
  render_output.GetDimensions(image_width, image_height);

  // dispatch 按 ceil(dim / 8) 分组 → 每维最多多出 7 个线程；
  // 越界写 / 读图像是未定义行为（未启用 robustImageAccess），必须显式提前返回。
  if (any(dispatch_thread_id.xy >= uint2(image_width, image_height)))
  {
    return;
  }

  uint2  launch_id   = dispatch_thread_id.xy;
  float2 launch_size = float2(image_width, image_height);

  // ---- 路径状态初始化 ----
  path_vertex path;
  path.radiance     = float3(0.0);
  path.throughput   = float3(1.0);
  path.bsdf_pdf     = 0.0;
  path.depth        = 0;
  path.ray_inside   = false;
  path.has_bsdf_pdf = false;  // 相机主光线没有 MIS 伙伴
  path.rng_state.reset(uint64_t(launch_id.x) * 1664525ull + uint64_t(launch_id.y) * 1013904223ull + uint64_t(scene_params.frame_index) * 0x9e3779b9ull);

  // ---- 主光线：子像素抖动 + 逆投影（与 rayGenMain 完全一致）----
  float2 clip = (float2(launch_id) + path.rng_state.next_float2()) / launch_size * 2.0 - 1.0;

  float4 near_point_v = mul(scene_params.proj_inv_matrix, float4(clip, 0.0, 1.0));
  float4 far_point_v  = mul(scene_params.proj_inv_matrix, float4(clip, 1.0, 1.0));
  near_point_v /= near_point_v.w;
  far_point_v  /= far_point_v.w;

  RayDesc ray;
  ray.Origin    = mul(scene_params.view_inv_matrix, near_point_v).xyz;
  ray.Direction = safe_normalize(mul(scene_params.view_inv_matrix, far_point_v).xyz - ray.Origin);
  ray.TMin      = SURFACE_EPSILON;
  ray.TMax      = RAY_TMAX;

  // ---- 路径追踪循环（与 rayGenMain + rayClosestHitMain + rayMissMain 等价）----
  for (int step = 0; step <= MAX_BOUNCES; ++step)
  {
    uint   model_index;
    uint   primitive_id;
    float2 bary;

    if (!trace_closest_hit(ray, model_index, primitive_id, bary))
    {
      // ---- Miss：逃逸到环境（与 rayMissMain 一致）----
      environment env          = make_environment();
      float3      env_radiance = environment_radiance(env, ray.Direction);
      path.radiance += environment_miss_contribution(path, env_radiance);
      break;
    }

    // ---- 几何与材质解析（与 rayClosestHitMain 一致）----
    gpu_model_data model = scene_params.models[model_index];

    hit_geometry geom = interpolate_hit(model, primitive_id, bary, ray.Origin, ray.Direction);

    material_lookup lookup = fetch_material(scene_params.materials, scene_params.material_count, model.material_index);

    bool  has_pattern    = false;
    float pattern_weight = sample_pattern_weight(lookup, geom.uv, has_pattern);
    lookup.has_pattern    = has_pattern;
    lookup.pattern_weight = pattern_weight;

    resolved_surface surface = make_surface(geom, lookup, ray.Direction);
    shading_context  ctx     = surface.make_context();

    // ---- 介质衰减 + 自发光 ----
    path.apply_medium_absorption(surface);
    path.add_emission(surface.material.emitted_radiance());

    // ---- 直接光照（NEE）----
    path.radiance += path.throughput * estimate_direct_lighting(ctx, surface, path.rng_state);

    // ---- 间接光照 ----
    if (path.depth >= MAX_BOUNCES)
    {
      break;
    }

    float rr_scale = russian_roulette_scale(path.rng_state, path.throughput, path.depth);
    if (rr_scale <= 0.0)
    {
      break;
    }

    bsdf_sample bs = ctx.sample(path.rng_state);
    if (!bs.valid)
    {
      break;
    }

    // ---- 介质状态推进（与 rayClosestHitMain 一致）----
    bool next_inside = false;
    if (bs.is_transmission)
    {
      next_inside = !surface.front_face;
    }
    else if (bs.is_delta)
    {
      next_inside = path.ray_inside;
    }

    float3 offset_normal = bs.is_transmission ? -surface.normal : surface.normal;

    // ---- 推进路径状态并继续循环 ----
    path.throughput   *= bs.weight * rr_scale;
    path.bsdf_pdf      = bs.pdf;
    path.depth        += 1;
    path.ray_inside    = next_inside;
    path.has_bsdf_pdf  = !bs.is_delta;

    ray.Origin    = surface.position + offset_normal * SURFACE_EPSILON;
    ray.Direction = bs.direction;
    ray.TMin      = SURFACE_EPSILON;
    ray.TMax      = RAY_TMAX;
  }

  // ---- 时域累积（严格样本均值 + 相机曝光）----
  write_accumulated(render_output, int2(launch_id), path.radiance, scene_params.frame_index, scene_params.exposure);
}
