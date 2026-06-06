struct Vertex
{
    float3 position;
    float normalx;
    float2 normalyz;
    float2 uv;
};

struct PushConstant
{
    float3 cameraOrigin;
    float4x4 projInvMatrix;
    float4x4 viewInvMatrix;
    float3 cameraDirection;
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

// Push constants containing scene information and camera data
[[vk::push_constant]]
ConstantBuffer<PushConstant> pushConst;
// Top-level acceleration structure containing the scene geometry
RaytracingAccelerationStructure topLevelAS;
// Output image where the final rendered result will be stored
RWTexture2D<float4> outImage;
// Models Info
StructuredBuffer<model_data> models;
// Material Info
StructuredBuffer<material_data> materials;
// Texture Info
Sampler2D[] samplers;

// 真坑啊 SSBO和UBO布局不一致，std430和std140

// Ray payload structure - carries data through the ray tracing pipeline
struct HitPayload
{
    float3 color; // Accumulated color along the ray path
    float weight; // Weight/importance of this ray (for importance sampling)
    int depth;    // Current recursion depth (for limiting bounces)
};

// 辅助函数: 重心坐标插值UV
float2 interpolateUV(float2 bary, float2 uv0, float2 uv1, float2 uv2)
{
    return (1.0 - bary.x - bary.y) * uv0 + bary.x * uv1 + bary.y * uv2;
}

// Ray generation shader - entry point for each pixel
[shader("raygeneration")]
void rgenMain()
{
    // Get the current pixel coordinates and image dimensions
    float2 launchID = (float2)DispatchRaysIndex().xy;        // Current pixel (x,y)
    float2 launchSize = (float2)DispatchRaysDimensions().xy; // Image size (width,height)

    // Set ray tracing flags (0 = no special flags)
    const uint rayFlags = 0;

    // Convert pixel coordinates to normalized device coordinates (NDC)
    // Range: [-1,1] for both x and y
    const float2 clipCoords = launchID / launchSize * 2.0 - 1.0;

    // 近平面深度 = 0，远平面深度 = 1 (Vulkan 的 NDC 深度范围)
    float4 nearPointV = mul(float4(clipCoords, 0.0, 1.0), pushConst.projInvMatrix);
    float4 farPointV = mul(float4(clipCoords, 1.0, 1.0), pushConst.projInvMatrix);

    // 透视除法（正交投影下 w 通常为 1，但为了安全必须做）
    nearPointV /= nearPointV.w;
    farPointV /= farPointV.w;

    // 转换到世界空间
    float3 worldNear = mul(nearPointV, pushConst.viewInvMatrix).xyz;
    float3 worldFar = mul(farPointV, pushConst.viewInvMatrix).xyz;

    // Create the primary ray
    RayDesc ray;
    ray.Origin = worldNear + pushConst.cameraOrigin; // 正交投影无法还原深度信息，需要摄像机的原始信息
    ray.Direction = pushConst.cameraDirection;       // 使用摄像机的方向
    ray.TMin = 0.001;                                // Minimum distance to avoid self-intersection
    ray.TMax = 1e23f;                                // Maximum distance (infinite for primary rays)

    //// Transform from NDC to view space using inverse projection matrix
    // const float4 viewCoords = mul(float4(clipCoords, 1.0, 1.0), pushConst.projInvMatrix);

    //// Create the primary ray
    // RayDesc ray;
    //// Transform camera origin (0,0,0) from view space to world space
    // ray.Origin = float3(0.0, 0.0, 1.1); // mul(float4(0.0, 0.0, 0.0, 1.0), pushConst.viewInvMatrix).xyz;
    //// Transform ray direction from view space to world space
    // ray.Direction = float3(0.0, 0.0, -1.0); // mul(float4(normalize(viewCoords.xyz), 0.0),
    // pushConst.viewInvMatrix).xyz; ray.TMin = 0.001;                       // Minimum distance to avoid
    // self-intersection ray.TMax = 1e23f;                       // Maximum distance (infinite for primary rays)

    // Initialize ray payload with default values
    HitPayload payload;
    payload.color = float3(0, 0, 0); // Start with black
    payload.weight = 1;              // Full weight for primary rays
    payload.depth = 0;               // Start at depth 0

    // Cast the ray into the scene using the acceleration structure
    // Parameters: AS, flags, instance mask, sbt offset, sbt stride, miss offset, ray, payload
    TraceRay(topLevelAS, rayFlags, 0xff, 0, 0, 0, ray, payload);

    // Get the final color from the ray tracing result
    float3 color = payload.color / payload.depth;

    // Write the result to the output image
    outImage[int2(launchID)] = float4(color, 1.0);
}

// Closest hit shader - called when ray hits geometry
[shader("closesthit")]
void rchitMain(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint primitiveID = PrimitiveIndex();
    uint32_t customIndex = InstanceID();    // VkAccelerationStructureInstanceKHR::instanceCustomIndex
    uint32_t model_index = InstanceIndex(); // This is blas order

    float2 bary = attr.barycentrics; // (bary.x, bary.y) = (u, v), w = 1-u-v

    float3 resultColor = float3(0, 0, 0);

    model_data now_model = models[model_index];
    if (now_model.material_index == 0xFFFFFFFF)
    {
        resultColor = float3(0.0, 0.0, 0.0);
    }
    else
    {
        material_data now_material = materials[now_model.material_index];
        Sampler2D now_texture = samplers[now_material.texture_index];

        uint32_t i0 = now_model.index_address[primitiveID * 3 + 0];
        uint32_t i1 = now_model.index_address[primitiveID * 3 + 1];
        uint32_t i2 = now_model.index_address[primitiveID * 3 + 2];

        Vertex v0 = now_model.vertex_address[i0];
        Vertex v1 = now_model.vertex_address[i1];
        Vertex v2 = now_model.vertex_address[i2];

        float2 uv = interpolateUV(bary, v0.uv, v1.uv, v2.uv);

        float weight = now_texture.SampleLevel(uv, 0).x;
        resultColor = lerp(now_material.background_color, now_material.foreground_color, weight);
    }

    payload.color += resultColor;
    payload.depth += 1;
}

// Miss shader - called when ray doesn't hit any geometry
[shader("miss")]
void rmissMain(inout HitPayload payload)
{
    // Set background color when ray misses all geometry
    // In a full implementation, this could sample from:
    // - Skybox textures
    // - Procedural sky gradients
    // - Environment maps
    payload.color = float3(0.5, 0.5, 0.5); // Gray background
    payload.depth += 1;
}
