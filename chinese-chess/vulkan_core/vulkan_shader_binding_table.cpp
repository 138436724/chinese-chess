#include "vulkan_commandbuffer.h"
#include "vulkan_common.h"
#include "vulkan_shader_binding_table.h"

vulkan_shader_binding_table::vulkan_shader_binding_table(vulkan_shader_binding_table&& _other) noexcept
	: handle_size(std::exchange(_other.handle_size, {})),
	handle_alignment(std::exchange(_other.handle_alignment, {})),
	base_alignment(std::exchange(_other.base_alignment, {})),
	raygen_region(std::exchange(_other.raygen_region, {})),
	miss_region(std::exchange(_other.miss_region, {})),
	hit_region(std::exchange(_other.hit_region, {})),
	callable_region(std::exchange(_other.callable_region, {})),
	sbt_buffer(std::exchange(_other.sbt_buffer, {}))
{
}

vulkan_shader_binding_table& vulkan_shader_binding_table::operator=(vulkan_shader_binding_table&& _other) noexcept
{
	if (this != &_other)
	{
		std::ranges::swap(handle_size, _other.handle_size);
		std::ranges::swap(handle_alignment, _other.handle_alignment);
		std::ranges::swap(base_alignment, _other.base_alignment);
		std::ranges::swap(raygen_region, _other.raygen_region);
		std::ranges::swap(miss_region, _other.miss_region);
		std::ranges::swap(hit_region, _other.hit_region);
		std::ranges::swap(callable_region, _other.callable_region);
		std::ranges::swap(sbt_buffer, _other.sbt_buffer);
	}
	return *this;
}

vulkan_buffer vulkan_shader_binding_table::create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, const vma::raii::Allocator& _allocator, const vk::raii::CommandBuffer& _commandbuffer, const vk::raii::Pipeline& _pipeline, uint32_t _group_count)
{
	if (handle_size == 0 && handle_alignment == 0 && base_alignment == 0)
	{
		auto props = _physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR, vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
		const auto& properties = props.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();

		handle_size = properties.shaderGroupHandleSize;
		handle_alignment = properties.shaderGroupHandleAlignment;
		base_alignment = properties.shaderGroupBaseAlignment;
	}

	// SBT layout for 5 groups:
	// [Group 0: raygen] [Group 1: miss_primary] [Group 2: miss_shadow] [Group 3: hit_primary] [Group 4: hit_shadow]
	//
	// Vulkan SBT regions:
	// - raygen_region: points to Group 0 (1 entry, stride = raygen_size)
	// - miss_region:   points to Group 1 (2 entries: miss_primary + miss_shadow, stride = miss_entry_size)
	// - hit_region:    points to Group 3 (2 entries: hit_primary + hit_shadow, stride = hit_entry_size)

	uint32_t raygen_entry_size = static_cast<uint32_t>(vulkan_common::align_up(handle_size, handle_alignment));
	uint32_t miss_entry_count = _group_count >= 5 ? 2 : 1; // 2 miss shaders (primary + shadow) if 5 groups present
	uint32_t hit_entry_count = _group_count >= 5 ? 2 : 1;   // 2 hit shaders (primary + shadow) if 5 groups present

	// Calculate offsets for each group in the SBT buffer
	// Group 0: raygen
	uint32_t raygen_offset = 0;
	// Group 1: miss_primary
	uint32_t miss_primary_offset = static_cast<uint32_t>(vulkan_common::align_up(raygen_offset + raygen_entry_size, base_alignment));
	// Group 2: miss_shadow
	uint32_t miss_shadow_offset = static_cast<uint32_t>(vulkan_common::align_up(miss_primary_offset + raygen_entry_size, base_alignment));
	// Group 3: hit_primary
	uint32_t hit_primary_offset = static_cast<uint32_t>(vulkan_common::align_up(miss_shadow_offset + raygen_entry_size, base_alignment));
	// Group 4: hit_shadow
	uint32_t hit_shadow_offset = static_cast<uint32_t>(vulkan_common::align_up(hit_primary_offset + raygen_entry_size, base_alignment));
	// Total size
	vk::DeviceSize buffer_size = static_cast<uint64_t>(vulkan_common::align_up(hit_shadow_offset + raygen_entry_size, base_alignment));

	// Stride must match actual spacing between groups (base_alignment-aligned), not handle_alignment-aligned
	uint32_t miss_stride = miss_shadow_offset - miss_primary_offset;
	uint32_t hit_stride = hit_shadow_offset - hit_primary_offset;

	sbt_buffer.create(_allocator, _device, buffer_size, vk::BufferUsageFlagBits::eShaderBindingTableKHR | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress, vk::MemoryPropertyFlagBits::eDeviceLocal);

	vulkan_buffer staging_buffer;
	staging_buffer.create(_allocator, _device, buffer_size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

	std::vector<uint8_t> shader_handles = _pipeline.getRayTracingShaderGroupHandlesKHR<uint8_t>(0, _group_count, static_cast<size_t>(handle_size) * _group_count);

	uint8_t* buffer_address = static_cast<uint8_t*>(staging_buffer.get_buffer_address().hostAddress);

	// Copy handles for all 5 groups
	// Group 0: raygen
	memcpy(buffer_address + raygen_offset, shader_handles.data() + 0 * handle_size, handle_size);
	// Group 1: miss_primary
	memcpy(buffer_address + miss_primary_offset, shader_handles.data() + 1 * handle_size, handle_size);
	// Group 2: miss_shadow
	memcpy(buffer_address + miss_shadow_offset, shader_handles.data() + 2 * handle_size, handle_size);
	// Group 3: hit_primary
	memcpy(buffer_address + hit_primary_offset, shader_handles.data() + 3 * handle_size, handle_size);
	// Group 4: hit_shadow
	memcpy(buffer_address + hit_shadow_offset, shader_handles.data() + 4 * handle_size, handle_size);

	// Create StridedDeviceAddressRegion for each Vulkan SBT region
	// raygen: single group at raygen_offset, stride = raygen_entry_size (size of 1 entry)
	raygen_region = vk::StridedDeviceAddressRegionKHR(
		sbt_buffer.get_buffer_address().deviceAddress + raygen_offset,
		raygen_entry_size,
		raygen_entry_size * 1  // total size = 1 entry
	);

	// miss: starts at miss_primary_offset, stride = actual interval between miss groups, 2 entries (primary + shadow)
	miss_region = vk::StridedDeviceAddressRegionKHR(
		sbt_buffer.get_buffer_address().deviceAddress + miss_primary_offset,
		miss_stride,
		miss_stride * miss_entry_count  // total size = 2 entries
	);

	// hit: starts at hit_primary_offset, stride = actual interval between hit groups, 2 entries (primary + shadow)
	hit_region = vk::StridedDeviceAddressRegionKHR(
		sbt_buffer.get_buffer_address().deviceAddress + hit_primary_offset,
		hit_stride,
		hit_stride * hit_entry_count  // total size = 2 entries
	);

	callable_region = vk::StridedDeviceAddressRegionKHR(0, 0, 0);

	vulkan_buffer::copy_buffer_to_buffer(_commandbuffer, staging_buffer.get_buffer(), sbt_buffer.get_buffer(), vk::BufferCopy2(0, 0, buffer_size));

	return staging_buffer;
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