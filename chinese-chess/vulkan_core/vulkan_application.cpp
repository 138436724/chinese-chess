/**
 * @file vulkan_application.cpp
 * @brief vulkan_application 的实现：实例与设备创建、OCIO 合成管线构建、逐帧提交与呈现。
 * @details create_pipeline 用 OCIO 生成 LUT，逐张 upload_image 后按 shader binding index 填进
 *          descriptor set layout；render 记录一条主命令缓冲，等待 UI/scene token 后画全屏三角形。
 *          所有一次性上传都登记到 recycle_bin，提交与呈现交给 swapchain 的帧调度器。
 */
#include "vulkan_application.h"

#include "tools/ocio_helper.h"
#include "tools/shader_compiler.h"
#include "vulkan_common.h"
#include "vulkan_submission_scope.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view VERT_ENTRY_NAME = "vertMain";
constexpr std::string_view FRAG_ENTRY_NAME = "fragMain";

/**
 * @brief 生成 clamp-to-edge 的采样器参数，屏幕合成与 OCIO LUT 共用；max_lod 默认 0 即只用基础层。
 */
[[nodiscard]] sampler_create_info generate_sampler_info(vk::Filter _filter, float _max_lod = 0.f) noexcept
{
    return sampler_create_info{.mag_filter        = _filter,
                               .min_filter        = _filter,
                               .mipmap_mode       = vk::SamplerMipmapMode::eLinear,
                               .address_mode_u    = vk::SamplerAddressMode::eClampToEdge,
                               .address_mode_v    = vk::SamplerAddressMode::eClampToEdge,
                               .address_mode_w    = vk::SamplerAddressMode::eClampToEdge,
                               .enable_anisotropy = false,
                               .min_lod           = 0.f,
                               .max_lod           = _max_lod};
}

#ifndef NDEBUG
/**
 * @brief Debug 构建的校验层回调：错误额外打一行标记，其余按 severity 打印 message id 与内容。
 */
VKAPI_ATTR vk::Bool32 VKAPI_CALL debug_callback(vk::DebugUtilsMessageSeverityFlagBitsEXT      _severity,
                                                vk::DebugUtilsMessageTypeFlagsEXT             _type,
                                                const vk::DebugUtilsMessengerCallbackDataEXT* _callback_data,
                                                void*)
{
    if (_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError || _severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
        || _severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo)
    {
        if (_severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) std::fputs("VULKAN_VALIDATION_ERROR\n", stderr);
        std::println("{}: {} {} {}\n", vk::to_string(_type), _callback_data->messageIdNumber,
                     _callback_data->pMessageIdName, _callback_data->pMessage);
    }
    return vk::False;
}
#endif  // !NDEBUG

}  // namespace

/**
 * @brief 先等 GPU 空闲再清空描述符声明；swapchain 的 shutdown 在成员析构阶段执行。
 */
vulkan_application::~vulkan_application()
{
    wait_idle_for_cleanup();
    descriptor.clear_descriptor_info();
}

/**
 * @brief 只负责创建实例，设备与交换链留给 create。
 */
void vulkan_application::init(const std::vector<const char*>& _instance_layers,
                              const std::vector<const char*>& _instance_extensions,
                              vk::InstanceCreateFlags         _flags)
{
    create_instance(_instance_layers, _instance_extensions, _flags);
}

/**
 * @brief 建成套初始化：设备、分配器、队列、命令缓冲、交换链、管线缓存与屏幕合成管线。
 * @note surface 先由 raii 守卫接管，直到作为参数 release 给 swapchain；此前抛异常由守卫销毁。
 */
void vulkan_application::create(vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height)
{
    vk::raii::SurfaceKHR surface_guard(instance, _surface);
    const auto present_index = create_physical_device_and_logical_device(*surface_guard);
    pick_msaa_sample_count(physical_device);
    pick_depth_format(physical_device);

    allocator = vma::raii::Allocator(instance, *device,
                                     vma::AllocatorCreateInfo(vma::AllocatorCreateFlagBits::eBufferDeviceAddress, *physical_device,
                                                              {}, {}, {}, {}, {}, {}, {}, vk::ApiVersion14));

    graphic_queue.create(*device, physical_device.get_queue_index(vk::QueueFlagBits::eGraphics));

    const uint32_t transfer_index = physical_device.get_queue_index(vk::QueueFlagBits::eTransfer);
    if (transfer_index == graphic_queue.get_index())
    {
        // Same VkQueue: share completion timeline, submission sequence and mutex.
        transfer_queue.create(*device, transfer_index, graphic_queue.get_submission_state());
    }
    else
    {
        transfer_queue.create(*device, transfer_index);
    }

    recycle_bin.add_queue(graphic_queue);
    recycle_bin.add_queue(transfer_queue);

    commandbuffers = vulkan_commandbuffer::create(*device,
                                                  vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                vk::CommandBufferLevel::ePrimary,
                                                                                vulkan_common::MAX_FRAMES_IN_FLIGHT),
                                                  &graphic_queue);

    swapchain.create(instance, *physical_device, *device, surface_guard.release(), graphic_queue, transfer_queue, present_index, _width, _height);

    pipeline_cache.create(*physical_device, *device);
    create_pipeline();

    // create sampler
    image_sampler.create(*physical_device, *device, generate_sampler_info(vk::Filter::eLinear), "screen");
}

/**
 * @brief 直接转发给 swapchain.recreate；尺寸无效或交换链 OutOfDate 时返回 false。
 */
bool vulkan_application::resize(uint32_t _width, uint32_t _height)
{
    return swapchain.recreate(_width, _height);
}

/**
 * @brief 先释放回收站再 acquire，保证本帧能复用上一帧已完成提交的资源。
 */
vulkan_frame_result vulkan_application::begin_frame()
{
    recycle_bin.release();
    return swapchain.acquire_image();
}

/**
 * @brief 转发给 swapchain.abandon_frame：归还 image、替换信号量并复位帧调度器。
 */
void vulkan_application::cancel_frame() { swapchain.abandon_frame(); }

/**
 * @brief 记录并提交屏幕合成命令缓冲，然后 present。
 * @note 先校验帧上下文，再把 UI/scene token 与 acquire semaphore 作为依赖；进入 fragment 前
 *       两张离屏图统一转到 eShaderReadOnlyOptimal，swapchain image 从 eUndefined 转 color attachment。
 */
vulkan_frame_result vulkan_application::render(const vulkan_frame_context& frame, vulkan_submission_token _ui_token, vulkan_submission_token _scene_token)
{
    const auto active = swapchain.frame();
    if (frame.slot != active.slot || frame.image != active.image || frame.generation != active.generation)
        throw std::logic_error("Frame context belongs to another acquisition.");

    vulkan_commandbuffer& commandbuffer = commandbuffers.at(frame.slot);
    commandbuffer.begin_record({});

    commandbuffer.add_dependency(_ui_token);
    commandbuffer.add_dependency(_scene_token);
    commandbuffer.add_waited_info(swapchain.get_waited_info());
    commandbuffer.add_signal_info(swapchain.get_signal_info());

    const auto scene_barrier =
        bind_scene_image->transition_state(vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                                           vk::ImageLayout::eShaderReadOnlyOptimal, vk::QueueFamilyIgnored);

    const auto ui_barrier =
        bind_ui_image->transition_state(vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::QueueFamilyIgnored);

    // src stage 与 acquire 的 wait stage 对齐；Undefined 只表示丢弃旧像素，不能省略 WSI 等待。
    const auto swapchain_barrier =
        vk::ImageMemoryBarrier2(vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
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
                            *(descriptor.get_descriptor_sets().at(frame.slot)), nullptr);
    (*commandbuffer).draw(3, 1, 0, 0);


    (*commandbuffer).endRendering();


    const std::array end_barrier = {vk::ImageMemoryBarrier2(
        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone, vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::ePresentSrcKHR, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, swapchain.get_current_image(),
        vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1))};
    (*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));


    commandbuffer.end_record();

    swapchain.submitted(commandbuffer.submit());
    return swapchain.present_image();
}

/**
 * @brief 设备未创建时直接返回，避免在初始化失败的对象上调用 waitIdle。
 */
void vulkan_application::wait_idle() const
{
    if (!device)
    {
        return;
    }

    (*device).waitIdle();
}

/**
 * @brief 设备丢失只打印；其他异常视为无法安全关闭，打印后 terminate。
 */
void vulkan_application::wait_idle_for_cleanup() const noexcept
{
    try { wait_idle(); }
    catch (const vk::DeviceLostError&)
    {
        std::fputs("Vulkan device lost; shutting down rendering resources.\n", stderr);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Cannot safely shut down Vulkan device: %s\n", error.what());
        std::terminate();
    }
}

/**
 * @brief 清空并重建屏幕合成描述符声明：scene 图、UI 图、屏幕采样器、OCIO 的 LUT 与 uniform 缓冲。
 * @note 描述符集数量固定为 MAX_FRAMES_IN_FLIGHT；加入顺序决定 binding 下标，OCIO 的图与采样器成对追加。
 */
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
            vulkan_common::MAX_FRAMES_IN_FLIGHT, vk::DescriptorImageInfo(*ocio_sampler, nullptr, vk::ImageLayout::eUndefined));
        descriptor.add_descriptor_info(vk::DescriptorType::eSampler, std::move(ocio_sampler_pool_info));
    });

    if (ocio_ubo.get_buffer() != nullptr)
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

vulkan_queue& vulkan_application::get_graphic_queue() noexcept
{
    return graphic_queue;
}

vulkan_queue& vulkan_application::get_transfer_queue() noexcept
{
    return transfer_queue;
}

/**
 * @brief 取 color/depth 采样数交集里最高的一档写入 vulkan_common::MSAA_SAMPLE_COUNT，都不支持则回退 1x。
 */
void vulkan_application::pick_msaa_sample_count(const vulkan_physical_device& _physical_device) noexcept
{
    const auto                 physical_device_properties = (*_physical_device).getProperties2().properties;
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

/**
 * @brief 依次探测 D32、D32S8、D24S8；一个都不可用时把 expected 的错误串转成 runtime_error 抛出。
 */
void vulkan_application::pick_depth_format(const vulkan_physical_device& _physical_device)
{
    const auto depth_format = vulkan_common::find_supported_format(
        *_physical_device, {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
        vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment);

    if (!depth_format)
    {
        throw std::runtime_error(depth_format.error());
    }

    vulkan_common::DEPTH_FORMAT = *depth_format;
}

/**
 * @brief 建 VkInstance：校验 loader 不低于 1.4、层与扩展可用，Debug 下追加校验层并建调试信使。
 * @note 最终生效的层与扩展会写回 required_instance_layers 与 required_instance_extensions。
 */
void vulkan_application::create_instance(const std::vector<const char*>& _instance_layers,
                                         const std::vector<const char*>& _instance_extensions,
                                         vk::InstanceCreateFlags         _flags)
{
    required_instance_layers     = _instance_layers;
    required_instance_extensions = _instance_extensions;
    required_instance_extensions.push_back(vk::KHRGetSurfaceCapabilities2ExtensionName);
    required_instance_extensions.push_back(vk::KHRSurfaceMaintenance1ExtensionName);

    const uint32_t loader_version = context.enumerateInstanceVersion();
    if (loader_version < vk::ApiVersion14)
    {
        throw std::runtime_error(std::format("Vulkan loader {}.{}.{} is older than the required 1.4.", vk::apiVersionMajor(loader_version),
                                             vk::apiVersionMinor(loader_version), vk::apiVersionPatch(loader_version)));
    }

    constexpr vk::ApplicationInfo app_info("Hello World", vk::makeApiVersion(0, 1, 0, 0), "Little Engine",
                                           vk::makeApiVersion(0, 1, 0, 0), vk::ApiVersion14, nullptr);

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
        if (std::ranges::none_of(required_instance_layers, [](const auto* name) { return std::strcmp(name, "VK_LAYER_KHRONOS_validation") == 0; }))
            required_instance_layers.emplace_back("VK_LAYER_KHRONOS_validation");
    }
#endif  // !NDEBUG

    auto extension_properties = context.enumerateInstanceExtensionProperties();
    for (const auto* layer : required_instance_layers)
    {
        const auto provided = context.enumerateInstanceExtensionProperties(std::string(layer));
        extension_properties.insert(extension_properties.end(), provided.begin(), provided.end());
    }
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

    vk::InstanceCreateInfo instance_create_info(_flags, &app_info, required_instance_layers,
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

/**
 * @brief 用 Vulkan 1.4、光追与 swapchain_maintenance1 的扩展及特性谓词选设备，再建逻辑设备。
 * @note 特性谓词与随后的启用链一一对应；队列列表含 graphics、transfer、compute 与 present 四个
 *       族下标，vulkan_device::create 会去重后为每个族各建一个队列。
 */
uint32_t vulkan_application::create_physical_device_and_logical_device(vk::SurfaceKHR _surface)
{
    constexpr std::array required_device_extensions = {vk::KHRSwapchainExtensionName, vk::KHRSwapchainMaintenance1ExtensionName, vk::KHRAccelerationStructureExtensionName,
                                                       vk::KHRRayTracingPipelineExtensionName, vk::KHRRayQueryExtensionName,
                                                       vk::KHRDeferredHostOperationsExtensionName};

    const auto has_all_required_features = [](const vk::raii::PhysicalDevice& _physical_device) static {
        const auto features =
            _physical_device.template getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan12Features,
                                                   vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                                                   vk::PhysicalDeviceRayTracingPipelineFeaturesKHR, vk::PhysicalDeviceRayQueryFeaturesKHR,
                                                   vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR>();

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
               && features.get<vk::PhysicalDeviceRayQueryFeaturesKHR>().rayQuery
               && features.get<vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR>().swapchainMaintenance1;
    };


    uint32_t present_index;
    try { present_index = physical_device.create(instance, required_device_extensions, has_all_required_features, _surface); }
    catch (const std::runtime_error& error)
    {
        throw std::runtime_error(std::format("GPU must support Vulkan 1.4, RT and VK_KHR_swapchain_maintenance1 (swapchainMaintenance1=true): {}", error.what()));
    }

    vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan11Features,
                       vk::PhysicalDeviceAccelerationStructureFeaturesKHR, vk::PhysicalDeviceRayTracingPipelineFeaturesKHR, vk::PhysicalDeviceRayQueryFeaturesKHR,
                       vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR>
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
            vk::PhysicalDeviceRayQueryFeaturesKHR().setRayQuery(vk::True),
            vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR().setSwapchainMaintenance1(vk::True));

    const std::array queues = {physical_device.get_queue_index(vk::QueueFlagBits::eGraphics),
                               physical_device.get_queue_index(vk::QueueFlagBits::eTransfer),
                               physical_device.get_queue_index(vk::QueueFlagBits::eCompute), present_index};
    device.create(*physical_device, queues, required_device_extensions, feature_pnext_chain.get<vk::PhysicalDeviceFeatures2>());
    std::fputs("WSI: VK_KHR_swapchain_maintenance1 enabled; presentation resources use present fences.\n", stdout);

    return present_index;
}

/**
 * @brief 编译 vulkan_blend_image.slang（USE_OCIO 时先注入 OCIO 生成的转换代码）并建合成管线。
 * @note binding 0/1/2 固定为 scene 图、UI 图与采样器；OCIO 有 uniform 时占 binding 3，
 *       其后按 shader binding index 成对追加 LUT 图与采样器。每张 LUT 上传都用
 *       vulkan_submission_scope 兜底，异常时先停提交再退栈。
 */
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
        shader_desc = ocio_helper::generate_shader_info(std::string(OCIOS_PATH) + "studio-config-all-views-v4.0.0_aces-v2.0_ocio-v2.5.ocio");
        spirv_code = ocio_helper::replace_and_compile(shader_desc, std::filesystem::path(SHADERS_PATH) / "vulkan_blend_image.slang",
                                                      {VERT_ENTRY_NAME, FRAG_ENTRY_NAME});
    }
    else
    {
        spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::filesystem::path(SHADERS_PATH) / "vulkan_blend_image.slang",
                                                           {VERT_ENTRY_NAME, FRAG_ENTRY_NAME});
    }

    if (!spirv_code)
    {
        throw std::runtime_error(spirv_code.error());
    }

    if constexpr (vulkan_common::USE_OCIO)
    {
        vulkan_commandbuffer& commandbuffer = commandbuffers.at(0);
        if (shader_desc->getNumUniforms() > 0 && shader_desc->getUniformBufferSize() > 0)
        {
            const auto buffer = ocio_helper::get_uniform_buffer_data(shader_desc);

            commandbuffer.add_dependency(vulkan_common::upload_buffer(
                allocator, *device, recycle_bin, graphic_queue, transfer_queue, ocio_ubo,
                vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead, buffer, "ocio_ubo"));

            bindings.emplace_back(static_cast<uint32_t>(bindings.size()), vk::DescriptorType::eUniformBuffer, 1,
                                  vk::ShaderStageFlagBits::eFragment, nullptr);
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
            const vulkan_submission_scope failure_scope(graphic_queue, transfer_queue);
            commandbuffer.add_dependency(vulkan_common::upload_image(
                allocator, *device, recycle_bin, graphic_queue, transfer_queue, image_type, image_view_type, format,
                vk::Extent3D(width, height, 1), image, vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                std::span(reinterpret_cast<uint8_t*>(rgba_values.data()), rgba_values.size() * sizeof(decltype(rgba_values)::value_type)),
                "ocio_lut"));
            ocio_images.push_back(std::move(image));

            vulkan_sampler sampler;
            sampler.create(*physical_device, *device,
                           generate_sampler_info(interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                           "ocio_lut");
            ocio_samplers.emplace_back(std::move(sampler));

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
            const vulkan_submission_scope failure_scope(graphic_queue, transfer_queue);
            commandbuffer.add_dependency(vulkan_common::upload_image(
                allocator, *device, recycle_bin, graphic_queue, transfer_queue, vk::ImageType::e3D,
                vk::ImageViewType::e3D, vk::Format::eR32G32B32A32Sfloat, vk::Extent3D(edge_len, edge_len, edge_len),
                image, vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                std::span(reinterpret_cast<uint8_t*>(rgba_values.data()), rgba_values.size() * sizeof(decltype(rgba_values)::value_type)),
                "ocio_lut"));
            ocio_images.push_back(std::move(image));

            vulkan_sampler sampler;
            sampler.create(*physical_device, *device,
                           generate_sampler_info(interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
                           "ocio_lut");
            ocio_samplers.emplace_back(std::move(sampler));

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
    const auto shader_stages = std::array{
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, VERT_ENTRY_NAME.data()),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, FRAG_ENTRY_NAME.data()),
    };

    const std::array color_format_array = {swapchain.get_format()};
    pipeline.create(*device, *pipeline_cache, bindings, {}, {}, {}, shader_stages, vk::PrimitiveTopology::eTriangleList,
                    vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise,
                    vk::SampleCountFlagBits::e1, vk::False, color_format_array, vk::Format::eUndefined);
}
