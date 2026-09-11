#define VMA_IMPLEMENTATION

#include "vulkan_buffer.h"

#include "vulkan_common.h"

#include <bit>
#include <stdexcept>
#include <utility>

vulkan_buffer::vulkan_buffer(vulkan_buffer&& _other) noexcept
    : stage(std::exchange(_other.stage, {}))
    , access(std::exchange(_other.access, {}))
    , queue(std::exchange(_other.queue, vk::QueueFamilyIgnored))
    , buffer(std::exchange(_other.buffer, nullptr))
    , buffer_size(std::exchange(_other.buffer_size, 0))
    , buffer_address(std::exchange(_other.buffer_address, {}))
{
}

vulkan_buffer& vulkan_buffer::operator=(vulkan_buffer&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(stage, _other.stage);
        std::ranges::swap(access, _other.access);
        std::ranges::swap(queue, _other.queue);
        std::ranges::swap(buffer, _other.buffer);
        std::ranges::swap(buffer_size, _other.buffer_size);
        std::ranges::swap(buffer_address, _other.buffer_address);
    }
    return *this;
}

void vulkan_buffer::create(const vma::raii::Allocator& _allocator,
                           const vk::raii::Device&     _device,
                           const vk::BufferCreateInfo& _buffer_info,
                           vma::MemoryUsage            _usage,
                           vma::AllocationCreateFlags  _flags,
                           const std::string&          _name)
{
    if (_buffer_info.queueFamilyIndexCount != 1 || _buffer_info.sharingMode != vk::SharingMode::eExclusive)
    {
        throw std::runtime_error("Only support Exclusive mode!");
    }

    queue       = *_buffer_info.pQueueFamilyIndices;
    buffer_size = _buffer_info.size;

    const bool host_visible = static_cast<bool>(
        _flags & (vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eHostAccessRandom));

    buffer = _allocator.createBuffer(
        _buffer_info, vma::AllocationCreateInfo(host_visible ? _flags | vma::AllocationCreateFlagBits::eMapped : _flags, _usage));

    if (host_visible)
    {
        buffer_address = buffer.getAllocation().getInfo().pMappedData;
    }
    else
    {
        buffer_address = _device.getBufferAddress(vk::BufferDeviceAddressInfo(buffer));
    }

#ifndef NDEBUG
    _device.setDebugUtilsObjectNameEXT(vk::DebugUtilsObjectNameInfoEXT(
        buffer.objectType, std::bit_cast<uint64_t>(static_cast<VkBuffer>(*buffer)), std::format("Buffer{}", _name).c_str()));
#else
    (void)_name;
#endif  // !NDEBUG
}

void vulkan_buffer::flush() const
{
    buffer.getAllocation().flush(0, VK_WHOLE_SIZE);
}

void vulkan_buffer::invalidate() const
{
    buffer.getAllocation().invalidate(0, VK_WHOLE_SIZE);
}

void vulkan_buffer::set_info(const vk::BufferMemoryBarrier2& _barrier)
{
    if (_barrier.buffer != *buffer)
    {
        throw std::runtime_error("The barrier not used by this buffer!");
    }
    if ((_barrier.srcQueueFamilyIndex == vk::QueueFamilyIgnored) != (_barrier.dstQueueFamilyIndex == vk::QueueFamilyIgnored))
    {
        throw std::runtime_error("Queue must all ignore or all set new value!");
    }

    stage  = _barrier.dstStageMask;
    access = _barrier.dstAccessMask;
    queue  = _barrier.dstQueueFamilyIndex == vk::QueueFamilyIgnored ? queue : _barrier.dstQueueFamilyIndex;
}

vk::PipelineStageFlags2 vulkan_buffer::get_stage() const noexcept
{
    return stage;
}

vk::AccessFlags2 vulkan_buffer::get_access() const noexcept
{
    return access;
}

uint32_t vulkan_buffer::get_queue() const noexcept
{
    return queue;
}

const vk::raii::Buffer& vulkan_buffer::get_buffer() const noexcept
{
    return buffer;
}

vk::DeviceOrHostAddressKHR vulkan_buffer::get_buffer_address() const noexcept
{
    return buffer_address;
}

vk::DeviceSize vulkan_buffer::get_size() const noexcept
{
    return buffer_size;
}

void vulkan_buffer::copy_buffer_to_buffer(const vk::raii::CommandBuffer& _commandbuffer,
                                          const vk::Buffer&              _src_buffer,
                                          const vk::Buffer&              _dst_buffer,
                                          const vk::BufferCopy2&         _copy_info) noexcept
{
    _commandbuffer.copyBuffer2(vk::CopyBufferInfo2(_src_buffer, _dst_buffer, _copy_info));
}

void vulkan_buffer::copy_buffer_to_image(const vk::raii::CommandBuffer& _commandbuffer,
                                         const vk::Buffer&              _buffer,
                                         const vk::Image&               _image,
                                         const vk::BufferImageCopy2&    _copy_info) noexcept
{
    _commandbuffer.copyBufferToImage2(vk::CopyBufferToImageInfo2(_buffer, _image, vk::ImageLayout::eTransferDstOptimal, _copy_info));
}
