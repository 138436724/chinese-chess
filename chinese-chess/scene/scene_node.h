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

	const vulkan_acceleration_structure& get_acceleration_structure() const noexcept { return blas; }

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