#pragma once

#include "vulkan_buffer.h"
#include "vulkan_commandbuffer.h"

#include <vulkan/vulkan_raii.hpp>

class vulkan_acceleration_structure
{
public:
    vulkan_acceleration_structure()                                          = default;
    ~vulkan_acceleration_structure()                                         = default;
    vulkan_acceleration_structure(vulkan_acceleration_structure&)            = delete;
    vulkan_acceleration_structure& operator=(vulkan_acceleration_structure&) = delete;
    vulkan_acceleration_structure(vulkan_acceleration_structure&& _other) noexcept;
    vulkan_acceleration_structure& operator=(vulkan_acceleration_structure&& _other) noexcept;

    [[nodiscard("Staging buffer must be kept!")]] vulkan_buffer create_bottom_level_acceleration_structure(
        const vk::raii::PhysicalDevice& _physical_device,
        const vk::raii::Device&         _device,
        const vma::raii::Allocator&     _allocator,
        const vk::raii::CommandBuffer&  _commandbuffer,
        uint32_t                        _vertex_count,
        vk::DeviceOrHostAddressConstKHR _vertex_data,
        uint32_t                        _index_count,
        vk::DeviceOrHostAddressConstKHR _index_data,
        uint32_t                        _queue);

    [[nodiscard("Staging buffer must be kept!")]] vulkan_buffer create_top_level_acceleration_structure(
        const vk::raii::PhysicalDevice& _physical_device,
        const vk::raii::Device&         _device,
        const vma::raii::Allocator&     _allocator,
        const vk::raii::CommandBuffer&  _commandbuffer,
        uint32_t                        _instances_size,
        vk::DeviceOrHostAddressConstKHR _instances_data,
        uint32_t                        _queue);

    const vk::raii::AccelerationStructureKHR& get_acceleration_structure() const noexcept;
    vk::DeviceAddress                         get_address() const noexcept;
    const vk::raii::Buffer&                   get_buffer() const noexcept;

private:
    [[nodiscard("Staging buffer must be kept!")]] vulkan_buffer create_acceleration_structure(
        const vk::raii::PhysicalDevice&        _physical_device,
        const vk::raii::Device&                _device,
        const vma::raii::Allocator&            _allocator,
        const vk::raii::CommandBuffer&         _commandbuffer,
        vk::AccelerationStructureTypeKHR       _type,
        vk::BuildAccelerationStructureFlagsKHR _flags,
        uint32_t                               _queue);

    uint32_t scratch_alignment = 0;

    vk::AccelerationStructureGeometryKHR       geometry;
    vk::AccelerationStructureBuildRangeInfoKHR range_info;

    vk::raii::AccelerationStructureKHR acceleration_structure = nullptr;
    vk::DeviceAddress                  address                = 0;
    vulkan_buffer                      buffer;
};
