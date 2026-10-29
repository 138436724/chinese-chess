/**
 * @file vulkan_data_type.h
 * @brief CPU 与 Slang 共用的 GPU 结构定义，是布局与字段语义的唯一来源。
 * @details 两侧通过 VULKAN_MAT4 / VULKAN_VEC3 / VULKAN_ADDRESS / VULKAN_DEFAULT 宏展开成各自类型，
 *          所以字段顺序就是 GPU ABI：C++ 侧检查 sizeof 与 offsetof，Slang 侧条件数组维度只检查 sizeof，不检查成员偏移。
 *          着色器编译加 -fvk-use-c-layout，使 Slang 采用与 MSVC 一致的 C 布局。
 *          因此不要在这些结构里引入 _pad 或 alignas，否则两侧布局会同时漂移。
 */
#pragma once

#ifndef __SLANG__
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <utility>
#endif  // !__SLANG__

#ifdef __SLANG__
#define VULKAN_MAT4 float4x4
#define VULKAN_VEC4 float4
#define VULKAN_VEC3 float3
#define VULKAN_VEC2 float2
#define VULKAN_ADDRESS(T) T*
#define VULKAN_DEFAULT(...)
#else
#define VULKAN_MAT4 glm::mat4
#define VULKAN_VEC4 glm::vec4
#define VULKAN_VEC3 glm::vec3
#define VULKAN_VEC2 glm::vec2
#define VULKAN_ADDRESS(T) uint64_t
#define VULKAN_DEFAULT(...) = __VA_ARGS__
#endif  // __SLANG__

/**
 * @brief 光源类型，CPU 侧 scene_light_data::active_type 与 GPU 侧的 uint32_t 共用同一组取值。
 * @details 数值就是着色器光照分支的判据；新增类型需要同时改 CPU 枚举、GPU 字段语义与三支着色器的分支。
 *          方向光忽略 position 与 range，点光忽略两个锥角，聚光使用全部字段。
 */
enum class light_type : uint32_t
{
    directional = 0,  ///< 方向光：只用 direction / color / intensity。
    point       = 1,  ///< 点光：用 position / range，忽略两个锥角。
    spot        = 2   ///< 聚光：position、range 与内外锥角全部参与。
};

/**
 * @brief 材质贴图通道，顺序照 glTF 2.0 核心五槽，数值即 CPU 侧 textures 数组下标。
 * @details albedo 是目前唯一真正参与着色的通道，GPU 侧 vulkan_material_data::texture_index 取它的紧凑绑定。
 *          其余四槽已在 CPU 侧保留句柄与引用计数，扩展 GPU ABI 时按同一顺序追加，不要插队。
 * @note channel_count 必须留在最后，MAX_CHANNEL_COUNT 由它推导。
 */
enum class texture_channel : uint32_t
{
    albedo,             ///< 基础色，目前唯一真正参与着色并进入 GPU texture_index 的通道。
    normal,             ///< 法线贴图通道；CPU 侧保留引用，GPU ABI 尚未扩展。
    metallic_roughness, ///< 金属度/粗糙度通道；CPU 侧保留引用，GPU ABI 尚未扩展。
    occlusion,          ///< 环境光遮蔽通道；CPU 侧保留引用，GPU ABI 尚未扩展。
    emissive,           ///< 自发光通道；CPU 侧保留引用，GPU ABI 尚未扩展。

    channel_count       ///< 哨兵，必须留在最后；MAX_CHANNEL_COUNT 由它推导。
};

#ifndef __SLANG__
/** @brief 贴图通道总数（不含 channel_count 自身），即材质 textures 数组的定长维度。 */
inline constexpr uint32_t MAX_CHANNEL_COUNT = std::to_underlying(texture_channel::channel_count);
#endif  // !__SLANG__

/**
 * @brief 一盏灯的 GPU 数据，方向光、点光与聚光共用同一布局。
 * @details 由 scene_light_manager 的共享 SSBO 存放，着色器经各自 scene data 的 lights 指针按 light_count 遍历。
 *          active_type 只用于选择分支，不适用类型的字段被忽略；字段顺序即 GPU ABI，不得重排。
 */
struct vulkan_light_data
{
    VULKAN_VEC3 color      VULKAN_DEFAULT(VULKAN_VEC3(1.f));  ///< @brief 灯光颜色，线性 RGB，未预乘强度；默认白
    uint32_t active_type   VULKAN_DEFAULT(0);  ///< @brief 光源类型，取 light_type 的底层值：0 方向光、1 点光、2 聚光
    VULKAN_VEC3 direction  VULKAN_DEFAULT(VULKAN_VEC3(0.f, -1.f, 0.f));  ///< @brief 方向光与聚光的照射方向，世界空间单位向量，默认 (0,-1,0)
    float intensity        VULKAN_DEFAULT(1.f);  ///< @brief 光强；方向光直接乘到 radiance，点光与聚光再乘 1/d² 与距离窗口
    VULKAN_VEC3 position   VULKAN_DEFAULT(VULKAN_VEC3(0.f));  ///< @brief 点光与聚光的世界空间位置；方向光忽略
    float range            VULKAN_DEFAULT(10.f);  ///< @brief 点光与聚光的最大影响距离，世界单位；小于等于 0 表示不做距离截断
    float inner_cone_angle VULKAN_DEFAULT(glm::radians(15.f));  ///< @brief 聚光内锥半角，弧度，锥内为满强度；默认 15 度
    float outer_cone_angle VULKAN_DEFAULT(glm::radians(30.f));  ///< @brief 聚光外锥半角，弧度；内外锥之间对余弦插值结果取平方衰减，默认 30 度
};

/**
 * @brief 一个材质的 GPU 参数，CPU 侧 scene_material_data 的 PBR 字段直接映射到这里。
 * @details 最终反照率等于 albedo 乘 albedo 贴图 RGB；电介质 F0 由 reflectance 推出，金属度在 F0 与 albedo 之间插值。
 *          结构共 68 字节，字段顺序即 GPU ABI，不得重排。
 */
struct vulkan_material_data
{
    VULKAN_VEC3 albedo      VULKAN_DEFAULT(VULKAN_VEC3(1.f));  ///< @brief 基础反照率，线性 RGB，会与 albedo 贴图 RGB 相乘；默认白
    uint32_t texture_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());  ///< @brief bindless 采样器数组下标（albedo 通道的紧凑绑定）；UINT32_MAX 表示无贴图
    float roughness         VULKAN_DEFAULT(0.5f);  ///< @brief 粗糙度，范围 0 到 1；着色器再平方成 GGX 的 alpha
    VULKAN_VEC3 emission    VULKAN_DEFAULT(VULKAN_VEC3(0.f));  ///< @brief 自发光颜色，线性 RGB；与 emission_strength 相乘后加到出射辐射度
    float emission_strength VULKAN_DEFAULT(0.f);  ///< @brief 自发光强度倍数，默认 0 表示不发光
    VULKAN_VEC3 absorption  VULKAN_DEFAULT(VULKAN_VEC3(0.f));  ///< @brief 介质吸收系数，线性 RGB，单位为每世界单位；Beer-Lambert 按段累乘
    float metallic          VULKAN_DEFAULT(0.f);  ///< @brief 金属度，范围 0 到 1；0 为电介质、1 为金属，决定 F0 插值
    float reflectance       VULKAN_DEFAULT(0.5f);  ///< @brief 电介质反射率，范围 0 到 1；F0 等于 0.08 乘本值
    float ior               VULKAN_DEFAULT(1.5f);  ///< @brief 折射率，默认 1.5（玻璃）；用于透射界面的折射方向
    float transmission      VULKAN_DEFAULT(0.f);  ///< @brief 平滑介质界面分支概率，范围 0 到 1；1 表示只选介质界面，界面内部仍按 Fresnel 选择反射或折射
    float opacity           VULKAN_DEFAULT(1.f);  ///< @brief 不透明度，范围 0 到 1；所有光线按本值概率接受表面覆盖，1 表示完全覆盖
};


/**
 * @brief 单个顶点，光栅化的顶点输入与加速结构几何共用同一份内存。
 * @details position / normal / uv 依次对应顶点属性 location 0/1/2，stride 等于 sizeof(vulkan_model_vertex) = 32。
 *          C++ 侧额外提供默认三路比较，Slang 侧没有该成员。
 */
struct vulkan_model_vertex
{
    VULKAN_VEC3 position;  ///< @brief 模型空间位置，3 个 float32，单位与源 glTF 模型一致
    VULKAN_VEC3 normal;  ///< @brief 模型空间法线，通常为单位向量；源数据缺失时由加载器按径向朝外兜底
    VULKAN_VEC2 uv;  ///< @brief 纹理坐标，加载器保留源 UV，不强制限制到 0..1；越界寻址由采样器决定

#ifndef __SLANG__
    auto operator<=>(const vulkan_model_vertex& _other) const noexcept = default;  ///< @brief 按成员逐项比较的默认三路比较，供容器或去重使用
#endif  // !__SLANG__
};

/**
 * @brief 一个可绘制实例的 GPU 数据：模型变换、材质下标与几何设备地址。
 * @details 由 scene_model_manager 上传到共享 SSBO；光栅化按 SV_DrawIndex（间接绘制命令下标）取它，光追在命中时按 InstanceIndex 取它。
 *          几何地址指向共享顶点/索引缓冲中该实例所用网格的片段，同网格实例共用地址；缓冲重建后必须重新填充。
 */
struct vulkan_model_data
{
    VULKAN_MAT4 model_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));  ///< @brief 模型到世界的 4x4 变换，列主序（glm 存储）；着色器直接用它变换顶点
    uint32_t material_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());  ///< @brief 材质数组下标；UINT32_MAX 表示无材质，着色器据此跳过材质解析
    VULKAN_ADDRESS(vulkan_model_vertex) vertex_address VULKAN_DEFAULT(0u);  ///< @brief 顶点数组的设备地址（BDA），元素类型为 vulkan_model_vertex
    VULKAN_ADDRESS(uint32_t) index_address VULKAN_DEFAULT(0u);  ///< @brief 索引数组的设备地址（BDA），元素为 uint32_t，与顶点地址配套
};

// 环境（天空盒）：背景、IBL 与环境 NEE 共用这一份数据
/**
 * @brief 环境（天空盒）的 GPU 数据：bindless 贴图槽与强度。
 * @details 背景、IBL 与环境 NEE 共用这一份数据；强度只缩放 HDR equirect 路径，程序化天空回退不受影响。
 *          该结构由 scene_manager 按帧构造并转写进光栅化与光追的 scene data，本身不单独上传。
 */
struct vulkan_environment_data
{
    uint32_t environment_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());  ///< @brief 环境贴图在 bindless 数组中的槽位；越界回退程序化天空。
    float environment_intensity VULKAN_DEFAULT(1.f);  ///< @brief 环境辐亮度倍率，只缩放 HDR equirect 采样结果，不影响程序化天空
};

/**
 * @brief 光栅化渲染器的每帧 UBO：相机矩阵、场景数组地址、计数与曝光。
 * @details 四个矩阵都是列主序，深度范围 0 到 1（GLM_FORCE_DEPTH_ZERO_TO_ONE）。
 *          三个地址指向各 manager 的共享 SSBO（BDA），计数用于着色器的越界守卫。
 *          结构按 C 布局整块 memcpy 进 UBO 槽，槽宽按 minUniformBufferOffsetAlignment 对齐。
 */
struct vulkan_raster_scene_data
{
    VULKAN_MAT4 proj_matrix     VULKAN_DEFAULT(VULKAN_MAT4(1.f));  ///< @brief 相机空间到裁剪空间的投影矩阵，列主序，透视除法后的深度为 0 到 1
    VULKAN_MAT4 proj_inv_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));  ///< @brief proj_matrix 的逆矩阵，天空盒反投影使用
    VULKAN_MAT4 view_matrix     VULKAN_DEFAULT(VULKAN_MAT4(1.f));  ///< @brief 世界到相机空间的视图矩阵，列主序
    VULKAN_MAT4 view_inv_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));  ///< @brief view_matrix 的逆矩阵（相机空间到世界）
    VULKAN_ADDRESS(vulkan_light_data) lights VULKAN_DEFAULT(0);  ///< @brief 灯光数组的设备地址（BDA），元素类型为 vulkan_light_data
    VULKAN_ADDRESS(vulkan_material_data) materials VULKAN_DEFAULT(0);  ///< @brief 材质数组的设备地址（BDA），元素类型为 vulkan_material_data
    VULKAN_ADDRESS(vulkan_model_data) models VULKAN_DEFAULT(0);  ///< @brief 实例数组的设备地址（BDA），元素类型为 vulkan_model_data
    uint32_t light_count        VULKAN_DEFAULT(0);  ///< @brief lights 中有效元素个数，着色器光照循环的上界
    uint32_t texture_count      VULKAN_DEFAULT(0);  ///< @brief bindless 贴图已占用数量，采样前用它守卫 texture_index
    uint32_t material_count     VULKAN_DEFAULT(0);  ///< @brief materials 中有效元素个数，材质下标守卫
    uint32_t model_count        VULKAN_DEFAULT(0);  ///< @brief models 中有效元素个数，SV_DrawIndex 与实例下标共用同一个上界
    uint32_t environment_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());  ///< @brief 环境贴图的 bindless 槽，越界时着色器回退到程序化天空
    VULKAN_VEC3 view_position   VULKAN_DEFAULT(VULKAN_VEC3(0.f));  ///< @brief 相机世界空间位置，着色器用它求表面到相机的出射方向
    float exposure              VULKAN_DEFAULT(1.f);  ///< @brief 曝光倍率，线性乘到出射辐射度上，不做 sRGB 编码
    float environment_intensity VULKAN_DEFAULT(1.f);  ///< @brief 环境辐亮度倍率，语义与 vulkan_environment_data 同名字段一致
};

/**
 * @brief 光追（RT pipeline 与 Ray Query）渲染器的每帧 UBO。
 * @details 比光栅化版少了正向 proj 与 view（rayGen 只用逆矩阵反投影主光线），多了 frame_index 供 RNG 播种与累积。
 *          两个光追分支共用同一布局与字段语义；逐像素分布的一致性还依赖各自的采样与求交实现。
 */
struct vulkan_rt_scene_data
{
    VULKAN_MAT4 proj_inv_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));  ///< @brief proj_matrix 的逆矩阵，rayGen 由近远平面反投影出主光线
    VULKAN_MAT4 view_inv_matrix VULKAN_DEFAULT(VULKAN_MAT4(1.f));  ///< @brief 相机空间到世界的逆视图矩阵
    VULKAN_ADDRESS(vulkan_light_data) lights VULKAN_DEFAULT(0);  ///< @brief 灯光数组的设备地址（BDA），元素类型为 vulkan_light_data
    VULKAN_ADDRESS(vulkan_material_data) materials VULKAN_DEFAULT(0);  ///< @brief 材质数组的设备地址（BDA），元素类型为 vulkan_material_data
    VULKAN_ADDRESS(vulkan_model_data) models VULKAN_DEFAULT(0);  ///< @brief 实例数组的设备地址（BDA），元素类型为 vulkan_model_data
    uint32_t light_count        VULKAN_DEFAULT(0);  ///< @brief lights 中有效元素个数，光源采样循环的上界
    uint32_t texture_count      VULKAN_DEFAULT(0);  ///< @brief bindless 贴图已占用数量，贴图采样与材质守卫共用
    uint32_t material_count     VULKAN_DEFAULT(0);  ///< @brief materials 中有效元素个数，命中着色时的材质守卫
    uint32_t model_count        VULKAN_DEFAULT(0);  ///< @brief models 中有效实例个数，InstanceIndex 守卫
    uint32_t environment_index  VULKAN_DEFAULT(std::numeric_limits<uint32_t>::max());  ///< @brief 环境贴图的 bindless 槽，越界时回退到程序化天空
    uint64_t frame_index        VULKAN_DEFAULT(0);  ///< @brief 帧序号，累积的样本下标并参与 RNG 播种；场景或相机变化时由 CPU 复位
    float exposure              VULKAN_DEFAULT(1.f);  ///< @brief 曝光倍率，与光栅化版同义，线性作用于样本颜色
    float environment_intensity VULKAN_DEFAULT(1.f);  ///< @brief 环境辐亮度倍率，语义与 vulkan_environment_data 同名字段一致
};

#ifdef __SLANG__
/**
 * @brief Slang 侧的静态布局自检：条件数组维度为负时编译失败。
 * @details 用 float 数组的条件维度表达“sizeof 必须等于某值”，与 C++ 侧的 sizeof 断言对应；
 *          这里没有检查 offsetof，改 GPU 结构时仍须核对着色器实际成员偏移。
 */
struct vulkan_data_layout_check
{
    float model_vertex_must_be_32_bytes[sizeof(vulkan_model_vertex) == 32 ? 1 : -1];  ///< @brief 断言 vulkan_model_vertex 的 sizeof 为 32 字节。
    float model_data_must_be_88_bytes[sizeof(vulkan_model_data) == 88 ? 1 : -1];  ///< @brief 断言 vulkan_model_data 的 sizeof 为 88 字节。
    float material_data_must_be_68_bytes[sizeof(vulkan_material_data) == 68 ? 1 : -1];  ///< @brief 断言 vulkan_material_data 的 sizeof 为 68 字节。
    float light_data_must_be_56_bytes[sizeof(vulkan_light_data) == 56 ? 1 : -1];  ///< @brief 断言 vulkan_light_data 的 sizeof 为 56 字节。
    float rt_scene_data_must_be_192_bytes[sizeof(vulkan_rt_scene_data) == 192 ? 1 : -1];  ///< @brief 断言 vulkan_rt_scene_data 的 sizeof 为 192 字节。
    float raster_scene_data_must_be_320_bytes[sizeof(vulkan_raster_scene_data) == 320 ? 1 : -1];  ///< @brief 断言 vulkan_raster_scene_data 的 sizeof 为 320 字节。
    float environment_data_must_be_8_bytes[sizeof(vulkan_environment_data) == 8 ? 1 : -1];  ///< @brief 断言 vulkan_environment_data 的 sizeof 为 8 字节。
};
#else
static_assert(sizeof(vulkan_light_data) == 56);
static_assert(offsetof(vulkan_light_data, color) == 0 && offsetof(vulkan_light_data, active_type) == 12
              && offsetof(vulkan_light_data, direction) == 16 && offsetof(vulkan_light_data, intensity) == 28
              && offsetof(vulkan_light_data, position) == 32 && offsetof(vulkan_light_data, range) == 44
              && offsetof(vulkan_light_data, inner_cone_angle) == 48 && offsetof(vulkan_light_data, outer_cone_angle) == 52);

static_assert(sizeof(vulkan_material_data) == 68);
static_assert(offsetof(vulkan_material_data, albedo) == 0 && offsetof(vulkan_material_data, texture_index) == 12
              && offsetof(vulkan_material_data, roughness) == 16 && offsetof(vulkan_material_data, emission) == 20
              && offsetof(vulkan_material_data, emission_strength) == 32
              && offsetof(vulkan_material_data, absorption) == 36 && offsetof(vulkan_material_data, metallic) == 48
              && offsetof(vulkan_material_data, reflectance) == 52 && offsetof(vulkan_material_data, ior) == 56
              && offsetof(vulkan_material_data, transmission) == 60 && offsetof(vulkan_material_data, opacity) == 64);

static_assert(sizeof(vulkan_model_vertex) == 32);
static_assert(offsetof(vulkan_model_vertex, position) == 0 && offsetof(vulkan_model_vertex, normal) == 12
              && offsetof(vulkan_model_vertex, uv) == 24);

static_assert(sizeof(vulkan_model_data) == 88);
static_assert(offsetof(vulkan_model_data, model_matrix) == 0 && offsetof(vulkan_model_data, material_index) == 64
              && offsetof(vulkan_model_data, vertex_address) == 72 && offsetof(vulkan_model_data, index_address) == 80);

static_assert(sizeof(vulkan_environment_data) == 8);
static_assert(offsetof(vulkan_environment_data, environment_index) == 0
              && offsetof(vulkan_environment_data, environment_intensity) == 4);

static_assert(sizeof(vulkan_raster_scene_data) == 320);
static_assert(offsetof(vulkan_raster_scene_data, proj_matrix) == 0 && offsetof(vulkan_raster_scene_data, proj_inv_matrix) == 64
              && offsetof(vulkan_raster_scene_data, view_matrix) == 128 && offsetof(vulkan_raster_scene_data, view_inv_matrix) == 192
              && offsetof(vulkan_raster_scene_data, lights) == 256 && offsetof(vulkan_raster_scene_data, materials) == 264
              && offsetof(vulkan_raster_scene_data, models) == 272 && offsetof(vulkan_raster_scene_data, light_count) == 280
              && offsetof(vulkan_raster_scene_data, texture_count) == 284
              && offsetof(vulkan_raster_scene_data, material_count) == 288 && offsetof(vulkan_raster_scene_data, model_count) == 292
              && offsetof(vulkan_raster_scene_data, environment_index) == 296
              && offsetof(vulkan_raster_scene_data, view_position) == 300 && offsetof(vulkan_raster_scene_data, exposure) == 312
              && offsetof(vulkan_raster_scene_data, environment_intensity) == 316);

static_assert(sizeof(vulkan_rt_scene_data) == 192);
static_assert(offsetof(vulkan_rt_scene_data, proj_inv_matrix) == 0 && offsetof(vulkan_rt_scene_data, view_inv_matrix) == 64
              && offsetof(vulkan_rt_scene_data, lights) == 128 && offsetof(vulkan_rt_scene_data, materials) == 136
              && offsetof(vulkan_rt_scene_data, models) == 144 && offsetof(vulkan_rt_scene_data, light_count) == 152
              && offsetof(vulkan_rt_scene_data, texture_count) == 156 && offsetof(vulkan_rt_scene_data, material_count) == 160
              && offsetof(vulkan_rt_scene_data, model_count) == 164 && offsetof(vulkan_rt_scene_data, environment_index) == 168
              && offsetof(vulkan_rt_scene_data, frame_index) == 176 && offsetof(vulkan_rt_scene_data, exposure) == 184
              && offsetof(vulkan_rt_scene_data, environment_intensity) == 188);
#endif  // __SLANG__
