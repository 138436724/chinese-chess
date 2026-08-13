#include "scene_rasterization_render.h"

#include "scene_light_manager.h"
#include "scene_material_manager.h"
#include "scene_model_manager.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_commandbuffer.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

scene_rasterization_render::scene_rasterization_render(const vma::raii::Allocator&     _allocator,
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
                                                       vulkan_image&                   _render_output,
                                                       vk::Format                      _color_format)
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
    , render_output(_render_output)
    , color_format(_color_format)
{
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

    // Push constant with camera
    const scene_rasterization_render::push_constant pc{
        _camera.get_projection_matrix(),
        _camera.get_view_matrix(),
    };
    _commandbuffer.pushConstants2(vk::PushConstantsInfo(pipeline.get_pipeline_layout(), vk::ShaderStageFlagBits::eVertex,
                                                        0, sizeof(scene_rasterization_render::push_constant), &pc));

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
    std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex, nullptr),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eCombinedImageSampler, 1024, vk::ShaderStageFlagBits::eFragment, nullptr)};

    const vk::PushConstantRange push_constant(vk::ShaderStageFlagBits::eVertex, 0, sizeof(scene_rasterization_render::push_constant));

    const auto binding = model_vertex::get_binding_description();
    const auto attribute = model_vertex::get_attribute_descriptions<model_vertex_type::position, model_vertex_type::uv>();

    const auto spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::filesystem::path(SHADERS_PATH) / "rasterization.slang",
                                                                  {VERT_ENTRY_NAME, FRAG_ENTRY_NAME});
    if (!spirv_code)
    {
        throw std::runtime_error(spirv_code.error());
    }

    const vk::raii::ShaderModule shaderModule(
        device, vk::ShaderModuleCreateInfo({}, spirv_code->size() * sizeof(char),
                                           reinterpret_cast<const uint32_t*>(spirv_code->data())));
    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, VERT_ENTRY_NAME.data()),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, FRAG_ENTRY_NAME.data()),
    };

    pipeline.create(device, bindings, std::span(&push_constant, 1), std::span(&binding, 1), attribute, shader_stages,
                    vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack,
                    vk::FrontFace::eCounterClockwise, vulkan_common::MSAA_SAMPLE_COUNT, vk::True,
                    std::span(&_color_format, 1), vulkan_common::DEPTH_FORMAT);
}

void scene_rasterization_render::update_descriptor()
{
    // update descriptor pool
    recycle_bin.retire(std::move(descriptor_sets), "rasterization descriptor sets.");
    recycle_bin.retire(std::move(descriptor_pool), "rasterization descriptor pool.");

    std::array pool_size = {vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, vulkan_common::MAX_FRAMES_IN_FLIGHT),
                            vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, vulkan_common::MAX_FRAMES_IN_FLIGHT),
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

        const vk::DescriptorBufferInfo model_buffer_info(model_manager.get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
        write_sets.emplace_back(vk::WriteDescriptorSet(descriptor_set, 0, {}, vk::DescriptorType::eStorageBuffer, {}, model_buffer_info));

        const vk::DescriptorBufferInfo material_buffer_info(material_manager.get_ssbo_buffer().get_buffer(), 0, vk::WholeSize);
        write_sets.emplace_back(
            vk::WriteDescriptorSet(descriptor_set, 1, {}, vk::DescriptorType::eStorageBuffer, {}, material_buffer_info));

        const auto material_sets =
            material_manager.get_descriptor_info() | std::views::enumerate
            | std::views::transform([&descriptor_set](const auto& _pair) {
                  const auto& [index, image_info] = _pair;
                  return vk::WriteDescriptorSet(descriptor_set, 2, static_cast<uint32_t>(index),
                                                vk::DescriptorType::eCombinedImageSampler, image_info, {});
              });
        std::ranges::move(material_sets, std::back_inserter(write_sets));

        device.updateDescriptorSets(write_sets, {});
    });
}
