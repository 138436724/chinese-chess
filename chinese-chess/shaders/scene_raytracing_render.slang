import shader_utils;
import vulkan_data_type;
import pbr;
import lighting;
import path_tracing;

[[vk::binding(0)]] RaytracingAccelerationStructure top_level_acceleration_structure;
[[vk::binding(1)]] RWTexture2D<float4> render_output;
[[vk::binding(2)]] ConstantBuffer<vulkan_rt_scene_data, ScalarDataLayout> scene_params;
[[vk::binding(3)]] Sampler2D[] samplers;
[[vk::binding(4)]] RWTexture2D<float4> accumulation;

// TraceRay 只返回交点；所有路径与阴影射线均从 raygen 发出，管线递归深度为 1。
struct trace_payload
{
  trace_hit hit;
  uint64_t coverage_seed;
};

struct rt_backend : path_backend
{
  trace_hit trace(RayDesc ray, uint64_t seed)
  {
    trace_payload payload;
    payload.hit = empty_hit();
    payload.coverage_seed = seed;
    TraceRay(top_level_acceleration_structure, RAY_FLAG_FORCE_NON_OPAQUE, 0xffu, 0, 0, 0, ray, payload);
    return payload.hit;
  }

  bool occluded(RayDesc ray, uint64_t seed)
  {
    trace_payload payload;
    payload.hit = empty_hit();
    payload.hit.found = true; // 被接受的命中结束遍历；仅 miss 将它改为 false
    payload.coverage_seed = seed;
    TraceRay(top_level_acceleration_structure,
      RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
      0xffu, 1, 0, 1, ray, payload);
    return payload.hit.found;
  }

  float3 texture(uint index, float2 uv)
  {
    return samplers[NonUniformResourceIndex(index)].SampleLevel(uv, 0).rgb;
  }
};

[shader("raygeneration")]
void rayGenMain()
{
  uint2 pixel = DispatchRaysIndex().xy;
  rng r = pixel_rng(pixel, scene_params.frame_index);
  RayDesc ray = camera_ray(scene_params, pixel, DispatchRaysDimensions().xy, r);
  rt_backend backend;
  float3 color = trace_path(backend, top_level_acceleration_structure, scene_params, ray, r);
  write_accumulated(accumulation, render_output, int2(pixel), color, scene_params.frame_index, scene_params.exposure);
}

[shader("closesthit")]
void rayClosestHitMain(inout trace_payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  payload.hit.found = true;
  payload.hit.model_index = InstanceIndex();
  payload.hit.primitive_id = PrimitiveIndex();
  payload.hit.bary = attr.barycentrics;
  payload.hit.distance = RayTCurrent();
}

[shader("miss")]
void rayMissMain(inout trace_payload payload) { payload.hit.found = false; }

[shader("miss")]
void rayShadowMissMain(inout trace_payload payload) { payload.hit.found = false; }

// 同一个 any-hit 同时绑定主命中组与阴影命中组，不能只给阴影应用 opacity。
[shader("anyhit")]
void rayAnyHitMain(inout trace_payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
  if(!accepts_coverage(scene_params, InstanceIndex(), PrimitiveIndex(), payload.coverage_seed)) IgnoreHit();
}
