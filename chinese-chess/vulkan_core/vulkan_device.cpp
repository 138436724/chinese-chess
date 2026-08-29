#include "vulkan_device.h"

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

vulkan_device::vulkan_device(vulkan_device&& _other) noexcept
    : device(std::exchange(_other.device, nullptr))
{
}

vulkan_device& vulkan_device::operator=(vulkan_device&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(device, _other.device);
    }
    return *this;
}

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

const vk::raii::Device& vulkan_device::operator*() const noexcept
{
    return device;
}
