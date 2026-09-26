#pragma once

#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_sampler.h"

#include <glm/glm.hpp>
#include <memory>

struct scene_image
{
    vulkan_image image;
    sampler_type type = sampler_type::diffuse;
};

struct scene_material
{
    // 颜色均为线性空间(不是 sRGB)
    //   background_color : 基底反射率。电介质 = 漫反射反照率;金属 = 垂直入射反射率 F0
    //   foreground_color : 刻字/纹样层反射率,与基底按 alpha 覆盖率混合
    glm::vec3                    background_color = glm::vec3(0.0f, 0.0f, 0.0f);
    glm::vec3                    foreground_color = glm::vec3(0.0f, 0.0f, 0.0f);
    std::shared_ptr<scene_image> alpha_map        = nullptr;
    float                        roughness        = 0.5f;
    float                        metallic         = 0.0f;
    float                        opacity          = 1.0f;
    float                        ior              = 1.5f;
    float                        transmission     = 0.0f;

    // ---- 介质(Beer-Lambert)光谱消光 ----
    // 逐波长消光系数 σ_t(1/世界单位)。玉石的颜色由 σ_t 的光谱差异物理涌现,
    // 不再依赖"标量密度 + 两个颜色插值"那种无法光谱化的近似。
    // 参考尺度:厚度 0.04 的棋子,σ_t≈25 时单程透过率约 0.37
    glm::vec3 absorption_coefficient = glm::vec3(0.0f);
    // 刻字层附加消光(按覆盖率加权叠加到 σ_t):刻痕更深更实
    glm::vec3 engrave_absorption     = glm::vec3(0.0f);

    // ---- 色散(为光谱渲染预留) ----
    // dispersion_model: 0 = 常数折射率(n = ior),1 = Cauchy(n = A + B/λ²)
    // dispersion_param: Cauchy 的 A(当前未启用模型时等于 ior)
    uint32_t dispersion_model = 0;
    float    dispersion_param = 1.5f;

    // ---- 贴图通道(阶段 1) ----
    // 任一为 nullptr 则该通道不参与着色(dispatch 到无贴图路径)。
    // normal_map: 切线空间法线图(NormalMap 约定)。TBN 由三角形 UV 有限差分求,
    //             不需要顶点切线 —— 现有 glb 因此无需改动。
    // orm_map   : R=AO G=roughness B=metallic(glTF ORM 约定)
    // emissive_map: 预留,阶段 4 接 NEE 显式光源后再启用
    std::shared_ptr<scene_image> normal_map   = nullptr;
    std::shared_ptr<scene_image> orm_map      = nullptr;
    std::shared_ptr<scene_image> emissive_map = nullptr;

    // 法线强度缩放(1 = 贴图原强度,0 = 关闭凹凸)
    float normal_scale = 1.0f;
    // AO 应用强度(1 = 完全应用,0 = 忽略贴图 AO)
    float occlusion_strength = 1.0f;
};
