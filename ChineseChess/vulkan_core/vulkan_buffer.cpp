module vulkan_buffer;

import vulkan_common;

vulkan_buffer::vulkan_buffer(vulkan_buffer&& _other) noexcept
	:buffer(std::move(_other.buffer)),
	buffer_memory(std::move(_other.buffer_memory)),
	buffer_address(std::move(_other.buffer_address))
{
}

vulkan_buffer& vulkan_buffer::operator=(vulkan_buffer&& _other) noexcept
{
	if (this != &_other)
	{
		std::swap(buffer, _other.buffer);
		std::swap(buffer_memory, _other.buffer_memory);
		std::swap(buffer_address, _other.buffer_address);
	}
	return *this;
}

void vulkan_buffer::create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, vk::DeviceSize _buffer_size, vk::BufferUsageFlags _buffer_usage, vk::MemoryPropertyFlags _properties)
{
	vk::BufferCreateInfo buffer_info({}, _buffer_size, _buffer_usage, vk::SharingMode::eExclusive);
	buffer = vk::raii::Buffer(_device, buffer_info);

	vk::MemoryRequirements memory_requirements = buffer.getMemoryRequirements();
	vk::MemoryAllocateInfo memory_info(memory_requirements.size, vulkan_common::find_memory_type(_physical_device, memory_requirements.memoryTypeBits, _properties));
	buffer_memory = vk::raii::DeviceMemory(_device, memory_info);

	buffer.bindMemory(*buffer_memory, 0);

	if (_properties & vk::MemoryPropertyFlagBits::eHostVisible)
	{
		buffer_address = buffer_memory.mapMemory(0, _buffer_size);
	}

}

const vk::raii::Buffer& vulkan_buffer::get_buffer() const
{
	return buffer;
}

void* vulkan_buffer::get_buffer_address() const
{
	return buffer_address;
}

void vulkan_buffer::copy_buffer_to_buffer(const vk::raii::CommandBuffer& commandbuffer, const vk::Buffer& _src_buffer, const vk::Buffer& _dst_buffer, uint64_t _src_offset, uint64_t _dst_offset, uint64_t _copy_size)
{
	vk::BufferCopy2 copy_regions(_src_offset, _dst_offset, _copy_size);
	return commandbuffer.copyBuffer2(vk::CopyBufferInfo2(_src_buffer, _dst_buffer, copy_regions));
}

void vulkan_buffer::copy_buffer_to_image(const vk::raii::CommandBuffer& commandbuffer, const vk::Buffer& _buffer, const vk::Image& _image, const vk::ImageSubresourceLayers& _subresource_layers, const vk::Offset3D& _image_offset, const vk::Extent3D& _image_extent)
{
	vk::BufferImageCopy2 copy_regions(0, 0, 0, _subresource_layers, _image_offset, _image_extent);
	commandbuffer.copyBufferToImage2(vk::CopyBufferToImageInfo2(_buffer, _image, vk::ImageLayout::eTransferDstOptimal, copy_regions));
}
