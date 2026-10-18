/**
 * @file vulkan_device.cpp
 * @brief vulkan_device 的实现：移动语义与逻辑设备创建。
 * @details create 先把队列族下标排序去重，保证同一族只出现在一个 DeviceQueueCreateInfo 里，
 *          每个族只建一个优先级 0 的队列；特性链由调用方通过 pNext 传入。
 *          设备句柄交给 vk::raii::Device 管理，析构即销毁，本文件没有显式 destroy 路径。
 */
#include "vulkan_device.h"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

/**
 * @brief 移动构造：用 exchange 把源设备句柄搬到新对象，源对象变为 nullptr。
 */
vulkan_device::vulkan_device(vulkan_device&& _other) noexcept
    : device(std::exchange(_other.device, nullptr))
{
}

/**
 * @brief 移动赋值：与源对象交换句柄，源对象拿到本对象原来的设备。
 */
vulkan_device& vulkan_device::operator=(vulkan_device&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(device, _other.device);
    }
    return *this;
}

/**
 * @brief 排序去重队列族下标后为每族建一个队列，再用调用方的特性链创建逻辑设备。
 * @note _features 以指针挂到 DeviceCreateInfo 的 pNext 上，链上的结构体必须在调用期间保持有效。
 */
void vulkan_device::create(const vk::raii::PhysicalDevice&    _physical_device,
                           const std::span<uint32_t const>    _queues,
                           const std::span<const char* const> _extensions,
                           const vk::PhysicalDeviceFeatures2& _features)
{
    std::vector<uint32_t> all_queues(_queues.begin(), _queues.end());
    std::ranges::sort(all_queues);
    const auto result = std::ranges::unique(all_queues);
    all_queues.erase(result.begin(), all_queues.end());

    constexpr float queue_priority           = 0.0f;
    const auto      device_queue_create_info = all_queues | std::views::transform([&queue_priority](const auto& index) {
                                              return vk::DeviceQueueCreateInfo({}, index, 1, &queue_priority);
                                               })
                                               | std::ranges::to<std::vector>();

    const vk::DeviceCreateInfo device_create_info({}, device_queue_create_info, {}, _extensions, {}, &_features);

    device = vk::raii::Device(_physical_device, device_create_info);
}

/**
 * @brief 解引用直接取内部 vk::raii::Device，不做空检查。
 */
const vk::raii::Device& vulkan_device::operator*() const noexcept
{
    return device;
}

/**
 * @brief 句柄非空即表示逻辑设备已创建。
 */
vulkan_device::operator bool() const noexcept
{
    return device != nullptr;
}
