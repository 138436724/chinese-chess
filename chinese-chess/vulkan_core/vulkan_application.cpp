#include "vulkan_application.h"

#include "tools/ocio_helper.h"
#include "tools/shader_compiler.h"
#include "vulkan_common.h"

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <print>
#include <ranges>

namespace {

constexpr std::string_view VERT_ENTRY_NAME = "vertMain";
constexpr std::string_view FRAG_ENTRY_NAME = "fragMain";

#ifndef NDEBUG
VKAPI_ATTR vk::Bool32 VKAPI_CALL debug_callback(vk::DebugUtilsMessageSeverityFlagBitsEXT      _severity,
                                                vk::DebugUtilsMessageTypeFlagsEXT             _type,
                                                const vk::DebugUtilsMessengerCallbackDataEXT* _callback_data,
                                                void*)
{
    if (_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError || _severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
        || _severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo)
    {
        std::println("{}: {} {} {}\n", vk::to_string(_type), _callback_data->messageIdNumber,
                     _callback_data->pMessageIdName, _callback_data->pMessage);
    }
    return vk::False;
}
#endif  // !NDEBUG

}  // namespace

vulkan_application::~vulkan_application()
{
    wait_idle();
    descriptor.clear_descriptor_info();
}

void vulkan_application::init(const std::vector<const char*>& _instance_layers,
                              const std::vector<const char*>& _instance_extensions,
                              vk::InstanceCreateFlags         _flags)
{
    create_instance(_instance_layers, _instance_extensions, _flags);
}

void vulkan_application::create(vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height)
{
    const auto present_index = create_physical_device_and_device(_surface);
    pick_msaa_sample_count();
    pick_depth_format();

    allocator = vma::raii::Allocator(instance, *device,
                                     vma::AllocatorCreateInfo(
#ifndef NDEBUG
                                         vma::AllocatorCreateFlagBits::eBufferDeviceAddress | vma::AllocatorCreateFlagBits::eExtMemoryBudget,
#else
                                         vma::AllocatorCreateFlagBits::eBufferDeviceAddress,
#endif  // !NDEBUG
                                         *physical_device, {}, {}, {}, {}, {}, {}, {}, vk::ApiVersion14));

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

    pipeline_cache.create(*physical_device, *device);
    create_pipeline();

    // create sampler
    image_sampler.create(*physical_device, *device, sampler_type::screen);
}

void vulkan_application::resize(uint32_t _width, uint32_t _height)
{
    swapchain.recreate(*physical_device, *device, _width, _height);
}

void vulkan_application::render(const vk::SemaphoreSubmitInfo& _ui_waited_info, const vk::SemaphoreSubmitInfo& _scene_waited_info)
{
    recycle_bin.release();

    vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
    commandbuffer.begin_record({});

    if (!swapchain.acquire_image()) [[unlikely]]
    {
        commandbuffer.end_record();
        return;
    }

    commandbuffer.add_waited_info(_ui_waited_info);
    commandbuffer.add_waited_info(_scene_waited_info);
    commandbuffer.add_waited_info(swapchain.get_waited_info());
    commandbuffer.add_signal_info(swapchain.get_signal_info());

    const auto scene_barrier =
        vk::ImageMemoryBarrier2(bind_scene_image->get_stage(), bind_scene_image->get_access(),
                                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                                bind_scene_image->get_layout(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, bind_scene_image->get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    bind_scene_image->set_info(scene_barrier);

    const auto ui_barrier =
        vk::ImageMemoryBarrier2(bind_ui_image->get_stage(), bind_ui_image->get_access(),
                                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                                bind_ui_image->get_layout(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, bind_ui_image->get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    bind_ui_image->set_info(ui_barrier);

    const auto swapchain_barrier =
        vk::ImageMemoryBarrier2(vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                                vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, swapchain.get_current_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

    const std::array begin_barriers = {scene_barrier, ui_barrier, swapchain_barrier};
    (*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barriers, nullptr));


    const auto [width, height] = swapchain.get_extent();
    const vk::RenderingAttachmentInfo swapchain_attachment_info(
        swapchain.get_current_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eNone,
        nullptr, vk::ImageLayout::eUndefined, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
        vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));
    const vk::RenderingInfo swapchain_rendering_info(
        {}, vk::Rect2D({0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}), 1, {},
        swapchain_attachment_info, nullptr, nullptr, nullptr);

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

    commandbuffer.submit();

    swapchain.present_image();

    current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;
}

void vulkan_application::wait_frame() const
{
    commandbuffers.at(current_frame).wait();
}

void vulkan_application::wait_idle() const
{
    if (!device)
    {
        return;
    }

    (*device).waitIdle();
}

void vulkan_application::bind_image(vulkan_image& _scene_image, vulkan_image& _ui_image)
{
    bind_scene_image = &_scene_image;
    bind_ui_image    = &_ui_image;


    // descriptor
    descriptor.clear_descriptor_info();

    std::vector<DescriptorBufferOrImageInfo> scene_pool_info(
        vulkan_common::MAX_FRAMES_IN_FLIGHT,
        vk::DescriptorImageInfo(nullptr, bind_scene_image->get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal));
    descriptor.add_descriptor_info(vk::DescriptorType::eSampledImage, std::move(scene_pool_info));

    std::vector<DescriptorBufferOrImageInfo> ui_pool_info(vulkan_common::MAX_FRAMES_IN_FLIGHT,
                                                          vk::DescriptorImageInfo(nullptr, bind_ui_image->get_imageview(),
                                                                                  vk::ImageLayout::eShaderReadOnlyOptimal));
    descriptor.add_descriptor_info(vk::DescriptorType::eSampledImage, std::move(ui_pool_info));

    std::vector<DescriptorBufferOrImageInfo> sampler_pool_info(
        vulkan_common::MAX_FRAMES_IN_FLIGHT, vk::DescriptorImageInfo(*image_sampler, nullptr, vk::ImageLayout::eUndefined));
    descriptor.add_descriptor_info(vk::DescriptorType::eSampler, std::move(sampler_pool_info));

    std::ranges::for_each(std::views::zip(ocio_images, ocio_samplers), [&](const auto& _binding) {
        const auto& [ocio_image, ocio_sampler] = _binding;

        std::vector<DescriptorBufferOrImageInfo> image_pool_info(
            vulkan_common::MAX_FRAMES_IN_FLIGHT,
            vk::DescriptorImageInfo(nullptr, ocio_image.get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal));
        descriptor.add_descriptor_info(vk::DescriptorType::eSampledImage, std::move(image_pool_info));

        std::vector<DescriptorBufferOrImageInfo> ocio_sampler_pool_info(
            vulkan_common::MAX_FRAMES_IN_FLIGHT, vk::DescriptorImageInfo(ocio_sampler, nullptr, vk::ImageLayout::eUndefined));
        descriptor.add_descriptor_info(vk::DescriptorType::eSampler, std::move(ocio_sampler_pool_info));
    });

    if (ocio_ubo.get_buffer_address().deviceAddress)
    {
        std::vector<DescriptorBufferOrImageInfo> ubo_pool_info(vulkan_common::MAX_FRAMES_IN_FLIGHT,
                                                               vk::DescriptorBufferInfo(ocio_ubo.get_buffer(), 0, vk::WholeSize));
        descriptor.add_descriptor_info(vk::DescriptorType::eUniformBuffer, std::move(ubo_pool_info));
    }

    descriptor.update_descriptor_sets(*device, pipeline.get_descriptor_set_layout());
}

const vk::raii::Instance& vulkan_application::get_instance() const noexcept
{
    return instance;
}

const vma::raii::Allocator& vulkan_application::get_allocator() const noexcept
{
    return allocator;
}

const vulkan_physical_device& vulkan_application::get_physical_device() const noexcept
{
    return physical_device;
}

const vulkan_device& vulkan_application::get_device() const noexcept
{
    return device;
}

const vulkan_pipeline_cache& vulkan_application::get_pipeline_cache() const noexcept
{
    return pipeline_cache;
}

const vulkan_swapchain& vulkan_application::get_swapchain() const noexcept
{
    return swapchain;
}

void vulkan_application::create_instance(const std::vector<const char*>& _instance_layers,
                                         const std::vector<const char*>& _instance_extensions,
                                         vk::InstanceCreateFlags         _flags)
{
    required_instance_layers     = _instance_layers;
    required_instance_extensions = _instance_extensions;

    constexpr vk::ApplicationInfo app_info("Hello World", VK_MAKE_VERSION(1, 0, 0), "Little Engine",
                                           VK_MAKE_VERSION(1, 0, 0), vk::ApiVersion14, nullptr);

    const auto layer_properties = context.enumerateInstanceLayerProperties();
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
    if (std::ranges::any_of(layer_properties, [](const auto& layer_property) static {
            return strcmp(layer_property.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        }))
    {
        required_instance_layers.emplace_back("VK_LAYER_KHRONOS_validation");
    }
#endif  // !NDEBUG

    const auto extension_properties = context.enumerateInstanceExtensionProperties();
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
    if (std::ranges::any_of(extension_properties, [](const auto& extension_property) static {
            return strcmp(extension_property.extensionName, vk::EXTDebugUtilsExtensionName) == 0;
        }))
    {
        required_instance_extensions.push_back(vk::EXTDebugUtilsExtensionName);
    }
#endif  // !NDEBUG

    const vk::InstanceCreateInfo instance_create_info(_flags, &app_info, required_instance_layers,
                                                      required_instance_extensions, nullptr);
    instance = vk::raii::Instance(context, instance_create_info);

#ifndef NDEBUG
    constexpr vk::DebugUtilsMessageSeverityFlagsEXT severity_flags(vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
                                                                   | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
                                                                   | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
    constexpr vk::DebugUtilsMessageTypeFlagsEXT message_type_flags(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral
                                                                   | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance
                                                                   | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
    const vk::DebugUtilsMessengerCreateInfoEXT debug_messenger_create_info({}, severity_flags, message_type_flags, &debug_callback);
    debug_messenger = instance.createDebugUtilsMessengerEXT(debug_messenger_create_info);
#endif  // !NDEBUG
}

uint32_t vulkan_application::create_physical_device_and_device(vk::SurfaceKHR _surface)
{
    constexpr std::array required_device_extensions = {vk::KHRSwapchainExtensionName,
                                                       vk::KHRSynchronization2ExtensionName,
                                                       vk::KHRAccelerationStructureExtensionName,
                                                       vk::KHRRayTracingPipelineExtensionName,
                                                       vk::KHRDeferredHostOperationsExtensionName,
                                                       vk::KHRBufferDeviceAddressExtensionName,
                                                       vk::KHRRayQueryExtensionName};

    const auto has_all_required_features = [](const vk::raii::PhysicalDevice& _physical_device) static {
        auto features =
            _physical_device.template getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan12Features,
                                                   vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                                                   vk::PhysicalDeviceRayTracingPipelineFeaturesKHR, vk::PhysicalDeviceRayQueryFeaturesKHR>();

        return features.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy
               && features.get<vk::PhysicalDeviceFeatures2>().features.multiDrawIndirect
               && features.get<vk::PhysicalDeviceFeatures2>().features.shaderInt64
               && features.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering
               && features.get<vk::PhysicalDeviceVulkan13Features>().synchronization2
               && features.get<vk::PhysicalDeviceVulkan13Features>().shaderIntegerDotProduct
               && features.get<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress
               && features.get<vk::PhysicalDeviceVulkan12Features>().runtimeDescriptorArray
               && features.get<vk::PhysicalDeviceVulkan12Features>().scalarBlockLayout
               && features.get<vk::PhysicalDeviceVulkan12Features>().timelineSemaphore
               && features.get<vk::PhysicalDeviceVulkan11Features>().shaderDrawParameters
               && features.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure
               && features.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline
               && features.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTraversalPrimitiveCulling
               && features.get<vk::PhysicalDeviceRayQueryFeaturesKHR>().rayQuery;
    };


    const auto present_index = physical_device.create(instance, required_device_extensions, has_all_required_features, _surface);

    vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan11Features,
                       vk::PhysicalDeviceAccelerationStructureFeaturesKHR, vk::PhysicalDeviceRayTracingPipelineFeaturesKHR, vk::PhysicalDeviceRayQueryFeaturesKHR>
        feature_pnext_chain(
            vk::PhysicalDeviceFeatures2().setFeatures(
                vk::PhysicalDeviceFeatures().setSamplerAnisotropy(vk::True).setMultiDrawIndirect(vk::True).setShaderInt64(vk::True)),
            vk::PhysicalDeviceVulkan13Features().setDynamicRendering(vk::True).setSynchronization2(vk::True).setShaderIntegerDotProduct(
                vk::True),
            vk::PhysicalDeviceVulkan12Features()
                .setBufferDeviceAddress(vk::True)
                .setRuntimeDescriptorArray(vk::True)
                .setScalarBlockLayout(vk::True)
                .setTimelineSemaphore(vk::True),
            vk::PhysicalDeviceVulkan11Features().setShaderDrawParameters(vk::True),
            vk::PhysicalDeviceAccelerationStructureFeaturesKHR().setAccelerationStructure(vk::True),
            vk::PhysicalDeviceRayTracingPipelineFeaturesKHR().setRayTracingPipeline(vk::True).setRayTraversalPrimitiveCulling(vk::True),
            vk::PhysicalDeviceRayQueryFeaturesKHR().setRayQuery(vk::True));

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

    std::expected<std::vector<char>, std::string> spirv_code;
    OCIO::GpuShaderDescRcPtr                      shader_desc = nullptr;

    if constexpr (vulkan_common::USE_OCIO)
    {
        shader_desc = ocio_helper::generate_shader_info(std::string(OCIOS_PATH) + "studio-config-all-views-v3.0.0_aces-v2.0_ocio-v2.4.ocio");
        spirv_code = ocio_helper::replace_and_compile(shader_desc, std::filesystem::path(SHADERS_PATH) / "blend_image.slang",
                                                      {VERT_ENTRY_NAME, FRAG_ENTRY_NAME});
    }
    else
    {
        spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::filesystem::path(SHADERS_PATH) / "blend_image.slang",
                                                           {VERT_ENTRY_NAME, FRAG_ENTRY_NAME});
    }

    if (!spirv_code)
    {
        throw std::runtime_error(spirv_code.error());
    }

    if constexpr (vulkan_common::USE_OCIO)
    {
        vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
        if (shader_desc->getNumUniforms() > 0 && shader_desc->getUniformBufferSize() > 0)
        {
            const std::vector<uint8_t> buffer = ocio_helper::get_uniform_buffer_data(shader_desc);

            commandbuffer.add_waited_info(vulkan_common::upload_buffer(
                allocator, *device, recycle_bin, semaphore, graphic_queue, transfer_queue, ocio_ubo,
                vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer,
                buffer, "ocio_ubo"));

            bindings.emplace_back(vk::DescriptorSetLayoutBinding(static_cast<uint32_t>(bindings.size()), vk::DescriptorType::eUniformBuffer,
                                                                 1, vk::ShaderStageFlagBits::eFragment, nullptr));
        }

        ocio_images.clear();
        ocio_samplers.clear();

        size_t bindings_index = bindings.size();
        size_t texture_num    = shader_desc->getNumTextures();
        bindings.resize(bindings.size() + texture_num * 2);
        std::ranges::for_each(std::views::iota(0u, texture_num), [&](const uint32_t i) {
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

            height                                  = height > 0 ? height : 1;
            const vk::ImageType     image_type      = (height == 1) ? vk::ImageType::e1D : vk::ImageType::e2D;
            const vk::ImageViewType image_view_type = (height == 1) ? vk::ImageViewType::e1D : vk::ImageViewType::e2D;
            const vk::Format format = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? vk::Format::eR32Sfloat :
                                                                                              vk::Format::eR32G32B32A32Sfloat;

            std::vector<float> rgba_values;
            if (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL)
            {
                rgba_values = std::vector<float>(values, values + width * height);
            }
            else
            {
                const size_t texel_count = static_cast<size_t>(width) * height;
                rgba_values.resize(texel_count * 4);
                for (size_t j = 0; j < texel_count; ++j)
                {
                    rgba_values.at(j * 4 + 0) = values[j * 3 + 0];
                    rgba_values.at(j * 4 + 1) = values[j * 3 + 1];
                    rgba_values.at(j * 4 + 2) = values[j * 3 + 2];
                    rgba_values.at(j * 4 + 3) = 1.f;
                }
            }

            vulkan_image image;
            commandbuffer.add_waited_info(vulkan_common::upload_image(
                allocator, *device, recycle_bin, semaphore, graphic_queue, transfer_queue, image_type, image_view_type,
                format, vk::Extent3D(width, height, 1), image,
                std::span(reinterpret_cast<uint8_t*>(rgba_values.data()), rgba_values.size() * sizeof(rgba_values.front())),
                "ocio_lut"));
            ocio_images.push_back(std::move(image));

            const vk::SamplerCreateInfo sampler_info(
                {}, (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
                vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, 0.f, vk::False, 1.f,
                vk::False, vk::CompareOp::eAlways, 0.f, 0.f, vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
            ocio_samplers.emplace_back(vk::raii::Sampler(*device, sampler_info));

            const size_t binding_idx = static_cast<size_t>(shader_desc->getTextureShaderBindingIndex(i));
            if (binding_idx == 0)
            {
                throw std::runtime_error("OCIO texture shader binding index is 0 (expected >= 1)");
            }
            const size_t index = bindings_index + 2 * (binding_idx - 1);
            bindings.at(index) = vk::DescriptorSetLayoutBinding(static_cast<uint32_t>(index), vk::DescriptorType::eSampledImage,
                                                                1, vk::ShaderStageFlagBits::eFragment, nullptr);
            bindings.at(index + 1) = vk::DescriptorSetLayoutBinding(static_cast<uint32_t>(index + 1), vk::DescriptorType::eSampler,
                                                                    1, vk::ShaderStageFlagBits::eFragment, nullptr);
        });

        bindings_index = bindings.size();
        texture_num    = shader_desc->getNum3DTextures();
        bindings.resize(bindings.size() + texture_num * 2);
        std::ranges::for_each(std::views::iota(0u, texture_num), [&](const uint32_t i) {
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

            const size_t       texel_count = static_cast<size_t>(edge_len) * edge_len * edge_len;
            std::vector<float> rgba_values(texel_count * 4);
            for (size_t j = 0; j < texel_count; ++j)
            {
                rgba_values.at(j * 4 + 0) = values[j * 3 + 0];
                rgba_values.at(j * 4 + 1) = values[j * 3 + 1];
                rgba_values.at(j * 4 + 2) = values[j * 3 + 2];
                rgba_values.at(j * 4 + 3) = 1.f;
            }

            vulkan_image image;
            commandbuffer.add_waited_info(vulkan_common::upload_image(
                allocator, *device, recycle_bin, semaphore, graphic_queue, transfer_queue, vk::ImageType::e3D,
                vk::ImageViewType::e3D, vk::Format::eR32G32B32A32Sfloat, vk::Extent3D(edge_len, edge_len, edge_len), image,
                std::span(reinterpret_cast<uint8_t*>(rgba_values.data()), rgba_values.size() * sizeof(rgba_values.front())),
                "ocio_lut"));
            ocio_images.push_back(std::move(image));

            const vk::SamplerCreateInfo sampler_info(
                {}, (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge,
                vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, 0.f, vk::False, 1.f,
                vk::False, vk::CompareOp::eAlways, 0.f, 0.f, vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
            ocio_samplers.emplace_back(vk::raii::Sampler(*device, sampler_info));

            const size_t binding_idx = static_cast<size_t>(shader_desc->get3DTextureShaderBindingIndex(i));
            if (binding_idx == 0)
            {
                throw std::runtime_error("OCIO 3D texture shader binding index is 0 (expected >= 1)");
            }
            const size_t index = bindings_index + 2 * (binding_idx - 1);
            bindings.at(index) = vk::DescriptorSetLayoutBinding(static_cast<uint32_t>(index), vk::DescriptorType::eSampledImage,
                                                                1, vk::ShaderStageFlagBits::eFragment, nullptr);
            bindings.at(index + 1) = vk::DescriptorSetLayoutBinding(static_cast<uint32_t>(index + 1), vk::DescriptorType::eSampler,
                                                                    1, vk::ShaderStageFlagBits::eFragment, nullptr);
        });

        if (ocio_images.size() != ocio_samplers.size())
        {
            throw std::runtime_error("Number of images and samplers are not equal.");
        }
    }

    const vk::raii::ShaderModule shaderModule(
        *device, vk::ShaderModuleCreateInfo({}, spirv_code->size() * sizeof(char),
                                            reinterpret_cast<const uint32_t*>(spirv_code->data())));
    const std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, VERT_ENTRY_NAME.data()),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, FRAG_ENTRY_NAME.data()),
    };

    const std::array color_format_array = {swapchain.get_format()};
    pipeline.create(*device, *pipeline_cache, bindings, {}, {}, {}, shader_stages, vk::PrimitiveTopology::eTriangleList,
                    vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise,
                    vk::SampleCountFlagBits::e1, vk::False, color_format_array, vk::Format::eUndefined);
}

void vulkan_application::pick_msaa_sample_count() const noexcept
{
    const auto                 physical_device_properties = (*physical_device).getProperties();
    const vk::SampleCountFlags counts = physical_device_properties.limits.framebufferColorSampleCounts
                                        & physical_device_properties.limits.framebufferDepthSampleCounts;

    constexpr std::array sample_count_flags = {vk::SampleCountFlagBits::e64, vk::SampleCountFlagBits::e32,
                                               vk::SampleCountFlagBits::e16, vk::SampleCountFlagBits::e8,
                                               vk::SampleCountFlagBits::e4,  vk::SampleCountFlagBits::e2};

    const auto support_sample_count = std::ranges::find_if(sample_count_flags, [&counts](vk::SampleCountFlagBits sample) {
        return static_cast<bool>(counts & sample);
    });

    vulkan_common::MSAA_SAMPLE_COUNT =
        (support_sample_count != sample_count_flags.end()) ? *support_sample_count : vk::SampleCountFlagBits::e1;
}

void vulkan_application::pick_depth_format() const
{
    const auto depth_format = vulkan_common::find_supported_format(
        *physical_device, {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
        vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment);

    if (!depth_format)
    {
        throw std::runtime_error(depth_format.error());
    }

    vulkan_common::DEPTH_FORMAT = *depth_format;
}
