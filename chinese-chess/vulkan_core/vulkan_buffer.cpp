#include "vulkan_buffer.h"
#include "vulkan_common.h"

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

	if (_properties & vk::MemoryPropertyFlagBits::eHostVisible)
	{
		buffer_memory = vk::raii::DeviceMemory(_device, memory_info);
	}
	else if (_properties & vk::MemoryPropertyFlagBits::eDeviceLocal)
	{
		vk::StructureChain<vk::MemoryAllocateInfo, vk::MemoryAllocateFlagsInfo> memory_info_chain(memory_info, vk::MemoryAllocateFlagsInfo(vk::MemoryAllocateFlagBits::eDeviceAddress));
		buffer_memory = vk::raii::DeviceMemory(_device, memory_info_chain.get<vk::MemoryAllocateInfo>());
	}

	buffer.bindMemory(*(buffer_memory), 0);

	if (_properties & vk::MemoryPropertyFlagBits::eHostVisible)
	{
		buffer_address = buffer_memory.mapMemory(0, _buffer_size);
	}
	else if (_properties & vk::MemoryPropertyFlagBits::eDeviceLocal)
	{
		buffer_address = _device.getBufferAddress(vk::BufferDeviceAddressInfo(buffer));
	}
}

const vk::raii::Buffer& vulkan_buffer::get_buffer() const noexcept
{
	return buffer;
}

const vk::DeviceOrHostAddressKHR& vulkan_buffer::get_buffer_address() const noexcept
{
	return buffer_address;
}

void vulkan_buffer::copy_buffer_to_buffer(const vk::raii::CommandBuffer& _commandbuffer, const vk::Buffer& _src_buffer, const vk::Buffer& _dst_buffer, const vk::BufferCopy2& _copy_info) noexcept
{
	_commandbuffer.copyBuffer2(vk::CopyBufferInfo2(_src_buffer, _dst_buffer, _copy_info));
}

void vulkan_buffer::copy_buffer_to_image(const vk::raii::CommandBuffer& _commandbuffer, const vk::Buffer& _buffer, const vk::Image& _image, const vk::BufferImageCopy2& _copy_info) noexcept
{
	_commandbuffer.copyBufferToImage2(vk::CopyBufferToImageInfo2(_buffer, _image, vk::ImageLayout::eTransferDstOptimal, _copy_info));
}
