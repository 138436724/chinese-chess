#include "vulkan_application.h"

#include "tools/image_helper.h"
#include "tools/ocio_helper.h"
#include "tools/shader_compiler.h"
#include "tools/string_helper.h"
#include "vulkan_common.h"

#include <algorithm>
#include <bitset>
#include <format>
#include <print>
#include <ranges>
#include <unordered_set>
#include <vulkan/utility/vk_format_utils.h>

void vulkan_application::init(const std::vector<const char*>& _instance_layers,
                              const std::vector<const char*>& _instance_extensions,
                              vk::InstanceCreateFlags         _flags)
{
    create_instance(_instance_layers, _instance_extensions, _flags);
}

void vulkan_application::create(vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height)
{
    auto present_index = create_physical_device_and_device(_surface);
    pick_msaa_sample_count();
    pick_depth_format();

    allocator = vma::raii::Allocator(instance, *device,
                                     vma::AllocatorCreateInfo(vma::AllocatorCreateFlagBits::eBufferDeviceAddress, *physical_device,
                                                              {}, {}, {}, {}, {}, {}, {}, vk::ApiVersion14));

    graphic_queue.create(*device, physical_device.get_queue_index(vk::QueueFlagBits::eGraphics));
    transfer_queue.create(*device, physical_device.get_queue_index(vk::QueueFlagBits::eTransfer));

    semaphore.create(*device);
    recycle_bin.create(&semaphore);

    commandbuffers = vulkan_commandbuffer::create(*device,
                                                  vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                vk::CommandBufferLevel::ePrimary,
                                                                                vulkan_common::MAX_FRAMES_IN_FLIGHT),
                                                  &graphic_queue, &semaphore);

    swapchain.create(instance, *physical_device, *device, _surface, present_index, _width, _height);
    create_pipeline();

    // create sampler
    vk::PhysicalDeviceProperties properties = (*physical_device).getProperties();
    vk::SamplerCreateInfo sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
                                       vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder,
                                       vk::SamplerAddressMode::eClampToBorder, 0.f, vk::True,
                                       properties.limits.maxSamplerAnisotropy, vk::False, vk::CompareOp::eAlways, 0.f,
                                       1.f, vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
    image_sampler = vk::raii::Sampler(*device, sampler_info);
}

void vulkan_application::resize(uint32_t _width, uint32_t _height)
{
    swapchain.recreate(*physical_device, *device, _width, _height);
}

void vulkan_application::begin() noexcept
{
    recycle_bin.release();
    commandbuffers.at(current_frame).begin_record({});
}

void vulkan_application::render(std::vector<vk::SemaphoreSubmitInfo>&& _waited_info)
{
    try
    {
        swapchain.acquire_next_image();
    }
    catch (vk::OutOfDateKHRError e)
    {
#ifndef NDEBUG
        std::println("{}", e.what());
#endif  // !NDEBUG
        return;
    }
    catch (std::system_error)
    {
        throw std::runtime_error("failed to present swap chain image!");
    }


    vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
    commandbuffer.add_waited_info(std::move(_waited_info));


    auto scene_barrier =
        vk::ImageMemoryBarrier2(bind_scene_image->get_stage(), bind_scene_image->get_access(),
                                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                                bind_scene_image->get_layout(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, bind_scene_image->get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    bind_scene_image->set_info(scene_barrier);

    auto ui_barrier = vk::ImageMemoryBarrier2(bind_ui_image->get_stage(), bind_ui_image->get_access(),
                                              vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                                              bind_ui_image->get_layout(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                              vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, bind_ui_image->get_image(),
                                              vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    bind_ui_image->set_info(ui_barrier);

    auto swapchain_barrier =
        vk::ImageMemoryBarrier2(vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                                vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, swapchain.get_current_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

    const std::array begin_barriers = {scene_barrier, ui_barrier, swapchain_barrier};
    (*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barriers, nullptr));


    auto [width, height] = swapchain.get_extent();
    vk::RenderingAttachmentInfo swapchain_attachment_info(swapchain.get_current_imageview(), vk::ImageLayout::eColorAttachmentOptimal,
                                                          vk::ResolveModeFlagBits::eNone, nullptr, vk::ImageLayout::eUndefined,
                                                          vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                                                          vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));
    vk::RenderingInfo swapchain_rendering_info({}, vk::Rect2D({0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}),
                                               1, {}, swapchain_attachment_info, nullptr, nullptr, nullptr);

    (*commandbuffer).setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f));
    (*commandbuffer).setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(width, height)));


    (*commandbuffer).beginRendering(swapchain_rendering_info);


    (*commandbuffer).bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
    (*commandbuffer)
        .bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline_layout(), 0,
                            *(descriptor.get_descriptor_sets().at(current_frame)), nullptr);
    (*commandbuffer).draw(3, 1, 0, 0);


    (*commandbuffer).endRendering();


    const std::array end_barrier = {vk::ImageMemoryBarrier2(
        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone, vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::ePresentSrcKHR, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, swapchain.get_current_image(),
        vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1))};
    (*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));


    commandbuffer.end_record();
}

void vulkan_application::end(bool _immediately)
{
    swapchain.present_image(commandbuffers.at(current_frame), _immediately);
    current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}

void vulkan_application::wait() const
{
    (*device).waitIdle();
}

void vulkan_application::bind_image(vulkan_image* _scene_image, vulkan_image* _ui_image)
{
    bind_scene_image = _scene_image;
    bind_ui_image    = _ui_image;


    // descriptor
    descriptor.clear_descriptor_info();

    auto scene_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
                           | std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo {
                                 return vk::DescriptorImageInfo(nullptr, bind_scene_image->get_imageview(),
                                                                vk::ImageLayout::eShaderReadOnlyOptimal);
                             })
                           | std::ranges::to<std::vector>();
    descriptor.add_descriptor_info(vk::DescriptorType::eSampledImage, std::move(scene_pool_info));

    auto ui_pool_info =
        std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
        | std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo {
              return vk::DescriptorImageInfo(nullptr, bind_ui_image->get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
          })
        | std::ranges::to<std::vector>();
    descriptor.add_descriptor_info(vk::DescriptorType::eSampledImage, std::move(ui_pool_info));

    auto sampler_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
                             | std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo {
                                   return vk::DescriptorImageInfo(image_sampler, nullptr, vk::ImageLayout::eUndefined);
                               })
                             | std::ranges::to<std::vector>();
    descriptor.add_descriptor_info(vk::DescriptorType::eSampler, std::move(sampler_pool_info));

    std::ranges::for_each(std::views::zip(ocio_images, ocio_samplers), [&](const auto& _binding) {
        const auto& [ocio_image, ocio_sampler] = _binding;

        auto image_pool_info =
            std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
            | std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo {
                  return vk::DescriptorImageInfo(nullptr, ocio_image.get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
              })
            | std::ranges::to<std::vector>();
        descriptor.add_descriptor_info(vk::DescriptorType::eSampledImage, std::move(image_pool_info));


        auto ocio_sampler_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
                                      | std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo {
                                            return vk::DescriptorImageInfo(ocio_sampler, nullptr, vk::ImageLayout::eUndefined);
                                        })
                                      | std::ranges::to<std::vector>();
        descriptor.add_descriptor_info(vk::DescriptorType::eSampler, std::move(ocio_sampler_pool_info));
    });

    descriptor.update_descriptor_sets(*device, pipeline.get_descriptor_set_layout());
}

// todo 保存到本地是否使用传输队列
void vulkan_application::save_image(vulkan_image& _image)
{
    //VkFormat image_format = static_cast<VkFormat>(_image.get_format());

    //vulkan_buffer save_buffer;
    //save_buffer.create(allocator, *device, static_cast<size_t>(_image.get_extent().width) * _image.get_extent().height * vkuFormatTexelBlockSize(image_format), vk::BufferUsageFlagBits::eTransferDst, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    //auto old_layout = _image.get_layout();
    //auto graphics_family = graphic_queue.get_index();
    //auto transfer_family = transfer_queue.get_index();
    //bool same_queue = (graphics_family == transfer_family);
    //auto subresource = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    //// Step 1: Release ownership from graphics queue to transfer queue
    //if (!same_queue)
    //{
    //	vulkan_commandbuffer release_cb = std::move(vulkan_commandbuffer::create(*device, vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), &graphic_queue, &semaphore).front());
    //	release_cb.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    //	std::vector<vk::ImageMemoryBarrier2> release_barrier = {
    //		vulkan_image::release_ownership(_image.get_image(), old_layout, old_layout, subresource, graphics_family, transfer_family)
    //	};
    //	(*release_cb).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, release_barrier));

    //	release_cb.end_record();
    //	release_cb.submit(true);
    //}

    //// Step 2: Transfer queue acquires ownership, copies, then releases back to graphics
    //vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(*device, vk::CommandBufferAllocateInfo(transfer_queue.get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), &transfer_queue, &semaphore).front());
    //commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    //// Acquire ownership + transition to TransferSrcOptimal
    //std::vector<vk::ImageMemoryBarrier2> blit_begin_barrier = {
    //	vulkan_image::acquire_ownership(_image.get_image(), old_layout, vk::ImageLayout::eTransferSrcOptimal, subresource, graphics_family, transfer_family)
    //};
    //(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, blit_begin_barrier));

    //vulkan_image::copy_image_to_buffer(*commandbuffer, _image.get_image(), save_buffer.get_buffer(), vk::ImageLayout::eTransferSrcOptimal, vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), vk::Offset3D(0, 0, 0), vk::Extent3D(_image.get_extent().width, _image.get_extent().height, 1)));

    //// Release ownership back to graphics + restore original layout
    //std::vector<vk::ImageMemoryBarrier2> blit_end_barrier = {
    //	vulkan_image::release_ownership(_image.get_image(), vk::ImageLayout::eTransferSrcOptimal, old_layout, subresource, transfer_family, graphics_family)
    //};
    //(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, blit_end_barrier));

    //commandbuffer.end_record();
    //commandbuffer.submit(true);

    //// Restore internal layout tracking (image is now back in old_layout, owned by graphics)
    //_image.set_layout(old_layout, subresource);

    //auto now = std::chrono::system_clock::now();
    //auto now_second = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(now));

    //std::u8string save_path = std::u8string(CAPTURES_PATH);
    //save_path += STRING_HELPER::convert_to<std::u8string, std::string>(std::format("{:%Y_%m_%d_%H_%M_%S}", now_second));

    //if (vkuFormatIsSFLOAT(image_format) && vkuFormatIs16bit(image_format))
    //{
    //	save_path += u8".exr";
    //	IMAGE_HELPER.write_image<half>(save_path, _image.get_extent().width, _image.get_extent().height, std::span<half>(static_cast<half*>(save_buffer.get_buffer_address().hostAddress), _image.get_extent().width * _image.get_extent().height * vkuFormatComponentCount(image_format)));
    //}
    //else if (vkuFormatIs8bit(image_format) && vkuFormatIsUINT(image_format))
    //{
    //	save_path += u8".png";
    //	IMAGE_HELPER.write_image<uint8_t>(save_path, _image.get_extent().width, _image.get_extent().height, { static_cast<uint8_t*>(save_buffer.get_buffer_address().hostAddress), _image.get_extent().width * _image.get_extent().height * vkuFormatComponentCount(image_format) });
    //}
    //else
    //{
    //	throw std::runtime_error("Not Support Format!");
    //}
}

const vk::raii::Instance& vulkan_application::get_instance() const noexcept
{
    return instance;
}

const vulkan_physical_device& vulkan_application::get_physical_device() const noexcept
{
    return physical_device;
}

const vulkan_device& vulkan_application::get_device() const noexcept
{
    return device;
}

const vma::raii::Allocator& vulkan_application::get_allocator() const noexcept
{
    return allocator;
}

const vulkan_swapchain& vulkan_application::get_swapchain() const noexcept
{
    return swapchain;
}

vulkan_semaphore* vulkan_application::get_semaphore_ptr() noexcept
{
    return &semaphore;
}

vulkan_recycle_bin& vulkan_application::get_recycle_bin() noexcept
{
    return recycle_bin;
}

void vulkan_application::create_instance(const std::vector<const char*>& _instance_layers,
                                         const std::vector<const char*>& _instance_extensions,
                                         vk::InstanceCreateFlags         _flags)
{
    required_instance_layers     = _instance_layers;
    required_instance_extensions = _instance_extensions;

    constexpr vk::ApplicationInfo app_info("Hello World", VK_MAKE_VERSION(1, 0, 0), "Little Engine",
                                           VK_MAKE_VERSION(1, 0, 0), vk::ApiVersion14, nullptr);

    auto layer_properties = context.enumerateInstanceLayerProperties();
    for (const auto& required_layer : required_instance_layers)
    {
        if (std::ranges::none_of(layer_properties, [required_layer](const auto& layer_property) {
                return strcmp(layer_property.layerName, required_layer) == 0;
            }))
        {
            throw std::runtime_error("One or more required layers are not supported!");
        }
    }

#ifndef NDEBUG
    if (std::ranges::any_of(layer_properties, [](const auto& layer_property) {
            return strcmp(layer_property.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        }))
    {
        required_instance_layers.emplace_back("VK_LAYER_KHRONOS_validation");
    }
#endif  // NDEBUG

    auto extension_properties = context.enumerateInstanceExtensionProperties();
    for (const auto& required_extension : required_instance_extensions)
    {
        if (std::ranges::none_of(extension_properties, [required_extension](const auto& extension_property) {
                return strcmp(extension_property.extensionName, required_extension) == 0;
            }))
        {
            throw std::runtime_error(std::format("Required extension not supported: {}", required_extension));
        }
    }

#ifndef NDEBUG
    if (std::ranges::any_of(extension_properties, [](const auto& extension_property) {
            return strcmp(extension_property.extensionName, vk::EXTDebugUtilsExtensionName) == 0;
        }))
    {
        required_instance_extensions.push_back(vk::EXTDebugUtilsExtensionName);
    }
#endif  // NDEBUG

    vk::InstanceCreateInfo instance_create_info(_flags, &app_info, required_instance_layers, required_instance_extensions, nullptr);
    instance = vk::raii::Instance(context, instance_create_info);

#ifndef NDEBUG
    vk::DebugUtilsMessageSeverityFlagsEXT severity_flags(vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
                                                         | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
                                                         | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
    vk::DebugUtilsMessageTypeFlagsEXT     message_type_flags(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral
                                                             | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance
                                                             | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
    vk::DebugUtilsMessengerCreateInfoEXT debug_messenger_create_info({}, severity_flags, message_type_flags, &debug_callback);
    debug_messenger = instance.createDebugUtilsMessengerEXT(debug_messenger_create_info);
#endif  // NDEBUG
}

uint32_t vulkan_application::create_physical_device_and_device(vk::SurfaceKHR _surface)
{
    constexpr std::array required_device_extensions = {
        vk::KHRSwapchainExtensionName,           vk::KHRSpirv14ExtensionName,
        vk::KHRSynchronization2ExtensionName,    vk::KHRAccelerationStructureExtensionName,
        vk::KHRRayTracingPipelineExtensionName,  vk::KHRDeferredHostOperationsExtensionName,
        vk::KHRBufferDeviceAddressExtensionName, vk::KHRPushDescriptorExtensionName};

    auto has_all_required_features = [](const vk::raii::PhysicalDevice& _physical_device) {
        auto features =
            _physical_device.template getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan14Features, vk::PhysicalDeviceVulkan13Features,
                                                   vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
                                                   vk::PhysicalDeviceAccelerationStructureFeaturesKHR, vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>();

        return features.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy
               && features.get<vk::PhysicalDeviceFeatures2>().features.fillModeNonSolid
               && features.get<vk::PhysicalDeviceFeatures2>().features.multiDrawIndirect
               && features.get<vk::PhysicalDeviceFeatures2>().features.shaderInt64
               && features.get<vk::PhysicalDeviceVulkan14Features>().pushDescriptor
               && features.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering
               && features.get<vk::PhysicalDeviceVulkan13Features>().synchronization2
               && features.get<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress
               && features.get<vk::PhysicalDeviceVulkan12Features>().runtimeDescriptorArray
               && features.get<vk::PhysicalDeviceVulkan12Features>().scalarBlockLayout
               && features.get<vk::PhysicalDeviceVulkan12Features>().timelineSemaphore
               && features.get<vk::PhysicalDeviceVulkan11Features>().shaderDrawParameters
               && features.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState
               && features.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure
               && features.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructureCaptureReplay
               && features.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().descriptorBindingAccelerationStructureUpdateAfterBind
               && features.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline
               && features.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipelineTraceRaysIndirect
               && features.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTraversalPrimitiveCulling;
    };


    auto present_index = physical_device.create(instance, required_device_extensions, has_all_required_features, _surface);

    vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan14Features, vk::PhysicalDeviceVulkan13Features,
                       vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
                       vk::PhysicalDeviceAccelerationStructureFeaturesKHR, vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>
        feature_pnext_chain(vk::PhysicalDeviceFeatures2().setFeatures(vk::PhysicalDeviceFeatures()
                                                                          .setSamplerAnisotropy(vk::True)
                                                                          .setFillModeNonSolid(vk::True)
                                                                          .setMultiDrawIndirect(vk::True)
                                                                          .setShaderInt64(vk::True)),
                            vk::PhysicalDeviceVulkan14Features().setPushDescriptor(vk::True),
                            vk::PhysicalDeviceVulkan13Features().setDynamicRendering(vk::True).setSynchronization2(vk::True),
                            vk::PhysicalDeviceVulkan12Features()
                                .setBufferDeviceAddress(vk::True)
                                .setRuntimeDescriptorArray(vk::True)
                                .setScalarBlockLayout(vk::True)
                                .setTimelineSemaphore(vk::True),
                            vk::PhysicalDeviceVulkan11Features().setShaderDrawParameters(vk::True),
                            vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT().setExtendedDynamicState(vk::True),
                            vk::PhysicalDeviceAccelerationStructureFeaturesKHR()
                                .setAccelerationStructure(vk::True)
                                .setAccelerationStructureCaptureReplay(vk::True)
                                .setDescriptorBindingAccelerationStructureUpdateAfterBind(vk::True),
                            vk::PhysicalDeviceRayTracingPipelineFeaturesKHR()
                                .setRayTracingPipeline(vk::True)
                                .setRayTracingPipelineTraceRaysIndirect(vk::True)
                                .setRayTraversalPrimitiveCulling(vk::True));

    const std::array queues = {physical_device.get_queue_index(vk::QueueFlagBits::eGraphics),
                               physical_device.get_queue_index(vk::QueueFlagBits::eTransfer),
                               physical_device.get_queue_index(vk::QueueFlagBits::eCompute), present_index};
    device.create(*physical_device, queues, required_device_extensions, feature_pnext_chain.get<vk::PhysicalDeviceFeatures2>());

    return present_index;
}

void vulkan_application::create_pipeline()
{
    // pipeline
    std::vector bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
    };

    std::vector<char>        spirv_code;
    OCIO::GpuShaderDescRcPtr shader_desc = nullptr;

    if constexpr (vulkan_common::USE_OCIO)
    {
        shader_desc = OCIO_HELPER.generate_shader_info(std::u8string(OCIOS_PATH)
                                                       + u8"studio-config-all-views-v3.0.0_aces-v2.0_ocio-v2.4.ocio");
        spirv_code  = OCIO_HELPER.replace_and_compile(shader_desc, std::u8string(SHADERS_PATH) + u8"blend_image.slang",
                                                      {VERT_ENTYR_NAME, FRAG_ENTYR_NAME});
    }
    else
    {
        spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::u8string(SHADERS_PATH) + u8"blend_image.slang",
                                                           {VERT_ENTYR_NAME, FRAG_ENTYR_NAME});
    }

    if (spirv_code.empty())
    {
        throw std::runtime_error("compile .spv failed!");
    }

    if constexpr (vulkan_common::USE_OCIO)
    {
        vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
        if (shader_desc->getNumUniforms() > 0 && shader_desc->getUniformBufferSize() > 0)
        {
            std::vector<uint8_t> buffer = OCIO_HELPER.get_uniform_buffer_data(shader_desc);

            commandbuffer.add_waited_info({vulkan_common::upload_buffer(
                allocator, *device, recycle_bin, &semaphore, graphic_queue, transfer_queue, ocio_ubo,
                vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer,
                buffer)});
        }

        ocio_images.clear();
        ocio_samplers.clear();

        std::ranges::for_each(std::views::iota(0u, shader_desc->getNum3DTextures()), [&](const uint32_t i) {
            const char*         texture_name  = nullptr;
            const char*         sampler_name  = nullptr;
            uint32_t            edge_len      = 0;
            OCIO::Interpolation interpolation = OCIO::INTERP_LINEAR;
            shader_desc->get3DTexture(i, texture_name, sampler_name, edge_len, interpolation);
            if (!texture_name || !sampler_name || edge_len == 0)
            {
                throw std::runtime_error("Invalid 3D texture data.");
            }

            const float* values = nullptr;
            shader_desc->get3DTextureValues(i, values);
            if (!values)
            {
                throw std::runtime_error("Missing 3D texture values");
            }

            std::vector<float> rgba_values =
                std::views::iota(0u, edge_len * edge_len * edge_len) | std::views::transform([&](const auto& i) {
                    return std::vector<float>{values[i * 3 + 0], values[i * 3 + 1], values[i * 3 + 2], 1.f};
                })
                | std::views::join | std::ranges::to<std::vector>();

            vulkan_image image;
            commandbuffer.add_waited_info({vulkan_common::upload_image(
                allocator, *device, recycle_bin, &semaphore, graphic_queue, transfer_queue, vk::ImageType::e3D,
                vk::ImageViewType::e3D, vk::Format::eR32G32B32A32Sfloat, vk::Extent3D(edge_len, edge_len, edge_len), image,
                std::span(reinterpret_cast<uint8_t*>(rgba_values.data()), rgba_values.size() * sizeof(rgba_values.front())), {})});
            ocio_images.push_back(std::move(image));

            vk::SamplerCreateInfo sampler_info(
                {}, (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
                vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, 0.f, vk::False, 1.f,
                vk::False, vk::CompareOp::eAlways, 0.f, 0.f, vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
            ocio_samplers.emplace_back(vk::raii::Sampler(*device, sampler_info));
        });

        std::ranges::for_each(std::views::iota(0u, shader_desc->getNumTextures()), [&](const uint32_t i) {
            const char*                            texture_name  = nullptr;
            const char*                            sampler_name  = nullptr;
            uint32_t                               width         = 0;
            uint32_t                               height        = 0;
            OCIO::GpuShaderDesc::TextureType       channel       = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
            OCIO::GpuShaderDesc::TextureDimensions dimensions    = OCIO::GpuShaderDesc::TEXTURE_1D;
            OCIO::Interpolation                    interpolation = OCIO::INTERP_LINEAR;
            shader_desc->getTexture(i, texture_name, sampler_name, width, height, channel, dimensions, interpolation);

            if (!texture_name || !sampler_name || width == 0)
            {
                throw std::runtime_error("Invalid texture data.");
            }

            const float* values = nullptr;
            shader_desc->getTextureValues(i, values);
            if (!values)
            {
                throw std::runtime_error("Missing texture values");
            }

            height                            = height > 0 ? height : 1;
            vk::ImageType     image_type      = (height == 1) ? vk::ImageType::e1D : vk::ImageType::e2D;
            vk::ImageViewType image_view_type = (height == 1) ? vk::ImageViewType::e1D : vk::ImageViewType::e2D;
            vk::Format format = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? vk::Format::eR32Sfloat :
                                                                                        vk::Format::eR32G32B32A32Sfloat;

            std::vector<float> rgba_values;
            if (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL)
            {
                rgba_values = std::vector<float>(values, values + width * height);
            }
            else
            {
                rgba_values = std::views::iota(0u, width * height) | std::views::transform([&](const auto& i) {
                                  return std::vector<float>{values[i * 3 + 0], values[i * 3 + 1], values[i * 3 + 2], 1.f};
                              })
                              | std::views::join | std::ranges::to<std::vector>();
            }

            vulkan_image image;
            commandbuffer.add_waited_info({vulkan_common::upload_image(
                allocator, *device, recycle_bin, &semaphore, graphic_queue, transfer_queue, image_type, image_view_type,
                format, vk::Extent3D(width, height, 1), image,
                std::span(reinterpret_cast<uint8_t*>(rgba_values.data()), rgba_values.size() * sizeof(rgba_values.front())), {})});
            ocio_images.push_back(std::move(image));

            vk::SamplerCreateInfo sampler_info(
                {}, (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
                vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, 0.f, vk::False, 1.f,
                vk::False, vk::CompareOp::eAlways, 0.f, 0.f, vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
            ocio_samplers.push_back(std::move(vk::raii::Sampler(*device, sampler_info)));
        });

        if (ocio_images.size() != ocio_samplers.size())
        {
            throw std::runtime_error("images and samplers number not equal.");
        }

        std::ranges::for_each(std::views::iota(0u, static_cast<uint32_t>(ocio_images.size())), [&bindings](const auto&) {
            bindings.emplace_back(vk::DescriptorSetLayoutBinding(static_cast<uint32_t>(bindings.size()), vk::DescriptorType::eSampledImage,
                                                                 1, vk::ShaderStageFlagBits::eFragment, nullptr));
            bindings.emplace_back(vk::DescriptorSetLayoutBinding(static_cast<uint32_t>(bindings.size()), vk::DescriptorType::eSampler,
                                                                 1, vk::ShaderStageFlagBits::eFragment, nullptr));
        });
    }

    vk::raii::ShaderModule shaderModule(*device, vk::ShaderModuleCreateInfo({}, spirv_code.size() * sizeof(char),
                                                                            reinterpret_cast<const uint32_t*>(spirv_code.data())));
    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, VERT_ENTYR_NAME.data()),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, FRAG_ENTYR_NAME.data()),
    };

    std::array color_format_array = {swapchain.get_format()};
    pipeline.create(*device, bindings, {}, {}, {}, shader_stages, vk::PrimitiveTopology::eTriangleList,
                    vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise,
                    vk::SampleCountFlagBits::e1, vk::False, color_format_array, vk::Format::eUndefined);
}

void vulkan_application::pick_msaa_sample_count() const noexcept
{
    auto                 physical_device_properties = (*physical_device).getProperties();
    vk::SampleCountFlags counts                     = physical_device_properties.limits.framebufferColorSampleCounts
                                                      & physical_device_properties.limits.framebufferDepthSampleCounts;

    constexpr std::array sample_count_flags = {vk::SampleCountFlagBits::e64, vk::SampleCountFlagBits::e32,
                                               vk::SampleCountFlagBits::e16, vk::SampleCountFlagBits::e8,
                                               vk::SampleCountFlagBits::e4,  vk::SampleCountFlagBits::e2};

    auto support_sample_count = std::ranges::find_if(sample_count_flags, [&counts](vk::SampleCountFlagBits sample) {
        return static_cast<bool>(counts & sample);
    });

    vulkan_common::MSAA_SAMPLE_COUNT =
        (support_sample_count != sample_count_flags.end()) ? *support_sample_count : vk::SampleCountFlagBits::e1;
}

void vulkan_application::pick_depth_format() const noexcept
{
    vulkan_common::DEPTH_FORMAT =
        vulkan_common::find_supported_format(*physical_device,
                                             {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
                                             vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment)
            .value();
}

#ifndef NDEBUG
VKAPI_ATTR vk::Bool32 VKAPI_CALL vulkan_application::debug_callback(vk::DebugUtilsMessageSeverityFlagBitsEXT _severity,
                                                                    vk::DebugUtilsMessageTypeFlagsEXT        _type,
                                                                    const vk::DebugUtilsMessengerCallbackDataEXT* _callback_data,
                                                                    void*)
{
    if (_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError || _severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
    {
        std::println("{}: {} {} {}\n", vk::to_string(_type), _callback_data->messageIdNumber,
                     _callback_data->pMessageIdName, _callback_data->pMessage);
    }
    return vk::False;
}
#endif  // NDEBUG
