#pragma once

#include <vulkan/vulkan_raii.hpp>

class vulkan_buffer
{
public:
	vulkan_buffer() = default;
	~vulkan_buffer() = default;
	vulkan_buffer(vulkan_buffer&) = delete;
	vulkan_buffer(vulkan_buffer&& _other) noexcept;
	vulkan_buffer& operator=(vulkan_buffer&) = delete;
	vulkan_buffer& operator=(vulkan_buffer&& _other) noexcept;

	void create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, vk::DeviceSize _buffer_size, vk::BufferUsageFlags _buffer_usage, vk::MemoryPropertyFlags _properties);
	void clear() noexcept;

	const vk::raii::Buffer& get_buffer() const noexcept;
	vk::DeviceOrHostAddressKHR get_buffer_address() const noexcept;

	static void copy_buffer_to_buffer(const vk::raii::CommandBuffer& _commandbuffer, const vk::Buffer& _src_buffer, const vk::Buffer& _dst_buffer, const vk::BufferCopy2& _copy_info) noexcept;
	static void copy_buffer_to_image(const vk::raii::CommandBuffer& _commandbuffer, const vk::Buffer& _buffer, const vk::Image& _image, const vk::BufferImageCopy2& _copy_info) noexcept;

private:
	vk::raii::Buffer buffer = nullptr;
	vk::raii::DeviceMemory buffer_memory = nullptr;
	vk::DeviceOrHostAddressKHR buffer_address = nullptr;
};