// ============================================================================
// scene_cubemap.slang — 等距矩形投影 → 立方体贴图转换（Legacy，当前 C++ 未引用）
// 将 2D 全景图渲染为 6 面立方体贴图（MRT 一次 Pass 输出 6 个面）。
// 入口：vertMain（全屏三角形，复用 common.slang）+ fragMain（6 面输出）。
// ============================================================================

import common;

struct vs_output
{
  float2 uv : TEXCOORD0;
  float4 position : SV_Position;
};

struct fs_output
{
  float4 face_0;
  float4 face_1;
  float4 face_2;
  float4 face_3;
  float4 face_4;
  float4 face_5;
};

[[vk::binding(0, 0)]]
Sampler2D panorama;

// 面索引 + UV（NDC [-1,1]）→ 方向向量
float3 face_uv_to_xyz(int face, float2 uv)
{
  if (face == 0)
    return float3(1.f, uv.y, -uv.x);
  else if (face == 1)
    return float3(-1.f, uv.y, uv.x);
  else if (face == 2)
    return float3(uv.x, -1.f, uv.y);
  else if (face == 3)
    return float3(uv.x, 1.f, -uv.y);
  else if (face == 4)
    return float3(uv.x, uv.y, 1.f);
  else
    return float3(-uv.x, uv.y, -1.f); // face == 5
}

// 方向向量 → 等距矩形 UV
float2 dir_to_uv(float3 dir)
{
  return float2(0.5f + 0.5f * atan2(dir.z, dir.x) / PI, 1.f - acos(dir.y) / PI);
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
fs_output fragMain(vs_output input)
{
  fs_output output;

  for (int face = 0; face < 6; face++)
  {
    float3 scan      = face_uv_to_xyz(face, input.uv * 2.0 - 1.0);
    float3 direction = normalize(scan);
    float2 src       = dir_to_uv(direction);
    float4 color     = float4(panorama.Sample(src).rgb, 1.f);

    if (face == 0)
      output.face_0 = color;
    else if (face == 1)
      output.face_1 = color;
    else if (face == 2)
      output.face_2 = color;
    else if (face == 3)
      output.face_3 = color;
    else if (face == 4)
      output.face_4 = color;
    else
      output.face_5 = color;
  }

  return output;
}