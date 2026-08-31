#include "scene_rayquery_render.h"

#include "scene_camera.h"
#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <array>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view RAY_QUERY_ENTRY_NAME = "rayQueryComputeMain";
constexpr uint32_t         WORKGROUP_SIZE       = 8;

}  // namespace

scene_rayquery_render::scene_rayquery_render(const vma::raii::Allocator&     _allocator,
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
                                             vulkan_image&                   _render_output)
    : allocator(_allocator)
    , physical_device(_physical_device)
    , device(_device)
    , pipeline_cache(_pipeline_cache)
    , recycle_bin(_recycle_bin)
    , semaphore(_semaphore)
    , graphic_queue(_graphic_queue)
    , compute_queue(_compute_queue)
    , transfer_queue(_transfer_queue)
    , model_manager(_model_manager)
    , material_manager(_material_manager)
    , light_manager(_light_manager)
    , render_output(_render_output)
{
    create_pipeline();
}

void scene_rayquery_render::resize(uint32_t _width, uint32_t _height)
{
    width  = _width;
    height = _height;
}

void scene_rayquery_render::update()
{
    frame_index = 0;
    update_descriptor();
}

void scene_rayquery_render::recreate()
{
    recycle_bin.retire(std::move(pipeline), "old ray query compute pipeline on reload.");
    create_pipeline();
}

void scene_rayquery_render::reset_accumulation() noexcept
{
    frame_index = 0;
}

void scene_rayquery_render::render(const scene_camera& _camera, const vk::raii::CommandBuffer& _commandbuffer, uint32_t _skybox_index)
{
    const auto image_begin_barrier =
        vk::ImageMemoryBarrier2(render_output.get_stage(), render_output.get_access(), vk::PipelineStageFlagBits2::eComputeShader,
                                vk::AccessFlagBits2::eShaderStorageWrite, render_output.get_layout(), vk::ImageLayout::eGeneral,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, render_output.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    render_output.set_info(image_begin_barrier);
    _commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, image_begin_barrier));

    _commandbuffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.get_pipeline());
    _commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline.get_pipeline_layout(), 0,
                                      *descriptor_sets.at(current_frame), nullptr);

    const scene_rayquery_render::push_constant pc{
        _camera.get_inv_projection_matrix(),
        _camera.get_inv_view_matrix(),
        _skybox_index,
        static_cast<uint32_t>(light_manager.get_lights().size()),
        frame_index,
    };
    _commandbuffer.pushConstants2(vk::PushConstantsInfo(pipeline.get_pipeline_layout(), vk::ShaderStageFlagBits::eCompute,
                                                        0, sizeof(scene_rayquery_render::push_constant), &pc));

    const uint32_t group_x = (width + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
    const uint32_t group_y = (height + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
    _commandbuffer.dispatch(group_x, group_y, 1);

    const auto image_end_barrier =
        vk::ImageMemoryBarrier2(render_output.get_stage(), render_output.get_access(), vk::PipelineStageFlagBits2::eComputeShader,
                                vk::AccessFlagBits2::eShaderStorageRead, render_output.get_layout(), vk::ImageLayout::eGeneral,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, render_output.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    render_output.set_info(image_end_barrier);
    _commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, image_end_barrier));

    frame_index++;
    current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}

void scene_rayquery_render::create_pipeline()
{
    constexpr std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eCompute, nullptr),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr),
        vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr),
        vk::DescriptorSetLayoutBinding(5, vk::DescriptorType::eCombinedImageSampler, 1024, vk::ShaderStageFlagBits::eCompute, nullptr),
    };

    constexpr vk::PushConstantRange push_constant(vk::ShaderStageFlagBits::eCompute, 0, sizeof(scene_rayquery_render::push_constant));

    constexpr std::array shader_stages = {shader_stage_info{RAY_QUERY_ENTRY_NAME, vk::ShaderStageFlagBits::eCompute}};

    pipeline.create_from_shader(device, pipeline_cache, bindings, std::span(&push_constant, 1),
                                std::filesystem::path(SHADERS_PATH) / "ray_query.slang", shader_stages);
}

void scene_rayquery_render::update_descriptor()
{
    recycle_bin.retire(std::move(descriptor_sets), "ray query descriptor sets.");
    recycle_bin.retire(std::move(descriptor_pool), "ray query descriptor pool.");

    constexpr std::array pool_size = {
        vk::DescriptorPoolSize(vk::DescriptorType::eAccelerationStructureKHR, vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3 * vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 1024 * vulkan_common::MAX_FRAMES_IN_FLIGHT)};

    vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet
                                                      | vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
                                                  vulkan_common::MAX_FRAMES_IN_FLIGHT, pool_size);
    descriptor_pool = vk::raii::DescriptorPool(device, pool_create_info);

    std::vector<vk::DescriptorSetLayout> layouts(vulkan_common::MAX_FRAMES_IN_FLIGHT, pipeline.get_descriptor_set_layout());
    auto alloc_info = vk::DescriptorSetAllocateInfo(*descriptor_pool, layouts);
    descriptor_sets = device.allocateDescriptorSets(alloc_info);

    std::ranges::for_each(descriptor_sets, [&](const auto& descriptor_set) {
        std::vector<vk::WriteDescriptorSet> write_sets;

        const vk::DescriptorBufferInfo as_buffer_info(model_manager.get_tlas().get_buffer(), 0,
                                                      sizeof(vk::AccelerationStructureInstanceKHR) * model_manager.get_models_size());
        vk::StructureChain<vk::WriteDescriptorSet, vk::WriteDescriptorSetAccelerationStructureKHR> as_write_set(
            vk::WriteDescriptorSet(descriptor_set, 0, {}, vk::DescriptorType::eAccelerationStructureKHR, {}, as_buffer_info),
            vk::WriteDescriptorSetAccelerationStructureKHR(*(model_manager.get_tlas().get_acceleration_structure())));
        write_sets.push_back(as_write_set.get());

        const vk::DescriptorImageInfo storage_image_info(nullptr, render_output.get_imageview(), vk::ImageLayout::eGeneral);
        write_sets.emplace_back(
            vk::WriteDescriptorSet(descriptor_set, 1, {}, vk::DescriptorType::eStorageImage, storage_image_info, {}));

        const vk::DescriptorBufferInfo model_buffer_info(model_manager.get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
        write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 2, {}, vk::DescriptorType::eStorageBuffer, {}, model_buffer_info));

        const vk::DescriptorBufferInfo material_buffer_info(material_manager.get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
        write_sets.emplace_back(
            vk::WriteDescriptorSet(descriptor_set, 3, {}, vk::DescriptorType::eStorageBuffer, {}, material_buffer_info));

        const vk::DescriptorBufferInfo light_buffer_info(light_manager.get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
        write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 4, {}, vk::DescriptorType::eStorageBuffer, {}, light_buffer_info));

        const auto material_sets =
            material_manager.get_descriptor_info() | std::views::enumerate
            | std::views::transform([&descriptor_set](const auto& _pair) {
                  const auto& [index, image_info] = _pair;
                  return vk::WriteDescriptorSet(descriptor_set, 5, static_cast<uint32_t>(index),
                                                vk::DescriptorType::eCombinedImageSampler, image_info, {});
              });
        std::ranges::move(material_sets, std::back_inserter(write_sets));

        (*device).updateDescriptorSets(write_sets, {});
    });
}
