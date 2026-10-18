/**
 * @file scene_rayquery_render.cpp
 * @brief Ray Query 计算渲染器的实现：UBO 打包、eGeneral 布局转换、dispatch 与描述符重建。
 * @details 计算入口 rayQueryComputeMain 是 [numthreads(8,8,1)]，组数按 ceil(width/8) x ceil(height/8)
 *          计算；输出与线性累积图均以 storage image 绑定，dispatch 前同步两张图。
 *          采样序号由 UBO 的 frame_index 表达，历史均值保存在 accumulation 图中，update() 与 reset_accumulation() 都把它清零。
 */

#include "scene_rayquery_render.h"

#include "scene_camera.h"
#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "scene_texture_manager.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_data_type.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

/** @brief 计算 UBO 槽跨度并建 UBO 与计算管线；不建附件图。 */
scene_rayquery_render::scene_rayquery_render(const vma::raii::Allocator&     _allocator,
                                             const vk::raii::PhysicalDevice& _physical_device,
                                             const vk::raii::Device&         _device,
                                             const vk::raii::PipelineCache&  _pipeline_cache,
                                             vulkan_recycle_bin&             _recycle_bin,
                                             const vulkan_queue&             _graphic_queue,
                                             const vulkan_queue&             _transfer_queue,
                                             const scene_model_manager&      _model_manager,
                                             const scene_material_manager&   _material_manager,
                                             const scene_light_manager&      _light_manager,
                                             const scene_texture_manager&    _texture_manager,
                                             vulkan_image&                   _render_output,
                                             vulkan_image&                   _accumulation)
    : allocator(_allocator)
    , physical_device(_physical_device)
    , device(_device)
    , pipeline_cache(_pipeline_cache)
    , recycle_bin(_recycle_bin)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
    , model_manager(_model_manager)
    , material_manager(_material_manager)
    , light_manager(_light_manager)
    , texture_manager(_texture_manager)
    , render_output(_render_output)
    , accumulation(_accumulation)
{
    ubo_offset = vulkan_common::align_up(sizeof(vulkan_rt_scene_data),
                                         physical_device.getProperties2().properties.limits.minUniformBufferOffsetAlignment);

    const std::array queue_array = {graphic_queue.get_index()};
    ubo.create(allocator, device,
               vk::BufferCreateInfo({}, ubo_offset * vulkan_common::MAX_FRAMES_IN_FLIGHT,
                                    vk::BufferUsageFlagBits::eUniformBuffer, vk::SharingMode::eExclusive, queue_array),
               vma::MemoryUsage::eAuto, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite, "ray query ubo");

    pipeline = create_pipeline();
}

/** @brief 只记录尺寸，宽高不变时直接返回；输出图与附件由 scene_manager 统一管理。 */
void scene_rayquery_render::resize(uint32_t _width, uint32_t _height)
{
    if (width == _width && height == _height)
    {
        return;
    }

    width  = _width;
    height = _height;
}

/** @brief 清零 frame_index 并重建描述符集，使新的 TLAS / 输出图绑定从第 0 个样本开始累积。 */
void scene_rayquery_render::update()
{
    frame_index = 0;
    update_descriptor();
}

/** @brief 先建新管线，创建成功后才登记旧管线回收；创建失败保留旧管线，回收登记失败不保证回滚。 */
void scene_rayquery_render::recreate()
{
    vulkan_pipeline new_pipeline = create_pipeline();
    recycle_bin.retire_after_submitted(std::move(pipeline), "old ray query compute pipeline on reload.");
    pipeline = std::move(new_pipeline);
}

/** @brief 只把样本序号清零；下一帧着色器覆盖累积图，描述符与管线保持不变。 */
void scene_rayquery_render::reset_accumulation() noexcept
{
    frame_index = 0;
}

/**
 * @brief 写 UBO 槽、同步输出与累积图到 eGeneral、绑定管线与描述符并 dispatch，末尾只为输出图补屏障。
 * @note 组数向上取整，非整倍数尺寸的最后一组可能超出图像边界；着色器按 GetDimensions 的结果提前 return。
 * @note descriptor_sets 必须已由 update() 建好，且 _frame_slot 在范围内。
 */
void scene_rayquery_render::render(const scene_camera&            _camera,
                                   const vk::raii::CommandBuffer& _commandbuffer,
                                   const vulkan_environment_data& _environment, uint32_t _frame_slot)
{
    // update ubo
    const vulkan_rt_scene_data ubo_data{
        .proj_inv_matrix       = _camera.get_inv_projection_matrix(),
        .view_inv_matrix       = _camera.get_inv_view_matrix(),
        .lights                = light_manager.get_ssbo_buffer().get_buffer_address().deviceAddress,
        .materials             = material_manager.get_ssbo_buffer().get_buffer_address().deviceAddress,
        .models                = model_manager.get_ssbo_buffer().get_buffer_address().deviceAddress,
        .light_count           = static_cast<uint32_t>(light_manager.get_lights_size()),
        .texture_count         = static_cast<uint32_t>(texture_manager.get_textures_size()),
        .material_count        = static_cast<uint32_t>(material_manager.get_materials_size()),
        .model_count           = static_cast<uint32_t>(model_manager.get_models_size()),
        .environment_index     = _environment.environment_index,
        .frame_index           = frame_index,
        .exposure              = _camera.get_exposure(),
        .environment_intensity = _environment.environment_intensity,
    };

    auto* ubo_ptr = static_cast<uint8_t*>(ubo.get_buffer_address().hostAddress) + static_cast<size_t>(_frame_slot) * ubo_offset;
    //std::memset(ubo_ptr, 0, ubo_offset);
    std::memcpy(ubo_ptr, &ubo_data, sizeof(ubo_data));
    ubo.flush();


    // render
    const auto image_begin_barrier =
        render_output.transition_state(vk::PipelineStageFlagBits2::eComputeShader,
                                       vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
                                       vk::ImageLayout::eGeneral, vk::QueueFamilyIgnored);
    const auto accumulation_barrier = accumulation.transition_state(
        image_begin_barrier.dstStageMask, vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
        vk::ImageLayout::eGeneral, vk::QueueFamilyIgnored);
    const std::array begin_barrier = {image_begin_barrier, accumulation_barrier};
    _commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

    _commandbuffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.get_pipeline());
    _commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline.get_pipeline_layout(), 0,
                                      *descriptor_sets.at(_frame_slot), nullptr);

    const uint32_t group_x = (width + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
    const uint32_t group_y = (height + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
    _commandbuffer.dispatch(group_x, group_y, 1);

    const auto image_end_barrier =
        render_output.transition_state(vk::PipelineStageFlagBits2::eComputeShader,
                                       vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
                                       vk::ImageLayout::eGeneral, vk::QueueFamilyIgnored);
    const std::array end_barrier = {image_end_barrier};
    _commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));

    frame_index++;
}

/**
 * @brief 建计算管线：0 号 TLAS、1 号输出、2 号 UBO、3 号 bindless 贴图、4 号线性累积图。
 */
vulkan_pipeline scene_rayquery_render::create_pipeline()
{
    vulkan_pipeline new_pipeline;

    constexpr std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eCompute, nullptr),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eCombinedImageSampler,
                                       vulkan_common::MAX_BINDLESS_TEXTURES, vk::ShaderStageFlagBits::eCompute, nullptr),
        vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr),
    };

    constexpr std::array shader_stages = {shader_stage_info{RAY_QUERY_ENTRY_NAME, vk::ShaderStageFlagBits::eCompute}};

    new_pipeline.create_from_shader(device, pipeline_cache, bindings, {},
                                    std::filesystem::path(SHADERS_PATH) / "scene_rayquery_render.slang", shader_stages);

    return new_pipeline;
}

/**
 * @brief 重建描述符池与每帧描述符集：TLAS 用 StructureChain 写，输出与累积图均声明 eGeneral 布局。
 * @note 贴图数组按紧凑绑定序写入 binding 3 的各数组元素，数量来自 texture_manager.get_descriptor_info()。
 */
void scene_rayquery_render::update_descriptor()
{
    recycle_bin.retire_after_submitted(std::move(descriptor_sets), "ray query descriptor sets.");
    recycle_bin.retire_after_submitted(std::move(descriptor_pool), "ray query descriptor pool.");

    constexpr std::array pool_size = {
        vk::DescriptorPoolSize(vk::DescriptorType::eAccelerationStructureKHR, vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 2 * vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler,
                               vulkan_common::MAX_BINDLESS_TEXTURES * vulkan_common::MAX_FRAMES_IN_FLIGHT)};

    const vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                                        vulkan_common::MAX_FRAMES_IN_FLIGHT, pool_size);
    descriptor_pool = vk::raii::DescriptorPool(device, pool_create_info);

    const std::vector<vk::DescriptorSetLayout> layouts(vulkan_common::MAX_FRAMES_IN_FLIGHT, pipeline.get_descriptor_set_layout());
    const auto alloc_info = vk::DescriptorSetAllocateInfo(*descriptor_pool, layouts);
    descriptor_sets       = device.allocateDescriptorSets(alloc_info);

    const auto descriptor_infos = texture_manager.get_descriptor_info();
    std::ranges::for_each(descriptor_sets | std::views::enumerate, [&](const auto& _frame_pair) {
        const auto& [frame_index_set, descriptor_set] = _frame_pair;

        std::vector<vk::WriteDescriptorSet> write_sets;

        const vk::DescriptorBufferInfo as_buffer_info(model_manager.get_tlas().get_buffer(), 0, vk::WholeSize);
        const vk::StructureChain<vk::WriteDescriptorSet, vk::WriteDescriptorSetAccelerationStructureKHR> as_write_set(
            vk::WriteDescriptorSet(descriptor_set, 0, {}, vk::DescriptorType::eAccelerationStructureKHR, {}, as_buffer_info),
            vk::WriteDescriptorSetAccelerationStructureKHR(*(model_manager.get_tlas().get_acceleration_structure())));
        write_sets.push_back(as_write_set.get());

        const vk::DescriptorImageInfo storage_image_info(nullptr, render_output.get_imageview(), vk::ImageLayout::eGeneral);
        write_sets.emplace_back(
            vk::WriteDescriptorSet(descriptor_set, 1, {}, vk::DescriptorType::eStorageImage, storage_image_info, {}));

        const vk::DescriptorImageInfo accumulation_info(nullptr, accumulation.get_imageview(), vk::ImageLayout::eGeneral);
        write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 4, 0, vk::DescriptorType::eStorageImage, accumulation_info, {}));

        const vk::DescriptorBufferInfo scene_uniform_info(ubo.get_buffer(), static_cast<vk::DeviceSize>(frame_index_set) * ubo_offset,
                                                          sizeof(vulkan_rt_scene_data));
        write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 2, 0, vk::DescriptorType::eUniformBuffer, {}, scene_uniform_info));

        write_sets.reserve(write_sets.size() + descriptor_infos.size());

        const auto material_sets =
            descriptor_infos | std::views::enumerate | std::views::transform([&descriptor_set](const auto& _pair) {
                const auto& [index, image_info] = _pair;
                return vk::WriteDescriptorSet(descriptor_set, 3, static_cast<uint32_t>(index),
                                              vk::DescriptorType::eCombinedImageSampler, image_info, {});
            });
        std::ranges::move(material_sets, std::back_inserter(write_sets));

        (*device).updateDescriptorSets(write_sets, {});
    });
}
