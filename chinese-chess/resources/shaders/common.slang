// ============================================================================
// common.slang — 着色器公共工具库
// 所有可复用、与渲染路径无关的基础工具都放在这里：
//   常量 / 哈希与随机数 / 数学与颜色工具 / 切线空间 / 重心插值 / 低差异序列 / 全屏三角形
// 入口着色器通过 `import common;` 使用本模块，避免各文件重复定义。
// ============================================================================

// ---- 全局常量 ----
static const int   MAX_BOUNCES    = 8;       // 路径追踪最大弹射次数
static const uint  MAX_LIGHTS     = 64;      // 最大光源数（与 C++ 侧保持一致）
static const float SHADOW_EPSILON = 0.001f;  // 阴影/自相交偏移量

static const float PI     = 3.14159265358979323846;
static const float INV_PI = 0.31830988618379067154;

// ---- 哈希与随机数 ----
// 经典 Wang hash：低开销、分布尚可，用于逐像素/逐弹射的 RNG 种子
uint wang_hash(uint seed)
{
  seed = (seed ^ 61) ^ (seed >> 16);
  seed *= 9;
  seed = seed ^ (seed >> 4);
  seed *= 0x27d4eb2d;
  seed = seed ^ (seed >> 15);
  return seed;
}

// 返回 [0, 1) 均匀随机数，并推进种子
float random_float(inout uint seed)
{
  seed = wang_hash(seed);
  return float(seed) / float(0xFFFFFFFFu);
}

float2 random_float2(inout uint seed)
{
  return float2(random_float(seed), random_float(seed));
}

float3 random_float3(inout uint seed)
{
  return float3(random_float(seed), random_float(seed), random_float(seed));
}

// 根据 float2 坐标生成确定性哈希噪声（用于 BRDF LUT 的抖动，替代 sin-fmod 型伪随机）
float hash_noise(float2 co)
{
  const uint2 n    = uint2(co * 65536.0f);
  uint        seed = n.x * 1664525u + n.y * 1013904223u;
  return random_float(seed);
}

// ---- 数学与颜色工具 ----

// pow(x, 5) 的展开式，避免 GPU 上的通用 pow() 开销
float pow5(float x)
{
  float x2 = x * x;
  return x2 * x2 * x;
}

// BT.709 亮度权重，用于俄罗斯轮盘赌 / 路径重要度 / 钳制判断
float luminance(float3 c)
{
  return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// 最大颜色通道，用于估算反射率与材质响应强度
float max_component(float3 v)
{
  return max(max(v.x, v.y), v.z);
}

// ---- 切线空间 ----
struct tangent_space
{
  float3 T;  // 切线
  float3 B;  // 副切线
};

// 根据法线构建正交切线空间基（Frisvad 方法，对任意法线方向数值稳定）
tangent_space build_tangent_space(float3 N)
{
  tangent_space ts;
  if(abs(N.x) > abs(N.y))
    ts.T = normalize(cross(N, float3(0.0, 1.0, 0.0)));
  else
    ts.T = normalize(cross(N, float3(1.0, 0.0, 0.0)));
  ts.B = cross(N, ts.T);
  return ts;
}

// ---- 重心坐标插值（泛型，支持 float/float2/float3） ----
__generic<T : IFloat> T interpolate(float2 bary, T v0, T v1, T v2)
{
  return T(1.f - bary.x - bary.y) * v0 + T(bary.x) * v1 + T(bary.y) * v2;
}

// ---- Hammersley 低差异序列（BRDF LUT 预计算用） ----
float2 hammersley_2d(uint i, uint n)
{
  uint bits = (i << 16u) | (i >> 16u);
  bits      = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits      = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits      = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits      = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  float rdi = float(bits) * 2.3283064365386963e-10;
  return float2(float(i) / float(n), rdi);
}

// ---- 全屏三角形顶点（供 blend / cubemap / brdflut 等后处理入口复用） ----
struct fullscreen_vs_output
{
  float4 position : SV_Position;
  float2 uv;
};

fullscreen_vs_output fullscreen_triangle_vertex(uint vertex_id: SV_VertexID)
{
  fullscreen_vs_output output;
  output.uv       = float2((vertex_id << 1) & 2, vertex_id & 2);
  output.position = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
  return output;
}
