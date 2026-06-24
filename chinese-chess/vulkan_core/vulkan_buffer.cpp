#define VMA_IMPLEMENTATION

#include "vulkan_buffer.h"

vulkan_buffer::vulkan_buffer(vulkan_buffer&& _other) noexcept
	:buffer(std::exchange(_other.buffer, nullptr)),
	buffer_address(std::exchange(_other.buffer_address, {}))
{
}

vulkan_buffer& vulkan_buffer::operator=(vulkan_buffer&& _other) noexcept
{
	if (this != &_other)
	{
		std::ranges::swap(buffer, _other.buffer);
		std::ranges::swap(buffer_address, _other.buffer_address);
	}
	return *this;
}

void vulkan_buffer::create(const vma::raii::Allocator& _allocator, const vk::raii::Device& _device, vk::DeviceSize _buffer_size, vk::BufferUsageFlags _buffer_usage, vk::MemoryPropertyFlags _properties)
{
	vk::BufferCreateInfo buffer_info({}, _buffer_size, _buffer_usage, vk::SharingMode::eExclusive);

	vma::AllocationCreateInfo create_info{};
	if (_properties & vk::MemoryPropertyFlagBits::eHostVisible)
	{
		create_info.setFlags(vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped)
			.setUsage(vma::MemoryUsage::eAutoPreferHost);
	}
	else if (_properties & vk::MemoryPropertyFlagBits::eDeviceLocal)
	{
		create_info.setFlags(vma::AllocationCreateFlagBits::eDedicatedMemory /*| vma::AllocationCreateFlagBits::eMapped*/)
			.setUsage(vma::MemoryUsage::eGpuOnly);
	}
	else
	{
		create_info.setUsage(vma::MemoryUsage::eAuto);
	}

	buffer = _allocator.createBuffer(buffer_info, create_info);

	if (_properties & vk::MemoryPropertyFlagBits::eHostVisible)
	{
		buffer_address = buffer.getAllocation().getInfo().pMappedData;
	}
	else if (_properties & vk::MemoryPropertyFlagBits::eDeviceLocal)
	{
		buffer_address = _device.getBufferAddress(vk::BufferDeviceAddressInfo(buffer));
	}
}

void vulkan_buffer::clear() noexcept
{
	buffer_address = nullptr;
	buffer.clear();
}

const vk::raii::Buffer& vulkan_buffer::get_buffer() const noexcept
{
	return buffer;
}

vk::DeviceOrHostAddressKHR vulkan_buffer::get_buffer_address() const noexcept
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
