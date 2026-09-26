#pragma once

#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_pipeline.h"
#include "vulkan_core/vulkan_shader_binding_table.h"

#include <glm/glm.hpp>
#include <limits>
#include <utility>
#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>
#include <vulkan/vulkan_raii.hpp>

class scene_camera;
class vulkan_recycle_bin;
class vulkan_semaphore;
class vulkan_queue;
class vulkan_image;
class scene_model_manager;
class scene_material_manager;
class scene_light_manager;

// ============================================================================
// 物理正确性调试开关(与 resources/shaders/common.slang 的 DEBUG_* 常量按位对应)
//
// 存在的理由:能量守恒验证必须能在**无任何启发式钳制**的条件下进行。
// 四类钳制(firefly / 单光贡献 / 路径吞吐量 / 粗糙度下限)都会掩盖能量错误 ——
// 开着它们做白炉测试,测到的是钳制行为而不是物理。
// ============================================================================
enum class pbr_debug_flags : uint32_t
{
    none                      = 0u,
    disable_firefly_clamp     = 1u << 0,  // 关闭时域累积 firefly 钳制
    disable_direct_clamp      = 1u << 1,  // 关闭单光源贡献钳制
    disable_throughput_clamp  = 1u << 2,  // 关闭路径吞吐量钳制
    disable_ambient           = 1u << 3,  // 关闭环境光(隔离直接光)
    force_metallic            = 1u << 4,  // 强制金属度 = force_metallic(抹平所有材质)
    force_roughness           = 1u << 5,  // 强制粗糙度 = force_roughness(抹平所有材质)
    force_albedo              = 1u << 6,  // 只强制反照率为白,保留每格 metallic/roughness
    viz_normal                = 1u << 7,  // 可视化着色法线
    viz_f0                    = 1u << 8,  // 可视化 F0
    viz_roughness             = 1u << 9,  // 可视化粗糙度
    viz_metallic              = 1u << 10, // 可视化金属度
    viz_direct_only           = 1u << 11, // 只显示直接光(隔离 NEE)
    uniform_environment       = 1u << 12, // 均匀环境:天空处处为 1.0(真正的白炉)
    disable_direct_lights     = 1u << 13, // 关闭全部直接光(白炉:环境为唯一光源)
    disable_fog               = 1u << 14, // 关闭高度雾(雾含硬编码太阳项,白炉必须关)
};

// 白炉预设:关闭全部钳制,反照率强制为白,**保留每个材质自己的 metallic/roughness**。
// 球体阵场景靠它做逐格能量核查;若同时打开 force_metallic/force_roughness
// 会把整个网格抹成同一材质,那就失去"逐格"的意义了。
//
// **包含 uniform_environment** —— 没有均匀环境就不是白炉:
// 非均匀天空下"粗糙度变化改变反射方向"与"能量损失"无法区分
// (实测介电行与金属行都随粗糙度降 14%~18%,而介电镜面分量仅约 4%,
//  说明该下降主要来自环境方向性,而非能量损失)。
[[nodiscard]] constexpr uint32_t furnace_preset() noexcept
{
    return static_cast<uint32_t>(pbr_debug_flags::disable_firefly_clamp)
         | static_cast<uint32_t>(pbr_debug_flags::disable_direct_clamp)
         | static_cast<uint32_t>(pbr_debug_flags::disable_throughput_clamp)
         | static_cast<uint32_t>(pbr_debug_flags::force_albedo)
         | static_cast<uint32_t>(pbr_debug_flags::uniform_environment)
         | static_cast<uint32_t>(pbr_debug_flags::disable_direct_lights)
         | static_cast<uint32_t>(pbr_debug_flags::disable_fog);
}

// 白炉 + 关环境光:只剩直接光时核查 NEE 的 Fresnel 修正
[[nodiscard]] constexpr uint32_t furnace_preset_direct_only() noexcept
{
    return furnace_preset() | static_cast<uint32_t>(pbr_debug_flags::disable_ambient);
}

class scene_raytracing_render
{
public:
    scene_raytracing_render(const vma::raii::Allocator&     _allocator,
                            const vk::raii::PhysicalDevice& _physical_device,
                            const vk::raii::Device&         _device,
                            const vk::raii::PipelineCache&  _pipeline_cache,
                            vulkan_recycle_bin&             _recycle_bin,
                            vulkan_semaphore&               _semaphore,
                            const vulkan_queue&             _graphic_queue,
                            const vulkan_queue&             _transfer_queue,
                            const scene_model_manager&      _model_manager,
                            const scene_material_manager&   _material_manager,
                            const scene_light_manager&      _light_manager,
                            vulkan_image&                   _render_output);
    ~scene_raytracing_render() = default;

    void resize(uint32_t _width, uint32_t _height);
    void update();
    void render(const scene_camera& _camera, const vk::raii::CommandBuffer& _commandbuffer, uint32_t _skybox_index);
    void recreate();
    void reset_accumulation() noexcept;

    // 调试通道(见 pbr_debug_flags)
    void set_debug_flags(uint32_t _flags) noexcept;
    [[nodiscard]] uint32_t get_debug_flags() const noexcept;
    void                   set_debug_mat_override(float _metallic, float _roughness) noexcept;
    [[nodiscard]] float    get_debug_force_metallic() const noexcept;
    [[nodiscard]] float    get_debug_force_roughness() const noexcept;

private:
    [[nodiscard]] std::pair<vulkan_pipeline, vulkan_shader_binding_table> create_pipeline_and_sbt();
    void                                                                  update_descriptor();

private:
    const vma::raii::Allocator&     allocator;
    const vk::raii::PhysicalDevice& physical_device;
    const vk::raii::Device&         device;
    const vk::raii::PipelineCache&  pipeline_cache;
    vulkan_recycle_bin&             recycle_bin;
    vulkan_semaphore&               semaphore;
    const vulkan_queue&             graphic_queue;
    const vulkan_queue&             transfer_queue;

    const scene_model_manager&    model_manager;
    const scene_material_manager& material_manager;
    const scene_light_manager&    light_manager;
    vulkan_image&                 render_output;

    uint32_t width         = 0;
    uint32_t height        = 0;
    uint32_t current_frame = 0;
    uint32_t frame_index   = 0;

    // 物理正确性调试状态
    uint32_t debug_flags         = 0;
    float    env_intensity       = 1.0f;  // 环境光强度(阶段 3;0 = 关闭)
    float    debug_force_metallic  = 0.0f;
    float    debug_force_roughness = 0.5f;

    vk::DeviceSize ubo_offset = 0;
    vulkan_buffer  ubo;

    vulkan_pipeline                      pipeline;
    vk::raii::DescriptorPool             descriptor_pool = nullptr;
    std::vector<vk::raii::DescriptorSet> descriptor_sets;
    vulkan_shader_binding_table          sbt;
};
