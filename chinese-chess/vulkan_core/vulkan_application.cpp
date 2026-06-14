#include "tools/image_helper.h"
#include "tools/ocio_helper.h"
#include "tools/shader_compiler.h"
#include "tools/string_helper.h"
#include "vulkan_application.h"
#include "vulkan_common.h"
#include <algorithm>
#include <format>
#include <print>
#include <ranges>
#include <unordered_set>
#include <vulkan/utility/vk_format_utils.h>

void vulkan_application::init(const std::vector<const char*>& _instance_layers, const std::vector<const char*>& _instance_extensions, vk::InstanceCreateFlags _flags)
{
	create_instance(_instance_layers, _instance_extensions, _flags);
}

void vulkan_application::create(vk::SurfaceKHR _surface, bool _enable_graphics, bool _enable_compute, uint32_t _width, uint32_t _height)
{
	pick_physical_device_and_queue_family(_surface, _enable_graphics, _enable_compute);
	create_device_and_queue();

	pick_msaa_sample_count();
	pick_depth_format();

	swapchain.create(instance, physical_device, device, _surface, _width, _height);

	create_pipeline();

	// create sampler
	vk::PhysicalDeviceProperties properties = physical_device.getProperties();
	vk::SamplerCreateInfo sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
		vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder,
		0.f, vk::True, properties.limits.maxSamplerAnisotropy, vk::False, vk::CompareOp::eAlways, 0.f, 1.f,
		vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
	image_sampler = vk::raii::Sampler(device, sampler_info);

	commandbuffers = vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(), vk::CommandBufferLevel::ePrimary, vulkan_common::MAX_FRAMES_IN_FLIGHT), device, graphic_queue.get_queue());
}

void vulkan_application::resize(uint32_t _width, uint32_t _height)
{
	swapchain.recreate(physical_device, device, _width, _height);
}

void vulkan_application::begin() noexcept
{
	commandbuffers.at(current_frame).begin_record({});
}

void vulkan_application::render(const std::span<const vk::CommandBuffer> _commandbuffers)
{
	try
	{
		swapchain.acquire_next_image();
	}
	catch (vk::OutOfDateKHRError e)
	{
#ifndef NDEBUG
		std::println("{}", e.what());
#endif // !NDEBUG
		return;
	}
	catch (std::system_error)
	{
		throw std::runtime_error("failed to present swap chain image!");
	}


	vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
	(*commandbuffer).executeCommands(_commandbuffers);


	std::vector<vk::ImageMemoryBarrier2> barriers;
	barriers.emplace_back(bind_scene_image->set_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	barriers.emplace_back(bind_ui_image->set_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	barriers.emplace_back(vulkan_image::transition_image_layout(swapchain.get_current_image(), vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, barriers));


	auto [width, height] = swapchain.get_extent();
	vk::RenderingAttachmentInfo swapchain_attachment_info(swapchain.get_current_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eNone,
		nullptr, vk::ImageLayout::eUndefined, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));
	vk::RenderingInfo swapchain_rendering_info({}, vk::Rect2D({ 0, 0 }, { static_cast<uint32_t>(width), static_cast<uint32_t>(height) }), 1, {}, swapchain_attachment_info, nullptr, nullptr, nullptr);

	(*commandbuffer).setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f));
	(*commandbuffer).setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(width, height)));


	(*commandbuffer).beginRendering(swapchain_rendering_info);


	(*commandbuffer).bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
	(*commandbuffer).bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline_layout(), 0, *(descriptor.get_descriptor_sets().at(current_frame)), nullptr);
	(*commandbuffer).draw(3, 1, 0, 0);


	(*commandbuffer).endRendering();


	std::vector<vk::ImageMemoryBarrier2> end_barrier;
	end_barrier.emplace_back(vulkan_image::transition_image_layout(swapchain.get_current_image(), vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
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
	device.waitIdle();
}

void vulkan_application::bind_image(vulkan_image* _scene_image, vulkan_image* _ui_image)
{
	bind_scene_image = _scene_image;
	bind_ui_image = _ui_image;


	// descriptor
	descriptor.clear_descriptor_info();

	auto scene_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
		| std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo
			{
				return vk::DescriptorImageInfo(nullptr, bind_scene_image->get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
			})
		| std::ranges::to<std::vector>();
	descriptor.add_descriptor_info(vk::DescriptorType::eSampledImage, std::move(scene_pool_info));

	auto ui_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
		| std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo
			{
				return vk::DescriptorImageInfo(nullptr, bind_ui_image->get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
			})
		| std::ranges::to<std::vector>();
	descriptor.add_descriptor_info(vk::DescriptorType::eSampledImage, std::move(ui_pool_info));

	auto sampler_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
		| std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo
			{
				return vk::DescriptorImageInfo(image_sampler, nullptr, vk::ImageLayout::eUndefined);
			})
		| std::ranges::to<std::vector>();
	descriptor.add_descriptor_info(vk::DescriptorType::eSampler, std::move(sampler_pool_info));

	std::ranges::for_each(std::views::zip(ocio_images, ocio_samplers), [&](const auto& _binding)
		{
			const auto& [ocio_image, ocio_sampler] = _binding;

			auto image_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
				| std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo
					{
						return vk::DescriptorImageInfo(nullptr, ocio_image.get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);
					})
				| std::ranges::to<std::vector>();
			descriptor.add_descriptor_info(vk::DescriptorType::eSampledImage, std::move(image_pool_info));


			auto ocio_sampler_pool_info = std::views::iota(0u, vulkan_common::MAX_FRAMES_IN_FLIGHT)
				| std::views::transform([&](const auto&) -> DescriptorBufferOrImageInfo
					{
						return vk::DescriptorImageInfo(ocio_sampler, nullptr, vk::ImageLayout::eUndefined);
					})
				| std::ranges::to<std::vector>();
			descriptor.add_descriptor_info(vk::DescriptorType::eSampler, std::move(ocio_sampler_pool_info));
		});

	descriptor.update_descriptor_sets(device, pipeline.get_descriptor_set_layout());
}

void vulkan_application::save_image(vulkan_image& _image) const
{
	VkFormat image_format = static_cast<VkFormat>(_image.get_format());

	vulkan_buffer save_buffer;
	save_buffer.create(physical_device, device, _image.get_extent().width * _image.get_extent().height * vkuFormatTexelBlockSize(image_format), vk::BufferUsageFlagBits::eTransferDst, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), device, get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

	auto old_layout = _image.get_layout();

	std::vector<vk::ImageMemoryBarrier2> blit_begin_barrier = { _image.set_layout(vk::ImageLayout::eTransferSrcOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)) };
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, blit_begin_barrier));

	vulkan_image::copy_image_to_buffer(*commandbuffer, _image.get_image(), save_buffer.get_buffer(), _image.get_layout(), vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), vk::Offset3D(0, 0, 0), vk::Extent3D(_image.get_extent().width, _image.get_extent().height, 1)));

	std::vector<vk::ImageMemoryBarrier2> blit_end_barrier = { _image.set_layout(old_layout, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)) };
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, blit_end_barrier));

	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);


	auto now = std::chrono::system_clock::now();
	auto now_second = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(now));

	std::u8string save_path = std::u8string(CAPTURES_PATH);
	save_path += STRING_HELPER::convert_to<std::u8string, std::string>(std::format("{:%Y_%m_%d_%H_%M_%S}", now_second));

	OIIO::TypeDesc desc;

	if (vkuFormatIs8bit(image_format))
	{
		if (vkuFormatIsUINT(image_format))
		{
			desc = OIIO::TypeDesc::UINT8;
		}
		else
		{
			desc = OIIO::TypeDesc::INT8;
		}
		save_path += u8".png";
	}
	else if (vkuFormatIs16bit(image_format))
	{
		if (vkuFormatIsSFLOAT(image_format))
		{
			desc = OIIO::TypeDesc::HALF;
		}
		else if (vkuFormatIsUINT(image_format))
		{
			desc = OIIO::TypeDesc::UINT16;
		}
		else
		{
			desc = OIIO::TypeDesc::INT16;
		}
		save_path += u8".exr";
	}
	else if (vkuFormatIs32bit(image_format))
	{
		if (vkuFormatIsSFLOAT(image_format))
		{
			desc = OIIO::TypeDesc::FLOAT;
		}
		else if (vkuFormatIsUINT(image_format))
		{
			desc = OIIO::TypeDesc::UINT32;
		}
		else
		{
			desc = OIIO::TypeDesc::INT32;
		}
		save_path += u8".exr";
	}

	IMAGE_HELPER.save_to_local(save_path, _image.get_extent().width, _image.get_extent().height, vkuFormatComponentCount(image_format), desc, save_buffer.get_buffer_address().hostAddress);
}

const vk::raii::Instance& vulkan_application::get_instance() const noexcept
{
	return instance;
}

const vk::raii::PhysicalDevice& vulkan_application::get_physical_device() const noexcept
{
	return physical_device;
}

const vk::raii::Device& vulkan_application::get_device() const noexcept
{
	return device;
}

std::optional<std::reference_wrapper<const vulkan_queue>> vulkan_application::get_queue(vk::QueueFlagBits _queue_type) const noexcept
{
	switch (_queue_type)
	{
	case vk::QueueFlagBits::eOpticalFlowNV:
		break;
	case vk::QueueFlagBits::eVideoEncodeKHR:
		break;
	case vk::QueueFlagBits::eVideoDecodeKHR:
		break;
	case vk::QueueFlagBits::eProtected:
		break;
	case vk::QueueFlagBits::eSparseBinding:
		break;
	case vk::QueueFlagBits::eTransfer:
		break;
	case vk::QueueFlagBits::eCompute:
		return std::cref(compute_queue);
	case vk::QueueFlagBits::eGraphics:
		return std::cref(graphic_queue);
	default:
		break;
	}
	return std::nullopt;
}

const vulkan_swapchain& vulkan_application::get_swapchain() const noexcept
{
	return swapchain;
}

void vulkan_application::create_instance(const std::vector<const char*>& _instance_layers, const std::vector<const char*>& _instance_extensions, vk::InstanceCreateFlags _flags)
{
	required_instance_layers = _instance_layers;
	required_instance_extensions = _instance_extensions;

	constexpr vk::ApplicationInfo app_info("Hello World", VK_MAKE_VERSION(1, 0, 0), "Little Engine", VK_MAKE_VERSION(1, 0, 0), vk::ApiVersion14, nullptr);

	auto layer_properties = context.enumerateInstanceLayerProperties();
	for (const auto& required_layer : required_instance_layers)
	{
		if (std::ranges::none_of(layer_properties,
			[required_layer](const auto& layer_property)
			{ return strcmp(layer_property.layerName, required_layer) == 0; }))
		{
			throw std::runtime_error("One or more required layers are not supported!");
		}
	}

#ifndef NDEBUG
	if (std::ranges::any_of(layer_properties,
		[](const auto& layer_property)
		{ return strcmp(layer_property.layerName, "VK_LAYER_KHRONOS_validation") == 0; }))
	{
		required_instance_layers.emplace_back("VK_LAYER_KHRONOS_validation");
	}
#endif // NDEBUG

	auto extension_properties = context.enumerateInstanceExtensionProperties();
	for (const auto& required_extension : required_instance_extensions)
	{
		if (std::ranges::none_of(extension_properties,
			[required_extension](const auto& extension_property)
			{ return strcmp(extension_property.extensionName, required_extension) == 0; }))
		{
			throw std::runtime_error(std::format("Required extension not supported: {}", required_extension));
		}
	}

#ifndef NDEBUG
	if (std::ranges::any_of(extension_properties,
		[](const auto& extension_property)
		{ return strcmp(extension_property.extensionName, vk::EXTDebugUtilsExtensionName) == 0; }))
	{
		required_instance_extensions.push_back(vk::EXTDebugUtilsExtensionName);
	}
#endif // NDEBUG

	vk::InstanceCreateInfo instance_create_info(_flags, &app_info, required_instance_layers, required_instance_extensions, nullptr);
	instance = vk::raii::Instance(context, instance_create_info);

#ifndef NDEBUG
	vk::DebugUtilsMessageSeverityFlagsEXT severity_flags(vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
	vk::DebugUtilsMessageTypeFlagsEXT message_type_flags(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
	vk::DebugUtilsMessengerCreateInfoEXT debug_messenger_create_info({}, severity_flags, message_type_flags, &debug_callback);
	debug_messenger = instance.createDebugUtilsMessengerEXT(debug_messenger_create_info);
#endif // NDEBUG
}

void vulkan_application::pick_physical_device_and_queue_family(vk::SurfaceKHR _surface, bool _enable_graphics, bool _enable_compute)
{
	auto all_physical_devices = instance.enumeratePhysicalDevices();
	auto filtered_physical_devices = all_physical_devices
		| std::views::filter([this](const auto& _physical_device)
			{
				bool support_vulkan_1_3 = _physical_device.getProperties().apiVersion >= vk::ApiVersion13;

				auto queue_families = _physical_device.getQueueFamilyProperties();
				bool support_graphics = std::ranges::any_of(queue_families, [](const auto& qfp)
					{ return static_cast<bool>(qfp.queueFlags & vk::QueueFlagBits::eGraphics); });

				auto available_device_extensions = _physical_device.enumerateDeviceExtensionProperties();
				bool has_all_required_extensions = std::ranges::all_of(required_device_extensions,
					[&available_device_extensions](const auto& required_device_extension)
					{
						return std::ranges::any_of(available_device_extensions,
							[required_device_extension](const auto& available_device_extension)
							{ return strcmp(available_device_extension.extensionName, required_device_extension) == 0; });
					});

				auto features = _physical_device.template getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceRobustness2FeaturesEXT,
					vk::PhysicalDeviceVulkan14Features, vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan12Features,
					vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
					vk::PhysicalDeviceAccelerationStructureFeaturesKHR, /*vk::PhysicalDeviceRayQueryFeaturesKHR,*/
					vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>();

				bool has_all_required_features = features.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy
					&& features.get<vk::PhysicalDeviceFeatures2>().features.fillModeNonSolid
					&& features.get<vk::PhysicalDeviceFeatures2>().features.multiDrawIndirect
					&& features.get<vk::PhysicalDeviceRobustness2FeaturesEXT>().nullDescriptor
					&& features.get<vk::PhysicalDeviceVulkan14Features>().pushDescriptor
					&& features.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering
					&& features.get<vk::PhysicalDeviceVulkan13Features>().synchronization2
					&& features.get<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress
					&& features.get<vk::PhysicalDeviceVulkan12Features>().runtimeDescriptorArray
					&& features.get<vk::PhysicalDeviceVulkan11Features>().shaderDrawParameters
					&& features.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState
					&& features.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure
					&& features.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructureCaptureReplay
					&& features.get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().descriptorBindingAccelerationStructureUpdateAfterBind
					//&& features.get<vk::PhysicalDeviceRayQueryFeaturesKHR>().rayQuery
					&& features.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline
					&& features.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipelineTraceRaysIndirect
					&& features.get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTraversalPrimitiveCulling;

				return support_vulkan_1_3 && support_graphics && has_all_required_extensions && has_all_required_features;
			})
		| std::ranges::to<std::vector>();

	auto find_physical_device = std::ranges::find_if(filtered_physical_devices, [&](const auto& _physical_device)
		{
			auto queue_family_properties = _physical_device.getQueueFamilyProperties();

			for (const auto& [queue_family_index, queue_family_property] : queue_family_properties | std::views::enumerate)
			{
				bool support_graphics = _enable_graphics && queue_family_property.queueFlags & vk::QueueFlagBits::eGraphics;
				bool support_present = _physical_device.getSurfaceSupportKHR(static_cast<uint32_t>(queue_family_index), _surface) == vk::True;
				bool support_compute = _enable_compute && queue_family_property.queueFlags & vk::QueueFlagBits::eCompute;

				if (support_graphics)
				{
					graphic_queue.set_index(static_cast<uint32_t>(queue_family_index));
				}
				if (support_present)
				{
					vulkan_queue present_queue;
					present_queue.set_index(static_cast<uint32_t>(queue_family_index));
					swapchain.set_present_queue(std::move(present_queue));
				}
				if (support_compute)
				{
					compute_queue.set_index(static_cast<uint32_t>(queue_family_index));
				}

				if (swapchain.get_present_queue().get_index() != vk::QueueFamilyIgnored && ((_enable_graphics && graphic_queue.get_index() != vk::QueueFamilyIgnored) || (_enable_compute && compute_queue.get_index() != vk::QueueFamilyIgnored)))
				{
					return true;
				}
			}

			return false;
		});

	if (find_physical_device == filtered_physical_devices.end())
	{
		throw std::runtime_error("failed to find a suitable GPU!");
	}
	physical_device = *find_physical_device;
}

void vulkan_application::create_device_and_queue()
{
	vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceRobustness2FeaturesEXT,
		vk::PhysicalDeviceVulkan14Features, vk::PhysicalDeviceVulkan13Features,
		vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan11Features,
		vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT, vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
		/*vk::PhysicalDeviceRayQueryFeaturesKHR,*/ vk::PhysicalDeviceRayTracingPipelineFeaturesKHR> feature_pnext_chain(
			vk::PhysicalDeviceFeatures2().setFeatures(vk::PhysicalDeviceFeatures().setSamplerAnisotropy(vk::True).setFillModeNonSolid(vk::True).setMultiDrawIndirect(vk::True)),
			vk::PhysicalDeviceRobustness2FeaturesEXT().setNullDescriptor(vk::True),
			vk::PhysicalDeviceVulkan14Features().setPushDescriptor(vk::True),
			vk::PhysicalDeviceVulkan13Features().setDynamicRendering(vk::True).setSynchronization2(vk::True),
			vk::PhysicalDeviceVulkan12Features().setBufferDeviceAddress(vk::True).setRuntimeDescriptorArray(vk::True),
			vk::PhysicalDeviceVulkan11Features().setShaderDrawParameters(vk::True),
			vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT().setExtendedDynamicState(vk::True),
			vk::PhysicalDeviceAccelerationStructureFeaturesKHR().setAccelerationStructure(vk::True).setAccelerationStructureCaptureReplay(vk::True).setDescriptorBindingAccelerationStructureUpdateAfterBind(vk::True),
			//vk::PhysicalDeviceRayQueryFeaturesKHR().setRayQuery(vk::True),
			vk::PhysicalDeviceRayTracingPipelineFeaturesKHR().setRayTracingPipeline(vk::True).setRayTracingPipelineTraceRaysIndirect(vk::True).setRayTraversalPrimitiveCulling(vk::True));

	std::unordered_set<uint32_t> queue_indices = { swapchain.get_present_queue().get_index(), graphic_queue.get_index(), compute_queue.get_index() };
	queue_indices.erase(vk::QueueFamilyIgnored);

	float queue_priority = 0.0f;
	auto device_queue_create_info = queue_indices
		| std::views::transform(
			[&queue_priority](const auto& index) -> vk::DeviceQueueCreateInfo
			{
				return vk::DeviceQueueCreateInfo({}, index, 1, &queue_priority);
			})
		| std::ranges::to<std::vector>();

	vk::DeviceCreateInfo device_creat_info({}, device_queue_create_info, {}, required_device_extensions, {}, &feature_pnext_chain.get<vk::PhysicalDeviceFeatures2>());

	device = vk::raii::Device(physical_device, device_creat_info);

	if (graphic_queue.get_index() != vk::QueueFamilyIgnored)
	{
		graphic_queue.create(device, graphic_queue.get_index());
	}
	if (swapchain.get_present_queue().get_index() != vk::QueueFamilyIgnored)
	{
		vulkan_queue present_queue;
		present_queue.create(device, swapchain.get_present_queue().get_index());
		swapchain.set_present_queue(std::move(present_queue));
	}
	if (compute_queue.get_index() != vk::QueueFamilyIgnored)
	{
		compute_queue.create(device, compute_queue.get_index());
	}
}

void vulkan_application::pick_msaa_sample_count() const noexcept
{
	auto physical_device_properties = physical_device.getProperties();
	vk::SampleCountFlags counts = physical_device_properties.limits.framebufferColorSampleCounts & physical_device_properties.limits.framebufferDepthSampleCounts;

	constexpr std::array sample_count_flags = {
		vk::SampleCountFlagBits::e64, vk::SampleCountFlagBits::e32, vk::SampleCountFlagBits::e16,
		vk::SampleCountFlagBits::e8, vk::SampleCountFlagBits::e4, vk::SampleCountFlagBits::e2 };

	auto support_sample_count = std::ranges::find_if(sample_count_flags, [&counts](vk::SampleCountFlagBits sample) { return static_cast<bool>(counts & sample); });

	vulkan_common::MSAA_SAMPLE_COUNT = (support_sample_count != sample_count_flags.end()) ? *support_sample_count : vk::SampleCountFlagBits::e1;
}

void vulkan_application::pick_depth_format() const noexcept
{
	vulkan_common::DEPTH_FORMAT = vulkan_common::find_supported_format(physical_device,
		{ vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint },
		vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment).value();
}

void vulkan_application::create_pipeline()
{
	// pipeline
	std::vector bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
		vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
		vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
	};

	std::vector<char> spirv_code;
	OCIO::GpuShaderDescRcPtr shader_desc = nullptr;

	if constexpr (vulkan_common::USE_OCIO)
	{
		shader_desc = OCIO_HELPER.generate_shader_info(std::u8string(OCIOS_PATH) + u8"studio-config-all-views-v3.0.0_aces-v2.0_ocio-v2.4.ocio");
		spirv_code = OCIO_HELPER.replace_and_compile(shader_desc, std::u8string(SHADERS_PATH) + u8"blend_image.slang", { VERT_ENTYR_NAME, FRAG_ENTYR_NAME });
	}
	else
	{
		spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::u8string(SHADERS_PATH) + u8"blend_image.slang", { VERT_ENTYR_NAME, FRAG_ENTYR_NAME });
	}

	if (spirv_code.empty())
	{
		throw std::runtime_error("compile .spv failed!");
	}

	if constexpr (vulkan_common::USE_OCIO)
	{
		if (shader_desc->getNumUniforms() > 0 && shader_desc->getUniformBufferSize() > 0)
		{
			ocio_ubo.create(physical_device, device, shader_desc->getUniformBufferSize(), vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
			OCIO_HELPER.copy_uniform_to_buffer(shader_desc, ocio_ubo.get_buffer_address().hostAddress);
		}

		// todo 看能否封装到OCIO_HELPER的函数里面
		// begin a commandbuffer
		vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(get_queue(vk::QueueFlagBits::eGraphics)->get().get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), device, get_queue(vk::QueueFlagBits::eGraphics)->get().get_queue()).front());
		commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

		ocio_images.clear();
		ocio_samplers.clear();
		//ocio_images.resize(shader_desc->getNum3DTextures() + shader_desc->getNumTextures());
		std::vector<vulkan_buffer> stage_buffers;

		auto num_3d_textures = shader_desc->getNum3DTextures();
		for (uint32_t i = 0; i < num_3d_textures; i++)
		{
			const char* texture_name = nullptr;
			const char* sampler_name = nullptr;
			uint32_t edge_len = 0;
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


			std::vector<float> rgba_values = std::views::iota(0u, edge_len * edge_len * edge_len)
				| std::views::transform([&](const auto& i)
					{
						return std::vector<float>{ values[i * 3 + 0], values[i * 3 + 1], values[i * 3 + 2], 1.f };
					})
				| std::views::join
				| std::ranges::to<std::vector>();


			vulkan_image ocio_image;
			vk::ImageCreateInfo ocio_image_info({}, vk::ImageType::e3D, vk::Format::eR32G32B32A32Sfloat, vk::Extent3D(edge_len, edge_len, edge_len), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, vk::SharingMode::eExclusive, 0);
			vk::ImageViewCreateInfo ocio_view_info({}, {}, vk::ImageViewType::e3D, vk::Format::eR32G32B32A32Sfloat, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
			ocio_image.create(physical_device, device, ocio_image_info, ocio_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));


			std::vector<vk::ImageMemoryBarrier2> begin_barrier;
			begin_barrier.emplace_back(ocio_image.set_layout(vk::ImageLayout::eTransferDstOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
			(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));


			vulkan_buffer stage_buffer;
			vk::DeviceSize buffer_size = sizeof(rgba_values.front()) * rgba_values.size();
			stage_buffer.create(physical_device, device, buffer_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
			memcpy(stage_buffer.get_buffer_address().hostAddress, rgba_values.data(), buffer_size);

			vulkan_buffer::copy_buffer_to_image(*commandbuffer, stage_buffer.get_buffer(), ocio_image.get_image(), vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), vk::Offset3D(0, 0, 0), vk::Extent3D(edge_len, edge_len, edge_len)));

			std::vector<vk::ImageMemoryBarrier2> end_barrier;
			end_barrier.emplace_back(ocio_image.set_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
			(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));


			stage_buffers.push_back(std::move(stage_buffer));
			//ocio_images.at(shader_desc->getTextureShaderBindingIndex(i)) = std::move(ocio_image);
			ocio_images.push_back(std::move(ocio_image));


			// create sampler
			vk::PhysicalDeviceProperties properties = physical_device.getProperties();
			vk::SamplerCreateInfo sampler_info({}, (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
				(interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear), vk::SamplerMipmapMode::eNearest,
				vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
				0.f, vk::False, 1.f, vk::False, vk::CompareOp::eAlways, 0.f, 0.f,
				vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
			ocio_samplers.push_back(std::move(vk::raii::Sampler(device, sampler_info)));
		}


		auto num_textures = shader_desc->getNumTextures();
		for (uint32_t i = 0; i < num_textures; i++)
		{
			const char* texture_name = nullptr;
			const char* sampler_name = nullptr;
			uint32_t width = 0;
			uint32_t height = 0;
			OCIO::GpuShaderDesc::TextureType channel = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
			OCIO::Interpolation interpolation = OCIO::INTERP_LINEAR;
			OCIO::GpuShaderDesc::TextureDimensions dimensions = OCIO::GpuShaderDesc::TEXTURE_1D;
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

			height = (height > 0) ? height : 1;

			vk::ImageType image_type;
			vk::ImageViewType image_view_type;
			vk::Format format;
			std::vector<float> rgba_values;

			if (height == 1)
			{
				image_type = vk::ImageType::e1D;
				image_view_type = vk::ImageViewType::e1D;
			}
			else
			{
				image_type = vk::ImageType::e2D;
				image_view_type = vk::ImageViewType::e2D;
			}

			if (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL)
			{
				format = vk::Format::eR32Sfloat;
				rgba_values = std::vector<float>(values, values + width * height);
			}
			else
			{
				format = vk::Format::eR32G32B32A32Sfloat;
				rgba_values = std::views::iota(0u, width * height)
					| std::views::transform([&](const auto& i)
						{
							return std::vector<float>{ values[i * 3 + 0], values[i * 3 + 1], values[i * 3 + 2], 1.f };
						})
					| std::views::join
					| std::ranges::to<std::vector>();
			}


			vulkan_image ocio_image;
			vk::ImageCreateInfo ocio_image_info({}, image_type, format, vk::Extent3D(width, height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, vk::SharingMode::eExclusive, 0);
			vk::ImageViewCreateInfo ocio_view_info({}, {}, image_view_type, format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
			ocio_image.create(physical_device, device, ocio_image_info, ocio_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));


			std::vector<vk::ImageMemoryBarrier2> begin_barrier;
			begin_barrier.emplace_back(ocio_image.set_layout(vk::ImageLayout::eTransferDstOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
			(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));


			vulkan_buffer stage_buffer;
			vk::DeviceSize buffer_size = sizeof(rgba_values.front()) * rgba_values.size();
			stage_buffer.create(physical_device, device, buffer_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
			memcpy(stage_buffer.get_buffer_address().hostAddress, rgba_values.data(), buffer_size);

			vulkan_buffer::copy_buffer_to_image(*commandbuffer, stage_buffer.get_buffer(), ocio_image.get_image(), vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), vk::Offset3D(0, 0, 0), vk::Extent3D(width, height, 1)));

			std::vector<vk::ImageMemoryBarrier2> end_barrier;
			end_barrier.emplace_back(ocio_image.set_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
			(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));


			stage_buffers.push_back(std::move(stage_buffer));
			//ocio_images.at(shader_desc->getTextureShaderBindingIndex(i)) = std::move(ocio_image);
			ocio_images.push_back(std::move(ocio_image));


			// create sampler
			vk::PhysicalDeviceProperties properties = physical_device.getProperties();
			vk::SamplerCreateInfo sampler_info({}, (interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear),
				(interpolation == OCIO::INTERP_NEAREST ? vk::Filter::eNearest : vk::Filter::eLinear), vk::SamplerMipmapMode::eNearest,
				vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
				0.f, vk::False, 1.f, vk::False, vk::CompareOp::eAlways, 0.f, 0.f,
				vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
			ocio_samplers.push_back(std::move(vk::raii::Sampler(device, sampler_info)));
		}


		// commandbuffer submit
		commandbuffer.end_record();
		commandbuffer.submit({}, {}, true);

		if (ocio_images.size() != ocio_samplers.size())
		{
			throw std::runtime_error("images and samplers number not equal.");
		}

		std::ranges::for_each(std::views::iota(0u, static_cast<uint32_t>(ocio_images.size())), [&bindings](const auto&)
			{
				bindings.emplace_back(vk::DescriptorSetLayoutBinding(static_cast<uint32_t>(bindings.size()), vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment, nullptr));
				bindings.emplace_back(vk::DescriptorSetLayoutBinding(static_cast<uint32_t>(bindings.size()), vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eFragment, nullptr));
			});
	}

	vk::raii::ShaderModule shaderModule(device, vk::ShaderModuleCreateInfo({}, spirv_code.size() * sizeof(char), reinterpret_cast<const uint32_t*>(spirv_code.data())));
	std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, VERT_ENTYR_NAME.data()),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, FRAG_ENTYR_NAME.data()),
	};

	std::array color_format_array = { swapchain.get_format() };
	pipeline.create(device, bindings, {}, {}, {}, shader_stages,
		vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise,
		vk::SampleCountFlagBits::e1, vk::False, color_format_array, vk::Format::eUndefined);
}

#ifndef NDEBUG
VKAPI_ATTR vk::Bool32 VKAPI_CALL vulkan_application::debug_callback(vk::DebugUtilsMessageSeverityFlagBitsEXT _severity, vk::DebugUtilsMessageTypeFlagsEXT _type, const vk::DebugUtilsMessengerCallbackDataEXT* _pCallbackData, void*)
{
	if (_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError || _severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
	{
		std::println("validation layer: type {} msg: {}", to_string(_type), _pCallbackData->pMessage);
	}
	return vk::False;
}
#endif // NDEBUG