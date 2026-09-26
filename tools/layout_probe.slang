// ============================================================================
// tools/layout_probe.slang — 布局校验探针(不参与运行时,仅供 tools 离线校验)
//
// 目的:直接让 Slang 打印 material_data / model_data / light_data /
// rt_scene_data 的成员偏移与大小,与 C++ 侧的 gpu_*_data / uniform_buffer
// 逐字节比对。这是 scene_data.slang 顶部注释点名的设备丢失风险的唯一可靠检查。
//
// 用法(见 tools/verify_layout.ps1):
//   slangc -target spirv ... -entry probe -stage compute -reflection-json out.json
// ============================================================================

import scene_data;

// std430 存储缓冲:与 GPU 侧的 materials[] / models[] / lights[] 数组同一布局
RWStructuredBuffer<material_data> probe_materials;
RWStructuredBuffer<model_data>    probe_models;
RWStructuredBuffer<light_data>    probe_lights;

// std140 常量缓冲:与 scene_raytracing_render.cpp 的 uniform_buffer 同一布局
ConstantBuffer<rt_scene_data> probe_rt_scene;

[shader("compute")]
[numthreads(1, 1, 1)]
void probe(uint3 tid : SV_DispatchThreadID)
{
  material_data m = probe_materials[tid.x];
  model_data    d = probe_models[tid.x];
  light_data    l = probe_lights[tid.x];

  float acc = m.roughness + m.metallic + m.opacity + m.ior + m.transmission
            + m.absorption_coefficient.x + m.absorption_coefficient.y + m.absorption_coefficient.z
            + m.engrave_absorption.x + m.engrave_absorption.y + m.engrave_absorption.z
            + m.dispersion_param
            + d.model_matrix[0][0] + l.intensity
            + probe_rt_scene.force_metallic + probe_rt_scene.force_roughness;

  probe_materials[0].roughness = acc;
}
