/**
 * @file scene_data_type.h
 * @brief 场景实体的 CPU 侧纯数据定义，不持有任何 Vulkan 对象。
 * @details 灯光、材质、模型与贴图描述由 scene_handle_map 的槽持有，环境则按值由 scene_manager 持有；
 *          实体对外通过句柄访问。灯光/材质/模型的字段是上传 SSBO 与组装 GPU 结构的来源，
 *          语义与对应的 vulkan_*_data 一致；引用边（texture_handle）只能经 setter 写入，直接改写会让贴图被提前回收。
 * @note 刻意不引入 Vulkan 头：这一层只依赖 vulkan_data_type.h 的共享 POD 与 scene_handle.h 的句柄。
 */
#pragma once

#include "scene_handle.h"
#include "vulkan_core/vulkan_data_type.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>

/**
 * @brief 一盏灯的 CPU 侧参数，字段语义与 vulkan_light_data 对应。
 * @details 方向光使用 direction / color / intensity；点光另用 position / range，忽略 direction 与锥角；
 *          聚光使用全部字段。上传时统一归一化 direction，其余字段按值打包；着色器按 active_type 忽略无关字段。
 */
struct scene_light_data
{
    light_type active_type = light_type::directional;  ///< @brief 当前光源类型，决定着色器走哪个光照分支；默认方向光

    glm::vec3 color     = glm::vec3(1.f);  ///< @brief 灯光颜色，线性 RGB，未乘强度；默认白
    float     intensity = 1.f;  ///< @brief 光强；方向光直接乘到 radiance，点光与聚光再乘距离衰减

    glm::vec3 direction = glm::vec3(0.f, -1.f, 0.f);  ///< @brief 世界空间方向：方向光解释为着色点指向光源，聚光解释为光源射出轴；默认 (0,-1,0)

    glm::vec3 position = glm::vec3(0.f);  ///< @brief 点光与聚光的世界空间位置；方向光忽略
    float     range    = 10.f;  ///< @brief 点光与聚光的最大影响距离；小于等于 0 表示不做距离截断

    float inner_cone_angle = glm::radians(15.f);  ///< @brief 聚光内锥半角，弧度，锥内为满强度；默认 15 度
    float outer_cone_angle = glm::radians(30.f);  ///< @brief 聚光外锥半角，弧度，内外锥之间按归一化余弦插值的平方衰减；默认 30 度
};

/**
 * @brief 场景环境（天空盒）状态，按值由 scene_manager 持有。
 * @details 渲染时被翻译成 vulkan_environment_data：image 的紧凑 binding 加 intensity 交给渲染器。
 *          image 无效（未设天空盒或图已回收）时绑定越界，着色器退回程序化天空。
 */
struct scene_environment_data
{
    texture_handle image;  ///< @brief 环境贴图句柄；是引用边，只能经 scene_manager::set_environment_image 写入
    float          intensity = 1.f;  ///< @brief 环境光强度，只缩放 HDR 路径，程序化回退不受影响；默认 1
};

/**
 * @brief 一张贴图的源路径与用途描述，由 get(texture_handle) 返回指针。
 * @details 真正的 Vulkan 图像与采样器收在 scene_texture_manager 的槽里，此结构含 std::filesystem::path，不是 POD，只暴露去重键与用途，
 *          使贴图的判死口径与其它句柄一致。当前接口返回非 const 指针，但调用方不应直接修改去重键。
 */
struct scene_texture_data
{
    std::filesystem::path source;  ///< @brief 贴图源路径；字形图集是合成出来的虚拟路径，同一路径配不同用途会建两张图
    texture_usage         usage = texture_usage::color;  ///< @brief 贴图用途，决定采样器与压缩格式；默认普通颜色贴图
};

/**
 * @brief 一个材质的 CPU 侧参数，字段与 vulkan_material_data 的 PBR 语义一一对应。
 * @details 最终反照率等于 albedo 乘 albedo 通道贴图的 RGB；电介质 F0 由 reflectance 推出，
 *          金属度在 F0 与 albedo 之间插值。textures 是引用边数组，只能经 set_texture 改写。
 */
struct scene_material_data
{
    std::array<texture_handle, MAX_CHANNEL_COUNT> textures{};  ///< @brief 五个贴图通道的句柄，下标即 texture_channel；只能经 set_texture 写入以维持引用计数

    glm::vec3 albedo            = glm::vec3(1.0f);  ///< @brief 基础反照率，线性 RGB，会与 albedo 贴图 RGB 相乘；默认白
    glm::vec3 emission          = glm::vec3(0.0f);  ///< @brief 自发光颜色，线性 RGB；乘 emission_strength 后加到出射辐亮度
    glm::vec3 absorption        = glm::vec3(0.0f);  ///< @brief 介质吸收系数，线性 RGB，每世界单位；Beer-Lambert 按段累乘
    float     roughness         = 0.5f;  ///< @brief 粗糙度，范围 0 到 1；GGX alpha 为其平方，并限制在 [ALPHA_MIN, 1]
    float     metallic          = 0.0f;  ///< @brief 金属度，范围 0 到 1；决定 F0 在电介质反射率与 albedo 之间的插值
    float     reflectance       = 0.5f;  ///< @brief 电介质反射率，范围 0 到 1；F0 等于 0.08 乘本值
    float     ior               = 1.5f;  ///< @brief 折射率，默认 1.5（玻璃）；光追用于介质界面的 Fresnel、折射方向与辐亮度缩放
    float     transmission      = 0.0f;  ///< @brief 平滑介质界面分支概率，范围 0 到 1；1 时仍按 Fresnel 选择镜面反射或折射
    float     opacity           = 1.0f;  ///< @brief 表面覆盖率，范围 0 到 1；RT/RQ 对相机、间接和阴影光线统一随机接受交点，光栅路径未使用
    float     emission_strength = 0.0f;  ///< @brief 自发光强度倍数，默认 0 表示不发光
};

/**
 * @brief 一个模型实例的 CPU 侧数据：网格、材质与模型矩阵。
 * @details 网格与材质都是句柄，模型本身不拥有几何；光栅间接命令按 is_show 设置 instanceCount（0 或 1），光追设置 TLAS 实例 mask（0 / 0xFF），
 *          并把 model_matrix 送进 TLAS 实例变换与光栅化实例数据。
 */
struct scene_model_data
{
    mesh_handle     mesh;  ///< @brief 网格句柄，指向 scene_model_manager 的网格槽；同路径模型共享同一个网格
    material_handle material;  ///< @brief 材质句柄；无效时实例仍会上传，由着色器按越界处理

    bool      is_show      = true;  ///< @brief 是否参与绘制与光追：false 时隐藏该实例
    glm::mat4 model_matrix = glm::mat4(1.0f);  ///< @brief 模型到世界的变换矩阵，默认单位阵
};
