#pragma once

#include "scene_camera.h"
#include "tools/model_loader.h"
#include "vulkan_core/vulkan_acceleration_structure.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_descriptor.h"

class scene_node
{
public:
	virtual ~scene_node() = default;

	virtual void create(const vulkan_application*, vk::SampleCountFlagBits, vk::Format, vk::Format) = 0;
	virtual void resize(const vulkan_application*, uint32_t, uint32_t) = 0;
	virtual void update(const scene_camera*) noexcept = 0;
	virtual void render(const vk::raii::CommandBuffer&) noexcept = 0;
	virtual void destroy() noexcept = 0;

	virtual std::vector<vk::AccelerationStructureInstanceKHR> get_all_blas_info() const noexcept = 0;

	vk::DeviceAddress get_vertices_device_address() const noexcept { return vertices_buffer.get_buffer_address().deviceAddress; }
	vk::DeviceAddress get_indices_device_address() const noexcept { return indices_buffer.get_buffer_address().deviceAddress; }
	uint32_t get_vertex_count() const noexcept { return static_cast<uint32_t>(vertices.size()); }
	uint32_t get_index_count() const noexcept { return static_cast<uint32_t>(indices.size()); }

protected:
	uint32_t current_frame = 0;

	std::vector<model_vertex> vertices;
	vulkan_buffer vertices_buffer;

	std::vector<uint32_t> indices;
	vulkan_buffer indices_buffer;

	vulkan_pipeline pipeline;
	vulkan_descriptor descriptor;
	std::vector<vulkan_buffer> ubos;

	vulkan_acceleration_structure blas;
};
