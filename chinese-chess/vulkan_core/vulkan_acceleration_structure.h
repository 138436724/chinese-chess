#pragma once

#include "tools/model_loader.h"
#include "vulkan_buffer.h"
#include "vulkan_commandbuffer.h"
#include <vulkan/vulkan_raii.hpp>

class vulkan_acceleration_structure
{
public:
	vulkan_acceleration_structure() = default;
	~vulkan_acceleration_structure() = default;
	vulkan_acceleration_structure(vulkan_acceleration_structure&) = delete;
	vulkan_acceleration_structure(vulkan_acceleration_structure&& _other) noexcept;
	vulkan_acceleration_structure& operator=(vulkan_acceleration_structure&) = delete;
	vulkan_acceleration_structure& operator=(vulkan_acceleration_structure&& _other) noexcept;

	void create_bottom_level_accelerration_structure(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, const vulkan_commandbuffer& _commandbuffer,
		uint32_t _vertex_count, vk::DeviceOrHostAddressConstKHR _vertex_data, uint32_t _index_count, vk::DeviceOrHostAddressConstKHR _index_data);

	void create_top_level_accelerration_structure(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, const vulkan_commandbuffer& _commandbuffer,
		const std::span<vk::AccelerationStructureInstanceKHR>& _instances);

	const vk::raii::AccelerationStructureKHR& get_acceleration_structure() const noexcept;
	vk::DeviceAddress get_address() const noexcept;
	const vk::raii::Buffer& get_buffer() const noexcept;

private:
	void create_accelerration_structure(const vk::raii::PhysicalDevice& _physical_device,
		const vk::raii::Device& _device,
		const vulkan_commandbuffer& _commandbuffer,
		vk::AccelerationStructureTypeKHR _type,
		vk::BuildAccelerationStructureFlagsKHR _flags);

	vk::PhysicalDeviceAccelerationStructurePropertiesKHR properties;

	vk::AccelerationStructureGeometryKHR geometry;
	vk::AccelerationStructureBuildRangeInfoKHR range_info;

	vk::raii::AccelerationStructureKHR acceleration_structure = nullptr;
	vk::DeviceAddress address;
	vulkan_buffer buffer;

	vulkan_buffer scratch_buffer;
};