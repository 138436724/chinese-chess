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

// 路径长度由俄罗斯轮盘赌决定，不设置确定性的弹射上限。
static const int   RR_START_DEPTH     = 2;       // 从该级弹射开始做俄罗斯轮盘赌
static const float RR_MIN_PROBABILITY = 0.05;    // 存活概率下限（仍按真实概率归一，保持无偏）
static const float RR_MAX_PROBABILITY = 0.95;    // 无硬深度上限；长路径仍有非零存活概率

static const float ALPHA_MIN       = 1e-3;  // 连续 GGX 的最小 alpha：定义正则化材质模型
static const float RAY_TMAX        = 1e23;  // 主光线 / 弹射光线视为无限远

// ---------------------------------------------------------------------------
// rng：PCG32 随机数发生器
//   状态显式保存在 struct 内，沿路径以 inout 传递，沿同一路径推进伪随机序列；
//   像素与样本编号参与播种，不宣称有限状态 PRNG 的输出数学独立。
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
  uint64_t next_uint64() { uint hi = next_uint(); uint lo = next_uint(); return (uint64_t(hi) << 32u) | uint64_t(lo); }

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
  float next_float() { return float(next_uint() >> 8u) * (1.0 / 16777216.0); }

  [mutating]
  float2 next_float2() { float x = next_float(); float y = next_float(); return float2(x, y); }

  [mutating]
  float3 next_float3() { float x = next_float(); float y = next_float(); float z = next_float(); return float3(x, y, z); }
};

// ---------------------------------------------------------------------------
// basis：以法线为 +Z 的正交切线空间基
//   选择与法线不平行的坐标轴构造，成员函数负责局部 ↔ 世界变换。
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
  float scale = max(pdf_a, pdf_b);
  if(scale <= 0.0) return 0.0;
  float a2    = (pdf_a / scale) * (pdf_a / scale);
  float b2    = (pdf_b / scale) * (pdf_b / scale);
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
// 线性均值无样本上限；首样本由调用方直接写入，避免读取未初始化历史。
// FP32 存储仍有舍入误差，不宣称无限样本下浮点运算精确。
float3 accumulate_sample(float3 previous_mean, float3 sample_color, uint64_t sample_index)
{
  // 不乘 n，避免高亮均值在中间乘法溢出；曝光只施加于独立的显示输出。
  return previous_mean + (sample_color - previous_mean) / float(sample_index + 1ull);
}

// 逆转置的余子式形式；归一化约去 det 的大小，但必须保留它的符号。
float3 transform_normal(float4x4 m, float3 n)
{
  float3 x = mul(m, float4(1, 0, 0, 0)).xyz;
  float3 y = mul(m, float4(0, 1, 0, 0)).xyz;
  float3 z = mul(m, float4(0, 0, 1, 0)).xyz;
  float orientation = dot(x, cross(y, z)) < 0.0 ? -1.0 : 1.0;
  return safe_normalize(orientation * (n.x * cross(y, z) + n.y * cross(z, x) + n.z * cross(x, y)));
}

// 按浮点坐标精度移动到几何法线一侧；不再叠加固定世界单位的 TMin。
float3 offset_ray_origin(float3 p, float3 n)
{
  int3 delta = int3(256.0 * n);
  int3 bits = asint(p) + int3(p.x < 0 ? -delta.x : delta.x,
                            p.y < 0 ? -delta.y : delta.y,
                            p.z < 0 ? -delta.z : delta.z);
  float3 shifted = asfloat(bits);
  return float3(abs(p.x) < 0.03125 ? p.x + n.x / 65536.0 : shifted.x,
                abs(p.y) < 0.03125 ? p.y + n.y / 65536.0 : shifted.y,
                abs(p.z) < 0.03125 ? p.z + n.z / 65536.0 : shifted.z);
}
