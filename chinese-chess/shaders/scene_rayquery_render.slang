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

// 保留候选遍历：几何全部按 non-opaque 查询，覆盖样本按图元散列，遍历顺序不影响接受结果。
struct query_backend : path_backend
{
  trace_hit trace(RayDesc ray, uint64_t seed)
  {
    trace_hit hit = empty_hit();
    var query = RayQuery<RAY_FLAG_FORCE_NON_OPAQUE>();
    query.TraceRayInline(top_level_acceleration_structure, 0u, 0xffu, ray);
    while(query.Proceed())
    {
      uint model = query.CandidateInstanceIndex();
      uint primitive = query.CandidatePrimitiveIndex();
      float t = query.CandidateTriangleRayT();
      if(t < hit.distance && accepts_coverage(scene_params, model, primitive, seed))
      {
        hit.found = true;
        hit.model_index = model;
        hit.primitive_id = primitive;
        hit.bary = query.CandidateTriangleBarycentrics();
        hit.distance = t;
      }
    }
    return hit;
  }

  bool occluded(RayDesc ray, uint64_t seed)
  {
    var query = RayQuery<RAY_FLAG_FORCE_NON_OPAQUE>();
    query.TraceRayInline(top_level_acceleration_structure, 0u, 0xffu, ray);
    while(query.Proceed())
      if(accepts_coverage(scene_params, query.CandidateInstanceIndex(), query.CandidatePrimitiveIndex(), seed)) return true;
    return false;
  }

  float3 texture(uint index, float2 uv)
  {
    return samplers[NonUniformResourceIndex(index)].SampleLevel(uv, 0).rgb;
  }
};

[numthreads(8, 8, 1)]
[shader("compute")]
void rayQueryComputeMain(uint3 thread_id : SV_DispatchThreadID)
{
  uint width, height;
  render_output.GetDimensions(width, height);
  if(any(thread_id.xy >= uint2(width, height))) return;
  uint2 pixel = thread_id.xy;
  rng r = pixel_rng(pixel, scene_params.frame_index);
  RayDesc ray = camera_ray(scene_params, pixel, uint2(width, height), r);
  query_backend backend;
  float3 color = trace_path(backend, top_level_acceleration_structure, scene_params, ray, r);
  write_accumulated(accumulation, render_output, int2(pixel), color, scene_params.frame_index, scene_params.exposure);
}
