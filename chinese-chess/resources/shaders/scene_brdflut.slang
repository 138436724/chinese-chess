/* Copyright (c) 2025, Sascha Willems
 * SPDX-License-Identifier: MIT
 * 基于 Sascha Willems Vulkan Examples 的 brdflut 示例改写。
 */

import common;
import lighting;

// ============================================================================
// scene_brdflut.slang — BRDF 积分查找表预计算（Legacy，当前 C++ 未引用）
// Hammersley 2D 低差异序列 + GGX 重要性采样 → split-sum 近似的 2D LUT。
// 基于 Karis 2013 (Epic Games) "Real Shading in Unreal Engine 4"。
// Hammersley 序列与数学工具复用 common.slang / lighting.slang。
// ============================================================================

struct vs_output
{
  float4 position : SV_Position;
  float2 uv;
};

[[SpecializationConstant]]
const uint NUM_SAMPLES = 1024u;

// GGX VNDF 重要性采样（返回半向量 H）
float3 sample_ggx_half_vector(float2 xi, float roughness, float3 normal)
{
  float  alpha     = roughness * roughness;
  float  phi       = 2.0 * PI * xi.x + hash_noise(normal.xz) * 0.1;
  float  cos_theta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
  float  sin_theta = sqrt(1.0 - cos_theta * cos_theta);
  float3 h_local   = float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

  tangent_space ts = build_tangent_space(normal);
  return normalize(ts.T * h_local.x + ts.B * h_local.y + normal * h_local.z);
}

// Split-Sum BRDF LUT 积分
float2 compute_brdf_lut(float n_dot_v, float roughness)
{
  const float3 N = float3(0.0, 0.0, 1.0); // LUT 约定：法线指向 +Z
  float3       V = float3(sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v);

  float2 lut = float2(0.0, 0.0);
  for (uint i = 0u; i < NUM_SAMPLES; i++)
  {
    float2 xi = hammersley_2d(i, NUM_SAMPLES);
    float3 H  = sample_ggx_half_vector(xi, roughness, N);
    float3 L  = 2.0 * dot(V, H) * H - V;

    float dot_nl = max(dot(N, L), 0.0);
    float dot_nv = max(dot(N, V), 0.0);
    float dot_vh = max(dot(V, H), 0.0);
    float dot_nh = max(dot(H, N), 0.0);

    if (dot_nl > 0.0)
    {
      float G     = g_smith(dot_nl, dot_nv, roughness);
      float g_vis = (G * dot_vh) / (dot_nh * dot_nv);
      float Fc    = pow5(1.0 - dot_vh);
      lut += float2((1.0 - Fc) * g_vis, Fc * g_vis);
    }
  }
  return lut / float(NUM_SAMPLES);
}

[shader("vertex")]
vs_output vertMain(uint vertex_id : SV_VertexID)
{
  vs_output output;
  fullscreen_vs_output fs = fullscreen_triangle_vertex(vertex_id);
  output.uv       = fs.uv;
  output.position = fs.position;
  return output;
}

[shader("fragment")]
float4 fragMain(vs_output input)
{
  return float4(compute_brdf_lut(input.uv.x, input.uv.y), 0.0, 1.0);
}