#define VMA_IMPLEMENTATION

#include "vulkan_buffer.h"

#include "vulkan_common.h"

#include <bit>

vulkan_buffer::vulkan_buffer(vulkan_buffer&& _other) noexcept
    : stage(std::exchange(_other.stage, {}))
    , access(std::exchange(_other.access, {}))
    , queue(std::exchange(_other.queue, vk::QueueFamilyIgnored))
    , buffer(std::exchange(_other.buffer, nullptr))
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
        std::ranges::swap(buffer_address, _other.buffer_address);
    }
    return *this;
}

void vulkan_buffer::create(const vma::raii::Allocator& _allocator,
                           const vk::raii::Device&     _device,
                           const vk::BufferCreateInfo& _buffer_info,
                           vma::MemoryUsage            _usage,
                           const std::string&          _name)
{
    if (_buffer_info.queueFamilyIndexCount != 1 || _buffer_info.sharingMode != vk::SharingMode::eExclusive)
    {
        throw std::runtime_error("Only support Exclusive mode!");
    }

    queue = *_buffer_info.pQueueFamilyIndices;

    vma::AllocationCreateInfo create_info{};
    create_info.setUsage(_usage);

    if (vulkan_common::is_host_accessible_usage(_usage))
    {
        const vma::AllocationCreateFlagBits flags = _usage == vma::MemoryUsage::eGpuToCpu ?
                                                        vma::AllocationCreateFlagBits::eHostAccessRandom :
                                                        vma::AllocationCreateFlagBits::eHostAccessSequentialWrite;

        create_info.setFlags(flags | vma::AllocationCreateFlagBits::eMapped);
    }

    buffer = _allocator.createBuffer(_buffer_info, create_info);

    if (vulkan_common::is_host_accessible_usage(_usage))
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
