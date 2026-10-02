// ============================================================================
// shader_utils.slang — 通用基础设施：常量 / 随机数 / 数学 / 正交基 / 采样 / MIS / 累积
//
// 风格约定：本项目的 Slang 代码采用 C++ 风格封装 —— 有状态或有内聚语义的
// 概念一律写成 struct 并用成员函数表达行为（rng / basis 等），无状态的纯
// 数学工具写成自由函数。会修改自身状态的成员函数必须标注 [mutating]。
// ============================================================================

// ---------------------------------------------------------------------------
// 全局常量
// ---------------------------------------------------------------------------
static const float PI          = 3.14159265358979323846;
static const float INV_PI      = 0.31830988618379067154;
static const float INV_FOUR_PI = 0.07957747154594767;  // 1 / (4π)：均匀球面采样 pdf

// 路径追踪：主要靠俄罗斯轮盘赌终止路径，MAX_BOUNCES 只是防止病态输入的安全网。
// 注意：RT pipeline 走的是着色器内递归（closest-hit 里再次 TraceRay），
// 管线创建时 maxPipelineRayRecursionDepth 取设备的 maxRayRecursionDepth，
// 因此 MAX_BOUNCES 直接决定所需的递归深度（约 MAX_BOUNCES + 2 层调用栈）。
// 8 与旧实现一致，是在本项目设备上验证过的取值；调大前请先确认设备的
// VkPhysicalDeviceRayTracingPipelinePropertiesKHR::maxRayRecursionDepth。
static const int   MAX_BOUNCES        = 8;
static const int   RR_START_DEPTH     = 2;       // 从该级弹射开始做俄罗斯轮盘赌
static const float RR_MIN_PROBABILITY = 0.05;    // 存活概率下限（仍按真实概率归一，保持无偏）
static const float MAX_SAMPLES        = 2048.0;  // 时域累积的样本数上限

static const float ALPHA_MIN       = 1e-3;  // 粗糙度下限：纯数值保护，非估计量偏差
static const float SHADOW_EPSILON  = 1e-3;  // 阴影 / 自相交偏移
static const float SURFACE_EPSILON = 1e-3;  // 弹射光线起点偏移
static const float RAY_TMAX        = 1e23;  // 主光线 / 弹射光线视为无限远

// ---------------------------------------------------------------------------
// rng：Wang hash 随机数发生器
//   状态显式保存在 struct 内，沿路径以 inout 传递，保证同一像素同一帧内
//   每个采样点使用互不相关的随机序列（时域累积要求各帧独立同分布）。
// ---------------------------------------------------------------------------
struct rng
{
  uint64_t state;
  uint64_t inc;  // PCG32 的流选择：必须为奇数

  // SplitMix64（Steele et al. 2014）：种子扩展，把 64 位种子的位铺开
  uint64_t splitmix64(uint64_t z)
  {
    z = (z ^ (z >> 30u)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27u)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31u);
  }

  // PCG32（O'Neill 2014）：64 位 LCG 状态 + 输出置换（异或右移后再按高位旋转）。
  // 不用「迭代哈希」：哈希是为单次输入→输出打散设计的，连续序列（路径追踪里相邻随机数
  // 就是相邻弹射维度）的高维均匀性没有保证；PCG32 周期 2^64，且不同像素/帧用不同流（inc）。
  [mutating]
  uint next_uint()
  {
    const uint64_t old = state;
    state              = old * 6364136223846793005ull + inc;

    const uint xorshifted = uint(((old >> 18u) ^ old) >> 27u);
    const uint rot        = uint(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
  }

  // 64 位输出：两次 PCG32 拼接（阴影光线拿它当 any-hit 的局部种子）
  [mutating]
  uint64_t next_uint64() { return (uint64_t(next_uint()) << 32u) | uint64_t(next_uint()); }

  // 64 位种子：像素与帧的组合直接给满 64 位。32 位种子在 4K × 2048 帧下会撞车
  // （像素 × 帧的对数超过 2^32 ⇒ 不同像素共用同一条流 ⇒ 噪声出现空间相关）。
  [mutating]
  void reset(uint64_t seed)
  {
    const uint64_t mixed = splitmix64(seed);

    state = 0ull;
    inc   = (mixed << 1u) | 1ull;  // PCG 官方初始化：流号取奇数
    next_uint();
    state += splitmix64(seed ^ 0x9e3779b97f4a7c15ull);
    next_uint();
  }

  // [0, 1) 均匀分布
  [mutating]
  float next_float() { return float(next_uint()) / 4294967296.0; }

  [mutating]
  float2 next_float2() { return float2(next_float(), next_float()); }

  [mutating]
  float3 next_float3() { return float3(next_float(), next_float(), next_float()); }
};

// ---------------------------------------------------------------------------
// basis：以法线为 +Z 的正交切线空间基
//   Frisvad 方法构造（对任意法线方向数值稳定），成员函数负责局部 ↔ 世界变换。
// ---------------------------------------------------------------------------
struct basis
{
  float3 tangent;
  float3 bitangent;
  float3 normal;

  float3 to_world(float3 local) { return tangent * local.x + bitangent * local.y + normal * local.z; }

  float3 to_local(float3 v) { return float3(dot(v, tangent), dot(v, bitangent), dot(v, normal)); }
};

basis build_basis(float3 n)
{
  basis b;
  b.normal = n;
  if(abs(n.x) > abs(n.y))
  {
    b.tangent = normalize(cross(n, float3(0.0, 1.0, 0.0)));
  }
  else
  {
    b.tangent = normalize(cross(n, float3(1.0, 0.0, 0.0)));
  }
  b.bitangent = cross(n, b.tangent);
  return b;
}

// ---------------------------------------------------------------------------
// 数学与颜色工具
// ---------------------------------------------------------------------------

// BT.709 亮度：用于俄罗斯轮盘赌存活概率、环境 MIS 与调试输出
float luminance(float3 c)
{
  return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float max_component(float3 v)
{
  return max(max(v.x, v.y), v.z);
}

// pow(x, 5) 展开：避免通用 pow() 的开销
float pow5(float x)
{
  float x2 = x * x;
  return x2 * x2 * x;
}

float3 safe_normalize(float3 v)
{
  float len = length(v);
  return len > 1e-12 ? v / len : float3(0.0, 0.0, 1.0);
}

// 能量启发式 MIS 权重（指数 2）：返回策略 a 在 (a, b) 中的权重。
// 要求两个 pdf 是同一测度（同一方向上的立体角密度）。
float power_heuristic(float pdf_a, float pdf_b)
{
  float a2    = pdf_a * pdf_a;
  float b2    = pdf_b * pdf_b;
  float denom = a2 + b2;
  return denom > 0.0 ? a2 / denom : 0.0;
}

// 重心坐标插值（按类型重载，支持 float / float2 / float3）
__generic<T : IFloat> T interpolate(float2 bary, T v0, T v1, T v2)
{
  return T(1.f - bary.x - bary.y) * v0 + T(bary.x) * v1 + T(bary.y) * v2;
}

//float interpolate(float2 bary, float v0, float v1, float v2)
//{
//  return (1.0 - bary.x - bary.y) * v0 + bary.x * v1 + bary.y * v2;
//}

//float2 interpolate(float2 bary, float2 v0, float2 v1, float2 v2)
//{
//  return (1.0 - bary.x - bary.y) * v0 + bary.x * v1 + bary.y * v2;
//}

//float3 interpolate(float2 bary, float3 v0, float3 v1, float3 v2)
//{
//  return (1.0 - bary.x - bary.y) * v0 + bary.x * v1 + bary.y * v2;
//}

// ---------------------------------------------------------------------------
// 采样：全部以「局部 +Z = 法线」的半球/球面为基准，调用方用 basis.to_world 变换
// ---------------------------------------------------------------------------

// 余弦加权半球采样（Lambertian 的重要性采样），pdf = cosθ / π
float3 sample_cosine_hemisphere(float2 u)
{
  float phi       = 2.0 * PI * u.x;
  float cos_theta = sqrt(u.y);
  float sin_theta = sqrt(max(0.0, 1.0 - u.y));
  return float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
}

float cosine_hemisphere_pdf(float cos_theta)
{
  return max(cos_theta, 0.0) * INV_PI;
}

// 均匀球面采样（环境光 NEE），pdf = 1 / (4π)
float3 sample_uniform_sphere(float2 u)
{
  float z   = 1.0 - 2.0 * u.x;
  float r   = sqrt(max(0.0, 1.0 - z * z));
  float phi = 2.0 * PI * u.y;
  return float3(r * cos(phi), r * sin(phi), z);
}

float uniform_sphere_pdf()
{
  return INV_FOUR_PI;
}

// ---------------------------------------------------------------------------
// 时域累积：严格样本均值
//   mean_n = (mean_{n-1} * (n-1) + x_n) / n  ⇔  (previous_mean * n + x) / (n + 1)
//   这是无偏且**一致**的估计量（收敛到真值）。注意不要写成
//   lerp(previous, sample, alpha) 之类的指数滑动平均：虽然逐帧期望无偏，
//   但权重随帧号衰减，极限是随机变量而非真值，无法收敛。
//   sample_index 为「本次之前已累积的样本数」，由 C++ 侧逐帧递增，
//   相机或场景变化时复位为 0（首帧整帧替换）。
// ---------------------------------------------------------------------------
float3 accumulate_sample(float3 previous_mean, float3 sample_color, uint sample_index, float exposure)
{
  if(float(sample_index) >= MAX_SAMPLES)
  {
    // 达到样本数上限后冻结（冻结的是采样数而非数值，不引入估计量偏差）
    return previous_mean;
  }

  float3 exposed = sample_color * exposure;
  float  n       = float(sample_index);
  return (previous_mean * n + exposed) / (n + 1.0);
}
