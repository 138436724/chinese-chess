#pragma once

#include "vulkan_buffer.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_recycle_bin.h"
#include "vulkan_descriptor.h"
#include "vulkan_image.h"
#include "vulkan_pipeline.h"
#include "vulkan_queue.h"
#include "vulkan_semaphore.h"
#include "vulkan_swapchain.h"
#include <vector>
#include <vulkan/vulkan_raii.hpp>

class vulkan_application
{
public:
	vulkan_application() = default;
	~vulkan_application() = default;
	vulkan_application(const vulkan_application&) = delete;
	vulkan_application& operator=(const vulkan_application&) = delete;
	vulkan_application(const vulkan_application&&) = delete;
	vulkan_application& operator=(const vulkan_application&&) = delete;

	// public to use
	void init(const std::vector<const char*>& _instance_layers, const std::vector<const char*>& _instance_extensions, vk::InstanceCreateFlags _flags = {});
	void create(vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height);
	void resize(uint32_t _width, uint32_t _height);
	void begin() noexcept;
	void render(std::vector<vk::SemaphoreSubmitInfo>&& _waited_info);
	void end(bool _immediately);
	void wait() const;

	// frame
	void bind_image(vulkan_image* _scene_image, vulkan_image* _ui_image);

	// other function
	void save_image(vulkan_image& _image);

	// getters
	const vk::raii::Instance& get_instance() const noexcept;
	const vk::raii::PhysicalDevice& get_physical_device() const noexcept;
	const vk::raii::Device& get_device() const noexcept;
	const vma::raii::Allocator& get_allocator() const noexcept;
	uint32_t get_queue_index(vk::QueueFlagBits _queue_type) const noexcept;
	const vulkan_swapchain& get_swapchain() const noexcept;
	vulkan_semaphore* get_semaphore_ptr() noexcept;
	vulkan_recycle_bin* get_recycle_bin_ptr() noexcept;

private:
	// use in `init` and `create`
	void create_instance(const std::vector<const char*>& _instance_layers, const std::vector<const char*>& _instance_extensions, vk::InstanceCreateFlags _flags = {});
	void pick_physical_device_and_queue_family(vk::SurfaceKHR _surface);
	void create_device_and_queue();

	void pick_msaa_sample_count() const noexcept;
	void pick_depth_format() const noexcept;

	void create_pipeline();

#ifndef NDEBUG
	static VKAPI_ATTR vk::Bool32 VKAPI_CALL debug_callback(vk::DebugUtilsMessageSeverityFlagBitsEXT _severity, vk::DebugUtilsMessageTypeFlagsEXT _type, const vk::DebugUtilsMessengerCallbackDataEXT* _pCallbackData, void*);
#endif // NDEBUG

	std::vector<const char*> required_instance_layers;
	std::vector<const char*> required_instance_extensions;

	vk::raii::Context context;
	vk::raii::Instance instance = nullptr;
	vk::raii::DebugUtilsMessengerEXT debug_messenger = nullptr;

	vk::raii::PhysicalDevice physical_device = nullptr;
	vk::raii::Device device = nullptr;
	vma::raii::Allocator allocator = nullptr;

	std::vector<const char*> required_device_extensions = {
		vk::KHRSwapchainExtensionName,
		vk::KHRSpirv14ExtensionName,
		vk::KHRSynchronization2ExtensionName,
		vk::KHRCreateRenderpass2ExtensionName,
		vk::KHRAccelerationStructureExtensionName,
		vk::KHRRayTracingPipelineExtensionName,
		//vk::KHRRayQueryExtensionName,
		vk::KHRDeferredHostOperationsExtensionName,
		vk::KHRBufferDeviceAddressExtensionName,
		vk::KHRPushDescriptorExtensionName
	};

	uint32_t graphic_index = vk::QueueFamilyIgnored;
	uint32_t compute_index = vk::QueueFamilyIgnored;
	uint32_t transfer_index = vk::QueueFamilyIgnored;
	uint32_t present_index = vk::QueueFamilyIgnored;

	uint32_t current_frame = 0;

	vulkan_queue graphic_queue;
	vulkan_queue transfer_queue;

	vulkan_swapchain swapchain;

	vulkan_pipeline pipeline;
	vulkan_descriptor descriptor;

	vulkan_buffer ocio_ubo;
	std::vector<vulkan_image> ocio_images;
	std::vector<vk::raii::Sampler> ocio_samplers;

	vulkan_image* bind_scene_image = nullptr;
	vulkan_image* bind_ui_image = nullptr;
	vk::raii::Sampler image_sampler = nullptr;

	vulkan_semaphore semaphore;
	vulkan_recycle_bin recycle_bin;
	std::vector<vulkan_commandbuffer> commandbuffers;
};