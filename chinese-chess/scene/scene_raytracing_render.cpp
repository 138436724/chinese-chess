#include "scene_raytracing_render.h"

#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_commandbuffer.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

enum class stage_indices : uint32_t
{
    ray_gen,
    miss,
    miss_shadow,
    closest_hit,
    anyhit_shadow,
    shader_group_max_count
};

scene_raytracing_render::scene_raytracing_render(const vma::raii::Allocator&     _allocator,
                                                 const vk::raii::PhysicalDevice& _physical_device,
                                                 const vk::raii::Device&         _device,
                                                 vulkan_recycle_bin&             _recycle_bin,
                                                 vulkan_semaphore&               _semaphore,
                                                 const vulkan_queue&             _graphic_queue,
                                                 const vulkan_queue&             _compute_queue,
                                                 const vulkan_queue&             _transfer_queue,
                                                 const scene_model_manager&      _model_manager,
                                                 const scene_material_manager&   _material_manager,
                                                 const scene_light_manager&      _light_manager,
                                                 const vk::raii::Sampler&        _image_sampler,
                                                 vulkan_image&                   _render_output)
    : allocator(_allocator)
    , physical_device(_physical_device)
    , device(_device)
    , recycle_bin(_recycle_bin)
    , semaphore(_semaphore)
    , graphic_queue(_graphic_queue)
    , compute_queue(_compute_queue)
    , transfer_queue(_transfer_queue)
    , model_manager(_model_manager)
    , material_manager(_material_manager)
    , light_manager(_light_manager)
    , image_sampler(_image_sampler)
    , render_output(_render_output)
{
    create_pipeline_and_sbt();
}

void scene_raytracing_render::resize(uint32_t _width, uint32_t _height)
{
    if (width == _width && height == _height)
    {
        return;
    }

    width  = _width;
    height = _height;
}

void scene_raytracing_render::update()
{
    frame_index = 0;
    update_descriptor();
}

void scene_raytracing_render::recreate()
{
    recycle_bin.retire(std::move(pipeline), "old ray tracing pipeline on reload.");
    recycle_bin.retire(std::move(sbt), "old ray tracing sbt on reload.");
    create_pipeline_and_sbt();
}

void scene_raytracing_render::reset_accumulation() noexcept
{
    frame_index = 0;
}

void scene_raytracing_render::render(const scene_camera& _camera, const vk::raii::CommandBuffer& _commandbuffer, uint32_t _skybox_index)
{
    const auto image_begin_barrier =
        vk::ImageMemoryBarrier2(render_output.get_stage(), render_output.get_access(),
                                vk::PipelineStageFlagBits2::eRayTracingShaderKHR, vk::AccessFlagBits2::eShaderStorageWrite,
                                render_output.get_layout(), vk::ImageLayout::eGeneral, vk::QueueFamilyIgnored,
                                vk::QueueFamilyIgnored, render_output.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    render_output.set_info(image_begin_barrier);
    const std::array begin_barrier = {image_begin_barrier};
    _commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

    _commandbuffer.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, pipeline.get_pipeline());
    _commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, pipeline.get_pipeline_layout(), 0,
                                      *(descriptor_sets.at(current_frame)), nullptr);

    const scene_raytracing_render::push_constant pc{
        _camera.get_inv_projection_matrix(),
        _camera.get_inv_view_matrix(),
        _skybox_index,
        static_cast<uint32_t>(light_manager.get_lights().size()),
        frame_index,
    };
    _commandbuffer.pushConstants2(vk::PushConstantsInfo(pipeline.get_pipeline_layout(), vk::ShaderStageFlagBits::eAll,
                                                        0, sizeof(scene_raytracing_render::push_constant), &pc));

    _commandbuffer.traceRaysKHR(sbt.get_raygen_region(), sbt.get_miss_region(), sbt.get_hit_region(),
                                sbt.get_callable_region(), width, height, 1);

    const auto image_end_barrier =
        vk::ImageMemoryBarrier2(render_output.get_stage(), render_output.get_access(),
                                vk::PipelineStageFlagBits2::eRayTracingShaderKHR, vk::AccessFlagBits2::eShaderStorageRead,
                                render_output.get_layout(), vk::ImageLayout::eGeneral, vk::QueueFamilyIgnored,
                                vk::QueueFamilyIgnored, render_output.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    render_output.set_info(image_end_barrier);
    const std::array end_barrier = {image_end_barrier};
    _commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));

    frame_index++;
    current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}

void scene_raytracing_render::create_pipeline_and_sbt()
{
    // create pipeline
    std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eAll, nullptr),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eAll, nullptr),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr),
        vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr),
        vk::DescriptorSetLayoutBinding(5, vk::DescriptorType::eCombinedImageSampler, 1024, vk::ShaderStageFlagBits::eAll, nullptr),
    };

    const vk::PushConstantRange push_constant(vk::ShaderStageFlagBits::eAll, 0, sizeof(scene_raytracing_render::push_constant));

    const auto spirv_code =
        SHADER_COMPILER.compile_shader_to_spv(std::filesystem::path(SHADERS_PATH) / "ray_tracing.slang",
                                              {RAY_GEN_ENTRY_NAME, RAY_MISS_ENTRY_NAME, RAY_SHADOW_MISS_ENTRY_NAME,
                                               RAY_CLOSEST_HIT_ENTRY_NAME, RAY_SHADOW_ANY_HIT_ENTRY_NAME});
    if (!spirv_code)
    {
        throw std::runtime_error(spirv_code.error());
    }
    const vk::raii::ShaderModule shaderModule(
        device, vk::ShaderModuleCreateInfo({}, spirv_code->size() * sizeof(char),
                                           reinterpret_cast<const uint32_t*>(spirv_code->data())));

    std::array<vk::PipelineShaderStageCreateInfo, static_cast<size_t>(stage_indices::shader_group_max_count)> shader_stages = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eRaygenKHR, shaderModule, RAY_GEN_ENTRY_NAME.data()),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eMissKHR, shaderModule, RAY_MISS_ENTRY_NAME.data()),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eMissKHR, shaderModule,
                                          RAY_SHADOW_MISS_ENTRY_NAME.data()),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eClosestHitKHR, shaderModule,
                                          RAY_CLOSEST_HIT_ENTRY_NAME.data()),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eAnyHitKHR, shaderModule,
                                          RAY_SHADOW_ANY_HIT_ENTRY_NAME.data()),
    };

    std::vector<vk::RayTracingShaderGroupCreateInfoKHR> shader_groups = {
        // Group 0: Ray generation (general)
        vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                               static_cast<uint32_t>(stage_indices::ray_gen)),
        // Group 1: Primary miss (general)
        vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral, static_cast<uint32_t>(stage_indices::miss)),
        // Group 2: Shadow miss (general)
        vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                               static_cast<uint32_t>(stage_indices::miss_shadow)),
        // Group 3: Primary hit group (triangles)
        vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
                                               vk::ShaderUnusedKHR, static_cast<uint32_t>(stage_indices::closest_hit)),
        // Group 4: Shadow hit group (triangles, any-hit only)
        vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup, vk::ShaderUnusedKHR,
                                               vk::ShaderUnusedKHR, static_cast<uint32_t>(stage_indices::anyhit_shadow)),
    };

    const auto props = physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR,
                                                      vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
    const auto& properties = props.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
    pipeline.create(device, bindings, std::span(&push_constant, 1), shader_stages, shader_groups, properties.maxRayRecursionDepth);


    // create shader binding table
    vulkan_commandbuffer commandbuffer = std::move(
        vulkan_commandbuffer::create(
            device, vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(), vk::CommandBufferLevel::ePrimary, 1),
            &graphic_queue, &semaphore)
            .front());
    commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


    auto staging_buffer = sbt.create(physical_device, device, allocator, *commandbuffer, pipeline.get_pipeline(),
                                     static_cast<uint32_t>(shader_stages.size()), graphic_queue.get_index());

    commandbuffer.end_record();
    commandbuffer.submit(false);

    recycle_bin.retire(std::move(staging_buffer), "ray tracing staging buffer to create sbt.");
    recycle_bin.retire(std::move(commandbuffer), "ray tracing commandbuffer to create sbt.");
}

void scene_raytracing_render::update_descriptor()
{
    // update descriptor pool
    recycle_bin.retire(std::move(descriptor_sets), "ray tracing descriptor sets.");
    recycle_bin.retire(std::move(descriptor_pool), "ray tracing descriptor pool.");

    std::array pool_size = {
        vk::DescriptorPoolSize(vk::DescriptorType::eAccelerationStructureKHR, vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3 * vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 1024 * vulkan_common::MAX_FRAMES_IN_FLIGHT)};

    vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet
                                                      | vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
                                                  vulkan_common::MAX_FRAMES_IN_FLIGHT, pool_size);
    descriptor_pool = vk::raii::DescriptorPool(device, pool_create_info);

    // descriptor set
    std::vector<vk::DescriptorSetLayout> layouts(vulkan_common::MAX_FRAMES_IN_FLIGHT, pipeline.get_descriptor_set_layout());
    auto alloc_info = vk::DescriptorSetAllocateInfo(descriptor_pool, layouts);
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

        const std::array sampler = {*image_sampler};
        const auto       material_sets =
            material_manager.get_descriptor_info(sampler) | std::views::enumerate
            | std::views::transform([&descriptor_set](const auto& _pair) {
                  const auto& [index, image_info] = _pair;
                  return vk::WriteDescriptorSet(descriptor_set, 5, static_cast<uint32_t>(index),
                                                vk::DescriptorType::eCombinedImageSampler, image_info, {});
              });
        std::ranges::move(material_sets, std::back_inserter(write_sets));

        device.updateDescriptorSets(write_sets, {});
    });
}
