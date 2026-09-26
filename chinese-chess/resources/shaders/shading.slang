// ============================================================================
// shading.slang — 物理基础的着色层(RT pipeline 专用)
//
// 设计原则(与 pbr-exploration-plan.md 对应):
//  1. **唯一材质求值入口** sample_surface()  → 光谱化时只改这一处
//  2. **唯一 BSDF 接口** evaluate_bsdf / bsdf_pdf / sample_bsdf
//  3. **单次采样 → 逐波长求值**:采样策略(方向、波瓣选择、层选择)与波长
//     无关,因此 prob / pdf 恒为标量;反照率、Fresnel、透射率是逐波长量,
//     严格按分量独立求值 —— 绝不做 "取平均/取分量" 这类跨波长混合。
//  4. 采样与 pdf 用 MIS(balance heuristic)组合,故镜面采样概率的取值只
//     影响方差、不影响无偏性。
//
// 与旧实现的物理差异(有意为之,见计划 §1.1):
//  - 漫反射从 (1-kS)(1-metallic) 改为 (1-F)(1-metallic):Fresnel 用半向量
//    评估,kS 不再与 metallic 重复抑制
//  - 金属 Fresnel 从单参 Schlick 改为 F82(Karis):复折射率 (η, κ) 的
//    artist-friendly 拟合,阶段 5 可换成准确的复数 Fresnel
//  - Smith 几何项从 separable Schlick 近似改为高度相关形式
//  - 介质引入 Beer-Lambert 光谱消光系数,玉石颜色从物理涌现
//  - **二分量表面(基底 + 刻字)按覆盖率混合两个完整 BSDF**,而不是对
//    颜色做线性插值:覆盖率是几何覆盖比例,光学的 Fresnel/粗糙度差异
//    必须随之切换,否则刻字区会得到"基底的高光 + 刻字的颜色"这种非物理组合
//
// 函数定义顺序:材质求值 → GGX/Smith → 采样辅助 → 表面组装 → BSDF 接口。
// 引用约束:仅依赖 common.slang / scene_data.slang,不引用顶层描述符。
// ============================================================================

import common;
import scene_data;
import energy_compensation;

// ============================================================================
// 唯一材质求值入口
// ============================================================================

// 单层材质的物理参数(不含采样策略)
//   albedo : 基础色。电介质 = 漫反射反照率;金属 = 垂直入射反射率 F0
//   ior    : 折射率(色散见 dispersion_*)
//   absorption_coefficient : Beer-Lambert 消光系数 σ_t(逐波长),1/世界单位
struct material_eval
{
  float3 albedo;
  float  metallic;
  float  roughness;
  float  opacity;
  float  ior;
  float  transmission;
  float3 absorption_coefficient;
  uint   dispersion_model;
  float  dispersion_param;
};

// 表面 = 二分量混合(基底 material_data + 刻字 material_data)按覆盖率加权
//   coverage : 刻字层的几何覆盖率(由 alpha 图给出),不是反射率
struct surface_eval
{
  material_eval base;
  material_eval engrave;
  float         coverage;
  float3        normal;            // 已应用法线图、并已翻转为朝向入射侧的着色法线
  float3        geometric_normal;  // 未翻转、未加凹凸的几何法线
  float         occlusion;         // 贴图 AO(1 = 无遮蔽);阶段 3 接 IBL 后也应作用于间接漫反射
};

// 从材质索引 + 纹理权重组装一个 material_eval。
// _coverage 同时作为刻字层的 Beer-Lambert 附加消光权重。
material_eval resolve_material_data(material_data m, float _coverage)
{
  material_eval out;
  out.albedo                 = lerp(m.background_color, m.foreground_color, _coverage);
  out.metallic               = m.metallic;
  out.roughness              = m.roughness;
  out.opacity                = m.opacity;
  out.ior                    = max(m.ior, 1.0);
  out.transmission           = m.transmission;
  // 刻字层消光更强:料体吸收 + 刻痕附加吸收(逐波长)
  out.absorption_coefficient = max(m.absorption_coefficient, float3(0.0)) + max(m.engrave_absorption, float3(0.0)) * _coverage;
  out.dispersion_model       = m.dispersion_model;
  out.dispersion_param       = m.dispersion_param;
  return out;
}

// 无材质时的安全默认值(中性灰电介质)
material_eval default_material()
{
  material_eval out;
  out.albedo                 = float3(0.5);
  out.metallic               = 0.0;
  out.roughness              = 0.5;
  out.opacity                = 1.0;
  out.ior                    = 1.5;
  out.transmission           = 0.0;
  out.absorption_coefficient = float3(0.0);
  out.dispersion_model       = 0u;
  out.dispersion_param       = 1.5;
  return out;
}

// ============================================================================
// 调试覆盖(能量实验用):强制几何参数,隔离材质差异
//
// 两种覆盖的用途不同,不要混用:
//   · 强制 metallic / roughness:把所有材质**抹成同一个**,用于隔离单个参数。
//   · 强制 albedo 为白:只把反照率变白,保留每个材质的 metallic/roughness。
//     白炉球体阵靠它 —— 每格有自己的 roughness×metallic,但反照率统一为白,
//     这样"环境光下的方向反射率"才能逐格核查。
// ============================================================================
material_eval apply_debug_override(material_eval m, uint debug_flags, float force_metallic, float force_roughness)
{
  if((debug_flags & DEBUG_FORCE_METALLIC) != 0u)
    m.metallic = force_metallic;
  if((debug_flags & DEBUG_FORCE_ROUGHNESS) != 0u)
    m.roughness = force_roughness;
  if((debug_flags & DEBUG_FORCE_ALBEDO) != 0u)
    m.albedo = float3(1.0);
  return m;
}

// ============================================================================
// GGX / Smith 基元(标量:逐波长调用,签名在光谱化时不变)
// ============================================================================

float d_ggx(float NdotH, float roughness)
{
  float a     = roughness * roughness;
  float a2    = a * a;
  float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
  return a2 * INV_PI / max(denom * denom, 1e-7);
}

// Smith 几何项(高度相关形式,比 separable Schlick 近似更准)
float v_smith_ggx_correlated(float NdotV, float NdotL, float roughness)
{
  const float a2 = pow4(roughness);
  float       gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
  float       gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-5);
}

// Smith 单向几何项 G1(VNDF pdf 用)
float g1_smith_ggx(float NdotX, float roughness)
{
  const float a2 = pow4(roughness);
  return 2.0 * NdotX / max(NdotX + sqrt(a2 + (1.0 - a2) * NdotX * NdotX), 1e-5);
}

// Fresnel-Schlick,带 F90(默认 1 = 能量守恒)
float3 f_schlick_f90(float3 f0, float f90, float u)
{
  return f0 + (float3(f90) - f0) * pow5(1.0 - u);
}

// 金属 + 电介质统一 Fresnel(Karis 2013),复折射率 (η, κ) 的拟合
float3 f_f82(float3 f0, float3 f82, float u)
{
  return f0 + (f82 - f0) * pow5(1.0 - u);
}

float3 compute_f82(float3 f0, float metallic, float roughness)
{
  float3 f82 = saturate(f0 * 25.0) * metallic;
  f82        = lerp(f82, float3(1.0), roughness);
  return lerp(float3(1.0), f82, metallic);
}

float3 eval_fresnel(float3 f0_medium, float3 metal_f0, float3 f82, float metallic, float u)
{
  return lerp(f_schlick_f90(f0_medium, 1.0, u), f_f82(metal_f0, f82, u), metallic);
}

// ============================================================================
// 色散:折射率的波长依赖
//   0 = none   : n(λ) = ior(常数折射率)
//   1 = cauchy : n(λ) = cauchy_a + cauchy_b / λ²,λ 单位 µm
//
// 这是"光谱就绪"的关键接口:折射率是**逐波长量**,所以由它推出的电介质 F0
// 也必须逐波长计算 —— 否则色散永远只是装饰。见 make_layer_bsdf。
// 三波长样本取自 CIE 主波长近似(与 CIELAB 常用近似一致):
//   R ≈ 0.615 µm, G ≈ 0.540 µm, B ≈ 0.465 µm
// ============================================================================
float pbr_wavelength_um(uint channel)
{
  if(channel == 0u)
    return 0.615;
  if(channel == 1u)
    return 0.540;
  return 0.465;
}

float pbr_dispersion_ior(float ior, uint model, float cauchy_a, float cauchy_b, float lambda_um)
{
  if(model == 1u)
  {
    return cauchy_a + cauchy_b / max(lambda_um * lambda_um, 1e-6);
  }
  return ior;
}

// 法线变换:世界法线 = normalize((M^-1)^T · n)
//
// 直接用 M 变换法线在非均匀缩放下是错的(法线会被错误缩放,不再垂直于切平面,
// 表现是缩放后的物体高光方向偏、Fresnel 错)。
// 用伴随矩阵恒等式:(M^-1)^T = adj(M) / det(M);det 为标量且归一化会消掉,
// 故只需 adj(M)^T · n 再归一化。
float3 world_normal_from_matrix(float4x4 M, float3 n)
{
  float3x3 a = (float3x3)M;
  // adj(A) 的列 = 各余子式;adj(A)^T · n 等价于用矩阵的余子式做三重积
  float3 c0 = cross(a[1], a[2]);
  float3 c1 = cross(a[2], a[0]);
  float3 c2 = cross(a[0], a[1]);
  float3x3 adj_t = float3x3(c0, c1, c2);
  return normalize(mul(adj_t, n));
}

// 切线空间(由三角形 UV 有限差分求,而不是 Frisvad 任意基)
struct geometric_tangent_space
{
  float3 T;
  float3 B;
};

// 由三角形三个顶点的世界位置与 UV 建立切线空间。
//
// 为什么不能用 build_tangent_space 的 Frisvad 任意基:那个基的法线方向任意,
// 即使贴了法线图也会得到**随机旋转**的结果。真正的 TBN 必须来自几何的 UV 参数化。
//
// dpdu = (P1-P0) 在 u 方向的变化率;叉积得到几何法线,用几何法线定朝向,
// 用切线自身长度归一化以保持非正交基的正确缩放。
geometric_tangent_space build_geometric_tangent_space(float3 p0, float3 p1, float3 p2,
                                                      float2 uv0, float2 uv1, float2 uv2)
{
  float3 dp1 = p1 - p0;
  float3 dp2 = p2 - p0;
  float2 du1 = uv1 - uv0;
  float2 du2 = uv2 - uv0;

  float det = du1.x * du2.y - du2.x * du1.y;

  geometric_tangent_space out;
  if(abs(det) < 1e-12)
  {
    // UV 退化(顶点共 UV):退回任意正交基,等价于无法做凹凸
    tangent_space ts = build_tangent_space(normalize(cross(dp1, dp2)));
    out.T            = ts.T;
    out.B            = ts.B;
    return out;
  }

  float r = 1.0 / det;
  out.T   = (dp1 * du2.y - dp2 * du1.y) * r;
  out.B   = (dp2 * du1.x - dp1 * du2.x) * r;
  return out;
}

// 应用切线空间法线图:把 (x,y) 解码并缩放到几何法线上。
// _baked_normal : 贴图编码值(已由调用方 ×2-1)
// _scale        : normal_scale,1 = 贴图原强度
float3 apply_normal_map(float3 _baked_normal, float _scale, float3 _geometric_normal,
                        geometric_tangent_space _ts)
{
  float3 N = normalize(_geometric_normal);

  // 施密特正交化:法线图假设正交 TBN,而 UV 有限差分给的是非正交基
  float3 T = _ts.T - N * dot(N, _ts.T);
  float  lt = length(T);
  if(lt < 1e-8)
  {
    // 切线退化(UV 方向与法线重合)→ 无法定义切向,退回几何法线
    return N;
  }
  T /= lt;

  // 手性(glTF 约定:signed area 的正负决定副切线方向)
  float  handed = (dot(cross(_ts.T, _ts.B), N) < 0.0) ? -1.0 : 1.0;
  float3 B      = cross(N, T) * handed;

  float3 n = normalize(float3(_baked_normal.x * _scale, _baked_normal.y * _scale, _baked_normal.z));
  return normalize(T * n.x + B * n.y + N * n.z);
}

// ============================================================================
// 多次散射能量补偿(Kulla-Conty 路线)
//
// 问题:单散射 GGX 的 Smith 遮蔽项把"微表面之间的多次弹射"当作能量损失丢掉了。
// 白炉测试(F0=1、恒定环境光)下方向反射率随粗糙度掉到约 0.61(损失 38.6%),
// 这是纯几何效应,与颜色无关,因此可以用一张 (roughness, mu_v) → E_ss 的表补偿。
//
// 补偿项(Fdez-Agüera 2019 的结构,表值来自 tools/pbr_gen_energy_table.py):
//   白炉下应把能量补回 1: E_total = E_ss·k + k = 1,其中 k = (1-E_ss)/(1+E_ss)
//   有色表面按 F_avg 缩放(平均菲涅尔):E_total ≈ F_avg·E_ss·k + F_avg²·k
//
// 注意:表值不是拟合出来的经验系数,而是由解析恒等式 E_ss = E_{L~VNDF}[G1(L)]
// 直接估计得到的(该恒等式在 F0=1 时严格成立,见生成脚本的推导)。
// ============================================================================

// 双线性查表:u = roughness, v = mu_v
float energy_table_lookup(const float table[EC_ROUGH_COUNT][EC_ANGLE_COUNT], float u, float v)
{
  float ri = clamp(u, 0.0, 1.0) * float(EC_ROUGH_COUNT - 1);
  float ai = clamp(v, 0.0, 1.0) * float(EC_ANGLE_COUNT - 1);
  uint  r0 = uint(min(floor(ri), float(EC_ROUGH_COUNT - 2)));
  uint  a0 = uint(min(floor(ai), float(EC_ANGLE_COUNT - 2)));
  float fr = ri - float(r0);
  float fa = ai - float(a0);

  float c00 = table[r0][a0];
  float c10 = table[r0 + 1][a0];
  float c01 = table[r0][a0 + 1];
  float c11 = table[r0 + 1][a0 + 1];
  return lerp(lerp(c00, c10, fr), lerp(c01, c11, fr), fa);
}

// 单散射方向反射率(F0 = 1)
float energy_single_scatter(float roughness, float NdotV)
{
  return energy_table_lookup(EC_E_SS, roughness, NdotV);
}

// 平均菲涅尔
float fresnel_average(float roughness, float NdotV)
{
  return energy_table_lookup(EC_F_AVG, roughness, NdotV);
}

// 补偿系数 k(逐波长):使"总反射率 = 平均菲涅尔"在任意 F0 下成立
//
// 推导:设单散射镜面项缩放 (1+k)、多散射项为 f_avg²·k,则
//     E_total = E_ss·f_avg·(1+k) + f_avg²·k = f_avg
//     ⟹ k = (1 - E_ss) / (E_ss + f_avg)
// 两个极限校验(tools/pbr_verify_compensation.py 里做了穷举验证,勿凭记忆改):
//     f_avg = 1(白炉):    k = (1-E_ss)/(1+E_ss),E_total = 1 ✓
//     f_avg = 0.04(玻璃): k = (1-E_ss)/(E_ss+0.04),E_total = 0.04 ✓
float3 energy_compensation_factor(float e_ss, float3 f_avg)
{
  return (1.0 - e_ss) / max(e_ss + f_avg, float3(1e-4));
}

// ============================================================================
// 采样辅助(必须定义在 BSDF 采样之前)
// ============================================================================

// 余弦加权半球采样(重要性采样 Lambertian)
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

// GGX 可见法线分布(VNDF)采样(Heitz 2018 "Sampling the GGX Distribution of
// Visible Normals")。比经典 NDF 采样方差更低,且方向保证在上半球。
//
// 步骤(严格对应论文 §3~§4):
//   1) 切线空间中把视线拉伸到椭圆体配置 → Vh
//   2) 在 Vh 周围建立正交基 (T1,T2)
//   3) 在投影面积上采样(同心圆盘)得到 Nh
//   4) H = a·Nh.x·T1 + a·Nh.y·T2 + Nh.z·Vh,归一化 → 微表面法线
//   5) L = reflect(-V, H)
//
// ⚠ 第 4 步的乘法作用在**切线基分量**上(Nh 的 x/y 乘 a),不是在 Vh 基上;
// 且之后必须归一化。早期实现多做了"先变回世界再乘 a"与
// "max(0, Nh.z)",会让采样分布退化为近似均匀(已由 tools/ 下的
// 密度检验脚本定位:alpha=1 时 cosθ_L 的测量密度恒为 1/2π,不随 alpha 变化)。
float3 sample_ggx_vndf(float3 Ve, float roughness, float3 N, inout uint seed)
{
  float2 r = random_float2(seed);
  float  a = roughness * roughness;

  tangent_space ts = build_tangent_space(N);
  float3        Vl = float3(dot(Ve, ts.T), dot(Ve, ts.B), dot(Ve, N));

  // 1) 拉伸到椭圆体配置
  float3 Vh = normalize(float3(a * Vl.x, a * Vl.y, Vl.z));

  // 2) 正交基(半球配置特化)
  float  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
  float3 T1    = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
  float3 T2    = cross(Vh, T1);

  // 3) 投影面积参数化(同心圆盘采样)
  float r_disk = sqrt(r.x);
  float phi    = 2.0 * PI * r.y;
  float p1     = r_disk * cos(phi);
  float p2     = r_disk * sin(phi);
  float s      = 0.5 * (1.0 + Vh.z);
  p2           = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;

  float3 Nh = p1 * T1 + p2 * T2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * Vh;

  // 4) 变换回椭圆体配置 → 微表面法线(世界空间)
  float3 H_local = a * Nh.x * T1 + a * Nh.y * T2 + Nh.z * Vh;
  float3 H       = ts.T * H_local.x + ts.B * H_local.y + N * H_local.z;
  H              = normalize(H);

  // 5) 反射得到出射方向
  return normalize(reflect(-Ve, H));
}

// ============================================================================
// 单层 BSDF 系数
// ============================================================================
struct layer_bsdf
{
  float3 f0_medium;   // 电介质垂直入射反射率(由 ior 推出)
  float3 metal_f0;    // 金属 F0(= albedo)
  float3 f82;         // 金属 82° 反射率
  float3 albedo;      // 漫反射反照率(= albedo × (1 - metallic))
  float  roughness;
  float  metallic;
  float  transmission;
  float  ior;
  float  p_specular;  // 镜面采样概率(标量,只影响方差)
  // ---- 多次散射能量补偿 ----
  float  F_avg;       // 该视角下的平均菲涅尔(F0=0.04 的表值)
  float3 k_ms;        // 补偿强度(逐波长,见 make_layer_bsdf 的推导)
  float3 ms_scale;    // 该层色度的平均菲涅尔(逐波长)
  float  p_ms;        // 多散射波瓣的采样概率(只影响方差)
};

layer_bsdf make_layer_bsdf(material_eval m, float NdotV)
{
  layer_bsdf l;
  l.roughness    = clamp(m.roughness, PBR_ROUGHNESS_FLOOR, 1.0);
  l.metallic     = saturate(m.metallic);
  l.transmission = saturate(m.transmission);
  l.ior          = max(m.ior, 1.0);

  // 逐波长计算电介质 F0:色散在 Fresnel 上的体现。
  // 用显式数组 + 循环写出"每个波长独立求值"的结构 —— 这正是光谱化时的接口形状
  // (阶段 5 只需把循环上界换成采样波长数,循环体不动)。
  // ⚠ 折射方向(几何)仍用参考折射率 l.ior —— 方向本身对折射率的依赖弱,
  //   而波长相关的方向分歧会破坏单路径的几何一致性。这是有意的近似,
  //   真正的光谱折射需要逐波长分叉路径(阶段 5)。
  float f0_sq[PBR_WAVELENGTH_COUNT];
  for(uint ch = 0u; ch < PBR_WAVELENGTH_COUNT; ++ch)
  {
    const float n    = pbr_dispersion_ior(l.ior, m.dispersion_model, m.dispersion_param, 0.0, pbr_wavelength_um(ch));
    const float n_ok = max(n, 1.0);
    const float f0   = (n_ok - 1.0) / (n_ok + 1.0);
    f0_sq[ch]        = f0 * f0;
  }
  l.f0_medium         = float3(f0_sq[0], f0_sq[1], f0_sq[2]);
  l.metal_f0          = max(m.albedo, float3(0.0));
  l.f82               = compute_f82(l.metal_f0, l.metallic, l.roughness);
  l.albedo            = l.metal_f0 * (1.0 - l.metallic);

  // 采样概率(与波长无关):由 metallic / roughness 决定的镜面权重
  float k       = max(saturate(l.metallic), clamp(luminance(l.f0_medium), 0.0, 1.0));
  l.p_specular  = clamp(lerp(k, max(k, 0.25), l.roughness), PBR_SPECULAR_PROB_MIN, PBR_SPECULAR_PROB_MAX);

  // ---- 多次散射能量补偿 ----
  float e_ss    = energy_single_scatter(l.roughness, NdotV);
  l.F_avg       = fresnel_average(l.roughness, NdotV);
  // 金属的 F0 不是 0.04:F_avg_metal = F0 + (1-F0)·F_avg(0.04)
  float3 f_avg_metal = lerp(l.F_avg.rrr, float3(1.0), luminance(l.metal_f0));
  float3 f_avg       = lerp(float3(l.F_avg), f_avg_metal, l.metallic);

  // k 由"总反射率 = f_avg"解出(有色表面也守恒,不只是白炉):
  //   E_total = E_ss·f_avg·(1+k) + f_avg²·k = f_avg
  //   ⟹ k = (1 - E_ss) / (1 + E_ss - f_avg·(1 - E_ss))
  // 验证:f_avg=1(白炉) → k=(1-E_ss)/(2E_ss),且 E_total = E_ss(1+k)+k = 1 ✓
  //       f_avg→0(弱菲涅尔电介质) → k→1-E_ss,E_total→f_avg ✓
  l.k_ms     = energy_compensation_factor(e_ss, f_avg);
  l.ms_scale = max(f_avg, float3(0.0));
  l.p_ms     = clamp(PBR_MS_PROB * l.roughness, 0.0, 0.5);
  return l;
}

// 单散射镜面项的能量缩放系数:1 + k
//
// ⚠ 这里**不是** 1 + f_avg·k。两者只白炉(F_avg=1)等价,但对有色/弱反射表面差别很大:
//     总反射率 = E_ss·f_avg·(1 + α·k) + f_avg²·k
//   α = f_avg 时:F0=0.5 的金属只得 0.447(应为 0.5,欠 11%),
//                介电 F0=0.04 的镜面只得 0.026(应为 0.04,欠 35%);
//   α = 1 时:任意 f_avg 下总反射率**精确等于 f_avg** —— 这正是物理期望:
//             粗糙度只重分布能量,不改变积分反射率(粗糙金属应与光滑金属同亮)。
// 这个不一致是靠"验证脚本的模型 vs 着色器实现"交叉核对抓到的;
// tools/pbr_verify_compensation.py 里写明了两侧的确切表达式,改这里务必同步。
float3 layer_specular_energy_scale(const layer_bsdf l)
{
  return float3(1.0) + l.k_ms;
}

// 漫反射 lobe:逐波长反照率 × (1 - F(V·H)),能量由 Fresnel 分配给镜面
float3 layer_value_diffuse(const layer_bsdf l, float NdotL, float3 f)
{
  return max(l.albedo * (1.0 - f), float3(0.0)) * (max(NdotL, 0.0) * INV_PI);
}

// 镜面 lobe:逐波长 F,返回 BRDF × cos。
// 能量补偿通过对单散射项缩放 (1 + k) 完成 —— 见 layer_specular_energy_scale。
float3 layer_value_specular(const layer_bsdf l, float NdotV, float NdotL, float NdotH, float VdotH)
{
  float  D = d_ggx(NdotH, l.roughness);
  float  G = v_smith_ggx_correlated(NdotV, NdotL, l.roughness);
  float3 F = eval_fresnel(l.f0_medium, l.metal_f0, l.f82, l.metallic, VdotH);
  return D * G * F * max(NdotL, 0.0) * layer_specular_energy_scale(l);
}

// 多次散射补偿 lobe:常数 BRDF(均匀半球),量级 f_avg²·k。
// 物理含义:单散射 Smith 遮蔽丢掉的"微表面间多次弹射"能量,按 Kulla-Conty
// 的思路用低阶近似补回。它只作用于镜面分量(漫反射本身已守恒)。
// 返回的是 **BRDF 值**(不含 cos):均匀采样下 f·cos/pdf = f·cos·2π,
// 由调用方负责乘 cos —— 与漫反射/镜面两个 lobe 的约定保持一致。
float3 layer_value_multiscatter(const layer_bsdf l)
{
  return float3(l.ms_scale * l.ms_scale * l.k_ms) * INV_PI;
}

// 漫反射方向 pdf(余弦项)
float layer_pdf_diffuse(float NdotL)
{
  return max(NdotL, 0.0) * INV_PI;
}

// 多散射方向 pdf:均匀半球
float layer_pdf_multiscatter()
{
  return INV_PI / 2.0;
}

// 镜面方向 pdf:半向量趋近法线时 VNDF pdf → 0(此时镜面贡献本身也趋 0),
// 夹下限只为防止除零。
float layer_pdf_specular(float NdotH, float NdotV, float roughness)
{
  float D   = d_ggx(NdotH, roughness);
  float G1v = g1_smith_ggx(max(NdotV, 1e-5), roughness);
  return max(D * G1v / max(4.0 * NdotV, 1e-5), PBR_PDF_FLOOR);
}

// 单层内部的方向 pdf(不含层选择概率)
// 三个采样分支:漫反射 / 镜面 / 多散射,概率归一化
float layer_pdf(const layer_bsdf l, float3 N, float3 V, float3 L)
{
  float NdotL = dot(N, L);
  if(NdotL <= 0.0)
    return 0.0;

  float  NdotV = max(dot(N, V), 1e-4);
  float3 H     = normalize(V + L);
  float  NdotH = max(dot(N, H), 0.0);

  float p_ms   = lerp(l.p_ms, 0.0, l.transmission);
  float p_spec = lerp(l.p_specular, 0.0, l.transmission) * (1.0 - p_ms);
  float p_diff = max(1.0 - p_spec - p_ms, 1e-4);

  return p_diff * layer_pdf_diffuse(NdotL) + p_spec * layer_pdf_specular(NdotH, NdotV, l.roughness)
       + p_ms * layer_pdf_multiscatter();
}

// ============================================================================
// 表面 BSDF 接口(二分量按覆盖率混合)
// ============================================================================

// 逐波长参考值(BRDF × cos(NdotL))
struct bsdf_value
{
  float3 reflectance;
  float  pdf;
};

// 采样结果:value 为逐波长的 f·cos/pdf(未乘波瓣/层概率)
struct bsdf_sample
{
  float3 direction;
  float3 value;
  float  pdf;   // 整个表面 BSDF 的方向 pdf
  float  prob;  // 该采样分支的离散概率(层概率 × 波瓣概率)
  uint   lobe;  // 0=漫反射 1=镜面 2=透射 3=全反射回退 4=多散射补偿
  uint   layer; // 0=基底 1=刻字
};

// 层的选择概率:覆盖率即刻字层的几何权重
void surface_layer_probs(const surface_eval s, out float p_engrave, out float p_base)
{
  p_engrave = saturate(s.coverage);
  p_base    = max(1.0 - p_engrave, 1e-4);
}

// 逐波长评估整个表面 BSDF = Σ 层权 × 层 BRDF,再统一成一个方向 pdf
// _include_transmission: 仅 NEE 直接光传 true。透射分量(介质折射)必须由
// 路径采样负责,若在 NEE 里也算一遍就会把介质能量重复计入。
bsdf_value evaluate_bsdf(const surface_eval s, float3 V, float3 L, bool _include_transmission)
{
  bsdf_value out;
  out.reflectance = float3(0.0);
  out.pdf         = 0.0;

  float3 N     = s.normal;
  float  NdotL = dot(N, L);
  if(NdotL <= 0.0)
    return out;

  float p_engrave, p_base;
  surface_layer_probs(s, p_engrave, p_base);

  const float  NdotV = max(dot(N, V), 1e-4);
  const float3 H     = normalize(V + L);
  const float  NdotH = max(dot(N, H), 0.0);
  const float  VdotH = max(dot(V, H), 0.0);

  layer_bsdf lb = make_layer_bsdf(s.base, NdotV);
  {
    float3 F = eval_fresnel(lb.f0_medium, lb.metal_f0, lb.f82, lb.metallic, VdotH);
    // 透射材质的反射分量按 (1 - transmission) 缩放:剩余能量进入介质内部
    float trans_weight = _include_transmission ? lb.transmission : 0.0;
    out.reflectance += p_base * (1.0 - trans_weight)
                     * (layer_value_diffuse(lb, NdotL, F) + layer_value_specular(lb, NdotV, NdotL, NdotH, VdotH)
                        + layer_value_multiscatter(lb));
    out.pdf += p_base * layer_pdf(lb, N, V, L);
  }

  if(p_engrave > 0.0)
  {
    layer_bsdf le = make_layer_bsdf(s.engrave, NdotV);
    float3     F  = eval_fresnel(le.f0_medium, le.metal_f0, le.f82, le.metallic, VdotH);
    float trans_weight = _include_transmission ? le.transmission : 0.0;
    out.reflectance += p_engrave * (1.0 - trans_weight)
                     * (layer_value_diffuse(le, NdotL, F) + layer_value_specular(le, NdotV, NdotL, NdotH, VdotH)
                        + layer_value_multiscatter(le));
    out.pdf += p_engrave * layer_pdf(le, N, V, L);
  }
  return out;
}

// 整个表面 BSDF 在给定方向上的**采样密度**。必须与 evaluate_bsdf 的 out.pdf 完全一致。
//
// ⚠ 为什么不能直接用单波瓣 pdf:估计量是 f·cos / p,其中 p 必须是**真实采样密度**。
// 采样过程是"选层(p_layer)→ 选波瓣(p_branch)→ 按该波瓣采样(pdf_branch)",
// 故真实密度 = Σ_layer p_layer · Σ_branch p_branch·pdf_branch = 这里的返回值。
// 早期实现用的是**单波瓣 pdf**(漏了波瓣概率与其他波瓣的贡献),
// 会把间接光系统性低估,且低估幅度随 p_ms(∝roughness)增大 ——
// 表现为白炉球体随粗糙度变暗(由 tools/furnace_probe.ps1 抓到)。
float surface_pdf(const surface_eval s, float3 V, float3 L)
{
  float p_engrave, p_base;
  surface_layer_probs(s, p_engrave, p_base);

  const float3 N     = s.normal;
  const float  NdotV = max(dot(N, V), 1e-4);

  float pdf = p_base * layer_pdf(make_layer_bsdf(s.base, NdotV), N, V, L);
  if(p_engrave > 0.0)
  {
    pdf += p_engrave * layer_pdf(make_layer_bsdf(s.engrave, NdotV), N, V, L);
  }
  return pdf;
}

// 采样整个表面 BSDF:先选层(覆盖率),再选该层的波瓣与方向。
// 返回的 value 已是**完整的方向估计** f·cos/p,其中 p 是 surface_pdf 给出的
// 真实采样密度(含层概率与波瓣概率),调用方只需 × throughput / p_continue。
bsdf_sample sample_bsdf(const surface_eval s, float3 V, inout uint seed)
{
  bsdf_sample out;
  out.direction = s.normal;
  out.value     = float3(0.0);
  out.pdf       = 0.0;
  out.prob      = 1.0;
  out.lobe      = 0u;
  out.layer     = 0u;

  float p_engrave, p_base;
  surface_layer_probs(s, p_engrave, p_base);

  bool  use_engrave = p_engrave > 0.0 && random_float(seed) < p_engrave;
  float p_layer     = use_engrave ? p_engrave : p_base;
  out.layer         = use_engrave ? 1u : 0u;

  // NdotV 必须在使用之前算出:make_layer_bsdf 需要它来查能量补偿表
  float3 N     = s.normal;
  float  NdotV = max(dot(N, V), 1e-4);

  layer_bsdf l = make_layer_bsdf(use_engrave ? s.engrave : s.base, NdotV);

  // 界面 Fresnel(逐波长):反射取 F,透射取 (1-F)。这是介质的物理属性,
  // 与波瓣选择概率无关,必须按波长而非按概率分配能量。
  float  cos_theta = abs(dot(N, V));
  float3 F_face    = eval_fresnel(l.f0_medium, l.metal_f0, l.f82, l.metallic, cos_theta);

  // ---- 介质透射分支 ----
  float p_transmission = saturate(l.transmission);
  if(p_transmission > 0.0 && random_float(seed) < p_transmission)
  {
    float  eta = 1.0 / l.ior;
    float3 T   = refract(-V, N, eta);
    out.prob   = p_layer * p_transmission;
    if(dot(T, T) < 1e-8)
    {
      // 全反射:delta 方向,pdf 退化为离散概率;界面反射取 F
      out.lobe      = 3u;
      out.direction = reflect(-V, N);
      out.pdf       = 1.0;
      out.value     = F_face;
    }
    else
    {
      out.lobe      = 2u;
      out.direction = T;
      out.pdf       = 1.0;
      out.value     = float3(1.0) - F_face;
    }
    return out;
  }

  // ---- 反射:多散射 / 镜面 / 漫反射 ----
  float p_ms   = lerp(l.p_ms, 0.0, l.transmission);
  float p_spec = lerp(l.p_specular, 0.0, l.transmission) * (1.0 - p_ms);
  float p_diff = max(1.0 - p_spec - p_ms, 1e-4);

  float roll = random_float(seed);

  if(roll < p_ms)
  {
    // 多散射补偿:均匀半球采样(它的 BRDF 是常数,均匀采样即最优)
    out.lobe      = 4u;
    out.prob      = p_layer * p_ms;
    float2 r      = random_float2(seed);
    float  cos_t  = r.x;
    float  sin_t  = sqrt(max(0.0, 1.0 - cos_t * cos_t));
    float  phi    = 2.0 * PI * r.y;
    tangent_space ts = build_tangent_space(N);
    out.direction = ts.T * (sin_t * cos(phi)) + ts.B * (sin_t * sin(phi)) + N * cos_t;

    float NdotL = max(dot(N, out.direction), 0.0);
    if(NdotL <= 0.0)
    {
      out.value = float3(0.0);
      out.pdf   = 0.0;
      return out;
    }
    out.pdf   = surface_pdf(s, V, out.direction);
    out.value = layer_value_multiscatter(l) * NdotL / max(out.pdf, 1e-6);
    return out;
  }

  if(roll < p_ms + p_spec)
  {
    out.lobe      = 1u;
    out.prob      = p_layer * p_spec;
    out.direction = sample_ggx_vndf(V, l.roughness, N, seed);

    float NdotL = max(dot(N, out.direction), 0.0);
    if(NdotL <= 0.0)
    {
      out.value = float3(0.0);
      out.pdf   = 0.0;
      return out;
    }
    float3 H    = normalize(V + out.direction);
    float  NdotH = max(dot(N, H), 0.0);
    float  VdotH = max(dot(V, H), 0.0);

    out.pdf = surface_pdf(s, V, out.direction);
    // 镜面 lobe 本身的 Fresnel 已在 layer_value_specular 内;界面 Fresnel 不再重复施加
    float3 spec = layer_value_specular(l, NdotV, NdotL, NdotH, VdotH);
    out.value   = spec / max(out.pdf, 1e-6);
    return out;
  }

  out.lobe      = 0u;
  out.prob      = p_layer * p_diff;
  out.direction = sample_hemisphere_cosine(N, seed);

  float NdotL = max(dot(N, out.direction), 0.0);
  if(NdotL <= 0.0)
  {
    out.value = float3(0.0);
    out.pdf   = 0.0;
    return out;
  }
  float3 H    = normalize(V + out.direction);
  float  VdotH = max(dot(V, H), 0.0);
  float3 F    = eval_fresnel(l.f0_medium, l.metal_f0, l.f82, l.metallic, VdotH);

  out.pdf   = surface_pdf(s, V, out.direction);
  out.value = layer_value_diffuse(l, NdotL, F) / max(out.pdf, 1e-6);
  return out;
}

// ============================================================================
// 介质(Beer-Lambert)—— 逐波长消光
//   T = exp(-σ_t · d),d = 介质内实际穿越距离(调用方累积)
//   玉石的颜色来自 σ_t 的光谱差异,是物理涌现的结果,不再靠颜色插值
// ============================================================================
float3 transmittance_medium(float3 absorption_coefficient, float distance_inside)
{
  return exp(-max(absorption_coefficient, float3(0.0)) * max(distance_inside, 0.0));
}
