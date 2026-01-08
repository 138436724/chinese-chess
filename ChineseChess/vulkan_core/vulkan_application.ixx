export module vulkan_application;

import <vulkan/vulkan_core.h>;
import std;
import vulkan_hpp;
import vulkan_queue;
import vulkan_image;
import vulkan_common;
import vulkan_swapchain;
import vulkan_commandbuffer;

export class vulkan_application
{
public:
	vulkan_application() = default;
	~vulkan_application() = default;
	vulkan_application(const vulkan_application&) = delete;
	vulkan_application& operator=(const vulkan_application&) = delete;
	vulkan_application(const vulkan_application&&) = delete;
	vulkan_application& operator=(const vulkan_application&&) = delete;

	// init application functions
	void create_instance(const std::vector<const char*>& _instance_layers, const std::vector<const char*>& _instance_extensions, vk::InstanceCreateFlags _flags = {});
	void pick_physical_device_and_queue_family(vk::SurfaceKHR _surface, bool _enable_graphics, bool _enable_compute);
	void create_device_and_queue();

	void pick_msaa_sample_count();
	void pick_depth_format();

	// about swapchain
	void create_swapchain(vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height);
	void recreate_swapchain(uint32_t _width, uint32_t _height);
	void show_image_on_swapchain(const vk::raii::CommandBuffer& _commandbuffer, vulkan_image& _render_output);
	std::pair<vk::Result, vk::Semaphore> acquire_next_image();
	vk::Result present_image();
	const vulkan_swapchain& get_swapchain() const;

	// create command buffer
	std::vector<vulkan_commandbuffer> create_commandbuffers(vk::QueueFlagBits _queue_type, uint32_t _commandbuffer_count) const;

	// public to out to use
	void init(const std::vector<const char*>& _instance_layers, const std::vector<const char*>& _instance_extensions, vk::InstanceCreateFlags _flags = {});
	void create(vk::SurfaceKHR _surface, bool _enable_graphics, bool _enable_compute, uint32_t _width, uint32_t _height);
	void resize(uint32_t _width, uint32_t _height);
	void wait_idle();

	// get functions
	const vk::raii::Instance& get_instance() const;
	const vk::raii::PhysicalDevice& get_physical_device() const;
	const vk::raii::Device& get_device() const;
	const vk::raii::Queue& get_queue(vk::QueueFlagBits _queue_type) const; // default is present
	const vk::raii::CommandPool& get_command_pool(vk::QueueFlagBits _queue_type) const; // default is present

	// other to use
	void save_image(vulkan_image& _image);

#ifndef NDEBUG
	static VKAPI_ATTR vk::Bool32 VKAPI_CALL debug_callback(vk::DebugUtilsMessageSeverityFlagBitsEXT _severity, vk::DebugUtilsMessageTypeFlagsEXT _type, const vk::DebugUtilsMessengerCallbackDataEXT* _pCallbackData, void*);
#endif // NDEBUG

private:
	std::vector<const char*> required_instance_layers;
	std::vector<const char*> required_instance_extensions;

	vk::raii::Context context;
	vk::raii::Instance instance = nullptr;
	vk::raii::DebugUtilsMessengerEXT debug_messenger = nullptr;

	vk::raii::PhysicalDevice physical_device = nullptr;

	vk::raii::Device device = nullptr;

	std::vector<const char*> required_device_extensions = {
		vk::KHRSwapchainExtensionName,
		vk::KHRSpirv14ExtensionName,
		vk::KHRSynchronization2ExtensionName,
		vk::KHRCreateRenderpass2ExtensionName,
	};

	vulkan_queue graphic_queue;
	vulkan_queue present_queue;
	vulkan_queue compute_queue;

	vulkan_swapchain swapchain;
};