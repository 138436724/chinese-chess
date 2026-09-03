#pragma once

#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_pipeline.h"
#include "vulkan_core/vulkan_shader_binding_table.h"

#include <glm/glm.hpp>
#include <limits>
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
                            const vulkan_queue&             _compute_queue,
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

private:
    void create_pipeline_and_sbt();
    void update_descriptor();

private:
    const vma::raii::Allocator&     allocator;
    const vk::raii::PhysicalDevice& physical_device;
    const vk::raii::Device&         device;
    const vk::raii::PipelineCache&  pipeline_cache;
    vulkan_recycle_bin&             recycle_bin;
    vulkan_semaphore&               semaphore;
    const vulkan_queue&             graphic_queue;
    const vulkan_queue&             compute_queue;
    const vulkan_queue&             transfer_queue;

    const scene_model_manager&    model_manager;
    const scene_material_manager& material_manager;
    const scene_light_manager&    light_manager;
    vulkan_image&                 render_output;

    uint32_t width         = 0;
    uint32_t height        = 0;
    uint32_t current_frame = 0;
    uint32_t frame_index   = 0;

    vk::DeviceSize ubo_offset = 0;
    vulkan_buffer  ubo;

    vulkan_pipeline                      pipeline;
    vk::raii::DescriptorPool             descriptor_pool = nullptr;
    std::vector<vk::raii::DescriptorSet> descriptor_sets;
    vulkan_shader_binding_table          sbt;
};
