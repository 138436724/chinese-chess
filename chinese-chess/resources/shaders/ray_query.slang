import common;
import scene_data;
import lighting;

// ============================================================================
// ray_query.slang — compute + ray query 路径追踪（与 RT pipeline 行为一致）
//   渲染行为与 ray_tracing.slang（RT pipeline）对齐：NEE 直接光 + 环境光、
//   间接弹射（透射/GGX/Disney + 俄罗斯轮盘赌）、高度雾、HDR/程序化天空、
//   时域累积（子像素抖动 + firefly 钳制）。
//   差异（Slang 2026.7.1 RayQuery 限制所致，见下方"实验发现"）：
//     - 主光线与弹射用"手动最近命中"（遍历跟踪最小 t 的候选）替代硬件 committed；
//     - 阴影射线用 ray query + opacity 概率穿透（统计等价于 RT 的 any-hit）；
//     - 弹射用循环而非递归（路径追踪本就等价）。
//   遍历旗标与 pipeline 一致：主光线 CULL_BACK_FACING；透射弹射不剔除背面。
// ============================================================================

// ---- 恒定环境光与雾/透射参数（与 ray_tracing.slang 同值，保证输出一致）----
static const float AMBIENT_LIGHT = 0.25f;

static const float  FOG_DENSITY   = 0.005f;
static const float  FOG_FALLOFF   = 50.0f;
static const float3 FOG_COLOR     = float3(0.7f, 0.75f, 0.82f);
static const float3 FOG_SUN_COLOR = float3(1.0f, 0.95f, 0.8f);
static const float3 FOG_SUN_DIR   = float3(0.3f, 0.6f, 0.7f);

static const float PIECE_THICKNESS         = 0.04f;
static const float JADE_BASE_ABSORPTION    = 12.0f;
static const float JADE_ENGRAVE_ABSORPTION = 30.0f;
static const float MAX_THROUGHPUT          = 10.0f;  // 路径钳制（与 ray_tracing.slang 同值）

// ---- 描述符布局（与 scene_rayquery_render.cpp 一致）----
RaytracingAccelerationStructure top_level_acceleration_structure;  // b0
RWTexture2D<float4> render_output;                                 // b1
ConstantBuffer<rt_scene_data> scene_params;
Sampler2D[]                     samplers;                         // b2(bindless 纹理数组)
// b3: 场景参数 UBO(声明序自动分配)→ 相机逆矩阵 + models/materials/lights 基址 + 计数
// models/materials/lights 不再单独绑定:经 scene_params.scene_params.models[i] 等指针(设备地址)直接解引用;
// push constant 已废弃(原 rt_push_constant 144B 超规范下限 → 见 Known #1/H2)

// ============================================================================
// 实验发现（2026.7.1 版 Slang，slangc + SPIR-V 反汇编 + 运行时诊断证实）：
//   a) 编译期：RayQuery 未暴露候选确认 API（CandidateProceed/CommittedProceed/
//      ConfirmIntersection/Terminate/GetIntersectionType 均不存在）。
//   b) 运行时：CommittedStatus() 恒返回 None（候选存在但提交状态拿不到），
//      故依赖提交状态的判定全部走 miss → 必须"手动最近命中"。
//   c) CandidateInstanceID() 被降级为 OpRayQueryGetIntersectionInstanceCustomIndexKHR
//      （实例自定义索引，常量）→ 需用 CandidateInstanceIndex()（InstanceIdKHR）。
//   d) TraceRayInline(accel, runtimeFlags, instanceMask, ray)：第一个 uint 是附加运行时
//      rayFlags（与模板 OR），第二个才是 instanceMask。
//   e) spvRayQueryKHR 在任何 SPIR-V 版本（含 1.6）都不是 profile 内置能力，需显式声明。
// ============================================================================

// Miss 颜色：程序化天空优先，HDR 环境贴图可选（与 rayMissMain 一致）
float3 sample_sky_color(float3 direction)
{
  float3 sky_color = sample_sky(direction);
  // 天空索引守卫：0xFFFFFFFF（无 HDR 天空）恒 ≥ texture_count → 程序化回退
  if(scene_params.skybox_index < scene_params.texture_count)
  {
    float3 hdr_color = sample_hdr_sky(direction, samplers[scene_params.skybox_index]);
    if(luminance(hdr_color) > 0.02f)
      sky_color = hdr_color;
  }
  return sky_color;
}

// 手动最近命中：遍历所有候选，跟踪最小 t（CandidateTriangleRayT）。
// 旗标为编译期模板参数：CULL_BACK_FACING（主光线/不透明弹射）或 0（透射弹射）。
__generic<let ray_flags : int>
bool trace_closest_hit(RayDesc ray, out uint model_index, out uint primitive_id, out float2 bary)
{
  var query = RayQuery<ray_flags>();
  query.TraceRayInline(top_level_acceleration_structure, 0u, 0xFFu, ray);  // 掩码 0xFF 与 pipeline 一致

  bool  found  = false;
  float best_t = 1e30f;
  while(query.Proceed())
  {
    float t = query.CandidateTriangleRayT();
    if(t < best_t)
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

// 阴影（统计等价于 RT 的 any-hit 概率穿透）：取首个候选，半透明材质按 opacity 概率遮挡
bool shadow_ray(RaytracingAccelerationStructure tlas, float3 origin, float3 direction, float tmax, inout uint seed)
{
  RayDesc shadow_ray;
  shadow_ray.Origin    = origin;
  shadow_ray.Direction = direction;
  shadow_ray.TMin      = SHADOW_EPSILON;
  shadow_ray.TMax      = tmax - SHADOW_EPSILON;

  var query = RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_CULL_BACK_FACING_TRIANGLES>();
  query.TraceRayInline(tlas, 0u, 0xFFu, shadow_ray);

  uint model_index = 0xFFFFFFFFu;
  while(query.Proceed())
  {
    model_index = query.CandidateInstanceIndex();
    break;
  }
  if(model_index == 0xFFFFFFFFu)
    return false;

  // 阴影只读材质不透明度:按需取字段,避免整拷 model_data(阴影光线频率高)
  float    opacity        = 1.0f;
  uint32_t material_index = scene_params.models[model_index].material_index;
  // 计数上界守卫：0xFFFFFFFF（无材质）与越界/悬空索引都按不透明处理
  if(material_index < scene_params.material_count)
  {
    opacity = scene_params.materials[material_index].opacity;
  }
  return random_float(seed) < opacity;  // 遮挡概率 = opacity（与 rayShadowAnyHitMain 一致）
}

[numthreads(8, 8, 1)]
void rayQueryComputeMain(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
  uint image_width, image_height;
  render_output.GetDimensions(image_width, image_height);

  const float2 launch_id   = float2(dispatch_thread_id.xy);
  const float2 launch_size = float2(image_width, image_height);

  // RNG 种子（SPIR-V 1.6 整数点积 OpUDot；与 raygen 的像素+帧号种子等价）
  uint seed = wang_hash(dot(uint3(dispatch_thread_id.x, dispatch_thread_id.y, scene_params.frame_index),
                            uint3(1664525u, 1013904223u, 0x9e3779b9u)));

  // 子像素抖动（与 raygen 一致，时域累积抗锯齿）
  float2 clip_coords = (launch_id + random_float2(seed)) / launch_size * 2.f - 1.f;

  float4 near_point_v = mul(scene_params.proj_inv_matrix, float4(clip_coords, 0.0f, 1.0f));
  float4 far_point_v  = mul(scene_params.proj_inv_matrix, float4(clip_coords, 1.0f, 1.0f));
  near_point_v /= near_point_v.w;
  far_point_v  /= far_point_v.w;

  RayDesc ray;
  ray.Origin    = mul(scene_params.view_inv_matrix, near_point_v).xyz;
  ray.Direction = normalize(mul(scene_params.view_inv_matrix, far_point_v).xyz - ray.Origin);
  ray.TMin      = 0.001f;
  ray.TMax      = 1e23f;

  // ---- 路径追踪循环（与 ray_tracing.slang 的 raygen + closest-hit + miss 等价）----
  float3 radiance   = float3(0.f);
  float3 throughput = float3(1.f);
  bool   inside_transmissive = false;
  bool   ray_is_transmission = false;  // 当前光线是否透射（决定遍历旗标：透射不剔除背面）

  for(int depth = 0; depth <= MAX_BOUNCES; depth++)
  {
    uint   model_index, primitive_id;
    float2 bary;
    bool   found = ray_is_transmission ? trace_closest_hit<0>(ray, model_index, primitive_id, bary)
                                       : trace_closest_hit<RAY_FLAG_CULL_BACK_FACING_TRIANGLES>(ray, model_index, primitive_id, bary);

    if(!found)
    {
      // Miss（与 rayMissMain 一致）：天空 + 高度雾
      float3 sky_color = sample_sky_color(ray.Direction);
      if(FOG_DENSITY > 0.0f)
      {
        fog_result fog = evaluate_height_fog(ray.Origin, ray.Direction, 1e30f, FOG_DENSITY, FOG_FALLOFF, FOG_COLOR, FOG_SUN_COLOR, FOG_SUN_DIR);
        radiance = radiance * fog.transmittance + (sky_color * fog.transmittance + fog.inscatter) * throughput;
      }
      else
      {
        radiance += throughput * sky_color;
      }
      break;
    }

    model_data now_model = scene_params.models[model_index];

    uint i0 = now_model.index_address[primitive_id * 3u + 0u];
    uint i1 = now_model.index_address[primitive_id * 3u + 1u];
    uint i2 = now_model.index_address[primitive_id * 3u + 2u];

    Vertex v0 = now_model.vertex_address[i0];
    Vertex v1 = now_model.vertex_address[i1];
    Vertex v2 = now_model.vertex_address[i2];

    float3 world_position = mul(now_model.model_matrix, float4(interpolate(bary, v0.position, v1.position, v2.position), 1.0f)).xyz;
    float3 world_normal   = normalize(mul(now_model.model_matrix, float4(interpolate(bary, v0.normal, v1.normal, v2.normal), 0.0f)).xyz);
    float2 uv             = interpolate(bary, v0.uv, v1.uv, v2.uv);

    // ---- 材质解析（与 rayClosestHitMain 一致；material_index < material_count 守卫，
    // 覆盖 0xFFFFFFFF 无材质哨兵与越界/悬空索引）----
    float3 material_color        = float3(0.f);
    float  material_roughness    = 0.5f;
    float  material_metallic     = 0.0f;
    float  material_opacity      = 1.0f;
    float  material_ior          = 1.5f;
    float  material_transmission = 0.0f;
    float  texture_weight        = 0.f;

    if(now_model.material_index < scene_params.material_count)
    {
      material_data now_material = scene_params.materials[now_model.material_index];
      material_roughness         = now_material.roughness;
      material_metallic          = now_material.metallic;
      material_opacity           = now_material.opacity;
      material_ior               = now_material.ior;
      material_transmission      = now_material.transmission;
      if(now_material.texture_index < scene_params.texture_count)
      {
        float weight   = samplers[now_material.texture_index].SampleLevel(uv, 0.0f).x;
        material_color = lerp(now_material.background_color, now_material.foreground_color, weight);
        texture_weight = weight;
      }
      else
      {
        material_color = now_material.background_color;
      }
    }

    // ---- 高度雾：衰减已累积辐射（与 rayClosestHitMain 一致）----
    if(FOG_DENSITY > 0.0f)
    {
      float      bounce_dist = length(ray.Origin - world_position);
      fog_result fog = evaluate_height_fog(ray.Origin, normalize(world_position - ray.Origin), bounce_dist,
                                           FOG_DENSITY, FOG_FALLOFF, FOG_COLOR, FOG_SUN_COLOR, FOG_SUN_DIR);
      radiance = radiance * fog.transmittance + fog.inscatter * throughput;
    }

    // ---- 直接光照：NEE（与 rayClosestHitMain 一致）----
    float3 V     = normalize(ray.Origin - world_position);
    float3 F0    = lerp(float3(0.04f), material_color, material_metallic);
    float  NdotV = max(dot(world_normal, V), 0.001f);
    float3 kS    = f_schlick(F0, NdotV);
    float  clamped_roughness = max(material_roughness, 0.04f);

    float3 direct_lighting = float3(0.f);
    for(uint li = 0; li < scene_params.light_count; li++)
    {
      light_data light = scene_params.lights[li];
      if(light.intensity <= 0.0f)
        continue;

      light_sample ls = sample_light(light, world_position, seed);
      if(all(ls.color <= 0.0f))
        continue;

      float NdotL = max(dot(world_normal, ls.direction), 0.0f);
      if(NdotL <= 0.0f)
        continue;

      float3 shadow_origin = world_position + world_normal * SHADOW_EPSILON;
      if(shadow_ray(top_level_acceleration_structure, shadow_origin, ls.direction, ls.distance, seed))
        continue;

      float3 diffuse  = (float3(1.0f) - kS) * (1.0f - material_metallic) * material_color * INV_PI;
      float3 specular = evaluate_ggx(F0, clamped_roughness, world_normal, V, ls.direction);
      float  w_light  = 1.0f;  // delta 光源 MIS 权重恒 1（与 pipeline 一致）

      float3 contribution = ls.color * (diffuse + specular) * NdotL * w_light;
      float  contrib_lum  = luminance(contribution);
      float  clamp_max    = min(luminance(throughput) * 100.0f, 100.0f);
      if(contrib_lum > clamp_max)
      {
        contribution *= clamp_max / max(contrib_lum, 1e-6f);
      }
      direct_lighting += contribution;
    }
    float3 ambient = material_color * AMBIENT_LIGHT * (0.5f + 0.5f * NdotV);
    direct_lighting += ambient;

    radiance += throughput * direct_lighting;

    // ---- 俄罗斯轮盘赌（与 rayClosestHitMain 一致）----
    if(depth >= MAX_BOUNCES)
      break;

    float throughput_lum = luminance(throughput);
    float p_continue     = min(1.f, throughput_lum);
    if(p_continue < 0.05f || random_float(seed) > p_continue)
      break;

    // ---- 波瓣选择：透射/GGX 镜面/Disney 漫反射（与 rayClosestHitMain 一致）----
    float3 ray_dir  = ray.Direction;
    float3 N_face   = dot(ray_dir, world_normal) < 0.f ? world_normal : -world_normal;
    bool   is_entry = !inside_transmissive;

    float trans_prob = clamp(material_opacity * material_transmission, 0.f, 1.f);
    if(trans_prob <= 0.f)
    {
      inside_transmissive = false;
    }

    float f0_dielectric = (material_ior - 1.f) / (material_ior + 1.f);
    f0_dielectric *= f0_dielectric;
    float F_interface   = f0_dielectric + (1.f - f0_dielectric) * pow5(1.f - abs(dot(ray_dir, N_face)));

    float specular_prob = clamp(lerp(max_component(kS), 1.f, material_metallic), 0.05f, 0.95f);
    specular_prob *= (1.f - trans_prob);

    bool   is_transmission = false;
    float3 bounce_direction;
    float3 lobe_value;
    float  lobe_prob;

    float r = random_float(seed);
    if(r < trans_prob)
    {
      is_transmission = true;
      lobe_prob       = trans_prob;
      float  eta      = is_entry ? (1.f / max(material_ior, 1.01f)) : max(material_ior, 1.01f);
      float3 T        = refract(ray_dir, N_face, eta);
      if(dot(T, T) < 1e-8f)
      {
        bounce_direction      = reflect(ray_dir, N_face);
        lobe_value            = float3(1.f);
      }
      else
      {
        bounce_direction            = T;
        inside_transmissive         = !inside_transmissive;
        float  absorption_density   = JADE_BASE_ABSORPTION + JADE_ENGRAVE_ABSORPTION * texture_weight;
        float3 absorption           = is_entry ? exp(-absorption_density * PIECE_THICKNESS).xxx : float3(1.f);
        lobe_value                  = (1.f - F_interface) * absorption;
      }
    }
    else if(r < trans_prob + specular_prob)
    {
      brdf_sample bs = sample_ggx_brdf(material_color, F0, clamped_roughness, world_normal, V, seed);
      if(bs.pdf <= 0.f || dot(world_normal, bs.direction) <= 0.f)
        break;
      bounce_direction = bs.direction;
      lobe_value       = bs.value;
      lobe_prob        = specular_prob;
    }
    else
    {
      brdf_sample bs = sample_disney_diffuse(material_color, clamped_roughness, world_normal, V, seed);
      if(bs.pdf <= 0.f || dot(world_normal, bs.direction) <= 0.f)
        break;
      bounce_direction = bs.direction;
      lobe_value       = bs.value;
      lobe_prob        = 1.f - trans_prob - specular_prob;
    }

    // 吞吐量更新（与 pipeline 一致：× lobe_value / (lobe_prob × p_continue)）
    float3 new_throughput = throughput * lobe_value / (lobe_prob * p_continue);
    float  tput_lum       = luminance(new_throughput);
    if(tput_lum > MAX_THROUGHPUT)
    {
      new_throughput *= MAX_THROUGHPUT / tput_lum;
    }
    throughput = new_throughput;

    // 弹射光线（透射不剔除背面，与 bounce_ray_flags 一致）
    ray.Origin          = world_position + N_face * 0.001f;
    ray.Direction       = bounce_direction;
    ray.TMin            = 0.001f;
    ray.TMax            = 1e23f;
    ray_is_transmission = is_transmission;
  }

  // ---- 时域累积（与 rayGenMain 一致；firefly 钳制仅在累积帧生效，见 ray_tracing.slang）----
  float3 prev_color = render_output[int2(launch_id)].rgb;

  if(scene_params.frame_index > 0u)
  {
    float prev_lum = max(luminance(prev_color), 0.01f);
    float new_lum  = luminance(radiance);
    if(new_lum > prev_lum * 4.0f)
    {
      radiance *= (prev_lum * 4.0f) / max(new_lum, 1e-6f);
    }
  }

  float  alpha       = 1.0f / (1.0f + float(scene_params.frame_index) * 0.5f);
  float3 final_color = lerp(prev_color, radiance, alpha);

  render_output[int2(launch_id)] = float4(final_color, 1.f);
}
