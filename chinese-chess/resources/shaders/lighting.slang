import common;
import scene_data;

// ============================================================================
// lighting.slang — 光照库模块
// 光源数据结构在 scene_data.slang 中定义；本模块只负责：
//   光源采样（NEE）、BRDF 评估/采样/PDF、阴影光线、天空采样、高度雾
// 被 ray_tracing.slang / ray_query.slang（和 scene_brdflut.slang）复用。
// ============================================================================

enum light_type : uint32_t
{
  directional = 0,
  point       = 1,
  spot        = 2
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

// Lambertian PDF
float pdf_lambertian(float3 N, float3 L)
{
  return max(0.0, dot(N, L)) * INV_PI;
}

// Disney Diffuse BRDF 评估（Burley 2012）：粗糙表面掠射角变亮
float3 evaluate_disney_diffuse(float3 albedo, float roughness, float3 N, float3 V, float3 L, float3 H, float NdotL)
{
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
  bs.value = evaluate_disney_diffuse(albedo, roughness, N, V, bs.direction, H, NdotL) * NdotL / pdf_lambertian(N, bs.direction);
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

// GGX 可见法线分布（VNDF）重要性采样（Heitz 2018 "Sampling the GGX Distribution of Visible Normals"）
// 返回采样出的反射方向 L（世界空间）。较经典 Smith 采样方差更低、firefly 更少，
// 且采样方向保证在上半球（不会被 NdotL<=0 拒绝浪费）。
float3 sample_ggx(float3 V, float roughness, float3 N, inout uint seed)
{
  float2 r = random_float2(seed);
  float  a = roughness * roughness;

  // 视线变换到切线空间（局部半球，+Z = 法线）
  tangent_space ts = build_tangent_space(N);
  float3        Ve = float3(dot(V, ts.T), dot(V, ts.B), dot(V, N));

  // §3.2 拉伸到椭圆体配置
  float3 Vh = normalize(float3(a * Ve.x, a * Ve.y, Ve.z));

  // §4.1 正交基（半球配置特化）
  float  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
  float3 T1    = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
  float3 T2    = cross(Vh, T1);

  // §4.2 投影面积参数化（同心圆盘采样）
  float r_disk = sqrt(r.x);
  float phi    = 2.0 * PI * r.y;
  float t1     = r_disk * cos(phi);
  float t2     = r_disk * sin(phi);
  float s      = 0.5 * (1.0 + Vh.z);
  t2           = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;

  // §4.3 重投影到半球
  float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;

  // §3.4 变换回椭圆体配置并归一化 → 世界空间半向量 H
  float3 H_local = normalize(float3(a * Nh.x, a * Nh.y, max(0.0, Nh.z)));
  float3 H       = ts.T * H_local.x + ts.B * H_local.y + N * H_local.z;

  // 反射出射方向 L
  return normalize(reflect(-V, H));
}

// GGX BRDF 采样（VNDF）：pdf = D·G1(V)/(4·NdotV)，value = F·G1(L)（fr·NdotL/pdf 约简）
brdf_sample sample_ggx_brdf(float3 albedo, float3 F0, float roughness, float3 N, float3 V, inout uint seed)
{
  brdf_sample bs;
  float3      L = sample_ggx(V, roughness, N, seed);
  bs.direction  = L;

  float NdotV = max(dot(N, V), 0.0);
  float NdotL = max(dot(N, L), 0.0);
  if(NdotV <= 0.0 || NdotL <= 0.0)
  {
    bs.value = float3(0.0);
    bs.pdf   = 0.0;
    return bs;
  }

  float3 H     = normalize(V + L);
  float  NdotH = max(dot(N, H), 0.0);
  float  VdotH = max(dot(V, H), 0.001);

  // VNDF 对应项：采样分布 pdf = D·G1(V)/(4·NdotV)，吞吐量约简为 F·G1(L)
  float  D    = d_ggx(NdotH, roughness);
  float  G1_v = g_schlick_ggx(NdotV, roughness);
  float  G1_l = g_schlick_ggx(NdotL, roughness);
  float3 F    = f_schlick(F0, VdotH);

  bs.pdf   = D * G1_v / max(4.0 * NdotV, 0.001);
  bs.value = G1_l * F;
  return bs;
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
    float up_ground = clamp(-up, 0.0f, 1.0f);
    float t         = up_ground * up_ground;
    return lerp(HORIZON, GROUND, t);
  }
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
  sun_factor *= sun_factor;  // 2
  sun_factor *= sun_factor;  // 4
  sun_factor *= sun_factor;  // 8
  inscatter_color += (float3(1.0f) - res.transmittance) * sun_color * sun_factor * 0.5f;

  res.inscatter = inscatter_color;
  return res;
}
