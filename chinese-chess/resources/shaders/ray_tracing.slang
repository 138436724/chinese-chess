// Vertex结构 - 与C++ model_vertex对齐 (32 bytes: position(12) + normal(12) + uv(8))
struct Vertex
{
    float3 position;
    float normalx;
    float2 normalyz;
    float2 uv;
}

struct PushConstant
{
    float3 cameraOrigin;
    float4x4 projInvMatrix;
    float4x4 viewInvMatrix;
    float3 cameraDirection;
    Vertex *boardVerticesAddress;
    uint32_t *boardIndicesAddress;
    Vertex *boardLineVerticesAddress;
    uint32_t *boardLineIndicesAddress;
    Vertex *pieceVerticesAddress;
    uint32_t *pieceIndicesAddress;
};

// Push constants containing scene information and camera data
[[vk::push_constant]]
ConstantBuffer<PushConstant> pushConst;
// Top-level acceleration structure containing the scene geometry
RaytracingAccelerationStructure topLevelAS;
// Output image where the final rendered result will be stored
RWTexture2D<float4> outImage;

// 棋盘字体纹理 (binding 2, 2D)
Sampler2D boardFontTexture;
// 棋子字体纹理 (binding 3, 2D Array, 14层)
Sampler2DArray pieceFontTexture;

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
// instanceCustomIndex 编码:
//   customIndex 0: 棋盘
//   customIndex 1: 棋盘线
//   customIndex >= 2: 棋子 (customIndex = 2 | (pieceType<<2) | (pieceColor<<8))
//     pieceType: 0=General 1=Guard 2=Elephant 3=Horse 4=Chariot 5=Cannon 6=Pawn
//     pieceColor: 0=RED 1=BLACK
[shader("closesthit")]
void rchitMain(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint primitiveID = PrimitiveIndex();
    uint32_t customIndex = InstanceID(); // VkAccelerationStructureInstanceKHR::instanceCustomIndex
                                            // InstanceIndex is blas index

    float2 bary = attr.barycentrics; // (bary.x, bary.y) = (u, v), w = 1-u-v
    
    float3 resultColor = float3(0.0, 0.0, 0.0);

    if (customIndex == 0) // 棋盘
    {
        // 从棋盘顶点/索引buffer读取三角形
        uint32_t i0 = pushConst.boardIndicesAddress[primitiveID * 3 + 0];
        uint32_t i1 = pushConst.boardIndicesAddress[primitiveID * 3 + 1];
        uint32_t i2 = pushConst.boardIndicesAddress[primitiveID * 3 + 2];

        Vertex v0 = pushConst.boardVerticesAddress[i0];
        Vertex v1 = pushConst.boardVerticesAddress[i1];
        Vertex v2 = pushConst.boardVerticesAddress[i2];

        float2 uv = interpolateUV(bary, v0.uv, v1.uv, v2.uv);

        // 棋盘纹理采样
        float weight = boardFontTexture.SampleLevel(uv, 0).x;

        // 木色底色 + 黑色文字
        float3 baseColor = float3(224.0 / 255.0, 176.0 / 255.0, 124.0 / 255.0);
        float3 fontColor = float3(0.0, 0.0, 0.0);
        resultColor = lerp(baseColor, fontColor, weight);
    }
    else if (customIndex == 1) // 棋盘线
    {
        // 纯黑色线条
        resultColor = float3(0.0, 0.0, 0.0);
    }
    else // 棋子 (customIndex >= 2)
    {
        // 从棋子顶点/索引buffer读取三角形
        uint32_t i0 = pushConst.pieceIndicesAddress[primitiveID * 3 + 0];
        uint32_t i1 = pushConst.pieceIndicesAddress[primitiveID * 3 + 1];
        uint32_t i2 = pushConst.pieceIndicesAddress[primitiveID * 3 + 2];

        Vertex v0 = pushConst.pieceVerticesAddress[i0];
        Vertex v1 = pushConst.pieceVerticesAddress[i1];
        Vertex v2 = pushConst.pieceVerticesAddress[i2];

        float2 uv = interpolateUV(bary, v0.uv, v1.uv, v2.uv);

        // 解码customIndex获取棋子类型和颜色
        uint pieceType = (customIndex & 0x00F0) >> 4;
        uint pieceColor = (customIndex & 0x0F00) >> 8 ;

        // 棋子字体纹理采样 (2D Array, layer=pieceType)
        uint32_t layer = pieceType + uint32_t(abs(pieceColor - 1)) * 7;
        float weight = pieceFontTexture.SampleLevel(float3(uv, layer), 0).x;

        // 根据棋子颜色选择底色和文字色
        float3 bgColor, fgColor;
        if (pieceColor == 1) // 红方
        {
            bgColor = float3(1.0, 0.85, 0.75);
            fgColor = float3(0.6, 0.1, 0.1);
        }
        else // 黑方
        {
            bgColor = float3(0.85, 0.75, 0.65);
            fgColor = float3(0.1, 0.1, 0.1);
        }
        resultColor = lerp(bgColor, fgColor, weight);
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
