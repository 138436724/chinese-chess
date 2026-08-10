import common;
import scene_data;

// ============================================================================
// lighting.slang — 光照库模块
// 光源数据结构在 scene_data.slang 中定义；本模块只负责：
//   光源采样（NEE）、BRDF 评估/采样/PDF、阴影光线、MIS 权重、天空采样、高度雾
// 被 ray_tracing.slang（和 scene_brdflut.slang）复用。
// ============================================================================

enum light_type : uint32_t
{
  directional = 0,
  point       = 1,
  spot        = 2
};

// ---- BRDF 类型枚举 ----
enum brdf_type : uint32_t
{
  lambertian     = 0,
  ggx            = 1,
  disney_diffuse = 2  // Burley 2012 Disney Diffuse（粗糙表面掠射角柔化）
};

// ---- 光照采样结果（NEE 采样返回值） ----
struct light_sample
{
  float3 direction;  // 从命中点到光源的方向（单位向量）
  float  distance;   // 命中点到光源的距离（用于阴影光线 tmax）
  float3 color;      // 光源在该采样方向的辐射亮度
  float  pdf;        // 采样概率密度函数值（用于 MIS）
};

// ---- BRDF 采样结果（BSDF 采样返回值） ----
struct brdf_sample
{
  float3 direction;  // 采样出的出射方向（单位向量，世界空间）
  float  pdf;        // 采样概率密度函数值（用于 MIS）
  float3 value;      // BRDF 值 × cos_theta / pdf（用于路径吞吐量累积）
};

// ---- 阴影光线载荷（轻量级，仅需知道是否被遮挡） ----
struct shadow_payload
{
  bool is_shadowed;
  uint seed;  // 概率阴影（半透明穿透）用随机种子，与主路径种子解耦
};

// ---- 半球均匀采样（以法线为中心的切线空间） ----
float3 sample_hemisphere_uniform(float3 N, inout uint seed)
{
  float2 r     = random_float2(seed);
  float  z     = r.x;
  float  r_xy  = sqrt(max(0.0, 1.0 - z * z));
  float  phi   = 2.0 * PI * r.y;
  float3 local = float3(r_xy * cos(phi), r_xy * sin(phi), z);

  tangent_space ts = build_tangent_space(N);
  return ts.T * local.x + ts.B * local.y + N * local.z;
}

// ---- 余弦加权半球采样（重要性采样 Lambertian BRDF） ----
float3 sample_hemisphere_cosine(float3 N, inout uint seed)
{
  float2 r        = random_float2(seed);
  float  phi      = 2.0 * PI * r.x;
  float  cosTheta = sqrt(r.y);
  float  sinTheta = sqrt(max(0.0, 1.0 - r.y));
  float3 local    = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

  tangent_space ts = build_tangent_space(N);
  return ts.T * local.x + ts.B * local.y + N * local.z;
}

// ============================================================================
// 光源采样函数（NEE 核心）
// ============================================================================

// 方向光为 delta 分布（pdf = 1），方向恒定
light_sample sample_directional_light(light_data light, float3 hit_pos, inout uint seed)
{
  light_sample ls;
  ls.direction = normalize(light.direction);
  ls.color     = light.color * light.intensity;
  ls.pdf       = 1.0;   // delta 分布
  ls.distance  = 1e10;  // 无穷远
  return ls;
}

// 点光源为 delta 分布，含距离平方衰减与 range 线性裁剪
light_sample sample_point_light(light_data light, float3 hit_pos, inout uint seed)
{
  light_sample ls;
  float3       to_light = light.position - hit_pos;
  float        dist     = length(to_light);
  ls.direction          = to_light / dist;
  ls.distance           = dist;

  float attenuation = 1.0 / max(dist * dist, 0.001);
  float range_atten = (light.range > 0.0) ? max(0.0, 1.0 - dist / light.range) : 1.0;
  ls.color          = light.color * light.intensity * attenuation * range_atten;
  ls.pdf            = 1.0;  // delta 分布（确定性采样）
  return ls;
}

// 聚光灯为 delta 分布，含内外锥角平滑衰减
light_sample sample_spot_light(light_data light, float3 hit_pos, inout uint seed)
{
  light_sample ls;
  float3       to_light = light.position - hit_pos;
  float        dist     = length(to_light);
  ls.direction          = to_light / dist;
  ls.distance           = dist;

  float3 light_forward = normalize(light.direction);
  float  cos_angle     = dot(-ls.direction, light_forward);

  float cone_atten = 1.0;
  if(cos_angle < cos(light.outer_cone_angle))
  {
    // 完全在锥外，无光照
    ls.color = float3(0.0);
    ls.pdf   = 1.0;
    return ls;
  }
  if(cos_angle < cos(light.inner_cone_angle))
  {
    // 内外锥之间平滑过渡（二次衰减）
    float t = (cos_angle - cos(light.outer_cone_angle)) / max(cos(light.inner_cone_angle) - cos(light.outer_cone_angle), 0.001);
    cone_atten = t * t;
  }

  float attenuation = 1.0 / max(dist * dist, 0.001);
  float range_atten = (light.range > 0.0) ? max(0.0, 1.0 - dist / light.range) : 1.0;
  ls.color          = light.color * light.intensity * attenuation * range_atten * cone_atten;
  ls.pdf            = 1.0;  // delta 分布
  return ls;
}

// 按 light.active_type 调度到对应的采样函数
light_sample sample_light(light_data light, float3 hit_pos, inout uint seed)
{
  switch(light.active_type)
  {
    case light_type::directional:
      return sample_directional_light(light, hit_pos, seed);
    case light_type::point:
      return sample_point_light(light, hit_pos, seed);
    case light_type::spot:
      return sample_spot_light(light, hit_pos, seed);
    default:
      light_sample ls;
      ls.direction = float3(0.0, 1.0, 0.0);
      ls.color     = float3(0.0);
      ls.pdf       = 1.0;
      ls.distance  = 0.0;
      return ls;
  }
}

// ============================================================================
// BRDF 函数族（Lambertian / Disney Diffuse / GGX）
// ============================================================================

// Lambertian BRDF 评估：f_r = albedo / PI
float3 evaluate_lambertian(float3 albedo)
{
  return albedo * INV_PI;
}

// Lambertian BRDF 采样（余弦加权半球）：value = albedo（cos/pdf 相消）
brdf_sample sample_lambertian(float3 albedo, float3 N, inout uint seed)
{
  brdf_sample bs;
  bs.direction = sample_hemisphere_cosine(N, seed);
  bs.value     = albedo;
  bs.pdf       = max(0.0, dot(N, bs.direction)) * INV_PI;
  return bs;
}

// Lambertian PDF
float pdf_lambertian(float3 N, float3 L)
{
  return max(0.0, dot(N, L)) * INV_PI;
}

// Disney Diffuse BRDF 评估（Burley 2012）：粗糙表面掠射角变亮
float3 evaluate_disney_diffuse(float3 albedo, float roughness, float3 N, float3 V, float3 L, float3 H)
{
  float NdotL = max(dot(N, L), 0.0f);
  float NdotV = max(dot(N, V), 0.0f);
  if(NdotL <= 0.0f || NdotV <= 0.0f)
    return float3(0.0f);

  float LdotH = max(dot(L, H), 0.0f);
  float FD90  = 0.5f + 2.0f * roughness * LdotH * LdotH;

  float nl5 = pow5(1.0f - NdotL);
  float nv5 = pow5(1.0f - NdotV);
  float f   = (1.0f + (FD90 - 1.0f) * nl5) * (1.0f + (FD90 - 1.0f) * nv5);

  return albedo * INV_PI * f;
}

// Disney Diffuse 采样（与 Lambert 同分布，用评估值折算吞吐量）
brdf_sample sample_disney_diffuse(float3 albedo, float roughness, float3 N, float3 V, inout uint seed)
{
  brdf_sample bs;
  bs.direction = sample_hemisphere_cosine(N, seed);
  float NdotL  = max(dot(N, bs.direction), 0.0f);
  if(NdotL <= 0.0f)
  {
    bs.value = float3(0.0f);
    bs.pdf   = 0.0f;
    return bs;
  }

  float3 H = normalize(V + bs.direction);
  bs.value = evaluate_disney_diffuse(albedo, roughness, N, V, bs.direction, H) * NdotL / pdf_lambertian(N, bs.direction);
  bs.pdf = pdf_lambertian(N, bs.direction);
  return bs;
}

// GGX / Trowbridge-Reitz 法线分布函数
float d_ggx(float NdotH, float roughness)
{
  float a     = roughness * roughness;
  float a2    = a * a;
  float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
  return a2 * INV_PI / max(denom * denom, 1e-5);
}

// Smith 几何遮蔽（Schlick-GGX 近似）
float g_schlick_ggx(float NdotV, float roughness)
{
  float a = roughness * roughness;
  float k = a / 2.0;
  return NdotV / max(NdotV * (1.0 - k) + k, 1e-5);
}

float g_smith(float NdotV, float NdotL, float roughness)
{
  return g_schlick_ggx(NdotV, roughness) * g_schlick_ggx(NdotL, roughness);
}

// Fresnel-Schlick
float3 f_schlick(float3 F0, float VdotH)
{
  return F0 + (float3(1.0) - F0) * pow5(max(1.0 - VdotH, 0.0));
}

// GGX BRDF 评估：fr = D*G*F / (4*NdotV*NdotL)，调用方乘以 NdotL 与 Li
float3 evaluate_ggx(float3 F0, float roughness, float3 N, float3 V, float3 L)
{
  float3 H     = normalize(V + L);
  float  NdotH = max(dot(N, H), 0.0);
  float  NdotV = max(dot(N, V), 0.0);
  float  NdotL = max(dot(N, L), 0.0);

  if(NdotL <= 0.0 || NdotV <= 0.0)
    return float3(0.0);

  float  D = d_ggx(NdotH, roughness);
  float  G = g_smith(NdotV, NdotL, roughness);
  float3 F = f_schlick(F0, max(dot(V, H), 0.0));

  return D * G * F / max(4.0 * NdotV * NdotL, 0.001);
}

// GGX 重要性采样（反射方向）：采样半向量再反射出射方向
float3 sample_ggx(float3 V, float roughness, float3 N, inout uint seed)
{
  float2 r = random_float2(seed);
  float  a = roughness * roughness;

  float  phi      = 2.0 * PI * r.x;
  float  cosTheta = sqrt(max(0.0, (1.0 - r.y) / (1.0 + (a * a - 1.0) * r.y)));
  float  sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
  float3 hLocal   = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

  tangent_space ts = build_tangent_space(N);
  float3        H  = ts.T * hLocal.x + ts.B * hLocal.y + N * hLocal.z;
  return normalize(reflect(-V, H));
}

// GGX BRDF 采样：pdf = D*NdotH/(4*VdotH)，value = fr*NdotL/pdf
brdf_sample sample_ggx_brdf(float3 albedo, float3 F0, float roughness, float3 N, float3 V, inout uint seed)
{
  brdf_sample bs;
  float3      L = sample_ggx(V, roughness, N, seed);
  bs.direction  = L;

  float NdotL = max(dot(N, L), 0.0);
  if(NdotL <= 0.0)
  {
    bs.value = float3(0.0);
    bs.pdf   = 0.0;
    return bs;
  }

  float3 H     = normalize(V + L);
  float  NdotV = max(dot(N, V), 0.0);
  float  NdotH = max(dot(N, H), 0.0);
  float  VdotH = max(dot(V, H), 0.001);

  bs.pdf   = d_ggx(NdotH, roughness) * NdotH / max(4.0 * VdotH, 0.001);
  bs.value = evaluate_ggx(F0, roughness, N, V, L) * NdotL / max(bs.pdf, 0.001);
  return bs;
}

// GGX PDF（给定出射方向 L）
float pdf_ggx(float3 N, float3 V, float3 L, float roughness)
{
  float3 H     = normalize(V + L);
  float  NdotH = max(dot(N, H), 0.0);
  float  NdotL = max(dot(N, L), 0.0);
  float  VdotH = max(dot(V, H), 0.001);

  if(NdotL <= 0.0)
    return 0.0;

  return d_ggx(NdotH, roughness) * NdotH / max(4.0 * VdotH, 0.001);
}

// 统一 BRDF 评估（按类型调度）
float3 evaluate_brdf(uint type, float3 albedo, float3 F0, float3 N, float3 V, float3 L, float roughness)
{
  switch(type)
  {
    case brdf_type::lambertian:
      return evaluate_lambertian(albedo);
    case brdf_type::ggx:
      return evaluate_ggx(F0, roughness, N, V, L);
    case brdf_type::disney_diffuse: {
      float3 H = normalize(V + L);
      return evaluate_disney_diffuse(albedo, roughness, N, V, L, H);
    }
    default:
      return evaluate_lambertian(albedo);
  }
}

// 统一 BRDF 采样（按类型调度）
brdf_sample sample_brdf(uint type, float3 albedo, float3 F0, float3 N, float3 V, float roughness, inout uint seed)
{
  switch(type)
  {
    case brdf_type::lambertian:
      return sample_lambertian(albedo, N, seed);
    case brdf_type::ggx:
      return sample_ggx_brdf(albedo, F0, roughness, N, V, seed);
    case brdf_type::disney_diffuse:
      return sample_disney_diffuse(albedo, roughness, N, V, seed);
    default:
      return sample_lambertian(albedo, N, seed);
  }
}

// 统一 BRDF PDF（按类型调度）
float pdf_brdf(uint type, float3 N, float3 V, float3 L, float roughness)
{
  switch(type)
  {
    case brdf_type::lambertian:
    case brdf_type::disney_diffuse:  // 分布与 Lambert 相同
      return pdf_lambertian(N, L);
    case brdf_type::ggx:
      return pdf_ggx(N, V, L, roughness);
    default:
      return pdf_lambertian(N, L);
  }
}

// ============================================================================
// 阴影光线追踪
// ============================================================================

// 封装 TraceRay 调用：命中组 1（anyhit_shadow），miss 索引 1（miss_shadow）
bool trace_shadow_ray(RaytracingAccelerationStructure tlas, float3 origin, float3 direction, float tmax, inout uint seed)
{
  const uint shadow_ray_flags =
      RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

  RayDesc ray;
  ray.Origin    = origin;
  ray.Direction = direction;
  ray.TMin      = SHADOW_EPSILON;
  ray.TMax      = tmax - SHADOW_EPSILON;

  shadow_payload payload;
  payload.is_shadowed = false;
  payload.seed        = wang_hash(seed);  // 与主路径种子解耦，不扰动调用方 RNG

  TraceRay(tlas, shadow_ray_flags, 0xff,
           1,  // RayContributionToHitGroupIndex: shadow hit group
           0,  // MultiplierForGeometryContributionToHitGroupIndex
           1,  // MissShaderIndex: shadow miss shader
           ray, payload);

  return payload.is_shadowed;
}

// ============================================================================
// MIS（多重重要性采样）权重
// ============================================================================

// 幂启发式（Veach 1997），β = 2
float power_heuristic(float pdf_a, float pdf_b)
{
  float a2 = pdf_a * pdf_a;
  float b2 = pdf_b * pdf_b;
  return a2 / (a2 + b2 + 1e-5f);
}

// 球面均匀采样（天空/环境光照）
float3 sample_uniform_sphere(inout uint seed)
{
  float2 r    = random_float2(seed);
  float  z    = 1.0f - 2.0f * r.x;
  float  r_xy = sqrt(max(0.0, 1.0 - z * z));
  float  phi  = 2.0 * PI * r.y;
  return float3(r_xy * cos(phi), r_xy * sin(phi), z);
}

// ============================================================================
// 天空 / 环境光照采样
// ============================================================================

// 程序化天空渐变：天顶深蓝 → 地平线蓝白 → 地面深棕
float3 sample_sky(float3 direction)
{
  const float3 ZENITH  = float3(0.25f, 0.45f, 0.95f);
  const float3 HORIZON = float3(0.65f, 0.80f, 1.00f);
  const float3 GROUND  = float3(0.12f, 0.10f, 0.08f);

  float up = direction.y;
  if(up > 0.0f)
  {
    float t = pow(clamp(up, 0.0f, 1.0f), 0.4f);
    return lerp(HORIZON, ZENITH, t);
  }
  else
  {
    float t = pow(clamp(-up, 0.0f, 1.0f), 2.0f);
    return lerp(HORIZON, GROUND, t);
  }
}

// 自定义纯色天空（保留接口，供外部扩展）
float3 sample_sky(float3 direction, float3 sky_color)
{
  return sky_color;
}

// HDR 等距矩形环境贴图采样（方向 → 经纬 UV）
float3 sample_hdr_sky(float3 direction, Sampler2D hdr_env_map)
{
  float2 uv = float2(0.5f + 0.5f * atan2(direction.z, direction.x) * INV_PI, 1.0f - acos(clamp(direction.y, -1.0f, 1.0f)) * INV_PI);
  return hdr_env_map.SampleLevel(uv, 0.0f).rgb;
}

// ============================================================================
// 高度雾（Exponential Height Fog）— 解析解
// ============================================================================

struct fog_result
{
  float3 transmittance;  // 透射率 T(s) = exp(-τ)：0 全遮挡，1 完全清澈
  float3 inscatter;      // 路径上散射加入的光
};

// 从 origin 沿 direction 传播 distance，密度模型 ρ(h) = density * exp(-h / falloff)
fog_result evaluate_height_fog(float3 origin, float3 direction, float distance, float density, float falloff, float3 fog_color, float3 sun_color, float3 sun_direction)
{
  const float h0   = origin.y;
  const float dy   = direction.y;
  const float tiny = 1e-6f;

  fog_result res;
  float      optical_depth = 0.0f;

  if(abs(dy) < tiny)
  {
    // 水平光线：密度在高度 h0 处恒定
    float rho     = density * exp(-max(h0, 0.0f) / falloff);
    optical_depth = rho * clamp(distance, 0.0f, 1e30f);
  }
  else
  {
    float a       = -1.0f / falloff;
    float c0      = density * exp(a * h0);
    float t       = clamp(distance, 0.0f, 1e15f);
    optical_depth = c0 * (exp(a * dy * t) - 1.0f) / (a * dy);
  }

  optical_depth     = clamp(optical_depth, 0.0f, 50.0f);
  res.transmittance = exp(-optical_depth).rrr;

  float3 inscatter_color = (float3(1.0f) - res.transmittance) * fog_color;

  // 太阳散射增强：视线与太阳方向越接近，散射越亮
  float sun_factor = clamp(dot(direction, sun_direction), 0.0f, 1.0f);
  sun_factor       = pow(sun_factor, 8.0f);
  inscatter_color += (float3(1.0f) - res.transmittance) * sun_color * sun_factor * 0.5f;

  res.inscatter = inscatter_color;
  return res;
}

// 便捷包装：origin → hit_point 之间的雾效
fog_result evaluate_height_fog_between(float3 origin, float3 hit_point, float density, float falloff, float3 fog_color, float3 sun_color, float3 sun_direction)
{
  float3 dir  = hit_point - origin;
  float  dist = length(dir);
  if(dist < 1e-6f)
  {
    fog_result f;
    f.transmittance = float3(1.f);
    f.inscatter     = float3(0.f);
    return f;
  }
  return evaluate_height_fog(origin, dir / dist, dist, density, falloff, fog_color, sun_color, sun_direction);
}
