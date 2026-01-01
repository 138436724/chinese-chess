module vulkan_application;

import <vulkan/vulkan_core.h>;

void vulkan_application::create_instance(const std::vector<const char*>& _instance_layers, const std::vector<const char*>& _instance_extensions, vk::InstanceCreateFlags _flags)
{
	required_instance_layers = _instance_layers;
	required_instance_extensions = _instance_extensions;

	constexpr vk::ApplicationInfo app_info("Hello World", VK_MAKE_VERSION(1, 0, 0), "Little Engine", VK_MAKE_VERSION(1, 0, 0), vk::ApiVersion14, nullptr);

	auto layer_properties = context.enumerateInstanceLayerProperties();
	for (auto const& required_layer : required_instance_layers)
	{
		if (std::ranges::none_of(layer_properties,
			[required_layer](auto const& layer_property)
			{ return strcmp(layer_property.layerName, required_layer) == 0; }))
		{
			throw std::runtime_error("One or more required layers are not supported!");
		}
	}

#ifndef NDEBUG
	if (std::ranges::any_of(layer_properties,
		[](auto const& layer_property)
		{ return strcmp(layer_property.layerName, "VK_LAYER_KHRONOS_validation") == 0; }))
	{
		required_instance_layers.emplace_back("VK_LAYER_KHRONOS_validation");
	}
#endif // NDEBUG

	auto extension_properties = context.enumerateInstanceExtensionProperties();
	for (auto const& required_extension : required_instance_extensions)
	{
		if (std::ranges::none_of(extension_properties,
			[required_extension](auto const& extension_property)
			{ return strcmp(extension_property.extensionName, required_extension) == 0; }))
		{
			throw std::runtime_error(std::format("Required extension not supported: {}", required_extension));
		}
	}

#ifndef NDEBUG
	if (std::ranges::any_of(extension_properties,
		[](auto const& extension_property)
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
	vk::DebugUtilsMessengerCreateInfoEXT debug_messenger_create_info{ {}, severity_flags, message_type_flags, &debug_callback };
	debug_messenger = instance.createDebugUtilsMessengerEXT(debug_messenger_create_info);
#endif // NDEBUG
}

void vulkan_application::pick_physical_device_and_queue_family(vk::SurfaceKHR _surface, bool _enable_graphics, bool _enable_compute)
{
	auto all_physical_devices = instance.enumeratePhysicalDevices();
	auto filtered_physical_devices = all_physical_devices
		| std::views::filter([this](auto const& the_physical_device)
			{
				bool support_vulkan_1_3 = the_physical_device.getProperties().apiVersion >= vk::ApiVersion13;

				auto queue_families = the_physical_device.getQueueFamilyProperties();
				bool support_graphics = std::ranges::any_of(queue_families, [](auto const& qfp)
					{ return static_cast<bool>(qfp.queueFlags & vk::QueueFlagBits::eGraphics); });

				auto available_device_extensions = the_physical_device.enumerateDeviceExtensionProperties();
				bool has_all_required_extensions = std::ranges::all_of(required_device_extensions,
					[&available_device_extensions](auto const& required_device_extension)
					{
						return std::ranges::any_of(available_device_extensions,
							[required_device_extension](auto const& available_device_extension)
							{ return strcmp(available_device_extension.extensionName, required_device_extension) == 0; });
					});

				auto features = the_physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceRobustness2FeaturesEXT,
					vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
					vk::PhysicalDeviceVulkan11Features>();
				bool has_all_required_features = features.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy
					&& features.get<vk::PhysicalDeviceFeatures2>().features.fillModeNonSolid
					&& features.get<vk::PhysicalDeviceRobustness2FeaturesEXT>().nullDescriptor
					&& features.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering
					&& features.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState
					&& features.get<vk::PhysicalDeviceVulkan11Features>().shaderDrawParameters;

				return support_vulkan_1_3 && support_graphics && has_all_required_extensions && has_all_required_features;
			})
		| std::ranges::to<std::vector>();

	auto find_physical_device = std::ranges::find_if(filtered_physical_devices, [&](const auto& the_physical_device)
		{
			auto queue_family_properties = the_physical_device.getQueueFamilyProperties();

			for (const auto& [queue_family_index, queue_family_property] : queue_family_properties | std::views::enumerate)
			{
				bool support_graphics = _enable_graphics && queue_family_property.queueFlags & vk::QueueFlagBits::eGraphics;
				bool support_present = the_physical_device.getSurfaceSupportKHR(static_cast<uint32_t>(queue_family_index), _surface) == vk::True;
				bool support_compute = _enable_compute && queue_family_property.queueFlags & vk::QueueFlagBits::eCompute;

				if (support_graphics)
				{
					graphic_queue.set_index(static_cast<uint32_t>(queue_family_index));
				}
				if (support_present)
				{
					present_queue.set_index(static_cast<uint32_t>(queue_family_index));
				}
				if (support_compute)
				{
					compute_queue.set_index(static_cast<uint32_t>(queue_family_index));
				}

				if (present_queue.get_index() != vk::QueueFamilyIgnored && ((_enable_graphics && graphic_queue.get_index() != vk::QueueFamilyIgnored) || (_enable_compute && compute_queue.get_index() != vk::QueueFamilyIgnored)))
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
		vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
		vk::PhysicalDeviceVulkan11Features> feature_pnext_chain(
			vk::PhysicalDeviceFeatures2().setFeatures(vk::PhysicalDeviceFeatures().setSamplerAnisotropy(vk::True).setFillModeNonSolid(vk::True)),
			vk::PhysicalDeviceRobustness2FeaturesEXT().setNullDescriptor(vk::True),
			vk::PhysicalDeviceVulkan13Features().setDynamicRendering(vk::True).setSynchronization2(vk::True),
			vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT().setExtendedDynamicState(vk::True),
			vk::PhysicalDeviceVulkan11Features().setShaderDrawParameters(vk::True));

	std::unordered_set<uint32_t> queue_indices = { present_queue.get_index(), graphic_queue.get_index(), compute_queue.get_index() };
	queue_indices.erase(vk::QueueFamilyIgnored);

	float queue_priority = 0.0f;
	auto device_queue_create_info = queue_indices
		| std::views::transform(
			[queue_priority](const auto& index) -> vk::DeviceQueueCreateInfo
			{
				return vk::DeviceQueueCreateInfo({}, index, 1, &queue_priority);
			})
		| std::ranges::to<std::vector>();


	vk::DeviceCreateInfo device_creat_info({}, device_queue_create_info, {}, required_device_extensions, {}, &feature_pnext_chain.get<vk::PhysicalDeviceFeatures2>());

	device = vk::raii::Device(physical_device, device_creat_info);

	if (graphic_queue.get_index() != vk::QueueFamilyIgnored)
	{
		graphic_queue.create(device);
	}
	if (present_queue.get_index() != vk::QueueFamilyIgnored)
	{
		present_queue.create(device);
	}
	if (compute_queue.get_index() != vk::QueueFamilyIgnored)
	{
		compute_queue.create(device);
	}
}

void vulkan_application::pick_msaa_sample_count()
{
	auto physical_device_properties = physical_device.getProperties();
	vk::SampleCountFlags counts = physical_device_properties.limits.framebufferColorSampleCounts & physical_device_properties.limits.framebufferDepthSampleCounts;

	constexpr std::array sample_count_flags = {
		vk::SampleCountFlagBits::e64, vk::SampleCountFlagBits::e32, vk::SampleCountFlagBits::e16,
		vk::SampleCountFlagBits::e8, vk::SampleCountFlagBits::e4, vk::SampleCountFlagBits::e2 };

	auto support_sample_count = std::ranges::find_if(sample_count_flags, [&counts](vk::SampleCountFlagBits sample) { return static_cast<bool>(counts & sample); });

	vulkan_common::MASS_SAMPLE_COUNT = (support_sample_count != sample_count_flags.end()) ? *support_sample_count : vk::SampleCountFlagBits::e1;
}

void vulkan_application::pick_depth_format()
{
	vulkan_common::DEPTH_FORMAT = vulkan_common::find_supported_format(physical_device,
		{ vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint },
		vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment);
}

void vulkan_application::create_swapchain(vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height)
{
	swapchain.create(instance, physical_device, device, _surface, _width, _height);
}

void vulkan_application::recreate_swapchain(uint32_t _width, uint32_t _height)
{
	swapchain.recreate(physical_device, device, _width, _height);
}

void vulkan_application::show_image_on_swapchain(const vk::raii::CommandBuffer& _commandbuffer, vulkan_image& _render_output)
{
	auto old_layout = _render_output.get_layout();

	std::vector<vk::ImageMemoryBarrier2> blit_begin_barrier;
	blit_begin_barrier.emplace_back(_render_output.transition_to_layout(vk::ImageLayout::eTransferSrcOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	blit_begin_barrier.emplace_back(vulkan_image::transition_image_layout(swapchain.get_current_image(), vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	_commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, blit_begin_barrier));

	std::array<vk::Offset3D, 2> srcOffsets = { vk::Offset3D(0, 0, 0), vk::Offset3D(_render_output.get_extent().width, _render_output.get_extent().height, 1) };
	std::array<vk::Offset3D, 2> dstOffsets = { vk::Offset3D(0, 0, 0), vk::Offset3D(swapchain.get_extent().width,swapchain.get_extent().height, 1) };
	vk::ImageBlit2 blit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), srcOffsets, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), dstOffsets);
	_commandbuffer.blitImage2(vk::BlitImageInfo2(_render_output.get_image(), vk::ImageLayout::eTransferSrcOptimal, swapchain.get_current_image(), vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear));

	std::vector<vk::ImageMemoryBarrier2> blit_end_barrier;
	blit_end_barrier.emplace_back(_render_output.transition_to_layout(old_layout, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	blit_end_barrier.emplace_back(vulkan_image::transition_image_layout(swapchain.get_current_image(), vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	_commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, blit_end_barrier));
}

std::pair<vk::Result, vk::Semaphore> vulkan_application::acquire_next_image()
{
	return swapchain.acquire_next_image();
}

vk::Result vulkan_application::present_image()
{
	vk::PresentInfoKHR present_info = swapchain.present_image();
	return present_queue.get_queue().presentKHR(present_info);
}

const vulkan_swapchain& vulkan_application::get_swapchain() const
{
	return swapchain;
}

std::vector<vulkan_commandbuffer> vulkan_application::create_commandbuffers(vk::QueueFlagBits _queue_type, uint32_t _commandbuffer_count) const
{
	vk::CommandBufferAllocateInfo alloc_info(get_command_pool(_queue_type), vk::CommandBufferLevel::ePrimary, _commandbuffer_count);
	std::vector<vk::raii::CommandBuffer> commandbuffers = vk::raii::CommandBuffers(device, alloc_info);

	auto res_commmandbuffers = commandbuffers
		| std::views::transform([this, &_queue_type](vk::raii::CommandBuffer& commandbuffer)
			{
				vk::raii::Fence fence(device, vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
				return vulkan_commandbuffer(commandbuffer, fence, &device, &get_queue(_queue_type)); })
		| std::ranges::to<std::vector>();

	return res_commmandbuffers;
}

void vulkan_application::init(const std::vector<const char*>& _instance_layers, const std::vector<const char*>& _instance_extensions, vk::InstanceCreateFlags _flags)
{
	create_instance(_instance_layers, _instance_extensions, {});
}

void vulkan_application::create(vk::SurfaceKHR _surface, bool _enable_graphics, bool _enable_compute, uint32_t _width, uint32_t _height)
{
	pick_physical_device_and_queue_family(_surface, _enable_graphics, _enable_compute);
	create_device_and_queue();
	create_swapchain(_surface, _width, _height);
	pick_msaa_sample_count();
	pick_depth_format();
}

void vulkan_application::resize(uint32_t _width, uint32_t _height)
{
	recreate_swapchain(_width, _height);
}

void vulkan_application::wait_idle()
{
	device.waitIdle();
}


const vk::raii::Instance& vulkan_application::get_instance() const
{
	return instance;
}

const vk::raii::PhysicalDevice& vulkan_application::get_physical_device() const
{
	return physical_device;
}

const vk::raii::Device& vulkan_application::get_device() const
{
	return device;
}

const vk::raii::Queue& vulkan_application::get_queue(vk::QueueFlagBits _queue_type) const
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
		return compute_queue.get_queue();
	case vk::QueueFlagBits::eGraphics:
		return graphic_queue.get_queue();
	default:
		break;
	}
	return present_queue.get_queue();
}

const vk::raii::CommandPool& vulkan_application::get_command_pool(vk::QueueFlagBits _queue_type) const
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
		return compute_queue.get_command_pool();
	case vk::QueueFlagBits::eGraphics:
		return graphic_queue.get_command_pool();
	default:
		break;
	}
	return present_queue.get_command_pool();
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