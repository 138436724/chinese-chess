#include "vulkan_commandbuffer.h"
#include "vulkan_common.h"
#include "vulkan_shader_binding_table.h"

vulkan_shader_binding_table::vulkan_shader_binding_table(vulkan_shader_binding_table&& _other) noexcept
	:raygen_region(std::move(_other.raygen_region)),
	miss_region(std::move(_other.miss_region)),
	hit_region(std::move(_other.hit_region)),
	callable_region(std::move(_other.callable_region)),
	sbt_buffer(std::move(_other.sbt_buffer))
{
}

vulkan_shader_binding_table& vulkan_shader_binding_table::operator=(vulkan_shader_binding_table&& _other) noexcept
{
	if (this != &_other)
	{
		std::ranges::swap(raygen_region, _other.raygen_region);
		std::ranges::swap(miss_region, _other.miss_region);
		std::ranges::swap(hit_region, _other.hit_region);
		std::ranges::swap(callable_region, _other.callable_region);
		std::ranges::swap(sbt_buffer, _other.sbt_buffer);
	}
	return *this;
}

void vulkan_shader_binding_table::create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, const vk::raii::CommandBuffer& _commandbuffer, const vk::raii::Pipeline& _pipeline, uint32_t _group_count, vulkan_buffer& _staging_buffer)
{
	if (handle_size == 0 && handle_alignment == 0 && base_alignment == 0)
	{
		auto props = _physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR, vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
		const auto& properties = props.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();

		handle_size = properties.shaderGroupHandleSize;
		handle_alignment = properties.shaderGroupHandleAlignment;
		base_alignment = properties.shaderGroupBaseAlignment;
	}

	uint32_t raygen_size = static_cast<uint32_t>(vulkan_common::align_up(handle_size, handle_alignment));
	uint32_t miss_size = static_cast<uint32_t>(vulkan_common::align_up(handle_size, handle_alignment));
	uint32_t hit_size = static_cast<uint32_t>(vulkan_common::align_up(handle_size, handle_alignment));
	uint32_t callable_size = 0; // not now

	uint32_t raygen_offset = 0;
	uint32_t miss_offset = static_cast<uint32_t>(vulkan_common::align_up(raygen_size, base_alignment));
	uint32_t hit_offset = static_cast<uint32_t>(vulkan_common::align_up(miss_offset + miss_size, base_alignment));
	uint32_t callable_offset = static_cast<uint32_t>(vulkan_common::align_up(hit_offset + hit_size, base_alignment));


	vk::DeviceSize buffer_size = static_cast<uint64_t>(callable_offset) + callable_size;

	sbt_buffer.create(_physical_device, _device, buffer_size, vk::BufferUsageFlagBits::eShaderBindingTableKHR | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

	_staging_buffer.create(_physical_device, _device, buffer_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);


	std::vector<uint8_t> shader_handles = _pipeline.getRayTracingShaderGroupHandlesKHR<uint8_t>(0, _group_count, static_cast<size_t>(handle_size) * _group_count);

	uint8_t* buffer_address = static_cast<uint8_t*>(_staging_buffer.get_buffer_address().hostAddress);

	memcpy(buffer_address + raygen_offset, shader_handles.data() + 0 * handle_size, handle_size);
	memcpy(buffer_address + miss_offset, shader_handles.data() + 1 * handle_size, handle_size);
	memcpy(buffer_address + hit_offset, shader_handles.data() + 2 * handle_size, handle_size);

	raygen_region = vk::StridedDeviceAddressRegionKHR(sbt_buffer.get_buffer_address().deviceAddress + raygen_offset, raygen_size, raygen_size);
	miss_region = vk::StridedDeviceAddressRegionKHR(sbt_buffer.get_buffer_address().deviceAddress + miss_offset, miss_size, miss_size);
	hit_region = vk::StridedDeviceAddressRegionKHR(sbt_buffer.get_buffer_address().deviceAddress + hit_offset, hit_size, hit_size);
	callable_region = vk::StridedDeviceAddressRegionKHR(0, 0, 0);

	vulkan_buffer::copy_buffer_to_buffer(_commandbuffer, _staging_buffer.get_buffer(), sbt_buffer.get_buffer(), vk::BufferCopy2(0, 0, buffer_size));
}

const vk::StridedDeviceAddressRegionKHR& vulkan_shader_binding_table::get_raygen_region() const noexcept
{
	return raygen_region;
}

const vk::StridedDeviceAddressRegionKHR& vulkan_shader_binding_table::get_miss_region() const noexcept
{
	return miss_region;
}

const vk::StridedDeviceAddressRegionKHR& vulkan_shader_binding_table::get_hit_region() const noexcept
{
	return hit_region;
}

const vk::StridedDeviceAddressRegionKHR& vulkan_shader_binding_table::get_callable_region() const noexcept
{
	return callable_region;
}

const vulkan_buffer& vulkan_shader_binding_table::get_shader_binding_table_buffer() const noexcept
{
	return sbt_buffer;
}
