/**
 * @file scene_rasterization_render.cpp
 * @brief 光栅化前向 PBR 的实现：动态渲染录制、MSAA resolve、天空盒与间接绘制、管线与描述符创建。
 * @details 两条管线都经 vulkan_pipeline::create_from_shader 从 scene_rasterization_render.slang 编译；
 *          顶点输入由匿名命名空间的 vertex_binding_description / vertex_attribute_descriptions 定义，
 *          与 vulkan_model_vertex 的字段顺序一致。render() 把场景数据 memcpy 进 UBO 的 _frame_slot
 *          槽并 flush，随后录 barrier、beginRendering、天空盒、几何与 endRendering。
 */

#include "scene_rasterization_render.h"

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
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view VERT_ENTRY_NAME             = "vertMain";
constexpr std::string_view FRAG_ENTRY_NAME             = "fragMain";
constexpr std::string_view ENVIRONMENT_VERT_ENTRY_NAME = "environmentVertMain";
constexpr std::string_view ENVIRONMENT_FRAG_ENTRY_NAME = "environmentFragMain";

[[nodiscard]] consteval vk::VertexInputBindingDescription vertex_binding_description() noexcept
{
    return vk::VertexInputBindingDescription{0, sizeof(vulkan_model_vertex), vk::VertexInputRate::eVertex};
}

[[nodiscard]] consteval auto vertex_attribute_descriptions() noexcept
{
    return std::array{
        vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(vulkan_model_vertex, position)),
        vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat, offsetof(vulkan_model_vertex, normal)),
        vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32Sfloat, offsetof(vulkan_model_vertex, uv)),
    };
}
}  // namespace

/**
 * @brief 计算 UBO 槽跨度并建 UBO 与两条图形管线。
 * @note UBO 用 eHostAccessSequentialWrite 分配，render 里按 _frame_slot 偏移写入；构造不建附件图。
 */
scene_rasterization_render::scene_rasterization_render(const vma::raii::Allocator&     _allocator,
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
                                                       vk::Format                      _color_format)
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
    , color_format(_color_format)
{
    ubo_offset = vulkan_common::align_up(sizeof(vulkan_raster_scene_data),
                                         physical_device.getProperties2().properties.limits.minUniformBufferOffsetAlignment);

    const std::array queue_array = {graphic_queue.get_index()};
    ubo.create(allocator, device,
               vk::BufferCreateInfo({}, ubo_offset * vulkan_common::MAX_FRAMES_IN_FLIGHT,
                                    vk::BufferUsageFlagBits::eUniformBuffer, vk::SharingMode::eExclusive, queue_array),
               vma::MemoryUsage::eAuto, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite, "ray tracing ubo");

    environment_pipeline = create_environment_pipeline(color_format);
    pipeline             = create_pipeline(color_format);
}

/**
 * @brief 重建 MSAA 颜色图与深度图：格式分别取 render_output 与 DEPTH_FORMAT，采样数 MSAA_SAMPLE_COUNT。
 * @note 旧图先进回收站；宽高不变时直接返回，附件内容每次都 clear / dontcare，无需保留。
 */
void scene_rasterization_render::resize(uint32_t _width, uint32_t _height)
{
    if (width == _width && height == _height)
    {
        return;
    }

    width                        = _width;
    height                       = _height;
    const std::array queue_array = {graphic_queue.get_index()};

    // msaa color
    recycle_bin.retire_after_submitted(std::move(color_image), "rasterization old color image.");
    const vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, render_output.get_format(),
                                               vk::Extent3D(width, height, 1), 1, 1, vulkan_common::MSAA_SAMPLE_COUNT,
                                               vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment,
                                               vk::SharingMode::eExclusive, queue_array);
    const vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, render_output.get_format(), {},
                                                  vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    color_image.create(allocator, device, color_image_info, color_view_info, vma::MemoryUsage::eAutoPreferDevice, {},
                       vk::ClearColorValue(0.f, 0.f, 0.f, 1.f), "rasterization_color");

    // depth
    recycle_bin.retire_after_submitted(std::move(depth_image), "rasterization old depth image.");
    const vk::ImageCreateInfo depth_image_info({}, vk::ImageType::e2D, vulkan_common::DEPTH_FORMAT,
                                               vk::Extent3D(width, height, 1), 1, 1, vulkan_common::MSAA_SAMPLE_COUNT,
                                               vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                               vk::SharingMode::eExclusive, queue_array);
    const vk::ImageViewCreateInfo depth_view_info({}, {}, vk::ImageViewType::e2D, vulkan_common::DEPTH_FORMAT, {},
                                                  vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, {}, 1, 0, 1), nullptr);
    depth_image.create(allocator, device, depth_image_info, depth_view_info, vma::MemoryUsage::eAutoPreferDevice, {},
                       vk::ClearDepthStencilValue(1.f, 0), "rasterization_depth");
}

/** @brief 只重建描述符集；光栅化的每帧数据都在 render() 里写 UBO，无需其它更新。 */
void scene_rasterization_render::update()
{
    update_descriptor();
}

/**
 * @brief 写 UBO 槽并录制整帧：三条 barrier、动态渲染、天空盒、几何、endRendering。
 * @note 天空盒先画且关闭深度测试 / 写入，几何随后覆盖；不透明几何按 eBack 剔除。
 * @note descriptor_sets 必须已由 update() 建好，且 _frame_slot 在范围内。
 */
void scene_rasterization_render::render(const scene_camera&            _camera,
                                        const vk::raii::CommandBuffer& _commandbuffer,
                                        const vulkan_environment_data& _environment, uint32_t _frame_slot)
{
    // update ubo
    const vulkan_raster_scene_data ubo_data{
        .proj_matrix           = _camera.get_projection_matrix(),
        .proj_inv_matrix       = _camera.get_inv_projection_matrix(),
        .view_matrix           = _camera.get_view_matrix(),
        .view_inv_matrix       = _camera.get_inv_view_matrix(),
        .lights                = light_manager.get_ssbo_buffer().get_buffer_address().deviceAddress,
        .materials             = material_manager.get_ssbo_buffer().get_buffer_address().deviceAddress,
        .models                = model_manager.get_ssbo_buffer().get_buffer_address().deviceAddress,
        .light_count           = static_cast<uint32_t>(light_manager.get_lights_size()),
        .texture_count         = static_cast<uint32_t>(texture_manager.get_textures_size()),
        .material_count        = static_cast<uint32_t>(material_manager.get_materials_size()),
        .model_count           = static_cast<uint32_t>(model_manager.get_models_size()),
        .environment_index     = _environment.environment_index,
        .view_position         = _camera.get_position(),
        .exposure              = _camera.get_exposure(),
        .environment_intensity = _environment.environment_intensity,
    };

    auto* ubo_ptr = static_cast<uint8_t*>(ubo.get_buffer_address().hostAddress) + static_cast<size_t>(_frame_slot) * ubo_offset;
    //std::memset(ubo_ptr, 0, ubo_offset);
    std::memcpy(ubo_ptr, &ubo_data, sizeof(ubo_data));
    ubo.flush();


    // render
    const auto render_output_barrier =
        render_output.transition_state(vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                                       vk::ImageLayout::eColorAttachmentOptimal, vk::QueueFamilyIgnored);

    const auto color_barrier = color_image.transition_state(vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                                            vk::AccessFlagBits2::eColorAttachmentWrite,
                                                            vk::ImageLayout::eColorAttachmentOptimal, vk::QueueFamilyIgnored);

    const auto depth_barrier =
        depth_image.transition_state(vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                                     vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::QueueFamilyIgnored,
                                     vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1));

    const std::array begin_barriers = {render_output_barrier, color_barrier, depth_barrier};
    _commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barriers, nullptr));


    const vk::RenderingAttachmentInfo color_attachment_info(
        color_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eAverage,
        render_output.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eStore, color_image.get_clear_value());
    const vk::RenderingAttachmentInfo depth_attachment_info(depth_image.get_imageview(), vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                                            vk::ResolveModeFlagBits::eNone, {}, vk::ImageLayout::eUndefined,
                                                            vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                                                            depth_image.get_clear_value());

    const vk::RenderingInfo rendering_info({}, vk::Rect2D({0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}),
                                           1, {}, color_attachment_info, &depth_attachment_info, nullptr, nullptr);

    _commandbuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f));
    _commandbuffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(width, height)));

    _commandbuffer.beginRendering(rendering_info);

    _commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, environment_pipeline.get_pipeline());
    _commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, environment_pipeline.get_pipeline_layout(), 0,
                                      *(descriptor_sets.at(_frame_slot)), nullptr);
    _commandbuffer.draw(3, 1, 0, 0);

    _commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
    _commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline_layout(), 0,
                                      *(descriptor_sets.at(_frame_slot)), nullptr);

    if (model_manager.get_models_size() > 0) [[likely]]
    {
        _commandbuffer.bindVertexBuffers(0, *(model_manager.get_vertices_buffer().get_buffer()), vk::DeviceSize(0));
        _commandbuffer.bindIndexBuffer(*(model_manager.get_indices_buffer().get_buffer()), vk::DeviceSize(0), vk::IndexType::eUint32);
        _commandbuffer.drawIndexedIndirect(model_manager.get_draw_commands().get_buffer(), 0,
                                           static_cast<uint32_t>(model_manager.get_models_size()),
                                           sizeof(vk::DrawIndexedIndirectCommand));
    }

    _commandbuffer.endRendering();
}

/**
 * @brief 两条新管线创建成功后依次回收旧管线并替换；编译/创建失败保留旧对象，回收登记不具备整组回滚。
 */
void scene_rasterization_render::recreate()
{
    vulkan_pipeline new_environment_pipeline = create_environment_pipeline(color_format);
    vulkan_pipeline new_pipeline             = create_pipeline(color_format);

    recycle_bin.retire_after_submitted(std::move(environment_pipeline), "old rasterization environment pipeline on reload.");
    recycle_bin.retire_after_submitted(std::move(pipeline), "old rasterization pipeline on reload.");

    environment_pipeline = std::move(new_environment_pipeline);
    pipeline             = std::move(new_pipeline);
}

/** @brief 光栅化没有累积缓冲，本函数不做任何事。 */
void scene_rasterization_render::reset_accumulation() noexcept {}

/**
 * @brief 建天空盒管线：同一套描述符布局（UBO + bindless 贴图），但无顶点输入、深度测试关闭。
 * @note 与几何管线的描述符布局相同，因此同一批描述符集可以先后绑定两条管线。
 */
vulkan_pipeline scene_rasterization_render::create_environment_pipeline(vk::Format _color_format)
{
    vulkan_pipeline new_pipeline;

    constexpr std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1,
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, nullptr),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eCombinedImageSampler, vulkan_common::MAX_BINDLESS_TEXTURES,
                                       vk::ShaderStageFlagBits::eFragment, nullptr)};

    constexpr std::array shader_stages = {
        shader_stage_info{ENVIRONMENT_VERT_ENTRY_NAME, vk::ShaderStageFlagBits::eVertex},
        shader_stage_info{ENVIRONMENT_FRAG_ENTRY_NAME, vk::ShaderStageFlagBits::eFragment},
    };

    new_pipeline.create_from_shader(device, pipeline_cache, bindings, {}, {}, {},
                                    std::filesystem::path(SHADERS_PATH) / "scene_rasterization_render.slang", shader_stages,
                                    vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone,
                                    vk::FrontFace::eCounterClockwise, vulkan_common::MSAA_SAMPLE_COUNT, vk::False,
                                    std::span(&_color_format, 1), vulkan_common::DEPTH_FORMAT);

    return new_pipeline;
}

/**
 * @brief 建几何管线：绑定 vertex_binding_description / vertex_attribute_descriptions，开启深度测试与写入。
 * @note 正面绕序 eCounterClockwise，eBack 剔除；MSAA 采样数与附件图一致。
 */
vulkan_pipeline scene_rasterization_render::create_pipeline(vk::Format _color_format)
{
    vulkan_pipeline new_pipeline;

    constexpr std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1,
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, nullptr),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eCombinedImageSampler, vulkan_common::MAX_BINDLESS_TEXTURES,
                                       vk::ShaderStageFlagBits::eFragment, nullptr)};

    constexpr auto binding   = vertex_binding_description();
    constexpr auto attribute = vertex_attribute_descriptions();

    constexpr std::array shader_stages = {
        shader_stage_info{VERT_ENTRY_NAME, vk::ShaderStageFlagBits::eVertex},
        shader_stage_info{FRAG_ENTRY_NAME, vk::ShaderStageFlagBits::eFragment},
    };

    new_pipeline.create_from_shader(device, pipeline_cache, bindings, {}, std::span(&binding, 1), attribute,
                                    std::filesystem::path(SHADERS_PATH) / "scene_rasterization_render.slang", shader_stages,
                                    vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack,
                                    vk::FrontFace::eCounterClockwise, vulkan_common::MSAA_SAMPLE_COUNT, vk::True,
                                    std::span(&_color_format, 1), vulkan_common::DEPTH_FORMAT);

    return new_pipeline;
}

/**
 * @brief 重建描述符池与 MAX_FRAMES_IN_FLIGHT 个描述符集：0 号 UBO 窗口 + 1 号 bindless 贴图数组。
 * @note 贴图数组按紧凑绑定序写，数量来自 texture_manager.get_descriptor_info()。
 */
void scene_rasterization_render::update_descriptor()
{
    // update descriptor pool
    recycle_bin.retire_after_submitted(std::move(descriptor_sets), "rasterization descriptor sets.");
    recycle_bin.retire_after_submitted(std::move(descriptor_pool), "rasterization descriptor pool.");

    constexpr std::array pool_size = {
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler,
                               vulkan_common::MAX_BINDLESS_TEXTURES * vulkan_common::MAX_FRAMES_IN_FLIGHT)};

    const vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                                        vulkan_common::MAX_FRAMES_IN_FLIGHT, pool_size);
    descriptor_pool = vk::raii::DescriptorPool(device, pool_create_info);

    // descriptor set
    const std::vector<vk::DescriptorSetLayout> layouts(vulkan_common::MAX_FRAMES_IN_FLIGHT, pipeline.get_descriptor_set_layout());
    const auto alloc_info = vk::DescriptorSetAllocateInfo(descriptor_pool, layouts);
    descriptor_sets       = device.allocateDescriptorSets(alloc_info);

    const auto descriptor_infos = texture_manager.get_descriptor_info();
    std::ranges::for_each(descriptor_sets | std::views::enumerate, [&](const auto& _pair) {
        const auto& [index, descriptor_set] = _pair;
        std::vector<vk::WriteDescriptorSet> write_sets;

        const vk::DescriptorBufferInfo scene_uniform_info(ubo.get_buffer(), ubo_offset * index, sizeof(vulkan_raster_scene_data));
        write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 0, 0, vk::DescriptorType::eUniformBuffer, {}, scene_uniform_info));

        write_sets.reserve(write_sets.size() + descriptor_infos.size());

        const auto material_sets =
            descriptor_infos | std::views::enumerate | std::views::transform([&descriptor_set](const auto& _pair) {
                const auto& [index, image_info] = _pair;
                return vk::WriteDescriptorSet(descriptor_set, 1, static_cast<uint32_t>(index),
                                              vk::DescriptorType::eCombinedImageSampler, image_info, {});
            });
        std::ranges::move(material_sets, std::back_inserter(write_sets));

        device.updateDescriptorSets(write_sets, {});
    });
}
