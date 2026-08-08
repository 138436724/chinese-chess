import utils;

// ============================================================================
// lighting.slang — 光线追踪灯光处理模块
// 提供光源数据结构、采样函数、BRDF 评估和阴影光线追踪
// ============================================================================

enum light_type : uint32_t
{
    directional = 0,
    point = 1,
    spot = 2
};

// ---- BRDF 类型枚举 ----
enum brdf_type : uint32_t
{
    lambertian = 0,
    ggx = 1 // 预留：微面元 BRDF
};

// ---- 光源数据结构（std430 布局，与 C++ scene_light_manager.cpp 中 light_data 对齐） ----
struct light_data
{
    float3 color;           // 光源颜色 (RGB)，线性空间
    uint32_t active_type;   // LightType 枚举值: 0=方向光, 1=点光源, 2=聚光灯
    float3 direction;       // 方向（单位向量，方向光 & 聚光灯使用）
    float intensity;        // 强度倍率
    float3 position;        // 世界坐标位置（点光源 & 聚光灯使用）
    float range;            // 最大照射距离（点光源 & 聚光灯使用）
    float inner_cone_angle; // 内锥角（弧度，聚光灯使用）
    float outer_cone_angle; // 外锥角（弧度，聚光灯使用）
};

// ---- 光照采样结果（NEE 采样返回值） ----
struct light_sample
{
    float3 direction; // 从命中点到光源的方向（单位向量）
    float distance;   // 命中点到光源的距离（用于阴影光线 tmax）
    float3 color;     // 光源在该采样方向的辐射亮度
    float pdf;        // 采样概率密度函数值（用于 MIS）
};

// ---- BRDF 采样结果（BSDF 采样返回值） ----
struct brdf_sample
{
    float3 direction; // 采样出的出射方向（单位向量，世界空间）
    float pdf;        // 采样概率密度函数值（用于 MIS）
    float3 value;     // BRDF 值 × cos_theta / pdf（用于路径吞吐量累积）
};

// ---- 阴影光线载荷（轻量级，仅需知道是否被遮挡） ----
struct shadow_payload
{
    bool is_shadowed;
};

// ---- 半球均匀采样（以法线为中心的切线空间） ----
float3 sample_hemisphere_uniform(float3 N, inout uint seed)
{
    float2 r = random_float2(seed);
    float z = r.x;
    float r_xy = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2.0 * PI * r.y;
    float3 local_dir = float3(r_xy * cos(phi), r_xy * sin(phi), z);

    // 构建切线空间基向量
    float3 T, B;
    if (abs(N.x) > abs(N.y))
        T = normalize(cross(N, float3(0.0, 1.0, 0.0)));
    else
        T = normalize(cross(N, float3(1.0, 0.0, 0.0)));
    B = cross(N, T);

    return T * local_dir.x + B * local_dir.y + N * local_dir.z;
}

// ---- 余弦加权半球采样（重要性采样 Lambertian BRDF） ----
float3 sample_hemisphere_cosine(float3 N, inout uint seed)
{
    float2 r = random_float2(seed);
    float phi = 2.0 * PI * r.x;
    float cos_theta = sqrt(r.y);
    float sin_theta = sqrt(max(0.0, 1.0 - r.y));
    float3 local_dir = float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    // 构建切线空间
    float3 T, B;
    if (abs(N.x) > abs(N.y))
        T = normalize(cross(N, float3(0.0, 1.0, 0.0)));
    else
        T = normalize(cross(N, float3(1.0, 0.0, 0.0)));
    B = cross(N, T);

    return T * local_dir.x + B * local_dir.y + N * local_dir.z;
}

// ============================================================================
// 光源采样函数（NEE 核心）
// ============================================================================

// ---- 方向光采样 ----
// 方向光为 delta 分布（pdf=1.0），方向始终指向光源
light_sample sample_directional_light(light_data light, float3 hit_pos, inout uint seed)
{
    light_sample ls;
    ls.direction = normalize(light.direction);
    ls.color = light.color * light.intensity;
    ls.pdf = 1.0;       // delta 分布
    ls.distance = 1e10; // 无穷远
    return ls;
}

// ---- 点光源采样 ----
// 点光源为 delta 分布，含距离平方衰减和 range 裁剪
light_sample sample_point_light(light_data light, float3 hit_pos, inout uint seed)
{
    light_sample ls;
    float3 to_light = light.position - hit_pos;
    float dist = length(to_light);
    ls.direction = to_light / dist;
    ls.distance = dist;

    // 距离平方衰减
    float attenuation = 1.0 / max(dist * dist, 0.001);
    // range 裁剪（线性衰减到 0）
    float range_atten = (light.range > 0.0) ? max(0.0, 1.0 - dist / light.range) : 1.0;
    ls.color = light.color * light.intensity * attenuation * range_atten;
    ls.pdf = 1.0; // delta 分布（确定性采样）
    return ls;
}

// ---- 聚光灯采样 ----
// 聚光灯为 delta 分布，含锥角衰减（内外锥平滑过渡）
light_sample sample_spot_light(light_data light, float3 hit_pos, inout uint seed)
{
    light_sample ls;
    float3 to_light = light.position - hit_pos;
    float dist = length(to_light);
    ls.direction = to_light / dist;
    ls.distance = dist;

    float3 light_forward = normalize(light.direction);
    float cos_angle = dot(-ls.direction, light_forward);

    // 锥角衰减
    float cone_atten = 1.0;
    if (cos_angle < cos(light.outer_cone_angle))
    {
        // 完全在锥外，无光照
        ls.color = float3(0.0, 0.0, 0.0);
        ls.pdf = 1.0;
        return ls;
    }
    if (cos_angle < cos(light.inner_cone_angle))
    {
        // 内外锥之间平滑过渡（二次衰减）
        float t = (cos_angle - cos(light.outer_cone_angle)) /
                  max(cos(light.inner_cone_angle) - cos(light.outer_cone_angle), 0.001);
        cone_atten = t * t;
    }

    // 距离衰减 + range 裁剪
    float attenuation = 1.0 / max(dist * dist, 0.001);
    float range_atten = (light.range > 0.0) ? max(0.0, 1.0 - dist / light.range) : 1.0;
    ls.color = light.color * light.intensity * attenuation * range_atten * cone_atten;
    ls.pdf = 1.0; // delta 分布
    return ls;
}

// 根据 light.active_type 调度到对应的采样函数
light_sample sample_light(light_data light, float3 hit_pos, inout uint seed)
{
    switch (light.active_type)
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
        ls.color = float3(0.0, 0.0, 0.0);
        ls.pdf = 1.0;
        ls.distance = 0.0;
        return ls;
    }
}

// ============================================================================
// BRDF 函数族（Lambertian + 扩展接口）
// ============================================================================

// ---- Lambertian BRDF 评估 ----
// 返回 f_r = albedo / PI
float3 evaluate_lambertian(float3 albedo)
{
    return albedo * INV_PI;
}

// ---- Lambertian BRDF 采样（余弦加权半球） ----
// 使用重要性采样：pdf = cos_theta / PI
// value = f_r * cos_theta / pdf = albedo
brdf_sample sample_lambertian(float3 albedo, float3 N, inout uint seed)
{
    brdf_sample bs;
    bs.direction = sample_hemisphere_cosine(N, seed);
    bs.value = albedo; // cos_theta / pdf 在余弦加权采样下抵消
    bs.pdf = max(0.0, dot(N, bs.direction)) * INV_PI;
    return bs;
}

// ---- Lambertian BRDF PDF ----
// 给定入射方向 L，返回 pdf(L) = max(cos(N·L), 0) / PI
float pdf_lambertian(float3 N, float3 L)
{
    return max(0.0, dot(N, L)) * INV_PI;
}

// ---- 统一 BRDF 评估（按 brdf_type 调度） ----
// ============================================================================
// GGX / Cook-Torrance 微面元 BRDF（高光）
// ============================================================================

// ---- GGX/Trowbridge-Reitz 法线分布函数 ----
float d_ggx(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 * INV_PI / max(denom * denom, 1e-5);
}

// ---- Smith 几何遮蔽函数（Schlick-GGX 近似） ----
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

// ---- Fresnel-Schlick ----
float3 f_schlick(float3 F0, float VdotH)
{
    return F0 + (float3(1.0) - F0) * pow(max(1.0 - VdotH, 0.0), 5.0);
}

// ---- GGX BRDF 评估 ----
// 返回 fr = D*G*F / (4*NdotV*NdotL)，调用方需自行乘以 NdotL 和 Li
float3 evaluate_ggx(float3 F0, float roughness, float3 N, float3 V, float3 L)
{
    float3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);

    if (NdotL <= 0.0 || NdotV <= 0.0) return float3(0.0);

    float D = d_ggx(NdotH, roughness);
    float G = g_smith(NdotV, NdotL, roughness);
    float3 F = f_schlick(F0, max(dot(V, H), 0.0));

    return D * G * F / max(4.0 * NdotV * NdotL, 0.001);
}

// ---- GGX 重要性采样（VNDF 采样） ----
float3 sample_ggx(float3 V, float roughness, float3 N, inout uint seed)
{
    float2 r = random_float2(seed);
    float a = roughness * roughness;

    // 各项同性 GGX 半球采样
    float phi = 2.0 * PI * r.x;
    float cos_theta = sqrt(max(0.0, (1.0 - r.y) / (1.0 + (a * a - 1.0) * r.y)));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
    float3 H_local = float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    // 构建切线空间（以 N 为 Z 轴）
    float3 T, B;
    if (abs(N.x) > abs(N.y))
        T = normalize(cross(N, float3(0.0, 1.0, 0.0)));
    else
        T = normalize(cross(N, float3(1.0, 0.0, 0.0)));
    B = cross(N, T);

    float3 H = T * H_local.x + B * H_local.y + N * H_local.z;
    return normalize(reflect(-V, H));
}

// ---- GGX BRDF 采样（返回 brdf_sample） ----
brdf_sample sample_ggx_brdf(float3 albedo, float3 F0, float roughness, float3 N, float3 V, inout uint seed)
{
    brdf_sample bs;
    float3 L = sample_ggx(V, roughness, N, seed);
    bs.direction = L;

    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0)
    {
        bs.value = float3(0.0);
        bs.pdf = 0.0;
        return bs;
    }

    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.001);

    // PDF = D * NdotH / (4 * VdotH)
    bs.pdf = d_ggx(NdotH, roughness) * NdotH / max(4.0 * VdotH, 0.001);
    // value = f_r * NdotL / pdf
    bs.value = evaluate_ggx(F0, roughness, N, V, L) * NdotL / max(bs.pdf, 0.001);

    return bs;
}

float3 evaluate_brdf(uint type, float3 albedo, float3 N, float3 V, float3 L, float roughness)
{
    switch (type)
    {
    case brdf_type::lambertian:
        return evaluate_lambertian(albedo);
    case brdf_type::ggx:
        return evaluate_ggx(albedo, roughness, N, V, L);
    default:
        return evaluate_lambertian(albedo);
    }
}

// ---- 统一 BRDF 采样（按 brdf_type 调度） ----
brdf_sample sample_brdf(uint type, float3 albedo, float3 N, float3 V, float roughness, inout uint seed)
{
    switch (type)
    {
    case brdf_type::lambertian:
        return sample_lambertian(albedo, N, seed);
    // case BrdfType_GGX:
    //     return sample_ggx(albedo, N, V, roughness, seed);
    default:
        return sample_lambertian(albedo, N, seed);
    }
}

// ---- 统一 BRDF PDF（按 brdf_type 调度） ----
float pdf_brdf(uint type, float3 N, float3 V, float3 L, float roughness)
{
    switch (type)
    {
    case brdf_type::lambertian:
        return pdf_lambertian(N, L);
    // case BrdfType_GGX:
    //     return pdf_ggx(N, V, L, roughness);
    default:
        return pdf_lambertian(N, L);
    }
}

// ============================================================================
// 阴影光线追踪
// ============================================================================

// ---- 追踪阴影光线 ----
// 封装 TraceRay 调用，使用命中组索引 1（anyhit_shadow），miss 索引 1（miss_shadow）
// 参数：
//   tlas      - 顶层加速结构（由 ray_tracing.slang 的全局绑定提供）
//   origin    - 光线起点（命中点 + 法线偏移避开自相交）
//   direction - 阴影光线方向（指向光源，单位向量）
//   tmax      - 最大传播距离（光源距离）
//   seed      - 随机数种子（保留用于未来软阴影/面积光源扩展）
// 返回：true 表示被遮挡（在阴影中），false 表示光源可见
bool trace_shadow_ray(RaytracingAccelerationStructure tlas, float3 origin, float3 direction, float tmax,
                      inout uint seed)
{
    const uint shadow_ray_flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
                                  | RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = SHADOW_EPSILON;
    ray.TMax = tmax - SHADOW_EPSILON;

    shadow_payload payload;
    payload.is_shadowed = false;

    // SBT 偏移：
    //   RayContributionToHitGroupIndex = 1 → anyhit_shadow
    //   MissShaderIndex                = 1 → miss_shadow
    TraceRay(tlas, shadow_ray_flags, 0xff,
             1, // RayContributionToHitGroupIndex: shadow hit group
             0, // MultiplierForGeometryContributionToHitGroupIndex
             1, // MissShaderIndex: shadow miss shader
             ray, payload);

    return payload.is_shadowed;
}

// ============================================================================
// MIS（多重重要性采样）权重函数
// ============================================================================

// ---- 幂启发式（Power Heuristic，Veach 1997） ----
// 推荐 β = 2，用于组合不同采样策略
// 返回策略 a 的 MIS 权重：w_a = pdf_a² / (∑ pdf_i²)
// 调用示例：
//   float w_light = power_heuristic(light_sample.pdf, brdf_sample.pdf);
//   float w_brdf  = power_heuristic(brdf_sample.pdf, light_sample.pdf);
float power_heuristic(float pdf_a, float pdf_b)
{
    float a2 = pdf_a * pdf_a;
    float b2 = pdf_b * pdf_b;
    return a2 / (a2 + b2 + 1e-5f);
}

// ---- 球体均匀采样方向 ----
// 在整个单位球面上均匀采样方向，用于天空/环境光照重要性采样
float3 sample_uniform_sphere(inout uint seed)
{
    float2 r = random_float2(seed);
    float z = 1.0f - 2.0f * r.x;
    float r_xy = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2.0 * PI * r.y;
    return float3(r_xy * cos(phi), r_xy * sin(phi), z);
}

// ============================================================================
// 天空/环境光照采样函数
// ============================================================================

// ---- 天空颜色采样（默认天空色） ----
// 当前阶段使用硬编码天空色，与 ray_tracing.slang 中 rayMissMain 的颜色保持一致。
// 后续可扩展为 HDR 环境贴图查找。
// 参数 direction：世界空间光线方向（用于未来渐变/贴图查找）
// 返回：天空辐射亮度（线性空间 RGB）
float3 sample_sky(float3 direction)
{
    // 默认天空色 — 与 ray_tracing.slang rayMissMain 中的 sky_color 一致
    const float3 DEFAULT_SKY_COLOR = float3(0.53f, 0.81f, 0.98f);
    return DEFAULT_SKY_COLOR;
}

// ---- 天空颜色采样（自定义天空色） ----
// 支持从 push_constant 传入天空色，替代硬编码默认值。
// 参数 direction：世界空间光线方向
// 参数 sky_color：自定义天空色（线性空间 RGB）
float3 sample_sky(float3 direction, float3 sky_color)
{
    return sky_color;
}

// ---- HDR 环境贴图天空采样 ----
// 从等距矩形投影（Equirectangular）的 HDR 环境贴图中采样天空辐射亮度。
// 参数 direction：世界空间光线方向（单位向量）
// 参数 hdr_env_map：HDR 环境贴图（2D 纹理，等距矩形投影，线性空间 HDR 浮点格式）
// 参数 hdr_sampler：用于采样的 SamplerState
// 返回：天空辐射亮度（线性空间 RGB，HDR 范围）
float3 sample_hdr_sky(float3 direction, Sampler2D hdr_env_map)
{
    // 等距矩形投影：方向向量 → UV 坐标
    // u ∈ [0, 1] 对应方位角 [-π, π]
    // v ∈ [0, 1] 对应极角 [π, 0]（从上到下）
    float2 uv = float2(
        0.5f + 0.5f * atan2(direction.z, direction.x) * INV_PI,
        1.0f - acos(clamp(direction.y, -1.0f, 1.0f)) * INV_PI
    );
    return hdr_env_map.SampleLevel(uv, 0.0f).rgb;
}
