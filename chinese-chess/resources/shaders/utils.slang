// Maximum number of indirect bounces
static const int MAX_BOUNCES = 8;
static const uint MAX_LIGHTS = 64;
static const float SHADOW_EPSILON = 0.001f;
static const float PI = 3.14159265358979323846;
static const float INV_PI = 0.31830988618379067154;

// hash function
uint wang_hash(uint seed)
{
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

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

// 重心坐标插值函数
__generic<T : IFloat> T interpolate(float2 bary, T v0, T v1, T v2)
{
    return T(1.f - bary.x - bary.y) * v0 + T(bary.x) * v1 + T(bary.y) * v2;
}