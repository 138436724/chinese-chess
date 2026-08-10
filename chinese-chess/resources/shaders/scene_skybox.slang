/* Copyright (c) 2025, Sascha Willems
 * SPDX-License-Identifier: MIT
 * 基于 Sascha Willems Vulkan Examples 的 skybox 示例改写。
 */

import common;

// ============================================================================
// scene_skybox.slang — 天空盒渲染（Legacy 显示路径，当前 C++ 未引用）
// 采样立方体贴图 + Uncharted 2 色调映射 + Gamma 校正。
// 注意：光线追踪路径在 ray_tracing.slang 中自行处理天空。
// ============================================================================

struct vs_input
{
  float3 position;
};

struct vs_output
{
  float4 position : SV_Position;
  float3 uvw;
};

struct uniform_buffer
{
  float4x4 projection;
  float4x4 model;
};
ConstantBuffer<uniform_buffer> ubo;

struct uniform_buffer_params
{
  float4 lights[4];
  float  exposure;
  float  gamma;
};
ConstantBuffer<uniform_buffer_params> ubo_params;

SamplerCube sampler_env;

// Uncharted 2 电影色调映射算子（John Hable）
float3 uncharted2_tonemap(float3 color)
{
  const float A = 0.15;
  const float B = 0.50;
  const float C = 0.10;
  const float D = 0.20;
  const float E = 0.02;
  const float F = 0.30;
  const float W = 11.2;
  return ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
}

[shader("vertex")]
vs_output vertMain(vs_input input)
{
  vs_output output;
  output.uvw = input.position;

  // 天空盒跟随相机：只保留视图旋转、去掉平移（相机永远位于立方体中心）
  float4x4 view_rot = ubo.model;
  view_rot[3][0] = 0.f;
  view_rot[3][1] = 0.f;
  view_rot[3][2] = 0.f;

  output.position = mul(ubo.projection, mul(view_rot, float4(input.position.xyz, 1.0)));
  return output;
}

[shader("fragment")]
float4 fragMain(vs_output input)
{
  float3 color = sampler_env.Sample(float3(input.uvw.x, -input.uvw.y, input.uvw.z)).rgb;

  color = uncharted2_tonemap(color * ubo_params.exposure);
  color = color * (1.0f / uncharted2_tonemap((11.2f).xxx));

  color = pow(color, (1.0f / ubo_params.gamma).xxx);

  return float4(color, 1.0);
}