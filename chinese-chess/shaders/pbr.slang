import shader_utils;
import vulkan_data_type;

// ============================================================================
// pbr.slang — 物理着色模型：Fresnel / 微表面 / 材质 / BSDF 采样
//
// 模型（metallic-roughness + 平滑介质界面）：
//   BSDF = (1 - transmission) · [ Lambert(diffuse_albedo) + GGX(f0) ]
//        + transmission · [ 平滑介质界面：F·δ_reflect + (1-F)·δ_refract ]
//
//   · 反射部分（Lambert + GGX）是连续 BRDF，可求值、可采样、有 pdf：
//     它参与直接光 NEE、环境光 NEE 与 MIS（本文件提供严格一致的
//     eval / pdf / sample 三者，采样权重直接取 fr·cosθ / pdf，
//     不手写解析约简，避免三者不一致引入偏差）。
//   · 介质界面是 delta 波瓣：NEE 对其贡献恒为 0（立体角测度为零），
//     只能由 BSDF 采样处理，其权重取「波瓣反射率 / 分支概率」。
//   · 介质内部的吸收由「每段行进距离上的 Beer-Lambert 指数衰减」精确结算
//     （见 path_tracing.slang），不使用固定厚度近似，因此对任意厚度无偏。
//
// 说明：GGX 采用单次散射（无多重散射能量补偿），这是 Disney / glTF 的
// 标准建模选择；蒙特卡洛估计量对该 BRDF 无偏。粗糙度下限 ALPHA_MIN 是
// 数值保护（eval / pdf / sample 使用同一个 alpha），不构成估计量偏差。
// ============================================================================

// ---------------------------------------------------------------------------
// Fresnel
// ---------------------------------------------------------------------------

// Schlick 近似（用于微表面反射的 F 项）
float3 fresnel_schlick(float3 f0, float cos_theta)
{
  return f0 + (float3(1.0) - f0) * pow5(clamp(1.0 - cos_theta, 0.0, 1.0));
}

// 平滑介质界面的精确 Fresnel 反射率（非偏振平均）
//   cos_theta_i：入射角余弦（≥ 0）
//   eta        ：eta_i / eta_t（入射侧折射率 / 透射侧折射率）
//   返回 1.0 表示全反射
float fresnel_dielectric(float cos_theta_i, float eta)
{
  float cos_i  = clamp(cos_theta_i, 0.0, 1.0);
  float sin2_t = eta * eta * (1.0 - cos_i * cos_i);

  if(sin2_t >= 1.0)
  {
    return 1.0;  // 全反射
  }

  float cos_t      = sqrt(max(0.0, 1.0 - sin2_t));
  float r_parallel = (eta * cos_i - cos_t) / max(eta * cos_i + cos_t, 1e-6);
  float r_perp     = (cos_i - eta * cos_t) / max(cos_i + eta * cos_t, 1e-6);
  return 0.5 * (r_parallel * r_parallel + r_perp * r_perp);
}

// ---------------------------------------------------------------------------
// microfacet：GGX / Trowbridge-Reitz 法线分布 + 高度相关 Smith 遮蔽
//   alpha = roughness²（感知粗糙度到线性粗糙度的标准映射）
// ---------------------------------------------------------------------------
struct microfacet
{
  float3 n;      // 宏观法线（已朝向观察者）
  float  alpha;  // 已钳制到 [ALPHA_MIN, 1]

  // D(m)
  float d(float n_dot_m)
  {
    float a2    = alpha * alpha;
    float denom = n_dot_m * n_dot_m * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-12);
  }

  // Smith 遮蔽辅助量 Λ(w)
  float lambda(float n_dot_w)
  {
    float c  = abs(n_dot_w);
    float c2 = max(c * c, 1e-8);
    return 0.5 * (sqrt(1.0 + alpha * alpha * (1.0 - c2) / c2) - 1.0);
  }

  float g1(float n_dot_w) { return 1.0 / (1.0 + lambda(n_dot_w)); }

  // 高度相关 Smith 联合遮蔽
  float g2(float n_dot_v, float n_dot_l) { return 1.0 / (1.0 + lambda(n_dot_v) + lambda(n_dot_l)); }

  float3 half_vector(float3 wo, float3 wi) { return safe_normalize(wo + wi); }

  // 反射 BRDF 值（不含 cosθ）：fr = D·G2·F / (4·|n·wo|·|n·wi|)
  float3 eval(float3 f0, float3 wo, float3 wi)
  {
    float n_dot_v = dot(n, wo);
    float n_dot_l = dot(n, wi);
    if(n_dot_v <= 0.0 || n_dot_l <= 0.0)
    {
      return float3(0.0);
    }

    float3 h       = half_vector(wo, wi);
    float  n_dot_h = max(dot(n, h), 0.0);
    float  v_dot_h = max(dot(wo, h), 0.0);

    return d(n_dot_h) * g2(n_dot_v, n_dot_l) * fresnel_schlick(f0, v_dot_h) / (4.0 * n_dot_v * n_dot_l);
  }

  // VNDF 采样对应的方向 pdf（立体角密度）：
  //   pdf(wi) = D_visible(h) / (4·|wo·h|) = G1(wo)·D(h) / (4·|n·wo|)
  float pdf(float3 wo, float3 wi)
  {
    float n_dot_v = dot(n, wo);
    float n_dot_l = dot(n, wi);
    if(n_dot_v <= 0.0 || n_dot_l <= 0.0)
    {
      return 0.0;
    }

    float n_dot_h = max(dot(n, half_vector(wo, wi)), 0.0);
    return g1(n_dot_v) * d(n_dot_h) / (4.0 * n_dot_v);
  }

  // 可见法线分布（VNDF）重要性采样（Heitz 2018）：返回世界空间反射方向
  float3 sample_direction(float3 wo, float2 u)
  {
    basis  b       = build_basis(n);
    float3 v_local = b.to_local(wo);

    // §3.2 拉伸到椭球配置
    float3 vh = normalize(float3(alpha * v_local.x, alpha * v_local.y, v_local.z));

    // §4.1 正交基（半球配置特化）
    float  lensq = vh.x * vh.x + vh.y * vh.y;
    float3 t1    = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 t2    = cross(vh, t1);

    // §4.2 投影面积参数化（同心圆盘采样）
    float r   = sqrt(u.x);
    float phi = 2.0 * PI * u.y;
    float p1  = r * cos(phi);
    float p2  = r * sin(phi);
    float s   = 0.5 * (1.0 + vh.z);
    p2        = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;

    // §4.3 重投影到半球
    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;

    // §3.4 变换回椭球配置并归一化 → 世界空间半向量
    float3 h_local = normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
    float3 h       = b.to_world(h_local);

    return reflect(-wo, h);
  }
};

// ---------------------------------------------------------------------------
// pbr_material：解析后的材质参数（与渲染路径无关的着色侧形式）
//   反照率由 vulkan_material_data 的基色与 albedo 贴图在入口处解析后填入
//   （见 resolve_albedo）。
// ---------------------------------------------------------------------------
struct pbr_material
{
  float3 albedo;
  float3 emission;
  float3 absorption;  // σ_a：介质吸收系数（1/世界单位）
  float  roughness;
  float  metallic;
  float  reflectance;  // 电介质镜面反射率 [0,1]
  float  ior;
  float  transmission;
  float  opacity;
  float  emission_strength;

  // 线性粗糙度（微表面对齐）
  float alpha() { return clamp(roughness * roughness, ALPHA_MIN, 1.0); }

  // 透射占比 = 平滑介质界面的分支概率
  float transmission_probability() { return clamp(transmission, 0.0, 1.0); }

  float3 dielectric_f0() { return float3(0.08 * clamp(reflectance, 0.0, 1.0)); }

  // 金属 F0 = 反照率；电介质 F0 由 reflectance 控制（默认 0.5 → 0.04）
  float3 f0() { return lerp(dielectric_f0(), albedo, clamp(metallic, 0.0, 1.0)); }

  // 漫反射反照率：金属没有漫反射
  float3 diffuse_albedo() { return (1.0 - clamp(metallic, 0.0, 1.0)) * albedo; }

  // 连续波瓣中 GGX 镜面的采样概率。
  // 任意 (0,1) 内的取值都不影响无偏性（eval / pdf 使用同一取值），
  // 此处按两种波瓣的能量占比分配以降方差。
  float specular_probability()
  {
    float e_spec = luminance(f0());
    float e_diff = luminance(diffuse_albedo());
    return clamp(e_spec / max(e_spec + e_diff, 1e-4), 0.05, 0.95);
  }

  // 自发光辐亮度
  float3 emitted_radiance() { return emission * emission_strength; }
};

// ---------------------------------------------------------------------------
// GPU vulkan_material_data → 着色侧 pbr_material
//   三套渲染路径（RT / Ray Query / 光栅化）共用同一份转换，避免同一套 PBR 参数
//   在不同路径里有不同解释。
// ---------------------------------------------------------------------------

// 反照率：材质基色 × 贴图采样。贴图（含字体图集）在 CPU 侧就烘焙好了颜色，
// 这里直接乘；无贴图（texture_index 越界）时退化为纯基色。
float3 resolve_albedo(vulkan_material_data data, float3 texture_albedo, bool has_texture)
{
  return has_texture ? data.albedo * texture_albedo : data.albedo;
}

pbr_material make_material(vulkan_material_data data, float3 albedo)
{
  pbr_material m;
  m.albedo            = albedo;
  m.emission          = data.emission;
  m.absorption        = data.absorption;
  m.roughness         = data.roughness;
  m.metallic          = data.metallic;
  m.reflectance       = data.reflectance;
  m.ior               = data.ior;
  m.transmission      = data.transmission;
  m.opacity           = data.opacity;
  m.emission_strength = data.emission_strength;
  return m;
}

// ---------------------------------------------------------------------------
// bsdf_sample：BSDF 采样结果
// ---------------------------------------------------------------------------
struct bsdf_sample
{
  float3 direction;        // 出射方向（世界空间，单位向量）
  float3 weight;           // 吞吐量乘子 fr·cosθ / pdf
  float  pdf;              // 方向 pdf（立体角密度）；delta 波瓣为 0
  bool   is_delta;         // 是否来自 delta 波瓣（不参与 MIS，环境光权重取 1）
  bool   is_transmission;  // 是否为折射（决定进入 / 离开介质）
  bool   valid;            // false 表示该方向贡献恒为 0，路径可安全终止
};

// ---------------------------------------------------------------------------
// shading_context：一次表面交互的着色上下文（材质 + 几何 + 视线）
// ---------------------------------------------------------------------------
struct shading_context
{
  pbr_material material;
  microfacet   ggx;
  float3       n;           // 朝向观察者的宏观法线
  float3       wo;          // 出射方向（指向观察者，单位向量）
  bool         front_face;  // 是否从几何正面入射（决定介质界面的 eta 方向）

  // 非 delta 部分：Lambert 漫反射 + GGX 镜面反射
  float3 eval(float3 wi)
  {
    float n_dot_l = dot(n, wi);
    if(n_dot_l <= 0.0 || dot(n, wo) <= 0.0)
    {
      return float3(0.0);
    }

    float3 f_diffuse  = material.diffuse_albedo() * INV_PI;
    float3 f_specular = ggx.eval(material.f0(), wo, wi);
    return (1.0 - material.transmission_probability()) * (f_diffuse + f_specular);
  }

  // 与 eval 严格一致的采样 pdf（供 NEE 的 MIS 使用）
  float pdf(float3 wi)
  {
    float n_dot_l = dot(n, wi);
    if(n_dot_l <= 0.0 || dot(n, wo) <= 0.0)
    {
      return 0.0;
    }

    float p_spec   = material.specular_probability();
    float pdf_spec = ggx.pdf(wo, wi);
    float pdf_diff = cosine_hemisphere_pdf(n_dot_l);
    return (1.0 - material.transmission_probability()) * (p_spec * pdf_spec + (1.0 - p_spec) * pdf_diff);
  }

  // BSDF 采样。三分支：
  //   1) 平滑介质界面（delta）：Fresnel 反射 / 折射
  //   2) GGX 镜面（VNDF）
  //   3) Lambert 漫反射（余弦加权）
  // 连续波瓣统一用 fr·cosθ / pdf 计算权重 —— 与 eval / pdf 同源，
  // 且混合 pdf 有余弦项托底，权重天然有界（不会出现 firefly）。
  //
  // 介质界面的权重恒为 1（**不**再除以分支概率）：本模型的混合式是
  //   BSDF = (1 - p)·[Lambert + GGX] + p·[F·δ_reflect + (1-F)·δ_refract]
  // 采样该界面的概率已经是 p·F（或 p·(1-F)），即分支概率本身就携带了 p 这份能量；
  // 若再写成「波瓣反射率 / 分支概率」= F/(p·F)，期望能量会多出 1/p 倍
  // （p < 1 时直接违反能量守恒，半透材质会明显过亮）。
  bsdf_sample sample(inout rng r)
  {
    bsdf_sample result;
    result.direction       = float3(0.0, 0.0, 1.0);
    result.weight          = float3(0.0);
    result.pdf             = 0.0;
    result.is_delta        = false;
    result.is_transmission = false;
    result.valid           = false;

    float p_transmission = material.transmission_probability();

    if(r.next_float() < p_transmission)
    {
      // ---------- 平滑介质界面（delta 波瓣）----------
      result.is_delta = true;

      float cos_i   = max(dot(n, wo), 1e-6);
      float eta     = front_face ? (1.0 / max(material.ior, 1.0)) : max(material.ior, 1.0);
      float fresnel = fresnel_dielectric(cos_i, eta);

      if(r.next_float() < fresnel)
      {
        // 镜面反射：采样概率 = p·F，贡献 = p·F·L → 权重 = 1
        result.direction = reflect(-wo, n);
        result.valid     = true;
        return result;
      }

      float3 refracted = refract(-wo, n, eta);
      if(dot(refracted, refracted) < 1e-8)
      {
        // 全反射：有效反射率 = 1，采样概率 = p → 贡献 = p·L → 权重 = 1
        result.direction = reflect(-wo, n);
        result.valid     = true;
        return result;
      }

      // 折射：采样概率 = p·(1-F)，贡献 = p·(1-F)·T_medium·L → 权重 = 1。
      // 介质内的 Beer-Lambert 衰减按「每段行进距离」在命中下一个表面时精确结算
      // （与分段长度无关，对任意厚度无偏）。
      result.direction       = refracted;
      result.is_transmission = true;
      result.valid           = true;
      return result;
    }

    // ---------- 不透明部分：GGX 镜面 / Lambert 漫反射 ----------
    float3 wi;
    if(r.next_float() < material.specular_probability())
    {
      wi = ggx.sample_direction(wo, r.next_float2());
    }
    else
    {
      wi = build_basis(n).to_world(sample_cosine_hemisphere(r.next_float2()));
    }

    float n_dot_l = dot(n, wi);
    if(n_dot_l <= 0.0)
    {
      // 采样方向位于宏观表面以下：该方向的 BRDF 值为 0，
      // 去掉这份恰好为 0 的贡献不引入偏差，直接终止路径即可。
      return result;
    }

    float p = pdf(wi);
    if(p <= 0.0)
    {
      return result;
    }

    result.direction = wi;
    result.pdf       = p;
    result.weight    = eval(wi) * n_dot_l / p;
    result.valid     = true;
    return result;
  }
};
