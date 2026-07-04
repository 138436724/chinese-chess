#pragma once

#include <vulkan/vulkan_raii.hpp>

class vulkan_device
{
public:
	vulkan_device() = default;
	~vulkan_device() = default;
	vulkan_device(vulkan_device&) = delete;
	vulkan_device(vulkan_device&& _other) noexcept;
	vulkan_device& operator=(vulkan_device&) = delete;
	vulkan_device& operator=(vulkan_device&& _other) noexcept;

	void create(const vk::raii::PhysicalDevice& _physical_device, const std::span<uint32_t const> _queues, const std::span<const char* const> _extensions, const vk::PhysicalDeviceFeatures2& _features);
	
	const vk::raii::Device& operator*() const noexcept;

private:
	vk::raii::Device device = nullptr;
};
