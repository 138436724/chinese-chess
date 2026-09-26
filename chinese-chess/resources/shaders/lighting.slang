// ============================================================================
// lighting.slang — 光源与大气模块
//
// 职责边界(2026 起):
//   本模块只负责**光传输的采样与介质**:光源采样(NEE)、阴影光线、天空/环境
//   采样、高度雾。
//   **所有 BRDF/BSDF 已迁至 shading.slang** —— 这里不再定义 Lambertian /
//   Disney / GGX / Fresnel。原因见 pbr-exploration-plan.md:材质求值必须
//   收敛到 shading.slang 的单一入口,才能保证光谱化时只有一处需要改,且
//   不会出现"标量近似"与"逐波长求值"两套实现。
//
// 被 ray_tracing.slang 复用。
// ============================================================================

import common;
import scene_data;

enum light_type : uint32_t
{
  directional = 0,
  point       = 1,
  spot        = 2
};

// ---- 光照采样结果（NEE 采样返回值） ----
// pdf 为**标量**:光源采样策略是几何+亮度决策,与波长无关。
// 光谱化时必须保持这一点,否则 pdf 里会混入颜色而无法分离。
struct light_sample
{
  float3 direction;  // 从命中点到光源的方向（单位向量）
  float  distance;   // 命中点到光源的距离（用于阴影光线 tmax）
  float3 color;      // 光源在该采样方向的辐射亮度(逐波长)
  float  pdf;        // 采样概率密度函数值（用于 MIS）
};

// ---- 阴影光线载荷（轻量级，仅需知道是否被遮挡） ----
struct shadow_payload
{
  bool is_shadowed;
  uint seed;  // 概率阴影（半透明穿透）用随机种子，与主路径种子解耦
};

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
float2 direction_to_equirect_uv(float3 direction)
{
  return float2(0.5f + 0.5f * atan2(direction.z, direction.x) * INV_PI,
                1.0f - acos(clamp(direction.y, -1.0f, 1.0f)) * INV_PI);
}

float3 sample_hdr_sky(float3 direction, Sampler2D hdr_env_map)
{
  return hdr_env_map.SampleLevel(direction_to_equirect_uv(direction), 0.0f).rgb;
}

// ============================================================================
// 环境光照(阶段 3)—— 取代原来那个恒定的 AMBIENT_LIGHT 经验项
//
// 两个用途:
//   1) 漫反射环境光:irradiance(辐照度)= ∫ L(ω)cosθ dω,用**余弦重要性采样**直接估计,
//      每帧 8 个样本 + 时域累积收敛。这样不需要预计算 irradiance 贴图,
//      也不需要新增资产/纹理槽。
//   2) 环境 NEE(镜面):按天空亮度做重要性采样,并给出与 BSDF 采样之间的 MIS 权重。
//
// 物理要点:
//   · 余弦采样的 pdf = cosθ/π,而积分里被积函数自带 cosθ,
//     故估计量 = L(ω)·π,与 cosθ 无关(这正是不需要显式 cos 的原因)。
//   · irradiance 送入 Lambertian 时还要乘 albedo/π,两端 π 相消。
//   · 所有 pdf 与 MIS 权重均为**标量**(亮度),与波长无关 —— 光谱化时保持这个性质。
// ============================================================================

// 余弦重要性采样方向(切线空间 → 世界空间)。
// 估计量 = L·π:pdf = cosθ/π,而辐照度积分自带 cosθ,两者相消。
float3 sample_cosine_direction(float3 N, inout uint seed)
{
  float2 r   = random_float2(seed);
  float  phi = 2.0 * PI * r.x;
  float  ct  = sqrt(1.0 - r.y);
  float  st  = sqrt(r.y);
  tangent_space ts = build_tangent_space(N);
  return normalize(ts.T * (st * cos(phi)) + ts.B * (st * sin(phi)) + N * ct);
}

// 环境 NEE 的方向 pdf。
//
// ⚠ 这是**唯一**的定义处:MIS 的两侧(环境 NEE 的估计量、以及 BSDF 采样命中天空
// 时的权重)必须用同一个 pdf,否则 balance heuristic 的权重错、能量会系统性偏移。
// 本仓库踩过这个坑:采样改成了均匀半球,但弹射前算 MIS 权重的地方仍用余弦 pdf,
// 结果是白炉球体阵整体偏暗(由 tools/furnace_probe.ps1 在真白炉下抓到)。
// 与 sample_uniform_upper_hemisphere 的分布必须一致。
float environment_nee_pdf()
{
  return 1.0 / (2.0 * PI);
}

// 上半球均匀采样(绕给定法线):cosθ 均匀取 [0,1)、方位角均匀 ⟹ pdf = 1/(2π)。
// 环境 NEE 用它 —— **不要**用"全空间采样 + 拒绝下半球 + 重试补偿"那种写法:
// 重试 N 次只保留 1 个样本时,概率因子的补偿极易写成放大 2 倍的偏差
// (本仓库踩过:估计量恰好变成解析值的 2 倍,由 tools/pbr_env_check.py 抓到)。
float3 sample_uniform_upper_hemisphere(float3 N, inout uint seed)
{
  float2 r   = random_float2(seed);
  float  ct  = r.x;                              // cosθ ∈ [0,1)
  float  st  = sqrt(max(0.0, 1.0 - ct * ct));
  float  phi = 2.0 * PI * r.y;
  tangent_space ts = build_tangent_space(N);
  return normalize(ts.T * (st * cos(phi)) + ts.B * (st * sin(phi)) + N * ct);
}

// 取天空辐射亮度(带"过暗回退程序化天空"的一致判据)
// _uniform_env:白炉模式 —— 返回常数 1.0,使环境在所有方向上等辐射亮度
float3 environment_radiance(float3 _direction, Sampler2D _hdr_env_map, uint _skybox_index, bool _uniform_env)
{
  if(_uniform_env)
  {
    return float3(1.0);
  }

  float3 sky = sample_sky(_direction);
  if(_skybox_index < 0xFFFFFFFFu)
  {
    float3 hdr = sample_hdr_sky(_direction, _hdr_env_map);
    sky        = (luminance(hdr) > 0.02f) ? hdr : sky;
  }
  return sky;
}

// 环境辐照度估计(E = ∫L cos dω),余弦重要性采样 + 可见性
// _uniform_env:白炉模式,天空处处为 1.0 ⟹ E = π(解析值,可用于像素级验收)
float3 sample_environment_irradiance(RaytracingAccelerationStructure tlas, float3 origin, float3 N,
                                     Sampler2D hdr_env_map, uint skybox_index, uint sample_count,
                                     bool _uniform_env, inout uint seed)
{
  if(sample_count == 0u)
    return float3(0.0);

  float3 sum = float3(0.0);
  for(uint i = 0u; i < sample_count; ++i)
  {
    float3 dir = sample_cosine_direction(N, seed);

    // 可见性:被几何遮挡的方向不贡献环境光。命中即为遮挡。
    // 注意:不能对追踪到天空的射线复用 trace_shadow_ray —— 它的 miss shader
    // 会把 is_shadowed 置 false,语义相反。这里用零初始化 + 命中即置真。
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = dir;
    ray.TMin      = SHADOW_EPSILON;
    ray.TMax      = 1e10f;

    shadow_payload occ;
    occ.is_shadowed = false;
    occ.seed        = wang_hash(seed);
    TraceRay(tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
                        | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
             0xff, 1, 0, 1, ray, occ);
    if(occ.is_shadowed)
      continue;

    sum += environment_radiance(dir, hdr_env_map, skybox_index, _uniform_env);
  }
  // 余弦采样 pdf = cos/π,被积函数自带 cos ⟹ 估计量 = L·π;再除样本数
  return sum * (PI / float(sample_count));
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
