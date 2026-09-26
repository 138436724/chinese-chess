import common;
import scene_data;
import lighting;
import shading;

// ============================================================================
// ray_tracing.slang — 路径追踪主着色器（5 个入口）
//   rayGenMain           : 主光线生成 + 时域累积（子像素抖动抗锯齿 + firefly 钳制）
//   rayClosestHitMain    : NEE 直接光（MIS）+ 表面 BSDF 采样 + 俄罗斯轮盘赌
//   rayMissMain          : HDR 天空采样（带程序化回退）+ 高度雾
//   rayShadowMissMain    : 阴影光线未命中（光源可见）
//   rayShadowAnyHitMain  : 投射/透射概率的阴影判定
//
// 材质求值与 BSDF 全部来自 shading.slang（唯一入口）；光源与大气来自
// lighting.slang。本文件只负责光线的组织:命中 → 求值 → 采样 → 弹射。
//
// 物理正确性约定（见 pbr-exploration-plan.md）:
//  - 介质透射沿**实际穿越距离**累积 Beer-Lambert 光学厚度（payload 记录入射点）
//  - 界面反射/透射按 Fresnel 分配能量
//  - 所有启发式钳制均由 scene_params.debug_flags 控制,能量实验必须能全关
// ============================================================================

// ---- 高度雾参数（可调） ----
static const float  FOG_DENSITY   = 0.005f;                      // 基础雾密度
static const float  FOG_FALLOFF   = 50.0f;                       // 高度衰减距离（Y 每升高 falloff，密度降至 1/e）
static const float3 FOG_COLOR     = float3(0.7f, 0.75f, 0.82f);  // 雾散射色
static const float3 FOG_SUN_COLOR = float3(1.0f, 0.95f, 0.8f);   // 太阳散射色
static const float3 FOG_SUN_DIR   = float3(0.3f, 0.6f, 0.7f);    // 太阳方向

// ---- 路径限制与启发式钳制（全部可由 debug_flags 关闭） ----
static const float MAX_THROUGHPUT = 10.0f;  // 路径吞吐量上限
static const float RR_MIN_PROB    = 0.05f;  // 俄罗斯轮盘赌最低继续概率

RaytracingAccelerationStructure top_level_acceleration_structure;  // b0
RWTexture2D<float4>             render_output;                     // b1(时域累积输出)
ConstantBuffer<rt_scene_data>   scene_params;                      // b2(场景参数 UBO)
Sampler2D[]                     samplers;                          // b3(bindless 纹理数组)

// ---- 光线载荷：沿路径传递累积辐射度、吞吐量与介质状态 ----
struct HitPayload
{
  float3 color;                   // 累积辐射度
  float3 throughput;              // 路径吞吐量（BRDF × cos / PDF 连乘）
  int    depth;                   // 当前弹射深度
  uint   seed;                    // 蒙特卡洛随机种子
  bool   inside_transmissive;     // 是否处于透射介质内部
  float  texture_weight;          // 材质覆盖率（刻字=1，基底=0）
  float3 medium_entry_position;   // 进入介质的位置(用于求真实穿越距离)
  float  medium_travelled;        // 介质内已累积的穿越距离(用于嵌套介质)

  // 调试可视化:xyz = 通道值,w = 1 表示主命中已写入。
  // 只放一个 float4,不按通道各开字段 —— payload 随每跳传递,越小越好。
  float4 debug_viz;

  // 环境 NEE 的 MIS:BSDF 采样的弹射射线若命中天空,需要用**上一跳**的
  // 采样 pdf 来计算 balance heuristic 权重,否则环境会被
  // "环境 NEE"与"BSDF 采样命中天空"两条路径重复计入。
  // 权重在弹射**之前**算好(此时 s.pdf 就在手上),故只需存一个标量。
  float  env_mis_weight;    // 命中天空时施加的 MIS 权重
  bool   env_mis_valid;     // 仅反射类波瓣需要(透射穿出不算环境入射)
};

// ============================================================================
// 材质解析辅助
// ============================================================================

// 取材质的覆盖率(图集的 R 通道)。无纹理会返回 0 = 纯基底。
// 当前程序化资产是单通道字形图,故采样 .x 作为覆盖率;换真实 RGB albedo 时
// 同一槽位改采 .rgb 直接作反射率即可。
float sample_coverage(material_data m, float2 uv)
{
  if(m.albedo_index < scene_params.texture_count)
  {
    Sampler2D tex = samplers[m.albedo_index];
    return saturate(tex.SampleLevel(uv, 0.f).x);
  }
  return 0.0f;
}

// 贴图通道对材质参数的修正结果
struct material_texture_mod
{
  float3 baked_normal;   // 切线空间法线(已 ×2-1 解码)
  bool   has_normal;
  float  roughness_scale;  // 来自 ORM.g(1 = 不修改)
  float  metallic_scale;   // 来自 ORM.b(1 = 不修改)—— 与材质金属度相乘
  float  occlusion;        // 来自 ORM.r(1 = 无遮蔽)
  float  normal_scale;
  float  occlusion_strength;
};

material_texture_mod sample_material_textures(material_data m, float2 uv)
{
  material_texture_mod out;
  out.baked_normal       = float3(0.0, 0.0, 1.0);
  out.has_normal         = false;
  out.roughness_scale    = 1.0;
  out.metallic_scale     = 1.0;
  out.occlusion          = 1.0;
  out.normal_scale       = max(m.normal_scale, 0.0);
  out.occlusion_strength = saturate(m.occlusion_strength);

  if(m.normal_index < scene_params.texture_count && m.normal_scale > 0.0)
  {
    float3 n = samplers[m.normal_index].SampleLevel(uv, 0.f).xyz;
    // NormalMap 约定:编码在 [0,1],解码到 [-1,1]
    out.baked_normal = n * 2.0 - 1.0;
    out.has_normal   = true;
  }

  if(m.orm_index < scene_params.texture_count)
  {
    float3 orm         = samplers[m.orm_index].SampleLevel(uv, 0.f).xyz;
    out.occlusion      = saturate(lerp(1.0, orm.x, out.occlusion_strength));
    out.roughness_scale = saturate(orm.y);
    out.metallic_scale  = saturate(orm.z);
  }
  return out;
}

// ============================================================================
// 主光线生成
// ============================================================================
[shader("raygeneration")]
void rayGenMain()
{
  uint2  launchID   = DispatchRaysIndex().xy;                // 当前像素
  float2 launchSize = (float2)DispatchRaysDimensions().xy;  // 图像尺寸

  // 背面剔除：闭合几何可加快 BVH 遍历
  const uint rayFlags = RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

  // 初始化 RNG 种子：像素 + 帧号（必须在子像素抖动前）
  uint seed = wang_hash(launchID.x * 1664525u + launchID.y * 1013904223u + scene_params.frame_index * 0x9e3779b9u);

  // 子像素抖动：每帧随机偏移 [0,1) 像素，时域累积实现抗锯齿
  float2 clipCoords = (float2(launchID) + random_float2(seed)) / launchSize * 2.f - 1.f;

  // 近/远平面逆投影（Vulkan NDC 深度 [0,1]）
  float4 nearPointV = mul(scene_params.proj_inv_matrix, float4(clipCoords, 0.f, 1.f));
  float4 farPointV  = mul(scene_params.proj_inv_matrix, float4(clipCoords, 1.f, 1.f));
  nearPointV /= nearPointV.w;
  farPointV /= farPointV.w;

  float3 worldNear = mul(scene_params.view_inv_matrix, nearPointV).xyz;
  float3 worldFar  = mul(scene_params.view_inv_matrix, farPointV).xyz;

  RayDesc ray;
  ray.Origin    = worldNear;
  ray.Direction = normalize(worldFar - worldNear);
  ray.TMin      = 0.001f;  // 避免自相交
  ray.TMax      = 1e23f;   // 主光线视为无限远

  HitPayload payload;
  payload.color                 = float3(0.f);
  payload.throughput            = float3(1.f);
  payload.depth                 = 0;
  payload.seed                  = seed;
  payload.inside_transmissive   = false;
  payload.texture_weight        = 0.f;
  payload.medium_entry_position = float3(0.f);
  payload.medium_travelled      = 0.f;
  // 调试可视化:xyz = 通道值,w = 1 表示主命中已写入。
  // 只存一个 float4(而不是每通道一个字段)—— ray payload 越小越好:
  // 它随每一跳传递,maxRayPayloadSize 是实现相关的上限,且直接影响寄存器/scratch 压力。
  payload.debug_viz      = float4(0.f);
  payload.env_mis_weight = 1.f;
  payload.env_mis_valid  = false;

  // SBT：主命中组 0，主 miss 0
  TraceRay(top_level_acceleration_structure, rayFlags, 0xff, 0, 0, 0, ray, payload);

  // ---- 调试可视化通道(直接替换输出,不参与时域累积) ----
  // 通道值由 closest-hit 按当前 debug_flags **就近**求值(只算被选中的那一项),
  // 这里只负责写出。主光线未命中时 w 保持 0 → 走正常累积路径(显示天空)。
  if(payload.debug_viz.w > 0.5f)
  {
    render_output[int2(launchID)] = float4(payload.debug_viz.rgb, 1.f);
    return;
  }

  // ---- 时域累积 ----
  float3     prev_color  = render_output[int2(launchID)].rgb;
  const uint debug_flags = scene_params.debug_flags;

  // Firefly 钳制：新采样亮度不超过已累积均值 × 4。
  // 仅在累积帧生效（frame_index > 0）：重置后的首帧 alpha=1 整帧替换，
  // 此时 render_output 仍是旧视角画面，拿陈旧均值钳制会把新采样错误压暗。
  // 能量实验必须关闭它,否则测到的是钳制而非物理。
  if(scene_params.frame_index > 0u && (debug_flags & DEBUG_DISABLE_FIREFLY_CLAMP) == 0u)
  {
    float prev_lum = max(luminance(prev_color), 0.01f);
    float new_lum  = luminance(payload.color);
    if(new_lum > prev_lum * 4.0f)
    {
      payload.color *= (prev_lum * 4.0f) / max(new_lum, 1e-6f);
    }
  }

  // 自适应混合：早期快速收敛（近 EMA），后期接近算术平均
  // alpha = 1 / (1 + N/2)
  float  alpha       = 1.0f / (1.0f + float(scene_params.frame_index) * 0.5f);
  float3 final_color = lerp(prev_color, payload.color, alpha);

  render_output[int2(launchID)] = float4(final_color, 1.f);
}

// ============================================================================
// 最近命中：直接光（NEE + MIS）+ 表面 BSDF 弹射
// ============================================================================
[shader("closesthit")]
void rayClosestHitMain(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  uint   primitiveID = PrimitiveIndex();
  uint   model_index = InstanceIndex();
  float2 bary        = attr.barycentrics;

  model_data now_model = scene_params.models[model_index];

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
  // 法线变换必须用模型矩阵的**逆转置**:直接乘模型矩阵在非均匀缩放下是错的
  // (分量被错误缩放,法线不再垂直切平面 → 高光方向与 Fresnel 都偏)。
  // 这里用恒等式 (M^-1)^T = adj(M)^T / det(M);det 是标量,归一化会消掉,
  // 所以只需 adj(M)^T 再 normalize。UI 支持任意缩放,故必须处理。
  float3 world_normal = world_normal_from_matrix(now_model.model_matrix, local_normal);

  float2 uv = interpolate(bary, v0.uv, v1.uv, v2.uv);

  // ---- 材质解析(计数上界守卫同时覆盖"无材质"与越界索引) ----
  // 未命中材质时先构造一份零值材质,避免读取未初始化数据;后续 surf 会走
  // default_material() 分支,而介质/阴影等按需读取的字段为零(安全)。
  material_data now_material = (material_data)0;
  float         coverage     = 0.0f;
  if(now_model.material_index < scene_params.material_count)
  {
    now_material = scene_params.materials[now_model.material_index];
    coverage     = sample_coverage(now_material, uv);
  }

  // ---- 介质穿越(Beer-Lambert):必须在着色之前结算 ----
  // 从介质内部命中界面 = 出射界面,此时用"入射点 → 出射点"的真实弦长
  // 计算光学厚度,由 transmittance_medium 逐波长消光。玉石颜色即由此涌现。
  //
  // ⚠ `was_inside_medium` 必须在下面清掉标志**之前**捕获:后面的介质状态机
  // (判断本界面是入射还是出射)依赖它。早期版本在这里先清零、之后才用
  // `!payload.inside_transmissive` 判断,导致 entering_medium **恒为 true** ——
  // 穿出介质后又被标记成"在介质内",下一跳会把空气路径按命中物体的吸收衰减
  // (两个棋子前后重叠时,中间那段空气会被当成玉石内部消光)。
  const bool was_inside_medium = payload.inside_transmissive;

  if(was_inside_medium)
  {
    float  chord   = length(world_position - payload.medium_entry_position);
    float3 sigma_t = max(now_material.absorption_coefficient, float3(0.f))
                   + max(now_material.engrave_absorption, float3(0.f)) * payload.texture_weight;
    payload.color *= transmittance_medium(sigma_t, payload.medium_travelled + chord);
    payload.inside_transmissive = false;
    payload.medium_travelled    = 0.f;
  }

  // DEBUG_VIZ_DIRECT_ONLY 需要"只含直接光"的颜色:在直接光结算之前记录,
  // 函数出口处恢复,这样深层弹射写回 payload.color 也不会污染该通道。
  const bool direct_only  = (scene_params.debug_flags & DEBUG_VIZ_DIRECT_ONLY) != 0u;
  const float3 direct_color = payload.color;

  // ---- 贴图通道(法线 / ORM),并把 ORM 乘进材质参数 ----
  material_texture_mod tex_mod;
  if(now_model.material_index < scene_params.material_count)
  {
    tex_mod = sample_material_textures(now_material, uv);
  }

  // ---- 表面组装(唯一材质求值入口) ----
  surface_eval surf;
  if(now_model.material_index < scene_params.material_count)
  {
    surf.base    = resolve_material_data(now_material, 0.f);
    surf.engrave = resolve_material_data(now_material, 1.f);
  }
  else
  {
    surf.base    = default_material();
    surf.engrave = default_material();
  }

  // 贴图先于调试覆盖:调试覆盖是"最终强制",应该盖过贴图
  surf.base.roughness    = saturate(surf.base.roughness * tex_mod.roughness_scale);
  surf.base.metallic     = saturate(surf.base.metallic * tex_mod.metallic_scale);
  surf.engrave.roughness = saturate(surf.engrave.roughness * tex_mod.roughness_scale);
  surf.engrave.metallic  = saturate(surf.engrave.metallic * tex_mod.metallic_scale);

  surf.base             = apply_debug_override(surf.base, scene_params.debug_flags, scene_params.force_metallic, scene_params.force_roughness);
  surf.engrave          = apply_debug_override(surf.engrave, scene_params.debug_flags, scene_params.force_metallic, scene_params.force_roughness);

  // ---- 切线空间法线图:必须在"双面翻转"之前应用 ----
  // 顺序很关键:凹凸是相对于**几何**法线的扰动,先翻转再扰动会让背面凹凸反向。
  float3 shaded_normal = world_normal;
  if(tex_mod.has_normal)
  {
    geometric_tangent_space gts =
        build_geometric_tangent_space(mul(now_model.model_matrix, float4(v0.position, 1.f)).xyz,
                                      mul(now_model.model_matrix, float4(v1.position, 1.f)).xyz,
                                      mul(now_model.model_matrix, float4(v2.position, 1.f)).xyz,
                                      v0.uv, v1.uv, v2.uv);
    shaded_normal = apply_normal_map(tex_mod.baked_normal, tex_mod.normal_scale, world_normal, gts);
  }

  surf.coverage         = coverage;
  surf.occlusion        = tex_mod.occlusion;
  surf.geometric_normal = world_normal;
  // 双面着色:着色法线翻转朝向来光侧
  surf.normal           = dot(WorldRayDirection(), shaded_normal) < 0.f ? shaded_normal : -shaded_normal;

  const float3 N     = surf.normal;
  const float3 V     = normalize(WorldRayOrigin() - world_position);
  const float  NdotV = max(dot(N, V), 1e-4);

  // 调试可视化:仅主命中(depth == 0)且选中了某个通道时,就地算**那一个**值。
  // 只算被选中的通道 —— 既省掉每跳传递 4 个通道的 payload 空间,也省掉多余求值。
  if(payload.depth == 0)
  {
    // 通道互斥(UI 用 Combo 选择),按位判断顺序与 UI 一致
    if((scene_params.debug_flags & DEBUG_VIZ_NORMAL) != 0u)
    {
      payload.debug_viz = float4(N * 0.5f + 0.5f, 1.f);  // 法线编码到 [0,1]
    }
    else if((scene_params.debug_flags & DEBUG_VIZ_F0) != 0u)
    {
      layer_bsdf debug_layer = make_layer_bsdf(surf.base, NdotV);
      payload.debug_viz      = float4(lerp(debug_layer.f0_medium, debug_layer.metal_f0, debug_layer.metallic), 1.f);
    }
    else if((scene_params.debug_flags & DEBUG_VIZ_ROUGHNESS) != 0u)
    {
      layer_bsdf debug_layer = make_layer_bsdf(surf.base, NdotV);
      payload.debug_viz      = float4(debug_layer.roughness.xxx, 1.f);
    }
    else if((scene_params.debug_flags & DEBUG_VIZ_METALLIC) != 0u)
    {
      layer_bsdf debug_layer = make_layer_bsdf(surf.base, NdotV);
      payload.debug_viz      = float4(debug_layer.metallic.xxx, 1.f);
    }
  }

  // ---- 高度雾：衰减从上一弹射点到当前命中点的辐射 ----
  if(FOG_DENSITY > 0.0f && (scene_params.debug_flags & DEBUG_DISABLE_FOG) == 0u)
  {
    float      bounce_dist = length(WorldRayOrigin() - world_position);
    fog_result fog = evaluate_height_fog(WorldRayOrigin(), normalize(world_position - WorldRayOrigin()), bounce_dist,
                                         FOG_DENSITY, FOG_FALLOFF, FOG_COLOR, FOG_SUN_COLOR, FOG_SUN_DIR);
    payload.color  = payload.color * fog.transmittance + fog.inscatter * payload.throughput;
  }

  // ---- 透射材质:介质内部不做 NEE 直接光(光在介质内被吸收,直接光路径不成立) ----
  float p_transmission = saturate(surf.base.transmission);

  // 介质状态机:
  //   was_inside_medium = false → 本界面是**入射**界面,采样到透射 = 进入介质
  //   was_inside_medium = true  → 本界面是**出射**界面(Beer-Lambert 已在上方结算),
  //                               采样到透射 = 离开介质,回到外部
  bool entering_medium = !was_inside_medium && p_transmission > 0.f;

  // ===== 直接光照：NEE + MIS =====
  // 介质的**内部**不做 NEE:此时"直接光"要穿过介质界面,属于次表面传输,
  // 当前不做介质内散射(见计划阶段 2 的取舍记录)。介质**表面**照常做 NEE
  // (反射分量),所以这里不能按 transmission 跳过。
  {
    float3 direct_lighting = float3(0.f);
    // 白炉:关掉全部直接光,让环境成为唯一光源(否则方向光的镜面项
    // 会把粗糙度依赖混进来,无法判定能量守恒)
    uint light_count = ((scene_params.debug_flags & DEBUG_DISABLE_DIRECT_LIGHTS) != 0u) ? 0u : scene_params.light_count;

    for(uint li = 0; li < light_count; li++)
    {
      light_data light = scene_params.lights[li];
      if(light.intensity <= 0.0f)
        continue;

      light_sample ls = sample_light(light, world_position, payload.seed);
      if(all(ls.color <= 0.0f))
        continue;

      float NdotL = dot(N, ls.direction);
      if(NdotL <= 0.0f)
        continue;

      float3 shadow_origin = world_position + N * SHADOW_EPSILON;
      if(trace_shadow_ray(top_level_acceleration_structure, shadow_origin, ls.direction, ls.distance, payload.seed))
        continue;

      // BSDF 逐波长评估(漫反射 + 镜面,内部含二分量覆盖率混合)
      // 第三参 false:只取反射分量 —— 透射(介质折射)由路径采样负责,
      // 否则会在 NEE 里把介质能量重复计入。
      bsdf_value bsdf = evaluate_bsdf(surf, V, ls.direction, false);

      // MIS:光源采样与 BSDF 采样两侧用 balance heuristic 统一权重。
      // 当前光源均为 delta 分布(pdf = 1 表示"确定性采样",非真实密度),
      // 因此 BSDF 侧的命中概率视为 0 → 权重恒为 1,与既有实现一致。
      // 一旦引入面积光/环境光 NEE,此处自动恢复真正的 MIS。
      float w_light = 1.0f;

      float3 contribution = ls.color * bsdf.reflectance * w_light;

      // 单光源贡献钳制：抑制极端 firefly，保留 HDR（可由 debug_flags 关闭）
      // 启停判断写成提前返回,把 luminance 之类的开销挡在分支之外
      if((scene_params.debug_flags & DEBUG_DISABLE_DIRECT_CLAMP) != 0u)
      {
        direct_lighting += contribution;
        continue;
      }
      {
        float contrib_lum = luminance(contribution);
        float clamp_max   = min(luminance(payload.throughput) * 100.0f, 100.0f);
        if(contrib_lum > clamp_max)
        {
          contribution *= clamp_max / max(contrib_lum, 1e-6f);
        }
      }
      direct_lighting += contribution;
    }

    // ===== 环境光(阶段 3):取代原来的恒定 AMBIENT_LIGHT 经验项 =====
    // 环境纹理与可用性只在此处算一次(供辐照度与环境 NEE 共用)。
    //
    // ⚠ 守卫必须同时覆盖"哨兵值"与"越界索引":bindless 数组未启用
    // partially-bound,越界索引会读到未初始化槽位。所以除了 env_valid,
    // 还要把传给采样函数的索引替换成哨兵值 —— 那两个函数只检查
    // `< 0xFFFFFFFF`,不能替我们挡住越界。
    const uint  env_slot  = scene_params.skybox_index;
    const bool  env_valid = env_slot < scene_params.texture_count;
    const uint  env_safe  = env_valid ? env_slot : 0xFFFFFFFFu;
    Sampler2D   env_tex   = samplers[env_valid ? env_slot : 0u];
    // 白炉模式:环境在所有方向上等辐射亮度(像素级能量验收的前提)
    const bool  uniform_env = (scene_params.debug_flags & DEBUG_UNIFORM_ENVIRONMENT) != 0u;

    // 用余弦重要性采样估计环境辐照度 E = ∫L cos dω,再按 Lambertian 送入
    // (albedo/π · E)。可见性已由辐照度采样自带(命中即遮挡),
    // 所以这里不能再乘 surf.occlusion —— 那会与可见性重复衰减。
    //
    // 刻字层与基底层按覆盖率混合(与 evaluate_bsdf 的二分量约定一致)。
    // **先算 albedo 再决定是否追踪**:全金属(metallic=1)的漫反射反照率为 0,
    // 此时 4 条辐照度射线纯属白费 —— 提前跳过是精确等价的(乘数为 0),不是近似。
    float3 env_albedo = lerp(surf.base.albedo, surf.engrave.albedo, saturate(surf.coverage));

    if((scene_params.debug_flags & DEBUG_DISABLE_AMBIENT) == 0u && scene_params.env_intensity > 0.0f
       && luminance(env_albedo) > 1e-4)
    {
      float3 irradiance = sample_environment_irradiance(
          top_level_acceleration_structure, world_position + N * SHADOW_EPSILON, N,
          env_tex, env_safe, ENV_IRRADIANCE_SAMPLES, uniform_env, payload.seed);

      float3 env_diffuse = irradiance * INV_PI * scene_params.env_intensity;

      // ---- 贴图 AO(ORM.r)----
      // 诚实说明:环境辐照度**已经**带真实可见性(4 条余弦采样阴影射线 + 时域累积),
      // 所以贴图 AO 在这里是**重复遮蔽**。保留它是因为:
      //   · 4 样本的低频估计抓不到烘焙 AO 里的高频缝隙遮蔽;
      //   · 没有 ORM 贴图时 occlusion 恒为 1.0,当前资产完全不受影响;
      //   · 需要纯物理时把材质的 occlusion_strength 设为 0 即可关闭。
      // 白炉能量测试用的是"无 ORM 贴图 + 反照率强制为白",故不受此项影响。
      float ao = saturate(surf.occlusion);

      direct_lighting += env_albedo * env_diffuse * ao;
    }

    // ===== 环境 NEE(镜面)=====
    // 直接按**上半球均匀**采样:pdf = 1/(2π),无需重试补偿,估计量无偏。
    // 不做"按亮度比例拒绝采样"—— 那需要正确的概率因子补偿,很容易写出 2 倍偏差。
    // 方差由 MIS(与 BSDF 采样各承担一半)+ 时域累积控制。
    // 只在有天空贴图时做:程序化渐变的亮度太均匀,单独做 NEE 收益不大。
    if((scene_params.debug_flags & DEBUG_DISABLE_AMBIENT) == 0u && scene_params.env_intensity > 0.0f && env_valid)
    {
      // 绕法线 N 采样上半球(pdf = 1/(2π)),保证方向都在可见半球内
      float3      env_dir = sample_uniform_upper_hemisphere(N, payload.seed);
      const float p_env   = environment_nee_pdf();

      float NdotL = dot(N, env_dir);
      if(NdotL > 0.0)
      {
        float3 shadow_origin = world_position + N * SHADOW_EPSILON;
        // 阴影射线:未命中 = 看得见天空(trace_shadow_ray 的 miss 会把 is_shadowed 置 false)
        if(!trace_shadow_ray(top_level_acceleration_structure, shadow_origin, env_dir, 1e10f, payload.seed))
        {
          bsdf_value bsdf = evaluate_bsdf(surf, V, env_dir, false);

          // balance heuristic:两侧都用同一方向 pdf 尺度(p_env 为 NEE 侧,
          // bsdf.pdf 为 BSDF 采样侧)。两者都是真实方向 pdf,量级可比。
          float w_bsdf = p_env / max(p_env + bsdf.pdf, 1e-6);

          float3 sky = environment_radiance(env_dir, env_tex, env_safe, uniform_env);
          direct_lighting += sky * scene_params.env_intensity * bsdf.reflectance * w_bsdf / p_env;
        }
      }
    }

    payload.color += payload.throughput * direct_lighting;
  }

  // ===== 间接光照：表面 BSDF 采样 + 路径弹射 =====
  // DEBUG_VIZ_DIRECT_ONLY 在 raygen 侧结算(只输出直接光),此处不提前返回,
  // 以免 payload.color 在深层弹射时丢失直接光贡献。
  if(payload.depth >= MAX_BOUNCES)
  {
    if(direct_only)
      payload.color = direct_color;
    return;
  }

  // 俄罗斯轮盘赌：基于吞吐量亮度决定继续概率(期望无偏)
  float throughput_lum = luminance(payload.throughput);
  float p_continue     = min(1.f, throughput_lum);
  if(p_continue < RR_MIN_PROB || random_float(payload.seed) > p_continue)
  {
    if(direct_only)
      payload.color = direct_color;
    return;
  }

  bsdf_sample s = sample_bsdf(surf, V, payload.seed);
  if(s.pdf <= 0.f || dot(N, s.direction) <= 0.f)
  {
    if(direct_only)
      payload.color = direct_color;
    return;
  }

  // 吞吐量更新:s.value 已是完整的方向估计 f·cos/pdf(含界面 Fresnel 分配、
  // 层概率与波瓣概率补偿),这里只需再补偿俄罗斯轮盘赌的继续概率。
  float3 new_throughput = payload.throughput * s.value / p_continue;

  // 路径钳制：防止高反射 + 不良采样导致能量爆炸（可由 debug_flags 关闭）
  // 启停判断写成提前跳过,把 luminance 开销挡在分支之外
  if((scene_params.debug_flags & DEBUG_DISABLE_THROUGHPUT_CLAMP) == 0u)
  {
    float tput_lum = luminance(new_throughput);
    if(tput_lum > MAX_THROUGHPUT)
    {
      new_throughput *= MAX_THROUGHPUT / tput_lum;
    }
  }

  // 弹射光线：透射必须能命中背面以穿出薄壳 → 不剔除背面
  const uint bounce_ray_flags = (s.lobe == 2u || s.lobe == 3u) ? 0u : RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

  RayDesc bounce_ray;
  bounce_ray.Origin    = world_position + N * SHADOW_EPSILON;
  bounce_ray.Direction = s.direction;
  bounce_ray.TMin      = SHADOW_EPSILON;
  bounce_ray.TMax      = 1e23f;

  HitPayload next_payload;
  next_payload.color                 = payload.color;
  next_payload.throughput            = new_throughput;
  next_payload.depth                 = payload.depth + 1;
  next_payload.seed                  = payload.seed;
  next_payload.inside_transmissive   = payload.inside_transmissive;
  next_payload.texture_weight        = surf.coverage;
  next_payload.medium_entry_position = payload.medium_entry_position;
  next_payload.medium_travelled      = payload.medium_travelled;

  // ---- 环境 MIS:命中天空时用的权重,在弹射前算好 ----
  // 环境 NEE 是否活跃 —— 自包含判定(与直接光块里的 env_valid 同义,
  // 但那个变量的作用域在直接光块内,这里要重新算)
  const bool env_nee_active = ((scene_params.debug_flags & DEBUG_DISABLE_AMBIENT) == 0u)
                            && scene_params.env_intensity > 0.0f
                            && scene_params.skybox_index < scene_params.texture_count;

  // balance heuristic 两侧必须用**同一个** pdf:
  //   BSDF 采样侧 = s.pdf(该波瓣的方向 pdf)
  //   环境 NEE 侧 = environment_nee_pdf()(= 采样分布的 pdf)
  // 只有反射类波瓣(漫反射/镜面)参与环境入射;透射穿出不算。
  //
  // ⚠ **环境 NEE 不活跃时权重必须为 1**。否则少了 NEE 那一半,
  // BSDF 路径仍被 < 1 的权重压低 → "关环境光"变成"环境光被系统性削弱"
  // (而不是归零)。这个 bug 由 tools/dark_probe.ps1 的反证测试抓到:
  // 白炉 + 关环境光后球不是纯黑。
  next_payload.env_mis_valid  = (s.lobe == 0u || s.lobe == 1u) && !payload.inside_transmissive;
  next_payload.env_mis_weight = 1.0f;
  if(next_payload.env_mis_valid && env_nee_active)
  {
    float p_bsdf = max(s.pdf, 1e-6);
    float p_env  = environment_nee_pdf();
    next_payload.env_mis_weight = p_bsdf / (p_bsdf + p_env);
  }

  // 介质状态转移(与上面的 entering_medium 判定配套)
  if(s.lobe == 2u)
  {
    if(entering_medium)
    {
      // 进入介质:记录入射点,后续在出射界面用弦长结算 Beer-Lambert
      next_payload.inside_transmissive   = true;
      next_payload.medium_entry_position = world_position;
      next_payload.medium_travelled      = 0.f;
    }
    else
    {
      // 离开介质:光学厚度已在上面结算,回到外部
      next_payload.inside_transmissive = false;
      next_payload.medium_travelled    = 0.f;
    }
  }
  else if(s.lobe == 3u && was_inside_medium)
  {
    // 全反射:光仍在介质**内部**,只是改变了方向。
    // 从当前位置起算新的一段(旧的一段已在上面结算过)。
    next_payload.inside_transmissive   = true;
    next_payload.medium_entry_position = world_position;
    next_payload.medium_travelled      = 0.f;
  }

  TraceRay(top_level_acceleration_structure, bounce_ray_flags, 0xff, 0, 0, 0, bounce_ray, next_payload);

  payload.color = next_payload.color;

  // 只显示直接光的调试通道:恢复本层直接光结算前的值(见函数入口的说明)
  if(direct_only)
  {
    payload.color = direct_color;
  }
}

// ============================================================================
// Miss：天空（HDR 环境贴图优先，程序化天空回退）+ 高度雾
// ============================================================================
[shader("miss")]
void rayMissMain(inout HitPayload payload)
{
  // 只显示直接光的调试通道:天空属于间接/环境贡献,直接跳过
  if((scene_params.debug_flags & DEBUG_VIZ_DIRECT_ONLY) != 0u)
  {
    payload.color = float3(0.f);
    return;
  }

  // 白炉模式:天空取常数 1.0(必须在 HDR 采样之前短路,否则等于没生效)
  float3 sky_color = ((scene_params.debug_flags & DEBUG_UNIFORM_ENVIRONMENT) != 0u)
                       ? float3(1.f)
                       : sample_sky(WorldRayDirection());

  // 天空索引守卫：0xFFFFFFFF（无 HDR 天空）恒 ≥ texture_count → 走程序化天空回退
  if((scene_params.debug_flags & DEBUG_UNIFORM_ENVIRONMENT) == 0u
     && scene_params.skybox_index < scene_params.texture_count)
  {
    // 尝试 HDR 环境贴图；过暗时回退程序化天空，保证 MISS 路径有基础亮度
    float3 hdr_color = sample_hdr_sky(WorldRayDirection(), samplers[scene_params.skybox_index]);
    float  hdr_lum   = luminance(hdr_color);
    sky_color        = (hdr_lum > 0.02f) ? hdr_color : sky_color;
  }

  // 弹射光线命中天空 = 环境照明的一部分;主光线命中天空 = 背景(照常可见)。
  // `env_mis_valid` 只在"来自表面的反射弹射"上为真 —— 用它区分两者。
  // 关闭环境光时**只有弹射路径归零**,背景天空仍然可见(否则画面全黑没法看)。
  float3 sky_contribution = sky_color;
  if(payload.env_mis_valid)
  {
    const bool env_lighting_on = ((scene_params.debug_flags & DEBUG_DISABLE_AMBIENT) == 0u)
                              && scene_params.env_intensity > 0.0f;
    sky_contribution = env_lighting_on
                         ? (sky_color * payload.env_mis_weight * scene_params.env_intensity)
                         : float3(0.f);
  }

  if(FOG_DENSITY > 0.0f && (scene_params.debug_flags & DEBUG_DISABLE_FOG) == 0u)
  {
    fog_result fog = evaluate_height_fog(WorldRayOrigin(), WorldRayDirection(), 1e30f, FOG_DENSITY, FOG_FALLOFF,
                                         FOG_COLOR, FOG_SUN_COLOR, FOG_SUN_DIR);
    payload.color = payload.color * fog.transmittance + (sky_contribution * fog.transmittance + fog.inscatter) * payload.throughput;
  }
  else
  {
    payload.color += payload.throughput * sky_contribution;
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
// 阴影 AnyHit：投射/透射概率穿透 → 介质产生柔和阴影
// 用 transmission 而非 opacity:透射强度才是介质的物理属性,
// 二者混用会让"半透明但不透射"的材质错误地产生软阴影。
// ============================================================================
[shader("anyhit")]
void rayShadowAnyHitMain(inout shadow_payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  // 阴影只判透射:按需取字段,避免整拷 model_data / material_data
  // (阴影 any-hit 是调用最频繁的入口之一)
  float    p_transmit     = 0.f;
  uint32_t material_index = scene_params.models[InstanceIndex()].material_index;
  // 计数上界守卫：0xFFFFFFFF（无材质）与越界/悬空索引都按不透明处理
  if(material_index < scene_params.material_count)
  {
    p_transmit = saturate(scene_params.materials[material_index].transmission);
  }
  if(p_transmit > 0.f && random_float(payload.seed) < p_transmit)
  {
    IgnoreHit();  // 概率穿过：本表面不遮挡
    return;
  }
  payload.is_shadowed = true;
  AcceptHitAndEndSearch();
}
