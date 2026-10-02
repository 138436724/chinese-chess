import shader_utils;
import gpu_shared_types;
import pbr;

// ============================================================================
// lighting.slang — 光源采样 / 环境光照 / NEE 贡献
//
// 直接光照一律走 NEE（Next Event Estimation）+ MIS：
//   · 方向光 / 点光源 / 聚光灯都是 delta 分布（pdf 形式上为 1），
//     BRDF 重要性采样命中该方向的概率为 0 → NEE 是唯一采样策略，
//     MIS 权重恒为 1（对非 delta 的面光源需恢复真正的 MIS 权重）。
//   · 环境光（天空）不是 delta：既做均匀球面采样的 NEE，也由 BSDF 采样命中，
//     两者都用能量启发式 MIS 组合，权重和为 1 → 无偏且方差显著低于只用一种。
//
// 可见性（阴影光线）由入口文件提供：RT pipeline 用 any-hit，
// Ray Query 用候选遍历（见 path_tracing.slang 的说明）。
// ============================================================================

// ---------------------------------------------------------------------------
// light_sample：一次光源采样的结果
// ---------------------------------------------------------------------------
struct light_sample
{
  float3 direction;  // 从着色点指向光源的方向（单位向量）
  float3 radiance;   // 该方向上的入射辐亮度（已含距离 / 锥角衰减）
  float  distance;   // 到光源的距离（阴影光线 tmax）
  float  pdf;        // 采样 pdf（立体角密度）
  bool   is_delta;   // delta 光源（无法与 BSDF 采样做 MIS）
  bool   valid;
};

// ---------------------------------------------------------------------------
// light_source：光源（包装 GPU 侧 gpu_light_data，成员函数表达采样行为）
//   衰减模型：
//     方向光：radiance = color · intensity（无距离衰减）
//     点光源：radiance = color · intensity · (1/d²) · range_window(d)
//     聚光灯：点光源衰减 × 内外锥角平滑过渡
//   range_window 是光源自身的辐射模型（不是估计量偏差）。
// ---------------------------------------------------------------------------
struct light_source
{
  gpu_light_data data;

  bool enabled() { return data.intensity > 0.0 && any(data.color > 0.0); }

  // 距离窗口：range ≤ 0 表示不做截断
  float range_window(float distance) { return data.range > 0.0 ? max(0.0, 1.0 - distance / data.range) : 1.0; }

  light_sample sample_directional()
  {
    light_sample ls;
    ls.direction = safe_normalize(data.direction);
    ls.radiance  = data.color * data.intensity;
    ls.distance  = RAY_TMAX;
    ls.pdf       = 1.0;  // delta 分布
    ls.is_delta  = true;
    ls.valid     = true;
    return ls;
  }

  light_sample sample_point(float3 position)
  {
    light_sample ls;
    float3       to_light = data.position - position;
    float        distance = length(to_light);

    ls.direction = distance > 1e-6 ? to_light / distance : float3(0.0, 1.0, 0.0);
    ls.distance  = max(distance, 1e-4);
    ls.radiance  = data.color * data.intensity * (1.0 / max(distance * distance, 1e-4)) * range_window(distance);
    ls.pdf       = 1.0;
    ls.is_delta  = true;
    ls.valid     = true;
    return ls;
  }

  light_sample sample_spot(float3 position)
  {
    light_sample ls = sample_point(position);
    if(!ls.valid)
    {
      return ls;
    }

    // 锥角衰减：以「光源指向场景」的轴为基准
    float3 axis      = safe_normalize(data.direction);
    float  cos_angle = dot(-ls.direction, axis);

    if(cos_angle < cos(data.outer_cone_angle))
    {
      ls.radiance = float3(0.0);  // 锥外无光照
      return ls;
    }

    if(cos_angle < cos(data.inner_cone_angle))
    {
      // 内外锥之间二次平滑过渡
      float t = (cos_angle - cos(data.outer_cone_angle)) / max(cos(data.inner_cone_angle) - cos(data.outer_cone_angle), 1e-4);
      ls.radiance *= t * t;
    }

    return ls;
  }

  // 按 active_type 分派
  light_sample sample(float3 position)
  {
    switch(data.active_type)
    {
      case light_type::directional:
        return sample_directional();
      case light_type::point:
        return sample_point(position);
      case light_type::spot:
        return sample_spot(position);
      default:
        break;
    }

    light_sample ls;
    ls.direction = float3(0.0, 1.0, 0.0);
    ls.radiance  = float3(0.0);
    ls.distance  = 0.0;
    ls.pdf       = 1.0;
    ls.is_delta  = true;
    ls.valid     = false;
    return ls;
  }
};

// ---------------------------------------------------------------------------
// environment：环境光照（HDR 等距矩形贴图；无贴图时回退程序化渐变天空）
//
// bindless 纹理数组是入口文件的全局绑定，因此贴图句柄由调用方在
// 「索引守卫成立」的分支内取得后传入（越界索引解引用是未定义行为）。
// ---------------------------------------------------------------------------
struct environment
{
  uint skybox_index;   // bindless 槽（0xFFFFFFFF 表示无 HDR 天空）
  uint texture_count;  // bindless 数组上界
  bool has_hdr;        // skybox_index 是否有效

  // 程序化天空渐变：天顶深蓝 → 地平线蓝白 → 地面深棕
  float3 procedural_sky(float3 direction)
  {
    const float3 zenith  = float3(0.25, 0.45, 0.95);
    const float3 horizon = float3(0.65, 0.80, 1.00);
    const float3 ground  = float3(0.12, 0.10, 0.08);

    float up = direction.y;
    if(up > 0.0)
    {
      return lerp(horizon, zenith, pow(clamp(up, 0.0, 1.0), 0.4));
    }

    float down = clamp(-up, 0.0, 1.0);
    return lerp(horizon, ground, down * down);
  }

  // HDR 等距矩形采样：方向 → 经纬 UV
  float3 equirect_radiance(float3 direction, Sampler2D hdr_texture)
  {
    float2 uv = float2(0.5 + 0.5 * atan2(direction.z, direction.x) * INV_PI, 1.0 - acos(clamp(direction.y, -1.0, 1.0)) * INV_PI);
    return hdr_texture.SampleLevel(uv, 0.0).rgb;
  }

  // 均匀球面采样（pdf = 1/4π）
  light_sample sample(inout rng r)
  {
    light_sample ls;
    ls.direction = sample_uniform_sphere(r.next_float2());
    ls.radiance  = float3(0.0);  // 由调用方按方向求值后填入
    ls.distance  = RAY_TMAX;
    ls.pdf       = uniform_sphere_pdf();
    ls.is_delta  = false;
    ls.valid     = true;
    return ls;
  }
};

// ---------------------------------------------------------------------------
// NEE 贡献：光源采样（delta 光源，MIS 权重恒 1）
// ---------------------------------------------------------------------------
float3 nee_light_contribution(shading_context ctx, light_sample ls, float visibility)
{
  if(!ls.valid || visibility <= 0.0 || all(ls.radiance <= 0.0))
  {
    return float3(0.0);
  }

  float n_dot_l = dot(ctx.n, ls.direction);
  if(n_dot_l <= 0.0)
  {
    return float3(0.0);
  }

  float3 f = ctx.eval(ls.direction);

  // delta 光源：BSDF 采样命中概率为 0，NEE 是唯一策略 → 权重 1（不做除 pdf）
  return ls.radiance * f * n_dot_l * visibility;
}

// ---------------------------------------------------------------------------
// NEE 贡献：环境光采样（非 delta → 与 BSDF 采样做 MIS）
//   pdf_light = 1/4π；pdf_bsdf = 连续波瓣的方向 pdf；权重和为 1。
// ---------------------------------------------------------------------------
float3 nee_environment_contribution(shading_context ctx, float3 direction, float3 radiance, float visibility)
{
  if(visibility <= 0.0)
  {
    return float3(0.0);
  }

  float n_dot_l = dot(ctx.n, direction);
  if(n_dot_l <= 0.0)
  {
    return float3(0.0);
  }

  float pdf_light = uniform_sphere_pdf();
  float pdf_bsdf  = ctx.pdf(direction);
  float weight    = power_heuristic(pdf_light, pdf_bsdf);

  return radiance * ctx.eval(direction) * n_dot_l * weight / pdf_light;
}

// ---------------------------------------------------------------------------
// 环境光命中时的 MIS 权重（BSDF 采样光线逃逸到天空时使用）
//   has_bsdf_pdf = false 表示当前方向不是由连续 BSDF 采样产生的：
//     · 相机主光线（根本没有 BSDF 侧策略）
//     · delta 波瓣采样（镜面 / 折射，均匀球面采样命中它的概率为 0）
//   这两种情况 MIS 无定义，权重取 1；对应地，环境光 NEE 对该方向的
//   连续 BRDF 求值恒为 0（delta 部分测度为零），不会重复计数。
// ---------------------------------------------------------------------------
float environment_miss_weight(bool has_bsdf_pdf, float bsdf_pdf)
{
  if(!has_bsdf_pdf)
  {
    return 1.0;
  }
  return power_heuristic(bsdf_pdf, uniform_sphere_pdf());
}
