#pragma once

#include <vulkan/vulkan_raii.hpp>

class vulkan_queue
{
public:
	vulkan_queue() = default;
	~vulkan_queue() = default;
	vulkan_queue(vulkan_queue&) = delete;
	vulkan_queue(vulkan_queue&& _other) noexcept;
	vulkan_queue& operator=(vulkan_queue&) = delete;
	vulkan_queue& operator=(vulkan_queue&& _other) noexcept;

	void create(const vk::raii::Device& _device, uint32_t _index);

	void set_index(uint32_t _index) noexcept;
	uint32_t get_index() const noexcept;
	const vk::raii::Queue& get_queue() const noexcept;
	const vk::raii::CommandPool& get_command_pool() const noexcept;

private:
	uint32_t index = vk::QueueFamilyIgnored;
	vk::raii::Queue queue = nullptr;
	vk::raii::CommandPool command_pool = nullptr;
};