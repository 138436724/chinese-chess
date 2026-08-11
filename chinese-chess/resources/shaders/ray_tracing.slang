import common;
import scene_data;
import lighting;

// ============================================================================
// ray_tracing.slang — 路径追踪主着色器（5 个入口）
//   rayGenMain           : 主光线生成 + 时域累积（子像素抖动抗锯齿 + firefly 钳制）
//   rayClosestHitMain    : NEE 直接光 + PBR 波瓣采样（GGX/Disney/透射）+ 俄罗斯轮盘赌
//   rayMissMain          : HDR 天空采样（带程序化回退）+ 高度雾
//   rayShadowMissMain    : 阴影光线未命中（光源可见）
//   rayShadowAnyHitMain  : 半透明概率穿透的阴影判定
// 数据结构与工具来自 scene_data.slang / common.slang / lighting.slang。
// ============================================================================

// ---- 恒定环境光（Ambient）----
// 保证完全阴影区域也有基础可见度；数值越小阴影越黑，越大整体越平
static const float AMBIENT_LIGHT = 0.25f;  // 环境光强度（相对漫反射亮度）

// ---- 高度雾参数（可调） ----
static const float  FOG_DENSITY   = 0.005f;                      // 基础雾密度
static const float  FOG_FALLOFF   = 50.0f;                       // 高度衰减距离（Y 每升高 falloff，密度降至 1/e）
static const float3 FOG_COLOR     = float3(0.7f, 0.75f, 0.82f);  // 雾散射色
static const float3 FOG_SUN_COLOR = float3(1.0f, 0.95f, 0.8f);   // 太阳散射色
static const float3 FOG_SUN_DIR   = float3(0.3f, 0.6f, 0.7f);    // 太阳方向

// ---- 玉石/半透明透射参数（可调） ----
static const float PIECE_THICKNESS         = 0.04f;  // 棋子厚度（Beer-Lambert 固定厚度）
static const float JADE_BASE_ABSORPTION    = 12.0f;  // 玉石底吸收系数：T = exp(-density × thickness)；调大 = 更不透
static const float JADE_ENGRAVE_ABSORPTION = 30.0f;  // 刻字区额外吸收（按 alpha 贴图权重叠加，刻痕更暗更实）

[[vk::push_constant]]
ConstantBuffer<rt_push_constant> push_const;                        // 相机逆矩阵 + 场景信息（scene_data.slang）
RaytracingAccelerationStructure  top_level_acceleration_structure;  // 场景加速结构
RWTexture2D<float4>              render_output;                     // 输出图像（时域累积）
StructuredBuffer<model_data>     models;                            // 模型运行时数据（binding 2）
StructuredBuffer<material_data>  materials;                         // 材质数据（binding 3）
StructuredBuffer<light_data>     lights;                            // 灯光数据（binding 4）
Sampler2D[] samplers;                                               // bindless 纹理数组（binding 5）
// 注意：SSBO 默认 std430 布局，UBO 为 std140。

// ---- 光线载荷：沿路径传递累积辐射度与吞吐量 ----
struct HitPayload
{
  float3 color;                // 累积辐射度
  float3 throughput;           // 路径吞吐量（BRDF × cos / PDF 连乘）
  int    depth;                // 当前弹射深度
  uint   seed;                 // 蒙特卡洛随机种子
  bool   inside_transmissive;  // 是否处于透射介质内部（玉石穿入/穿出判定）
  float  texture_weight;       // 材质 alpha 权重（刻字区=1，玉石底=0），用于玉石吸收调制
};

// ============================================================================
// 主光线生成
// ============================================================================
[shader("raygeneration")]
void rayGenMain()
{
  float2 launchID   = (float2)DispatchRaysIndex().xy;       // 当前像素
  float2 launchSize = (float2)DispatchRaysDimensions().xy;  // 图像尺寸

  // 背面剔除：闭合几何可加快 BVH 遍历
  const uint rayFlags = RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

  // 初始化 RNG 种子：像素 + 帧号（必须在子像素抖动前）
  uint2 pixelIndex = DispatchRaysIndex().xy;
  uint  seed = wang_hash(pixelIndex.x * 1664525u + pixelIndex.y * 1013904223u + push_const.frame_index * 0x9e3779b9u);

  // 子像素抖动：每帧随机偏移 [0,1) 像素，时域累积实现抗锯齿
  float2 clipCoords = (launchID + random_float2(seed)) / launchSize * 2.f - 1.f;

  // 近/远平面逆投影（Vulkan NDC 深度 [0,1]）
  float4 nearPointV = mul(push_const.proj_inv_matrix, float4(clipCoords, 0.f, 1.f));
  float4 farPointV  = mul(push_const.proj_inv_matrix, float4(clipCoords, 1.f, 1.f));
  nearPointV /= nearPointV.w;
  farPointV /= farPointV.w;

  float3 worldNear = mul(push_const.view_inv_matrix, nearPointV).xyz;
  float3 worldFar  = mul(push_const.view_inv_matrix, farPointV).xyz;

  RayDesc ray;
  ray.Origin    = worldNear;
  ray.Direction = normalize(worldFar - worldNear);
  ray.TMin      = 0.001f;  // 避免自相交
  ray.TMax      = 1e23f;   // 主光线视为无限远

  HitPayload payload;
  payload.color               = float3(0.f);
  payload.throughput          = float3(1.f);
  payload.depth               = 0;
  payload.seed                = seed;
  payload.inside_transmissive = false;
  payload.texture_weight      = 0.f;

  // SBT：主命中组 0，主 miss 0
  TraceRay(top_level_acceleration_structure, rayFlags, 0xff, 0, 0, 0, ray, payload);

  // ---- 时域累积 ----
  float3 prev_color = render_output[int2(launchID)].rgb;

  // Firefly 钳制：新采样亮度不超过已累积均值 × 4
  float prev_lum = max(luminance(prev_color), 0.01f);
  float new_lum  = luminance(payload.color);
  if(new_lum > prev_lum * 4.0f)
  {
    payload.color *= (prev_lum * 4.0f) / max(new_lum, 1e-6f);
  }

  // 自适应混合：早期快速收敛（近 EMA），后期接近算术平均
  // alpha = 1 / (1 + N/2)
  float  alpha       = 1.0f / (1.0f + float(push_const.frame_index) * 0.5f);
  float3 final_color = lerp(prev_color, payload.color, alpha);

  render_output[int2(launchID)] = float4(final_color, 1.f);
}

// ============================================================================
// 最近命中：直接光（NEE）+ 间接弹射（PBR 波瓣）
// ============================================================================
[shader("closesthit")]
void rayClosestHitMain(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  uint     primitiveID = PrimitiveIndex();
  uint32_t model_index = InstanceIndex();
  float2   bary        = attr.barycentrics;

  model_data now_model = models[model_index];

  uint32_t i0 = now_model.index_address[primitiveID * 3u + 0u];
  uint32_t i1 = now_model.index_address[primitiveID * 3u + 1u];
  uint32_t i2 = now_model.index_address[primitiveID * 3u + 2u];

  Vertex v0 = now_model.vertex_address[i0];
  Vertex v1 = now_model.vertex_address[i1];
  Vertex v2 = now_model.vertex_address[i2];

  // 重心插值：世界空间位置 / 法线 / UV
  float3 local_position = interpolate(bary, v0.position, v1.position, v2.position);
  float3 world_position = mul(now_model.model_matrix, float4(local_position, 1.f)).xyz;

  float3 local_normal = normalize(interpolate(bary, v0.normal, v1.normal, v2.normal));
  float3 world_normal = normalize(mul(now_model.model_matrix, float4(local_normal, 0.f)).xyz);

  float2 uv = interpolate(bary, v0.uv, v1.uv, v2.uv);

  // ---- 材质解析（默认值保证无材质/无纹理时也有合理结果） ----
  float3 material_color        = float3(0.f);
  float  material_roughness    = 0.5f;
  float  material_metallic     = 0.0f;
  float  material_opacity      = 1.0f;
  float  material_ior          = 1.5f;
  float  material_transmission = 0.0f;

  if(now_model.material_index != 0xFFFFFFFFu)
  {
    material_data now_material = materials[now_model.material_index];
    material_roughness         = now_material.roughness;
    material_metallic          = now_material.metallic;
    material_opacity           = now_material.opacity;
    material_ior               = now_material.ior;
    material_transmission      = now_material.transmission;

    if(now_material.texture_index != 0xFFFFFFFFu)
    {
      Sampler2D now_texture  = samplers[now_material.texture_index];
      float     weight       = now_texture.SampleLevel(uv, 0.f).x;
      material_color         = lerp(now_material.background_color, now_material.foreground_color, weight);
      payload.texture_weight = weight;  // 记录刻字权重，供透射分支调制吸收
    }
    else
    {
      material_color = now_material.background_color;
    }
  }

  // ---- 高度雾：衰减从上一弹射点到当前命中点的辐射 ----
  if(FOG_DENSITY > 0.0f)
  {
    float      bounce_dist = length(WorldRayOrigin() - world_position);
    fog_result fog = evaluate_height_fog(WorldRayOrigin(), normalize(world_position - WorldRayOrigin()), bounce_dist,
                                         FOG_DENSITY, FOG_FALLOFF, FOG_COLOR, FOG_SUN_COLOR, FOG_SUN_DIR);
    payload.color  = payload.color * fog.transmittance + fog.inscatter * payload.throughput;
  }

  // ===== 直接光照：NEE（Next Event Estimation） =====
  float3 V     = normalize(WorldRayOrigin() - world_position);  // 视线方向
  float3 F0    = lerp(float3(0.04f), material_color, material_metallic);
  float  NdotV = max(dot(world_normal, V), 0.001f);
  float3 kS    = f_schlick(F0, NdotV);

  // 钳制 roughness，避免高光 firefly
  float clamped_roughness = max(material_roughness, 0.04f);

  float3 direct_lighting = float3(0.f);
  for(uint li = 0; li < push_const.light_count; li++)
  {
    light_data light = lights[li];
    if(light.intensity <= 0.0f)
      continue;

    light_sample ls = sample_light(light, world_position, payload.seed);
    if(all(ls.color <= 0.0f))
      continue;

    float NdotL = max(dot(world_normal, ls.direction), 0.0f);
    if(NdotL <= 0.0f)
      continue;

    float3 shadow_origin = world_position + world_normal * SHADOW_EPSILON;
    if(trace_shadow_ray(top_level_acceleration_structure, shadow_origin, ls.direction, ls.distance, payload.seed))
      continue;

    // 漫反射 lobe（Lambertian，被金属度与 Fresnel 抑制）+ 镜面 lobe（GGX Cook-Torrance）
    float3 diffuse  = (float3(1.0f) - kS) * (1.0f - material_metallic) * material_color * INV_PI;
    float3 specular = evaluate_ggx(F0, clamped_roughness, world_normal, V, ls.direction);

    // MIS 权重：directional/point/spot 均为 delta 光源（pdf=1），BRDF 重要性采样命中
    // delta 光源的概率恒为 0，NEE 是唯一采样策略 → w_light 恒为 1（对非 delta 光源需恢复 MIS）
    float w_light = 1.0f;

    float3 contribution = ls.color * (diffuse + specular) * NdotL * w_light;

    // 单光源贡献钳制：抑制极端 firefly，保留 HDR
    float contrib_lum = luminance(contribution);
    float clamp_max   = min(luminance(payload.throughput) * 100.0f, 100.0f);
    if(contrib_lum > clamp_max)
    {
      contribution *= clamp_max / max(contrib_lum, 1e-6f);
    }

    direct_lighting += contribution;
  }
  // 恒定环境光：即使所有光源都被遮挡（死黑阴影），也提供基础可见度。
  // 乘 NdotV 衰减，避免轮廓边缘（掠射角）过度发亮。
  float3 ambient = material_color * AMBIENT_LIGHT * (0.5f + 0.5f * NdotV);
  direct_lighting += ambient;

  payload.color += payload.throughput * direct_lighting;

  // ===== 间接光照：PBR 波瓣采样 + 路径弹射 =====
  if(payload.depth >= MAX_BOUNCES)
    return;

  // 俄罗斯轮盘赌：基于吞吐量亮度决定继续概率
  float throughput_lum = luminance(payload.throughput);
  float p_continue     = min(1.f, throughput_lum);
  if(p_continue < 0.05f || random_float(payload.seed) > p_continue)
    return;

  // ---- 波瓣选择：透射(GGX)/反射(GGX)/漫射(Disney) ----
  float3 ray_dir  = WorldRayDirection();
  float3 N_face   = dot(ray_dir, world_normal) < 0.f ? world_normal : -world_normal;
  bool   is_entry = !payload.inside_transmissive;

  float trans_prob = clamp(material_opacity * material_transmission, 0.f, 1.f);
  if(trans_prob <= 0.f)
  {
    // 不透明表面：退出介质状态
    payload.inside_transmissive = false;
  }

  // 介质界面 Fresnel（基于 ior）
  float f0_dielectric = pow((material_ior - 1.f) / (material_ior + 1.f), 2.f);
  float F_interface   = f0_dielectric + (1.f - f0_dielectric) * pow5(1.f - abs(dot(ray_dir, N_face)));

  float specular_prob = clamp(lerp(max_component(kS), 1.f, material_metallic), 0.05f, 0.95f);
  specular_prob *= (1.f - trans_prob);  // 与透射概率归一化，三分支概率和为 1

  bool   is_transmission = false;
  float3 bounce_direction;
  float3 lobe_value;
  float  lobe_prob;

  float r = random_float(payload.seed);
  if(r < trans_prob)
  {
    // 透射分支：折射进入/穿出介质，Beer-Lambert 吸收
    is_transmission = true;
    lobe_prob       = trans_prob;
    float  eta      = is_entry ? (1.f / max(material_ior, 1.01f)) : max(material_ior, 1.01f);
    float3 T        = refract(ray_dir, N_face, eta);
    if(dot(T, T) < 1e-8f)
    {
      // 全反射：退回反射方向，内部状态不变
      bounce_direction = reflect(ray_dir, N_face);
      lobe_value       = float3(1.f);
    }
    else
    {
      bounce_direction            = T;
      payload.inside_transmissive = !payload.inside_transmissive;
      // 仅进入时应用 Beer-Lambert 吸收；刻字区（texture_weight 高）吸收更强，呈现玉石刻痕
      float  absorption_density = JADE_BASE_ABSORPTION + JADE_ENGRAVE_ABSORPTION * payload.texture_weight;
      float3 absorption         = is_entry ? exp(-absorption_density * PIECE_THICKNESS).xxx : float3(1.f);
      lobe_value                = (1.f - F_interface) * absorption;
    }
  }
  else if(r < trans_prob + specular_prob)
  {
    // 镜面 GGX 弹射（VNDF 重要性采样）
    brdf_sample bs = sample_ggx_brdf(material_color, F0, clamped_roughness, world_normal, V, payload.seed);
    if(bs.pdf <= 0.f || dot(world_normal, bs.direction) <= 0.f)
      return;
    bounce_direction = bs.direction;
    lobe_value       = bs.value;
    lobe_prob        = specular_prob;
  }
  else
  {
    // 漫射 Disney 弹射
    brdf_sample bs = sample_disney_diffuse(material_color, clamped_roughness, world_normal, V, payload.seed);
    if(bs.pdf <= 0.f || dot(world_normal, bs.direction) <= 0.f)
      return;
    bounce_direction = bs.direction;
    lobe_value       = bs.value;
    lobe_prob        = 1.f - trans_prob - specular_prob;
  }

  // 吞吐量更新：× BRDF_value / (lobe_prob × RR_prob)
  float3 new_throughput = payload.throughput * lobe_value / (lobe_prob * p_continue);

  // 路径钳制：防止高反射 + 不良采样导致能量爆炸
  const float MAX_THROUGHPUT = 10.0f;
  float       tput_lum       = luminance(new_throughput);
  if(tput_lum > MAX_THROUGHPUT)
  {
    new_throughput *= MAX_THROUGHPUT / tput_lum;
  }

  // 弹射光线：透射必须能命中背面以穿出薄盘 → 不剔除背面
  const float bounce_epsilon   = 0.001f;
  const uint  bounce_ray_flags = is_transmission ? 0u : RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

  RayDesc bounce_ray;
  bounce_ray.Origin    = world_position + N_face * bounce_epsilon;
  bounce_ray.Direction = bounce_direction;
  bounce_ray.TMin      = bounce_epsilon;
  bounce_ray.TMax      = 1e23f;

  HitPayload next_payload;
  next_payload.color               = payload.color;
  next_payload.throughput          = new_throughput;
  next_payload.depth               = payload.depth + 1;
  next_payload.seed                = payload.seed;
  next_payload.inside_transmissive = payload.inside_transmissive;
  next_payload.texture_weight      = payload.texture_weight;

  TraceRay(top_level_acceleration_structure, bounce_ray_flags, 0xff, 0, 0, 0, bounce_ray, next_payload);

  payload.color = next_payload.color;
}

// ============================================================================
// Miss：天空（HDR 环境贴图优先，程序化天空回退）+ 高度雾
// ============================================================================
[shader("miss")]
void rayMissMain(inout HitPayload payload)
{
  float3 sky_color = sample_sky(WorldRayDirection());

  if(push_const.skybox_index != 0xFFFFFFFFu)
  {
    // 尝试 HDR 环境贴图；过暗时回退程序化天空，保证 MISS 路径有基础亮度
    float3 hdr_color = sample_hdr_sky(WorldRayDirection(), samplers[push_const.skybox_index]);
    float  hdr_lum   = luminance(hdr_color);
    sky_color        = (hdr_lum > 0.02f) ? hdr_color : sky_color;
  }

  if(FOG_DENSITY > 0.0f)
  {
    fog_result fog = evaluate_height_fog(WorldRayOrigin(), WorldRayDirection(), 1e30f, FOG_DENSITY, FOG_FALLOFF,
                                         FOG_COLOR, FOG_SUN_COLOR, FOG_SUN_DIR);
    payload.color = payload.color * fog.transmittance + (sky_color * fog.transmittance + fog.inscatter) * payload.throughput;
  }
  else
  {
    payload.color += payload.throughput * sky_color;
  }
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
// 阴影 AnyHit：半透明材质（opacity < 1）按概率穿透 → 柔和玉石影
// ============================================================================
[shader("anyhit")]
void rayShadowAnyHitMain(inout shadow_payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  uint32_t   model_index = InstanceIndex();
  model_data now_model   = models[model_index];
  if(now_model.material_index != 0xFFFFFFFFu)
  {
    material_data now_material = materials[now_model.material_index];
    if(now_material.opacity < 1.f && random_float(payload.seed) >= now_material.opacity)
    {
      IgnoreHit();  // 概率穿过：本表面不遮挡
      return;
    }
  }
  payload.is_shadowed = true;
  AcceptHitAndEndSearch();
}
