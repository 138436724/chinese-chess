#pragma once

#include "vulkan_buffer.h"

#include <vulkan/vulkan_raii.hpp>

class vulkan_shader_binding_table
{
public:
    vulkan_shader_binding_table()                                        = default;
    ~vulkan_shader_binding_table()                                       = default;
    vulkan_shader_binding_table(vulkan_shader_binding_table&)            = delete;
    vulkan_shader_binding_table& operator=(vulkan_shader_binding_table&) = delete;
    vulkan_shader_binding_table(vulkan_shader_binding_table&& _other) noexcept;
    vulkan_shader_binding_table& operator=(vulkan_shader_binding_table&& _other) noexcept;

    [[nodiscard("Staging buffer must be kept!")]]
    vulkan_buffer create(const vk::raii::PhysicalDevice& _physical_device,
                         const vk::raii::Device&         _device,
                         const vma::raii::Allocator&     _allocator,
                         const vk::raii::CommandBuffer&  _commandbuffer,
                         const vk::raii::Pipeline&       _pipeline,
                         uint32_t                        _group_count,
                         uint32_t                        _queue);

    const vk::StridedDeviceAddressRegionKHR& get_raygen_region() const noexcept;
    const vk::StridedDeviceAddressRegionKHR& get_miss_region() const noexcept;
    const vk::StridedDeviceAddressRegionKHR& get_hit_region() const noexcept;
    const vk::StridedDeviceAddressRegionKHR& get_callable_region() const noexcept;
    const vulkan_buffer&                     get_shader_binding_table_buffer() const noexcept;

private:
    uint32_t handle_size      = 0;
    uint32_t handle_alignment = 0;
    uint32_t base_alignment   = 0;

    vk::StridedDeviceAddressRegionKHR raygen_region;
    vk::StridedDeviceAddressRegionKHR miss_region;
    vk::StridedDeviceAddressRegionKHR hit_region;
    vk::StridedDeviceAddressRegionKHR callable_region;

    vulkan_buffer sbt_buffer;
};
