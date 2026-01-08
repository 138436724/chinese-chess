export module vulkan_buffer;

import std;
import vulkan_hpp;

export class vulkan_buffer
{
public:
	vulkan_buffer() = default;
	~vulkan_buffer() = default;
	vulkan_buffer(vulkan_buffer&) = delete;
	vulkan_buffer(vulkan_buffer&& _other) noexcept;
	vulkan_buffer& operator=(vulkan_buffer&) = delete;
	vulkan_buffer& operator=(vulkan_buffer&& _other) noexcept;

	void create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, vk::DeviceSize _buffer_size, vk::BufferUsageFlags _buffer_usage, vk::MemoryPropertyFlags _properties);

	const vk::raii::Buffer& get_buffer() const;
	void* get_buffer_address() const;

	static void copy_buffer_to_buffer(const vk::raii::CommandBuffer& _commandbuffer, const vk::Buffer& _src_buffer, const vk::Buffer& _dst_buffer, uint64_t _src_offset, uint64_t _dst_offset, uint64_t _copy_size);
	static void copy_buffer_to_image(const vk::raii::CommandBuffer& _commandbuffer, const vk::Buffer& _buffer, const vk::Image& _image, const vk::ImageSubresourceLayers& _subresource_layers, const vk::Offset3D& _image_offset, const vk::Extent3D& _image_extent);

private:
	vk::raii::Buffer buffer = nullptr;
	vk::raii::DeviceMemory buffer_memory = nullptr;
	void* buffer_address = nullptr;
};
