/**
 * @file vulkan_buffer.cpp
 * @brief vulkan_buffer 的实现，同时是全工程唯一展开 VMA 实现的编译单元。
 * @details include 之前先定义 VMA_IMPLEMENTATION，整个工程只在这里展开一次 VMA。
 *          create 校验独占模式后按 flags 决定缓存映射指针还是设备地址；transition_state
 *          用追踪的旧状态作 src、入参作 dst 生成 barrier，返回前就把追踪字段改成目标值，
 *          因此录制 barrier 的顺序与状态更新顺序一致。
 */

#define VMA_IMPLEMENTATION

#include "vulkan_buffer.h"

#include <bit>
#include <concepts>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>

/**
 * @brief 逐成员 std::exchange：状态、句柄、大小与地址移到新对象，源对象留下空值。
 */
vulkan_buffer::vulkan_buffer(vulkan_buffer&& _other) noexcept
    : stage(std::exchange(_other.stage, {}))
    , access(std::exchange(_other.access, {}))
    , queue(std::exchange(_other.queue, vk::QueueFamilyIgnored))
    , buffer(std::exchange(_other.buffer, nullptr))
    , buffer_size(std::exchange(_other.buffer_size, 0))
    , buffer_address(std::exchange(_other.buffer_address, {}))
{
}

/**
 * @brief 自赋值提前返回，否则逐成员 std::ranges::swap，两边都可安全析构。
 */
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

/**
 * @brief 校验独占模式与单 queue family，分配 VMA 缓冲并缓存 queue family、大小与地址。
 * @note 含 HOST_ACCESS 位时自动补 eMapped，并把 pMappedData 当作 buffer_address；否则查 vkGetBufferAddress。
 * @note 重复调用会先用新分配覆盖旧句柄，旧内存随之由 VMA 释放，不做任何等待。
 */
void vulkan_buffer::create(const vma::raii::Allocator&         _allocator,
                           const vk::raii::Device&             _device,
                           const vk::BufferCreateInfo&         _buffer_info,
                           vma::MemoryUsage                    _usage,
                           vma::AllocationCreateFlags          _flags,
                           [[maybe_unused]] const std::string& _name)
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
#endif  // !NDEBUG
}

/**
 * @brief 对整块分配调 VMA 的 flush，把 CPU 写刷到设备可见。
 * @note 只对 host 可见且已映射的分配有意义。
 */
void vulkan_buffer::flush() const
{
    buffer.getAllocation().flush(0, vk::WholeSize);
}

/**
 * @brief 对整块分配调 VMA 的 invalidate，让后续 CPU 读取看到 GPU 写入。
 * @note 与 flush 对称，同样只适用于已映射的 host 可见分配。
 */
void vulkan_buffer::invalidate() const
{
    buffer.getAllocation().invalidate(0, vk::WholeSize);
}

/**
 * @brief 用追踪状态作 src、入参作 dst 生成 buffer barrier，并把三个追踪字段更新为目标值。
 * @note 只有目标 queue 有效且与当前不同才填 src/dstQueueFamilyIndex；传 QueueFamilyIgnored 时保持原 family。
 * @note 返回的 barrier 必须被 pipelineBarrier2 录制，否则追踪状态会领先于 GPU 实际状态。
 */
vk::BufferMemoryBarrier2 vulkan_buffer::transition_state(vk::PipelineStageFlags2 _stage, vk::AccessFlags2 _access, uint32_t _queue /*= vk::QueueFamilyIgnored*/)
{
    auto barrier = vk::BufferMemoryBarrier2(stage, access, _stage, _access, vk::QueueFamilyIgnored,
                                            vk::QueueFamilyIgnored, buffer, 0, vk::WholeSize);

    if (_queue != vk::QueueFamilyIgnored && _queue != queue)
    {
        barrier.srcQueueFamilyIndex = queue;
        barrier.dstQueueFamilyIndex = _queue;
    }

    stage  = _stage;
    access = _access;
    queue  = _queue == vk::QueueFamilyIgnored ? queue : _queue;

    return barrier;
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

/**
 * @brief 录一条 vkCmdCopyBuffer2，区域与大小全由 _copy_info 决定。
 * @note 两端需带 eTransferSrc/eTransferDst usage，并由调用方同步传输读写；buffer 不存在 image layout 转换。
 */
void vulkan_buffer::copy_buffer_to_buffer(const vk::raii::CommandBuffer& _commandbuffer,
                                          const vk::Buffer&              _src_buffer,
                                          const vk::Buffer&              _dst_buffer,
                                          const vk::BufferCopy2&         _copy_info) noexcept
{
    _commandbuffer.copyBuffer2(vk::CopyBufferInfo2(_src_buffer, _dst_buffer, _copy_info));
}

/**
 * @brief 录一条 vkCmdCopyBufferToImage2，目标布局在内部固定为 eTransferDstOptimal。
 * @note 签名里没有布局参数：调用方须先录入并正确同步布局屏障；仅调用 transition_state 不会执行布局转换。
 */
void vulkan_buffer::copy_buffer_to_image(const vk::raii::CommandBuffer& _commandbuffer,
                                         const vk::Buffer&              _buffer,
                                         const vk::Image&               _image,
                                         const vk::BufferImageCopy2&    _copy_info) noexcept
{
    _commandbuffer.copyBufferToImage2(vk::CopyBufferToImageInfo2(_buffer, _image, vk::ImageLayout::eTransferDstOptimal, _copy_info));
}
