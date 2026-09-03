#include "scene_rasterization_render.h"

#include "scene_camera.h"
#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "tools/model_loader.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <array>
#include <cstring>
#include <iterator>
#include <ranges>
#include <utility>

namespace {
constexpr std::string_view VERT_ENTRY_NAME = "vertMain";
constexpr std::string_view FRAG_ENTRY_NAME = "fragMain";

struct uniform_buffer  // std140 layout
{
    alignas(16) glm::mat4x4 proj_matrix{};
    alignas(16) glm::mat4x4 view_matrix{};
    alignas(16) uint64_t models_address = 0;
    uint64_t materials_address          = 0;
    alignas(16) uint32_t model_count    = 0;
    uint32_t material_count             = 0;
    uint32_t texture_count              = 0;
};

}  // namespace

scene_rasterization_render::scene_rasterization_render(const vma::raii::Allocator&     _allocator,
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
                                                       vulkan_image&                   _render_output,
                                                       vk::Format                      _color_format)
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
    , color_format(_color_format)
{
    ubo_offset = vulkan_common::align_up(sizeof(uniform_buffer), physical_device.getProperties().limits.minUniformBufferOffsetAlignment);

    const std::array queue_array = {graphic_queue.get_index()};
    ubo.create(allocator, device,
               vk::BufferCreateInfo({}, ubo_offset * vulkan_common::MAX_FRAMES_IN_FLIGHT,
                                    vk::BufferUsageFlagBits::eUniformBuffer, vk::SharingMode::eExclusive, queue_array),
               vma::MemoryUsage::eCpuToGpu, "ray tracing ubo");

    create_pipeline(color_format);
}

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
    recycle_bin.retire(std::move(color_image), "rasterization old color image.");
    vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, render_output.get_format(), vk::Extent3D(width, height, 1),
                                         1, 1, vulkan_common::MSAA_SAMPLE_COUNT, vk::ImageTiling::eOptimal,
                                         vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, queue_array);
    vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, render_output.get_format(), {},
                                            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    color_image.create(allocator, device, color_image_info, color_view_info, vma::MemoryUsage::eGpuOnly,
                       vk::ClearColorValue(0.f, 0.f, 0.f, 1.f), "rasterization_color");

    // depth
    recycle_bin.retire(std::move(depth_image), "rasterization old depth image.");
    vk::ImageCreateInfo depth_image_info({}, vk::ImageType::e2D, vulkan_common::DEPTH_FORMAT,
                                         vk::Extent3D(width, height, 1), 1, 1, vulkan_common::MSAA_SAMPLE_COUNT,
                                         vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                         vk::SharingMode::eExclusive, queue_array);
    vk::ImageViewCreateInfo depth_view_info({}, {}, vk::ImageViewType::e2D, vulkan_common::DEPTH_FORMAT, {},
                                            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, {}, 1, 0, 1), nullptr);
    depth_image.create(allocator, device, depth_image_info, depth_view_info, vma::MemoryUsage::eGpuOnly,
                       vk::ClearDepthStencilValue(1.f, 0), "rasterization_depth");
}

void scene_rasterization_render::update()
{
    update_descriptor();
}

void scene_rasterization_render::render(const scene_camera& _camera, const vk::raii::CommandBuffer& _commandbuffer, uint32_t /*_skybox_index*/)
{
    // update ubo
    uniform_buffer ubo_data{
        .proj_matrix       = _camera.get_projection_matrix(),
        .view_matrix       = _camera.get_view_matrix(),
        .models_address    = model_manager.get_ssbo_buffer().get_buffer_address().deviceAddress,
        .materials_address = material_manager.get_ssbo_buffer().get_buffer_address().deviceAddress,
        .model_count       = static_cast<uint32_t>(model_manager.get_models_size()),
        .material_count    = static_cast<uint32_t>(material_manager.get_materials_size()),
        .texture_count     = static_cast<uint32_t>(material_manager.get_textures_size()),
    };

    auto* ubo_ptr = static_cast<uint8_t*>(ubo.get_buffer_address().hostAddress) + static_cast<size_t>(current_frame) * ubo_offset;
    //std::memset(ubo_ptr, 0, ubo_offset);
    std::memcpy(ubo_ptr, &ubo_data, sizeof(ubo_data));
    ubo.flush();


    // render
    const auto render_output_barrier =
        vk::ImageMemoryBarrier2(render_output.get_stage(), render_output.get_access(),
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                                render_output.get_layout(), vk::ImageLayout::eColorAttachmentOptimal,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, render_output.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    render_output.set_info(render_output_barrier);

    const auto color_barrier =
        vk::ImageMemoryBarrier2(color_image.get_stage(), color_image.get_access(),
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                                color_image.get_layout(), vk::ImageLayout::eColorAttachmentOptimal,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, color_image.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    color_image.set_info(color_barrier);

    const auto depth_barrier =
        vk::ImageMemoryBarrier2(depth_image.get_stage(), depth_image.get_access(), vk::PipelineStageFlagBits2::eAllGraphics,
                                vk::AccessFlagBits2::eDepthStencilAttachmentWrite, depth_image.get_layout(),
                                vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::QueueFamilyIgnored,
                                vk::QueueFamilyIgnored, depth_image.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1));
    depth_image.set_info(depth_barrier);

    const std::array begin_barriers = {render_output_barrier, color_barrier, depth_barrier};
    _commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barriers, nullptr));


    const vk::RenderingAttachmentInfo colorAttachmentInfo(color_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal,
                                                          vk::ResolveModeFlagBits::eAverage, render_output.get_imageview(),
                                                          vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eClear,
                                                          vk::AttachmentStoreOp::eStore, color_image.get_clear_value());
    const vk::RenderingAttachmentInfo depthAttachmentInfo(depth_image.get_imageview(), vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                                          vk::ResolveModeFlagBits::eNone, {}, vk::ImageLayout::eUndefined,
                                                          vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                                                          depth_image.get_clear_value());

    const vk::RenderingInfo renderingInfo({}, vk::Rect2D({0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}),
                                          1, {}, colorAttachmentInfo, &depthAttachmentInfo, nullptr, nullptr);

    _commandbuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f));
    _commandbuffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(width, height)));

    _commandbuffer.beginRendering(renderingInfo);


    _commandbuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
    _commandbuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline_layout(), 0,
                                      *(descriptor_sets.at(current_frame)), nullptr);

    if (model_manager.get_models_size() > 0) [[likely]]
    {
        _commandbuffer.bindVertexBuffers(0, *(model_manager.get_vertices_buffer().get_buffer()), vk::DeviceSize(0));
        _commandbuffer.bindIndexBuffer(*(model_manager.get_indices_buffer().get_buffer()), vk::DeviceSize(0), vk::IndexType::eUint32);
        _commandbuffer.drawIndexedIndirect(model_manager.get_draw_commands().get_buffer(), 0,
                                           static_cast<uint32_t>(model_manager.get_models_size()),
                                           sizeof(vk::DrawIndexedIndirectCommand));
    }

    _commandbuffer.endRendering();

    current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}

void scene_rasterization_render::recreate()
{
    recycle_bin.retire(std::move(pipeline), "old rasterization pipeline on reload.");
    create_pipeline(color_format);
}

void scene_rasterization_render::reset_accumulation() noexcept {}

void scene_rasterization_render::create_pipeline(vk::Format _color_format)
{
    constexpr std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1,
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, nullptr),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eCombinedImageSampler, 1024, vk::ShaderStageFlagBits::eFragment, nullptr)};

    constexpr auto binding = model_vertex::get_binding_description();
    constexpr auto attribute = model_vertex::get_attribute_descriptions<model_vertex_type::position, model_vertex_type::uv>();

    constexpr std::array shader_stages = {
        shader_stage_info{VERT_ENTRY_NAME, vk::ShaderStageFlagBits::eVertex},
        shader_stage_info{FRAG_ENTRY_NAME, vk::ShaderStageFlagBits::eFragment},
    };

    pipeline.create_from_shader(device, pipeline_cache, bindings, {}, std::span(&binding, 1), attribute,
                                std::filesystem::path(SHADERS_PATH) / "rasterization.slang", shader_stages,
                                vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack,
                                vk::FrontFace::eCounterClockwise, vulkan_common::MSAA_SAMPLE_COUNT, vk::True,
                                std::span(&_color_format, 1), vulkan_common::DEPTH_FORMAT);
}

void scene_rasterization_render::update_descriptor()
{
    // update descriptor pool
    recycle_bin.retire(std::move(descriptor_sets), "rasterization descriptor sets.");
    recycle_bin.retire(std::move(descriptor_pool), "rasterization descriptor pool.");

    constexpr std::array pool_size = {
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, vulkan_common::MAX_FRAMES_IN_FLIGHT),
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 1024 * vulkan_common::MAX_FRAMES_IN_FLIGHT)};

    vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet
                                                      | vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
                                                  vulkan_common::MAX_FRAMES_IN_FLIGHT, pool_size);
    descriptor_pool = vk::raii::DescriptorPool(device, pool_create_info);

    // descriptor set
    std::vector<vk::DescriptorSetLayout> layouts(vulkan_common::MAX_FRAMES_IN_FLIGHT, pipeline.get_descriptor_set_layout());
    auto alloc_info = vk::DescriptorSetAllocateInfo(descriptor_pool, layouts);
    descriptor_sets = device.allocateDescriptorSets(alloc_info);

    std::ranges::for_each(descriptor_sets | std::views::enumerate, [&](const auto& _pair) {
        const auto& [index, descriptor_set] = _pair;
        std::vector<vk::WriteDescriptorSet> write_sets;

        const vk::DescriptorBufferInfo scene_uniform_info(ubo.get_buffer(), ubo_offset * index, sizeof(uniform_buffer));
        write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 0, 0, vk::DescriptorType::eUniformBuffer, {}, scene_uniform_info));

        const auto material_sets =
            material_manager.get_descriptor_info() | std::views::enumerate
            | std::views::transform([&descriptor_set](const auto& _pair) {
                  const auto& [index, image_info] = _pair;
                  return vk::WriteDescriptorSet(descriptor_set, 1, static_cast<uint32_t>(index),
                                                vk::DescriptorType::eCombinedImageSampler, image_info, {});
              });
        std::ranges::move(material_sets, std::back_inserter(write_sets));

        device.updateDescriptorSets(write_sets, {});
    });
}
