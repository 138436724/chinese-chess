/**
 * @file vulkan_physical_device.cpp
 * @brief vulkan_physical_device 的实现：移动语义与队列族下标查表。
 * @details 设备选择逻辑位于头文件的模板 create 中，本文件只保存移动或交换后的结果。
 *          队列族下标默认是 vk::QueueFamilyIgnored，未匹配的能力位一律返回该值。
 */
#include "vulkan_physical_device.h"

#include <concepts>
#include <cstdint>
#include <utility>

/**
 * @brief 移动构造：用 exchange 转移句柄，三个队列族下标复位为 QueueFamilyIgnored。
 */
vulkan_physical_device::vulkan_physical_device(vulkan_physical_device&& _other) noexcept
    : physical_device(std::exchange(_other.physical_device, nullptr))
    , graphic_index(std::exchange(_other.graphic_index, vk::QueueFamilyIgnored))
    , compute_index(std::exchange(_other.compute_index, vk::QueueFamilyIgnored))
    , transfer_index(std::exchange(_other.transfer_index, vk::QueueFamilyIgnored))
{
}

/**
 * @brief 移动赋值：句柄与三个队列族下标逐成员与源对象交换。
 */
vulkan_physical_device& vulkan_physical_device::operator=(vulkan_physical_device&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(physical_device, _other.physical_device);
        std::ranges::swap(graphic_index, _other.graphic_index);
        std::ranges::swap(compute_index, _other.compute_index);
        std::ranges::swap(transfer_index, _other.transfer_index);
    }
    return *this;
}

/**
 * @brief 按能力位返回 create 时缓存的队列族下标。
 * @note 只识别 Graphics / Compute / Transfer；OpticalFlow、Video、Protected、SparseBinding 等一律
 *       返回 vk::QueueFamilyIgnored，调用方需自行判断下标是否有效。
 */
uint32_t vulkan_physical_device::get_queue_index(vk::QueueFlagBits _queue_type) const noexcept
{
    switch (_queue_type)
    {
        case vk::QueueFlagBits::eOpticalFlowNV:
            break;
        case vk::QueueFlagBits::eVideoEncodeKHR:
            break;
        case vk::QueueFlagBits::eVideoDecodeKHR:
            break;
        case vk::QueueFlagBits::eProtected:
            break;
        case vk::QueueFlagBits::eSparseBinding:
            break;
        case vk::QueueFlagBits::eTransfer:
            return transfer_index;
        case vk::QueueFlagBits::eCompute:
            return compute_index;
        case vk::QueueFlagBits::eGraphics:
            return graphic_index;
        default:
            break;
    }
    return vk::QueueFamilyIgnored;
}

/**
 * @brief 解引用直接取内部 vk::raii::PhysicalDevice，不做空检查。
 */
const vk::raii::PhysicalDevice& vulkan_physical_device::operator*() const noexcept
{
    return physical_device;
}
