/**
 * @file vulkan_device.h
 * @brief 逻辑设备（VkDevice）的 RAII 包装，按给定的队列族创建队列并启用特性链。
 * @details create 把队列族下标排序去重，为每个族建一个优先级 0 的队列，再用调用方的
 *          PhysicalDeviceFeatures2 pNext 链一次性启用特性。设备由 vk::raii::Device 持有，
 *          析构即销毁；本类只做创建与持有，设备能力查询归 vulkan_physical_device。
 */
#pragma once

#include <cstdint>
#include <span>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 逻辑设备包装：按去重后的队列族建队列，并用 pNext 链启用 1.1/1.2/1.3 与光追特性。
 * @details 只负责创建与持有 vk::raii::Device，不查询设备能力（由 vulkan_physical_device 负责）。
 *          移动构造后源对象句柄被置空，移动赋值则与源对象交换句柄；operator bool 可判断是否已创建。
 */
class vulkan_device
{
public:
    /** @brief 构造空对象，此时 operator bool 为 false。 */
    vulkan_device()                                = default;
    /** @brief 默认析构：vk::raii::Device 成员负责销毁逻辑设备。 */
    ~vulkan_device()                               = default;
    /** @brief 禁止拷贝：VkDevice 句柄唯一。 */
    vulkan_device(const vulkan_device&)            = delete;
    vulkan_device& operator=(const vulkan_device&) = delete;  ///< @brief 禁止拷贝赋值
    /** @brief 移动构造：用 exchange 把源设备句柄搬到新对象，源对象变为 nullptr。 */
    vulkan_device(vulkan_device&& _other) noexcept;
    vulkan_device& operator=(vulkan_device&& _other) noexcept;  ///< @brief 移动赋值：与源对象交换句柄，源对象拿到本对象原来的设备

    /**
     * @brief 用给定的队列族下标与特性链创建逻辑设备。
     * @param _physical_device 已选中的物理设备。
     * @param _queues          需要创建队列的族下标；内部排序去重，同一族只建一个队列。
     * @param _extensions      要启用的设备扩展名列表。
     * @param _features        承载 pNext 特性链的 PhysicalDeviceFeatures2。
     * @throws vk::SystemError 驱动拒绝创建（缺扩展或特性、设备丢失等）时抛出。
     * @note 必须在 vulkan_physical_device::create 之后调用；特性链上的结构体只需在调用期间有效。
     */
    void create(const vk::raii::PhysicalDevice&    _physical_device,
                const std::span<uint32_t const>    _queues,
                const std::span<const char* const> _extensions,
                const vk::PhysicalDeviceFeatures2& _features);

    /**
     * @brief 解引用取内部设备句柄。
     * @return 内部 vk::raii::Device；未创建时为空句柄，本函数不做空检查。
     */
    [[nodiscard]] const vk::raii::Device& operator*() const noexcept;
    /**
     * @brief 查询逻辑设备是否已创建。
     * @return 句柄非空返回 true。
     */
    [[nodiscard]] explicit                operator bool() const noexcept;

private:
    vk::raii::Device device = nullptr;  ///< @brief 逻辑设备句柄，未创建时为 nullptr
};
