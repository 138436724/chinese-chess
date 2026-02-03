#pragma once
#define VK_USE_PLATFORM_WIN32_KHR

#include <vulkan/vulkan_raii.hpp>

class vulkan_swapchain
{
public:
	vulkan_swapchain() = default;
	~vulkan_swapchain() = default;
	vulkan_swapchain(vulkan_swapchain&) = delete;
	vulkan_swapchain(vulkan_swapchain&& _other) noexcept;
	vulkan_swapchain& operator=(vulkan_swapchain&) = delete;
	vulkan_swapchain& operator=(vulkan_swapchain&& _other) noexcept;

	void create(const vk::raii::Instance& _instance, const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, vk::SurfaceKHR _surface, uint32_t _width, uint32_t _height);
	void recreate(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, uint32_t _width, uint32_t _height);
	std::pair<vk::Result, vk::Semaphore> acquire_next_image();
	[[nodiscard("present info need submit!")]]
	vk::PresentInfoKHR present_image();

	const vk::raii::SwapchainKHR& get_swapchain() const noexcept;
	vk::Extent2D get_extent() const noexcept;
	vk::Format get_format() const noexcept;
	const vk::Image get_current_image() const noexcept;
	const vk::raii::ImageView& get_current_imageview() const noexcept;
	const vk::raii::Semaphore& get_current_waited_semaphore() const noexcept;

private:
	vk::SurfaceCapabilitiesKHR surface_capabilities = {};
	vk::raii::SurfaceKHR surface = nullptr;
	vk::raii::SwapchainKHR swapchain = nullptr;
	vk::Format format = vk::Format::eUndefined;
	vk::Extent2D extent = vk::Extent2D{ 0, 0 };
	vk::PresentModeKHR present_mode = vk::PresentModeKHR::eImmediate;
	std::vector<vk::Image> images;
	std::vector<vk::raii::ImageView> imageviews;

	uint32_t current_index = 0;
	uint32_t max_index = 0;
	std::vector<vk::raii::Semaphore> present_used;
	std::vector<vk::raii::Semaphore> present_waited;
};