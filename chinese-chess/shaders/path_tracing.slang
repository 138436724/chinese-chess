import shader_utils;
import vulkan_data_type;
import pbr;
import lighting;

// RT 与 Ray Query 共享的表面路径积分器；后端只提供求交与纹理读取。
// 数学上无固定路径截断；实现仍受浮点精度及离散几何表示的限制。

// ---------------------------------------------------------------------------
// hit_geometry：命中点的几何属性（顶点属性插值结果）
// ---------------------------------------------------------------------------
struct hit_geometry
{
  float3 position;
  float3 normal; // 着色法线
  float3 geometric_normal; // 几何外法线
  float2 uv;
  float distance;
};

// ---------------------------------------------------------------------------
// material_lookup：材质查找结果（含索引守卫与反照率贴图）
// ---------------------------------------------------------------------------
struct material_lookup
{
  bool                 valid;  // material_index < material_count
  vulkan_material_data data;
  bool                 has_albedo_map;  // 反照率贴图可用（texture_index < texture_count）
  float3               albedo_map;      // 贴图采样（RGB）

  // 反照率 = 材质基色 × 贴图（解析实现与光栅化路径共用 pbr.slang 的 resolve_albedo）
  float3 albedo() { return resolve_albedo(data, albedo_map, has_albedo_map); }

  // 转换成着色侧的材质形式（三套渲染路径共用 pbr.slang 的 make_material）
  pbr_material to_material() { return make_material(data, albedo()); }
};

// 默认材质（无材质实例时兜底：黑色不透明电介质）
vulkan_material_data default_material_data()
{
  vulkan_material_data md;
  md.albedo            = float3(0.0);
  md.texture_index     = 0xFFFFFFFFu;
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
material_lookup fetch_material(vulkan_material_data* materials, uint material_count, uint material_index)
{
  material_lookup lookup;
  lookup.valid          = material_index < material_count;
  lookup.has_albedo_map = false;
  lookup.albedo_map     = float3(0.0);
  lookup.data           = lookup.valid ? materials[material_index] : default_material_data();
  return lookup;
}


hit_geometry interpolate_hit(vulkan_model_data model, uint primitive_id, float2 bary, float3 origin, float3 direction)
{
  vulkan_model_vertex a = model.vertex_address[model.index_address[primitive_id * 3u]];
  vulkan_model_vertex b = model.vertex_address[model.index_address[primitive_id * 3u + 1u]];
  vulkan_model_vertex c = model.vertex_address[model.index_address[primitive_id * 3u + 2u]];
  hit_geometry hit;
  hit.position = mul(model.model_matrix, float4(interpolate(bary, a.position, b.position, c.position), 1)).xyz;
  hit.normal = transform_normal(model.model_matrix, interpolate(bary, a.normal, b.normal, c.normal));
  hit.geometric_normal = transform_normal(model.model_matrix, cross(b.position - a.position, c.position - a.position));
  if(dot(hit.normal, hit.geometric_normal) < 0) hit.normal = -hit.normal;
  hit.uv = interpolate(bary, a.uv, b.uv, c.uv);
  hit.distance = max(dot(hit.position - origin, direction), 0.0);
  return hit;
}

struct resolved_surface
{
  float3 position;
  float3 normal;
  float3 geometric_normal;
  float3 wo;
  float distance;
  bool front_face;
  pbr_material material;

  shading_context make_context()
  {
    shading_context ctx;
    ctx.material = material;
    ctx.ggx.n = normal;
    ctx.ggx.alpha = material.alpha();
    ctx.n = normal;
    ctx.geometric_n = geometric_normal;
    ctx.wo = wo;
    ctx.front_face = front_face;
    ctx.eta = front_face ? 1.0 / max(material.ior, 1.0) : max(material.ior, 1.0);
    return ctx;
  }
};

resolved_surface make_surface(hit_geometry hit, material_lookup lookup, float3 direction)
{
  resolved_surface s;
  s.position = hit.position;
  s.distance = hit.distance;
  s.wo = -direction;
  s.front_face = dot(hit.geometric_normal, s.wo) > 0;
  s.geometric_normal = s.front_face ? hit.geometric_normal : -hit.geometric_normal;
  s.normal = s.front_face ? hit.normal : -hit.normal;
  // 法线插值不应让 wo 落在着色半球背面；边界判定始终使用几何法线。
  if(dot(s.normal, s.wo) <= 0) s.normal = s.geometric_normal;
  s.material = lookup.to_material();
  return s;
}

struct trace_hit
{
  bool found;
  uint model_index;
  uint primitive_id;
  float2 bary;
  float distance;
};

trace_hit empty_hit()
{
  trace_hit h;
  h.found = false;
  h.model_index = 0xffffffffu;
  h.primitive_id = 0;
  h.bary = float2(0);
  h.distance = RAY_TMAX;
  return h;
}

// 两种求交后端只负责求交和纹理读取，积分、MIS、介质、RR 全部在此模块。
interface path_backend
{
  trace_hit trace(RayDesc ray, uint64_t seed);
  bool occluded(RayDesc ray, uint64_t seed);
  float3 texture(uint index, float2 uv);
}

// 每条光线、每个图元一个 Bernoulli 覆盖样本，与 BVH 候选遍历顺序无关。
// 主光线、间接光线、阴影光线必须使用同一规则；opacity 不等于玻璃 transmission。
bool accepts_coverage(vulkan_rt_scene_data scene, uint instance, uint primitive, uint64_t seed)
{
  if(instance >= scene.model_count) return true;
  uint material_index = scene.models[instance].material_index;
  if(material_index >= scene.material_count) return true;
  float opacity = clamp(scene.materials[material_index].opacity, 0.0, 1.0);
  if(opacity >= 1.0) return true;
  if(opacity <= 0.0) return false;
  rng r;
  r.reset(seed ^ (uint64_t(instance) << 32u) ^ uint64_t(primitive));
  return r.next_float() < opacity;
}

struct medium_state
{
  float ior;
  float3 absorption;
  float3 transmittance(float distance) { return exp(-absorption * distance); }
};

medium_state vacuum_medium()
{
  medium_state m;
  m.ior = 1.0;
  m.absorption = float3(0.0);
  return m;
}

medium_state material_medium(vulkan_material_data material)
{
  medium_state m;
  m.ior = max(material.ior, 1.0);
  m.absorption = max(material.absorption, float3(0.0));
  return m;
}

// 封闭、外法线一致、互不相交或严格嵌套的介质实例：从点发射探测射线，
// 各实例最近面为背面时点在内部。包含它的实例中，最近出口属于最内层介质。
// 初始相机与退出界面时查询，避免固定容量介质栈；不支持开放网格定义体积。
medium_state locate_medium(RaytracingAccelerationStructure accel, vulkan_rt_scene_data scene,
                           float3 origin, uint excluded_model = 0xffffffffu)
{
  medium_state medium = vacuum_medium();
  float nearest_exit = RAY_TMAX;
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = normalize(float3(0.3127, 0.7193, 0.6209));
  ray.TMin = 0.0;
  ray.TMax = RAY_TMAX;
  var candidates = RayQuery<RAY_FLAG_FORCE_NON_OPAQUE>();
  candidates.TraceRayInline(accel, 0u, 0xffu, ray);
  while(candidates.Proceed())
  {
    uint i = candidates.CandidateInstanceIndex();
    float candidate_t = candidates.CandidateTriangleRayT();
    if(i == excluded_model || i >= scene.model_count || candidate_t >= nearest_exit) continue;
    uint material_index = scene.models[i].material_index;
    if(material_index >= scene.material_count) continue;
    vulkan_material_data material = scene.materials[material_index];
    if(material.transmission <= 0 || material.opacity <= 0) continue;
    hit_geometry candidate = interpolate_hit(scene.models[i], candidates.CandidatePrimitiveIndex(),
      candidates.CandidateTriangleBarycentrics(), origin, ray.Direction);
    if(dot(candidate.geometric_normal, ray.Direction) <= 0) continue;

    // 只对真正遇到的出口候选验证包含关系，避免每个像素按全部模型重复遍历 TLAS。
    // 外部物体也有背面，必须确认本实例在它之前没有更近的正面，不能只取最近背面。
    RayDesc probe = ray;
    probe.TMax = candidate_t;
    var query = RayQuery<RAY_FLAG_FORCE_NON_OPAQUE>();
    query.TraceRayInline(accel, 0u, 0xffu, probe);
    float nearest = candidate_t;
    bool inside = true;
    while(query.Proceed())
    {
      if(query.CandidateInstanceIndex() != i) continue;
      float t = query.CandidateTriangleRayT();
      if(t >= nearest) continue;
      nearest = t;
      hit_geometry h = interpolate_hit(scene.models[i], query.CandidatePrimitiveIndex(),
                                      query.CandidateTriangleBarycentrics(), origin, ray.Direction);
      inside = dot(h.geometric_normal, ray.Direction) > 0;
    }
    if(inside && nearest < nearest_exit)
    {
      nearest_exit = nearest;
      medium = material_medium(material);
    }
  }
  return medium;
}

__generic<T : path_backend>
float3 path_environment(T backend, vulkan_rt_scene_data scene, float3 direction)
{
  environment env;
  env.environment_index = scene.environment_index;
  env.texture_count = scene.texture_count;
  env.has_hdr = env.environment_index < env.texture_count;
  env.environment_intensity = scene.environment_intensity;
  if(env.has_hdr)
  {
    float2 uv = float2(0.5 + 0.5 * atan2(direction.z, direction.x) * INV_PI,
                       1.0 - acos(clamp(direction.y, -1.0, 1.0)) * INV_PI);
    return backend.texture(env.environment_index, uv) * env.environment_intensity;
  }
  return env.procedural_sky(direction);
}

RayDesc make_shadow_ray(resolved_surface surface, float3 direction, float distance)
{
  RayDesc ray;
  ray.Origin = offset_ray_origin(surface.position, surface.geometric_normal);
  ray.Direction = direction;
  ray.TMin = 0.0;
  ray.TMax = distance;
  if(distance < RAY_TMAX)
  {
    float3 to_light = surface.position + direction * distance - ray.Origin;
    float length_to_light = length(to_light);
    ray.Direction = safe_normalize(to_light);
    ray.TMax = asfloat(max(asuint(length_to_light), 1u) - 1u);
  }
  return ray;
}

__generic<T : path_backend>
float3 estimate_direct(T backend, vulkan_rt_scene_data scene, resolved_surface surface,
                       shading_context ctx, medium_state medium, inout rng r)
{
  float3 direct = float3(0);
  for(uint i = 0; i < scene.light_count; ++i)
  {
    light_source light;
    light.data = scene.lights[i];
    if(!light.enabled()) continue;
    light_sample ls = light.sample(surface.position);
    if(!ls.valid || dot(surface.normal, ls.direction) <= 0 || dot(surface.geometric_normal, ls.direction) <= 0) continue;
    RayDesc shadow = make_shadow_ray(surface, ls.direction, ls.distance);
    if(!backend.occluded(shadow, r.next_uint64()))
      direct += medium.transmittance(ls.distance) * nee_light_contribution(ctx, ls, 1.0);
  }

  float3 direction = sample_uniform_sphere(r.next_float2());
  if(dot(surface.normal, direction) > 0 && dot(surface.geometric_normal, direction) > 0)
  {
    RayDesc shadow = make_shadow_ray(surface, direction, RAY_TMAX);
    if(!backend.occluded(shadow, r.next_uint64()))
      direct += medium.transmittance(RAY_TMAX) * nee_environment_contribution(
        ctx, direction, path_environment(backend, scene, direction), 1.0);
  }
  return direct;
}

// RR 的概率与存活补偿严格配对；上限小于 1 保证无硬深度限制时几乎必然终止。
float russian_roulette_scale(inout rng r, float3 throughput, float eta_scale, uint depth)
{
  if(depth < RR_START_DEPTH) return 1.0;
  float p = clamp(max_component(throughput) * eta_scale, RR_MIN_PROBABILITY, RR_MAX_PROBABILITY);
  return r.next_float() < p ? 1.0 / p : 0.0;
}

__generic<T : path_backend>
float3 trace_path(T backend, RaytracingAccelerationStructure accel, vulkan_rt_scene_data scene,
                  RayDesc ray, inout rng r)
{
  float3 radiance = float3(0);
  float3 throughput = float3(1);
  medium_state medium = locate_medium(accel, scene, ray.Origin);
  float eta_scale = 1.0;
  float previous_pdf = 0.0;
  bool has_previous_pdf = false;
  uint depth = 0;
  while(true)
  {
    trace_hit hit = backend.trace(ray, r.next_uint64());
    if(!hit.found)
    {
      radiance += throughput * medium.transmittance(RAY_TMAX)
        * path_environment(backend, scene, ray.Direction)
        * environment_miss_weight(has_previous_pdf, previous_pdf);
      break;
    }
    if(hit.model_index >= scene.model_count) break;
    vulkan_model_data model = scene.models[hit.model_index];
    hit_geometry geometry = interpolate_hit(model, hit.primitive_id, hit.bary, ray.Origin, ray.Direction);
    geometry.distance = hit.distance;
    material_lookup lookup = fetch_material(scene.materials, scene.material_count, model.material_index);
    if(lookup.valid && lookup.data.texture_index < scene.texture_count)
    {
      lookup.has_albedo_map = true;
      lookup.albedo_map = backend.texture(lookup.data.texture_index, geometry.uv);
    }
    resolved_surface surface = make_surface(geometry, lookup, ray.Direction);
    shading_context ctx = surface.make_context();
    // 刚经过的线段属于起点处的介质，与本次命中的物体材质无关。
    throughput *= medium.transmittance(hit.distance);
    radiance += throughput * surface.material.emitted_radiance();
    radiance += throughput * estimate_direct(backend, scene, surface, ctx, medium, r);

    medium_state destination = medium;
    if(surface.material.transmission_probability() > 0)
    {
      destination = surface.front_face ? material_medium(lookup.data)
        : locate_medium(accel, scene, offset_ray_origin(surface.position, -surface.geometric_normal), hit.model_index);
      ctx.eta = medium.ior / destination.ior;
    }
    float rr = russian_roulette_scale(r, throughput, eta_scale, depth);
    if(rr == 0) break;
    bsdf_sample bs = ctx.sample(r);
    if(!bs.valid) break;
    throughput *= bs.weight * rr;
    if(all(throughput == float3(0))) break;
    if(bs.is_transmission)
    {
      medium = destination;
      eta_scale /= ctx.eta * ctx.eta;
    }
    // 连续反射与全反射都留在原介质；只有透射穿越边界。
    previous_pdf = bs.pdf;
    has_previous_pdf = !bs.is_delta;
    depth = min(depth + 1u, uint(RR_START_DEPTH));
    ray.Origin = offset_ray_origin(surface.position, bs.is_transmission ? -surface.geometric_normal : surface.geometric_normal);
    ray.Direction = safe_normalize(bs.direction);
    ray.TMin = 0.0;
    ray.TMax = RAY_TMAX;
  }
  return radiance;
}

RayDesc camera_ray(vulkan_rt_scene_data scene, uint2 pixel, uint2 size, inout rng r)
{
  float2 clip = (float2(pixel) + r.next_float2()) / float2(size) * 2.0 - 1.0;
  float4 near_point = mul(scene.proj_inv_matrix, float4(clip, 0, 1));
  float4 far_point = mul(scene.proj_inv_matrix, float4(clip, 1, 1));
  near_point /= near_point.w;
  far_point /= far_point.w;
  RayDesc ray;
  ray.Origin = mul(scene.view_inv_matrix, near_point).xyz;
  ray.Direction = safe_normalize(mul(scene.view_inv_matrix, far_point).xyz - ray.Origin);
  ray.TMin = 0.0;
  ray.TMax = RAY_TMAX;
  return ray;
}

rng pixel_rng(uint2 pixel, uint64_t sample_index)
{
  rng r;
  r.state = 0ull;
  r.inc = 1ull;
  r.reset((uint64_t(pixel.x) << 32u) ^ uint64_t(pixel.y) ^ r.splitmix64(sample_index));
  return r;
}

void write_accumulated(RWTexture2D<float4> accumulation, RWTexture2D<float4> output,
                       int2 pixel, float3 sample_color, uint64_t sample_index, float exposure)
{
  float3 mean = sample_color;
  // 必须在加载图像之前分支：未初始化内容以及旧 NaN 都不能参与首样本运算。
  if(sample_index != 0ull)
    mean = accumulate_sample(accumulation[pixel].rgb, sample_color, sample_index);
  accumulation[pixel] = float4(mean, 1);
  output[pixel] = float4(mean * exposure, 1);
}
