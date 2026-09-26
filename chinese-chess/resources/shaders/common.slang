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

// ---- 物理正确性 / 光谱就绪相关常量(shading.slang 使用) ----
// 当前 RGB 路径 = 3 个固定波长样本;光谱阶段改为运行时采样的 N 个波长。
static const uint PBR_WAVELENGTH_COUNT = 3;

// 粗糙度下限:0 会让 GGX 分布退化为 delta 而数值爆炸,物理上任何表面都有微观粗糙度
static const float PBR_ROUGHNESS_FLOOR = 0.02;

// 镜面 lobe 采样概率的夹取范围(只影响方差,无偏性由 MIS pdf 保证)
static const float PBR_SPECULAR_PROB_MIN = 0.02;
static const float PBR_SPECULAR_PROB_MAX = 0.98;

// 镜面方向 pdf 下限:半向量趋近法线时 VNDF pdf → 0,防止除零
static const float PBR_PDF_FLOOR = 1e-4;

// 多次散射补偿波瓣的采样概率系数(实际概率 = 系数 × roughness)
// 粗糙度低时补偿量极小,不需要采样;粗糙度高时才值得分配样本。
static const float PBR_MS_PROB = 0.25;

// ---- 环境光照(阶段 3)----
// 环境辐照度的每命中采样数:余弦重要性采样 + 时域累积收敛。
// 增大它可降低单帧噪声,代价是每命中 N 次阴影射线。
// 实测:方向性天空下,4 样本的单帧相对标准差约 29%,靠时域累积收敛。
static const uint ENV_IRRADIANCE_SAMPLES = 4;

// ---- 路径追踪调试开关(按位组合,与 C++ 侧 debug_flags 枚举一致) ----
// 能量实验必须能关闭全部启发式钳制,否则测到的是钳制而不是物理。
static const uint DEBUG_DISABLE_FIREFLY_CLAMP   = 1u << 0;  // 关闭时域累积 firefly 钳制
static const uint DEBUG_DISABLE_DIRECT_CLAMP    = 1u << 1;  // 关闭单光源贡献钳制
static const uint DEBUG_DISABLE_THROUGHPUT_CLAMP = 1u << 2; // 关闭路径吞吐量钳制
static const uint DEBUG_DISABLE_AMBIENT         = 1u << 3;  // 关闭环境光(辐照度 + 环境 NEE)
static const uint DEBUG_FORCE_METALLIC          = 1u << 4;  // 强制金属度 = debug_force_metallic
static const uint DEBUG_FORCE_ROUGHNESS         = 1u << 5;  // 强制粗糙度 = debug_force_roughness
static const uint DEBUG_FORCE_ALBEDO            = 1u << 6;  // 强制反照率 = 中性灰
static const uint DEBUG_VIZ_NORMAL              = 1u << 7;  // 可视化着色法线
static const uint DEBUG_VIZ_F0                  = 1u << 8;  // 可视化 F0
static const uint DEBUG_VIZ_ROUGHNESS           = 1u << 9;  // 可视化粗糙度
static const uint DEBUG_VIZ_METALLIC            = 1u << 10; // 可视化金属度
static const uint DEBUG_VIZ_DIRECT_ONLY         = 1u << 11; // 只显示直接光(不弹射)
// 均匀环境(真正的白炉):环境辐照度与天空都取常数 1.0。
// 没有它就无法在**像素级**验证能量守恒 —— 非均匀天空下,"粗糙度变大导致反射
// 方向改变"与"能量真的损失了"无法区分(实测:介电行与金属行都随粗糙度降
// 14%~18%,而介电的镜面分量只有约 4%,说明这个下降主要来自环境方向性)。
static const uint DEBUG_UNIFORM_ENVIRONMENT     = 1u << 12;
// 关闭全部直接光。白炉测试需要它:否则方向光的镜面项又引入粗糙度依赖,
// 与"能量守恒是否成立"混淆。
static const uint DEBUG_DISABLE_DIRECT_LIGHTS   = 1u << 13;
// 关闭高度雾。白炉测试必须关它:雾的模型里有**硬编码**的太阳散射项
// (FOG_SUN_COLOR / FOG_SUN_DIR,不来自场景光源),在"无光源"配置下依然发光,
// 会污染读数(反证测试里球因此不是纯黑)。
static const uint DEBUG_DISABLE_FOG             = 1u << 14;

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

// pow(x, 4) 的展开式(Smith 几何项用)
float pow4(float x)
{
  float x2 = x * x;
  return x2 * x2;
}

// BT.709 亮度权重，用于俄罗斯轮盘赌 / 路径重要度 / 钳制判断
float luminance(float3 c)
{
  return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
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
