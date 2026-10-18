import shader_utils;
import vulkan_data_type;
import pbr;
import lighting;

// ============================================================================
// scene_rasterization_render.slang — 光栅化前向 PBR
//
// 与光追路径**共用同一套着色模型**：pbr.slang 的 microfacet / pbr_material /
// shading_context 与 lighting.slang 的 light_source / nee_light_contribution
// 都是同一份代码，因此材质的 BRDF、光源采样（含距离 / 锥角 / range 衰减）与
// 光追路径逐项一致。差异只在于「光栅化没有光线可追踪」：
//
//   1) **没有可见性项**：没有阴影光线 / 阴影贴图 / SSAO，直接光的 visibility
//      恒为 1，环境光也没有遮挡。这是前向光栅化在没有额外 pass 时的固有缺失，
//      不是着色模型的问题 —— 画面会缺少阴影与接触暗部。
//   2) **环境光用解析近似**（见 estimate_environment_lighting）：不做蒙特卡洛
//      采样，因此没有采样噪声（代价是不如真正的 IBL 精确）。
//   3) **透射是单次方向查询**：光栅化无法继续追踪折射光线，沿折射方向取一次
//      环境辐亮度代替介质内的真实传输（不含厚度与 Beer-Lambert 吸收）。
//
// 想补上阴影的正确做法是加一个方向光的 shadow map pass（另建 depth 管线 +
// 描述符）；本文件已把直接光照集中在 estimate_direct_lighting 一处，便于替换。
//
// 绑定（与 scene_rasterization_render.cpp 的 bindings 序一致）：
//   b0 = scene_params (ConstantBuffer<vulkan_raster_scene_data>)
//   b1 = samplers     (bindless Sampler2D[])
// 顶点输入：location 0 = position，1 = normal，2 = uv
//（与 scene_rasterization_render::create_pipeline 的 attribute 列表一一对应：
//  Slang 按结构体成员声明顺序分配 location）
// ============================================================================

ConstantBuffer<vulkan_raster_scene_data, ScalarDataLayout> scene_params;  // b0
Sampler2D[] samplers;                                                     // b1

// 顶点输入（局部空间）
struct raster_vertex
{
  float3 position;
  float3 normal;
  float2 uv;
};

// 顶点 → 片元插值输出（世界空间 + 材质索引）
struct raster_varying
{
  float4 position : SV_Position;
  float3 world_position;  // 世界空间位置（直接光采样 + 视线方向）
  float3 world_normal;    // 世界空间法线（未定向，片元里按视线定向）
  float2 uv;              // 纹理坐标（图案权重）
  uint   material_index;  // 该实例的材质索引（0xFFFFFFFF = 无材质）
};

// ---------------------------------------------------------------------------
// 入口文件侧的共享装配（bindless 数组是入口文件的全局绑定）
// ---------------------------------------------------------------------------
environment make_environment()
{
  environment env;
  env.environment_index     = scene_params.environment_index;
  env.texture_count         = scene_params.texture_count;
  env.has_hdr               = env.environment_index < env.texture_count;
  env.environment_intensity = scene_params.environment_intensity;
  return env;
}

// 环境辐亮度：HDR 等距矩形贴图优先，无贴图时程序化天空。
// bindless 取值必须在索引守卫成立的分支内（越界索引解引用是未定义行为）。
float3 environment_radiance(environment env, float3 direction)
{
  if(env.has_hdr)
  {
    return env.equirect_radiance(direction, samplers[NonUniformResourceIndex(env.environment_index)]);
  }
  return env.procedural_sky(direction);
}

// ---------------------------------------------------------------------------
// 直接光照：与光追路径共用 nee_light_contribution（delta 光源权重恒 1）
//   visibility 恒为 1 —— 光栅化没有阴影光线 / 阴影贴图，**这里没有阴影**。
// ---------------------------------------------------------------------------
float3 estimate_direct_lighting(shading_context ctx, float3 world_position)
{
  float3 direct = float3(0.0);

  for(uint i = 0; i < scene_params.light_count; ++i)
  {
    light_source light;
    light.data = scene_params.lights[i];
    if(!light.enabled())
    {
      continue;
    }

    light_sample ls = light.sample(world_position);
    if(!ls.valid || all(ls.radiance <= 0.0))
    {
      continue;
    }

    direct += nee_light_contribution(ctx, ls, 1.0);
  }

  return direct;
}

// ---------------------------------------------------------------------------
// 环境光照（IBL）—— 解析近似，**不做蒙特卡洛采样**。
//   光栅化没有时域累积，逐像素静态噪声无法收敛，因此这里刻意选零方差的近似：
//   漫反射：把环境当作局部常量 → ∫L·cos dω ≈ π·L(N)，于是 Lambert 的贡献
//           = albedo/π · π · L(N) = albedo · L(N)
//   镜面  ：沿「法线与镜面反射方向按粗糙度插值」的方向取一次辐亮度，
//           粗糙度越大越靠近法线（经典廉价 IBL，非物理精确但视觉合理）
// 与光追路径一致：辐亮度来自同一个 environment_radiance（HDR / 程序化天空），
// F0 来自同一个 pbr_material::f0()。缺少：可见性（无遮挡）与预滤波。
// ---------------------------------------------------------------------------
float3 estimate_environment_lighting(shading_context ctx, environment env)
{
  float3 n  = ctx.n;
  float3 wo = ctx.wo;

  float3 indirect = ctx.material.diffuse_albedo() * environment_radiance(env, n);

  float3 mirror_dir = reflect(-wo, n);
  float3 spec_dir   = safe_normalize(lerp(n, mirror_dir, 1.0 - clamp(ctx.material.roughness, 0.0, 1.0)));
  float3 f_env      = ctx.material.specular_weight() * fresnel_schlick(ctx.material.reflection_f0(), max(dot(n, wo), 0.0));
  indirect += f_env * environment_radiance(env, spec_dir);

  // 连续波瓣在混合式里的能量权重是 (1 - transmission)，与 BSDF 一致
  return indirect * (1.0 - ctx.material.transmission_probability());
}

// ---------------------------------------------------------------------------
// 透射近似：沿折射方向取一次环境辐亮度。
//   与光追路径的能量一致：介质界面的期望贡献含 transmission·(1-F)·eta²·T_medium·L；
//   光栅化算不出厚度与 Beer-Lambert 吸收，这里只保留 transmission·(1-F)·eta²·L
//   （即 T_medium ≈ 1）。全反射时没有透射分量。
// ---------------------------------------------------------------------------
float3 estimate_transmitted_lighting(shading_context ctx, environment env)
{
  float p_transmission = ctx.material.transmission_probability();
  if(p_transmission <= 0.0)
  {
    return float3(0.0);
  }

  float  eta       = ctx.front_face ? (1.0 / max(ctx.material.ior, 1.0)) : max(ctx.material.ior, 1.0);
  float  fresnel   = fresnel_dielectric(max(dot(ctx.n, ctx.wo), 1e-6), eta);
  float3 refracted = refract(-ctx.wo, ctx.n, eta);

  if(dot(refracted, refracted) < 1e-8)
  {
    return float3(0.0);  // 全反射：无透射分量
  }

  return p_transmission * (1.0 - fresnel) * eta * eta * environment_radiance(env, refracted);
}

[shader("vertex")]
raster_varying vertMain(raster_vertex input, uint draw_id: SV_DrawIndex)
{
  // 间接绘制：SV_DrawIndex 即 draw command 索引 == 模型索引
  // （与 TLAS 实例数组 / 光追路径同序；经场景 UBO 中的设备地址解引用）
  raster_varying output;

  // 实例索引守卫：draw command / 模型 SSBO / TLAS 实例是分别枚举生成的（三处规则不同），
  // 越界解引用是未定义行为 ⇒ 兜底成"无材质 + 整个图元裁掉"
  if(draw_id >= scene_params.model_count)
  {
    output.position       = float4(0.0, 0.0, 2.0, 1.0);  // z > w ⇒ 视锥外
    output.world_position = float3(0.0);
    output.world_normal   = float3(0.0, 1.0, 0.0);
    output.uv             = float2(0.0);
    output.material_index = 0xFFFFFFFFu;
    return output;
  }

  float4x4 model_matrix   = scene_params.models[draw_id].model_matrix;
  uint     material_index = scene_params.models[draw_id].material_index;

  // 局部 → 世界：位置用模型矩阵，法线用逆转置（支持非等比缩放）。
  float4 world = mul(model_matrix, float4(input.position, 1.0));

  output.position       = mul(scene_params.proj_matrix, mul(scene_params.view_matrix, world));
  output.world_position = world.xyz;
  output.world_normal   = transform_normal(model_matrix, input.normal);
  output.uv             = input.uv;
  output.material_index = material_index;
  return output;
}

[shader("fragment")]
float4 fragMain(raster_varying input)
    : SV_Target
{
  // 无材质实例时兜底为黑色（与光追路径的默认材质语义一致）
  float3 color = float3(0.0);

  // 材质守卫：单条比较同时覆盖 0xFFFFFFFF（无材质哨兵）与越界 / 悬空索引
  if(input.material_index >= scene_params.material_count)
  {
    return float4(color, 1.0);
  }

  vulkan_material_data material_data = scene_params.materials[input.material_index];

  // 反照率贴图：纹理索引守卫成立才取 bindless 槽（越界解引用是未定义行为）
  bool   has_albedo_map = material_data.texture_index < scene_params.texture_count;
  float3 albedo_map     = float3(0.0);
  if(has_albedo_map)
  {
    albedo_map = samplers[NonUniformResourceIndex(material_data.texture_index)].Sample(input.uv).rgb;
  }

  // 几何：把几何法线定向到朝向观察者的一侧
  float3 n_geometric = safe_normalize(input.world_normal);
  float3 wo          = safe_normalize(scene_params.view_position - input.world_position);
  bool   front_face  = dot(n_geometric, wo) > 0.0;

  pbr_material material = make_material(material_data, resolve_albedo(material_data, albedo_map, has_albedo_map));

  shading_context ctx;
  ctx.material   = material;
  ctx.ggx.n      = front_face ? n_geometric : -n_geometric;
  ctx.ggx.alpha  = material.alpha();
  ctx.n          = ctx.ggx.n;
  ctx.wo         = wo;
  ctx.front_face = front_face;
  ctx.geometric_n = ctx.n; // 光栅近似路径，无体积边界追踪
  ctx.eta = front_face ? 1.0 / max(material.ior, 1.0) : max(material.ior, 1.0);

  environment env = make_environment();

  color += material.emitted_radiance();
  color += estimate_direct_lighting(ctx, input.world_position);
  color += estimate_environment_lighting(ctx, env);
  color += estimate_transmitted_lighting(ctx, env);

  // 曝光：与光追路径同一语义
  color *= scene_params.exposure;

  return float4(color, 1.0);
}

// ============================================================================
// 天空盒：全屏三角形 + 直接采样等距矩形 2D HDR（**不生成 CubeMap**）
//
// 方向构造与 scene_raytracing_render.slang 的 rayGenMain 是同一套：用 proj_inv 把像素的
// NDC 反投影到近 / 远平面，两点之差即世界空间射线方向。这样透视与正交投影都
// 正确（正交下所有像素方向相同），不需要任何视锥参数，也不需要采样 CubeMap。
//
// 该方向随 NDC **仿射**变化，因此在 3 个角点上算好、交由光栅器线性插值即精确；
// 顶点位置取 w = 1 是为了让插值退化为仿射（不做透视校正）。片元里再归一化。
//
// 绘制顺序：深度测试 / 写入全部关闭（`_use_depth = false`），先铺满整屏，
// 几何随后正常覆盖 —— 不需要 `eLessOrEqual` 之类的深度约定。
// ============================================================================
struct environment_varying
{
  float4 position : SV_Position;
  float3 view_ray;  // 未归一化的世界空间方向
};

[shader("vertex")]
environment_varying environmentVertMain(uint vertex_id: SV_VertexID)
{
  // 全屏三角形 (-1,-1) / (3,-1) / (-1,3)：覆盖整个 NDC，无需顶点缓冲
  uint2  corner = uint2((vertex_id << 1u) & 2u, vertex_id & 2u);
  float2 ndc    = float2(corner) * 2.0 - 1.0;

  // 反投影近 / 远平面（Vulkan NDC：深度 ∈ [0,1]）
  float4 near_point = mul(scene_params.proj_inv_matrix, float4(ndc, 0.0, 1.0));
  float4 far_point  = mul(scene_params.proj_inv_matrix, float4(ndc, 1.0, 1.0));
  near_point /= near_point.w;
  far_point /= far_point.w;

  float3 world_near = mul(scene_params.view_inv_matrix, near_point).xyz;
  float3 world_far  = mul(scene_params.view_inv_matrix, far_point).xyz;

  environment_varying output;
  output.position = float4(ndc, 1.0, 1.0);   // z = 远平面；w = 1 → 仿射插值
  output.view_ray = world_far - world_near;  // 未归一化：随 NDC 仿射 → 插值精确
  return output;
}

[shader("fragment")]
float4 environmentFragMain(environment_varying input)
    : SV_Target
{
  float3 direction = safe_normalize(input.view_ray);

  // 与几何着色共用同一份环境辐亮度（HDR 等距矩形 或 程序化天空回退）
  float3 radiance = environment_radiance(make_environment(), direction);

  // 曝光：与几何 / 光追路径同一语义，否则天空与模型亮度不匹配
  return float4(radiance * scene_params.exposure, 1.0);
}
