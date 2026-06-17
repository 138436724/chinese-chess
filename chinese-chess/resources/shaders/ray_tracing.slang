import lighting;

struct Vertex
{
    float3 position;
    float3 normal;
    float2 uv;
};

struct push_constant
{
    float4x4 proj_inv_matrix;
    float4x4 view_inv_matrix;
    float3 ambient_color;   // ambient term for shadowed areas
    uint light_count;       // 灯光数量（0 = 无灯光，仅环境光）
    uint frame_index;       // frame counter for progressive rendering and RNG seeding
};

struct model_data
{
    float4x4 model_matrix;
    uint32_t material_index;
    Vertex *vertex_address;
    uint32_t *index_address;
};

struct material_data
{
    float3 background_color;
    float3 foreground_color;
    uint32_t texture_index;
};

[[vk::push_constant]]
ConstantBuffer<push_constant> push_const;                         // camera info
RaytracingAccelerationStructure top_level_acceleration_structure; // ray tracing need, has scene info
RWTexture2D<float4> render_output;                                // output image, like render output in rasterization
StructuredBuffer<model_data> models;                              // SSBO, has all model info
StructuredBuffer<material_data> materials;                        // SSBO, has all material info
StructuredBuffer<light_data> lights;                                 // SSBO，灯光数据（binding 4）
[[vk::binding(5)]] Sampler2D[] samplers;                          // bindless descriptor, texture used in materials
// attention please, SSBO is std430 layout default, but UBO is std140

// Ray payload structure - carries data through the primary ray tracing pipeline
struct HitPayload
{
    float3 color;      // Accumulated radiance along the ray path
    float3 throughput; // Current path throughput (BRDF * cos_theta / PDF product)
    int depth;         // Current recursion depth
    uint seed;         // Random seed for Monte Carlo sampling
};

// Cosine-weighted hemisphere sampling around a normal
// PDF = cos(theta) / PI
float3 sample_cosine_hemisphere(float2 uv, float3 normal)
{
    // Build orthonormal basis around the normal
    float3 tangent = lerp(normalize(cross(normal, float3(1.f, 0.f, 0.f))),
                          normalize(cross(normal, float3(0.f, 1.f, 0.f))), step(0.999f, abs(normal.x)));
    float3 bitangent = cross(normal, tangent);
    float phi = 2.f * 3.14159265359f * uv.x;
    float cos_theta = sqrt(uv.y);
    float sin_theta = sqrt(1.f - uv.y);

    return normalize(tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) + normal * cos_theta);
}

// 重心坐标插值函数
__generic<T : IFloat> T interpolate(float2 bary, T v0, T v1, T v2)
{
    return T(1.f - bary.x - bary.y) * v0 + T(bary.x) * v1 + T(bary.y) * v2;
}

float max_component(float3 v)
{
    return max(max(v.x, v.y), v.z);
}

// Maximum number of indirect bounces
static const int MAX_BOUNCES = 8;

// ---- Shader entry points ----

// Ray generation shader - entry point for each pixel
[shader("raygeneration")]
void rayGenMain()
{
    // Get the current pixel coordinates and image dimensions
    float2 launchID = (float2)DispatchRaysIndex().xy;        // Current pixel (x,y)
    float2 launchSize = (float2)DispatchRaysDimensions().xy; // Image size (width,height)

    // Set ray tracing flags (0 = no special flags)
    const uint rayFlags = 0;

    // Convert pixel coordinates to normalized device coordinates (NDC)
    // Range: [-1,1] for both x and y
    const float2 clipCoords = launchID / launchSize * 2.f - 1.f;

    // 近平面深度 = 0，远平面深度 = 1 (Vulkan 的 NDC 深度范围)
    float4 nearPointV = mul(push_const.proj_inv_matrix, float4(clipCoords, 0.f, 1.f));
    float4 farPointV = mul(push_const.proj_inv_matrix, float4(clipCoords, 1.f, 1.f));

    // 透视除法（正交投影下 w 通常为 1，但为了安全必须做）
    nearPointV /= nearPointV.w;
    farPointV /= farPointV.w;

    // 转换到世界空间
    float3 worldNear = mul(push_const.view_inv_matrix, nearPointV).xyz;
    float3 worldFar = mul(push_const.view_inv_matrix, farPointV).xyz;

    // Create the primary ray
    RayDesc ray;
    ray.Origin = worldNear;
    ray.Direction = normalize(worldFar - worldNear);
    ray.TMin = 0.001f; // Minimum distance to avoid self-intersection
    ray.TMax = 1e23f;  // Maximum distance (infinite for primary rays)

    // Seed RNG using pixel index, frame index, and a hash
    uint2 pixelIndex = DispatchRaysIndex().xy;
    uint seed = wang_hash(pixelIndex.x * 1664525u + pixelIndex.y * 1013904223u + push_const.frame_index * 0x9e3779b9u);

    // Initialize ray payload
    HitPayload payload;
    payload.color = float3(0.f, 0.f, 0.f);
    payload.throughput = float3(1.f, 1.f, 1.f);
    payload.depth = 0;
    payload.seed = seed;

    // Cast the ray into the scene using the acceleration structure
    // Parameters: AS, flags, instance mask, sbt offset, sbt stride, miss offset, ray, payload
    TraceRay(top_level_acceleration_structure, rayFlags, 0xff, 0, 0, 0, ray, payload);

    // Temporal accumulation: blend new sample with running average
    float3 prev_color = render_output[int2(launchID)].rgb;
    float t = 1.f / float(push_const.frame_index + 1u);
    float3 final_color = lerp(prev_color, payload.color, t);

    // Write the accumulated color to the output image
    render_output[int2(launchID)] = float4(final_color, 1.f);
}

// Closest hit shader - called when ray hits geometry
// Implements full path tracing: direct lighting + indirect bounce
[shader("closesthit")]
void rayClosestHitMain(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint primitiveID = PrimitiveIndex();
    uint32_t model_index = InstanceIndex();

    float2 bary = attr.barycentrics;

    model_data now_model = models[model_index];

    // get indices by primitiveID
    uint32_t i0 = now_model.index_address[primitiveID * 3u + 0u];
    uint32_t i1 = now_model.index_address[primitiveID * 3u + 1u];
    uint32_t i2 = now_model.index_address[primitiveID * 3u + 2u];

    // get vertices by indices
    Vertex v0 = now_model.vertex_address[i0];
    Vertex v1 = now_model.vertex_address[i1];
    Vertex v2 = now_model.vertex_address[i2];

    // calculate interpolated world-space position
    float3 local_pos0 = v0.position;
    float3 local_pos1 = v1.position;
    float3 local_pos2 = v2.position;
    float3 local_position = interpolate(bary, local_pos0, local_pos1, local_pos2);
    float3 world_position = mul(now_model.model_matrix, float4(local_position, 1.f)).xyz;

    // reconstruct and interpolate normals
    float3 n0 = v0.normal;
    float3 n1 = v1.normal;
    float3 n2 = v2.normal;
    float3 local_normal = normalize(interpolate(bary, n0, n1, n2));

    // transform normal to world space
    float3 world_normal = normalize(mul(now_model.model_matrix, float4(local_normal, 0.f)).xyz);

    // calculate uv
    float2 uv = interpolate(bary, v0.uv, v1.uv, v2.uv);

    // Get material color
    float3 material_color = float3(0.f, 0.f, 0.f); // default
    if (now_model.material_index != 0xFFFFFFFFu)
    {
        material_data now_material = materials[now_model.material_index];
        if (now_material.texture_index != 0xFFFFFFFFu)
        {
            Sampler2D now_texture = samplers[now_material.texture_index];
            float weight = now_texture.SampleLevel(uv, 0.f).x;
            material_color = lerp(now_material.background_color, now_material.foreground_color, weight);
        }
        else
        {
            material_color = now_material.background_color;
        }
    }

    // ===== Direct lighting: NEE（Next Event Estimation）多光源直接光照 =====
    // 对每个灯光做重要性采样 + 阴影光线追踪
    // 物理 Lambertian: Lo = ambient + Σ (Li × albedo/π × NdotL)
    float3 direct_lighting = push_const.ambient_color;
    for (uint li = 0; li < push_const.light_count; li++)
    {
        light_data light = lights[li];
        if (light.intensity <= 0.0f) continue;
        light_sample ls = sample_light(light, world_position, payload.seed);
        if (all(ls.color <= 0.0f)) continue;
        float NdotL = max(dot(world_normal, ls.direction), 0.0f);
        if (NdotL <= 0.0f) continue;
        float3 shadow_origin = world_position + world_normal * SHADOW_EPSILON;
        bool is_shadowed = trace_shadow_ray(top_level_acceleration_structure, shadow_origin, ls.direction, ls.distance, payload.seed);
        if (is_shadowed) continue;
        // Lambertian: Li × (albedo/π) × NdotL
        direct_lighting += ls.color * evaluate_lambertian(material_color) * NdotL;
    }
    payload.color += payload.throughput * direct_lighting;

    // ===== Indirect lighting: path tracing bounce =====
    // Check max depth
    if (payload.depth >= MAX_BOUNCES)
    {
        return;
    }

    // Russian roulette: terminate path with probability based on throughput brightness
    float3 new_throughput = payload.throughput * material_color;
    float p_continue = min(1.f, max_component(new_throughput));

    // Bias: if p_continue is very small, just kill the path
    if (p_continue < 0.05f)
    {
        return;
    }

    if (random_float(payload.seed) > p_continue)
    {
        return; // Path terminated by Russian roulette
    }

    // Scale throughput by Russian roulette probability
    new_throughput /= p_continue;

    // Sample a random direction from cosine-weighted hemisphere for indirect bounce
    // 使用 Lambertian BRDF 采样出射方向（余弦加权半球，对接 lighting.slang）
    // bs.value = albedo，与 material_color 一致，保持吞吐量计算不变
    brdf_sample bs = sample_lambertian(material_color, world_normal, payload.seed);
    float3 bounce_direction = bs.direction;

    // Prepare ray for next bounce
    const float bounce_epsilon = 0.001f;
    const uint primary_ray_flags = 0;

    RayDesc bounce_ray;
    bounce_ray.Origin = world_position + world_normal * bounce_epsilon;
    bounce_ray.Direction = bounce_direction;
    bounce_ray.TMin = bounce_epsilon;
    bounce_ray.TMax = 1e23f;

    // Update payload for next bounce
    HitPayload next_payload;
    next_payload.color = payload.color;
    next_payload.throughput = new_throughput;
    next_payload.depth = payload.depth + 1;
    next_payload.seed = payload.seed;

    // Trace indirect bounce ray
    // Use sbtRecordOffset=0 (primary hit group) and missIndex=0 (primary miss)
    TraceRay(top_level_acceleration_structure, primary_ray_flags, 0xff,
             0, // RayContributionToHitGroupIndex: primary hit group
             0, // MultiplierForGeometryContributionToHitGroupIndex
             0, // MissShaderIndex: primary miss shader
             bounce_ray, next_payload);

    // Copy accumulated color back from recursive trace
    payload.color = next_payload.color;
}

// Miss shader - called when ray doesn't hit any geometry (sky)
[shader("miss")]
void rayMissMain(inout HitPayload payload)
{
    // Sky color (simple gradient based on ray direction)
    // Add sky contribution to accumulated color
    float3 sky_color = sample_sky(WorldRayDirection());

    payload.color += payload.throughput * sky_color;
}

// Shadow miss shader - called when shadow ray doesn't hit anything (point is lit)
[shader("miss")]
void rayShadowMissMain(inout shadow_payload payload)
{
    payload.is_shadowed = false;
}

// Shadow any-hit shader - called when shadow ray hits something (point is in shadow)
[shader("anyhit")]
void rayShadowAnyHitMain(inout shadow_payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.is_shadowed = true;
    AcceptHitAndEndSearch();
}
