#pragma once
#include "scene_material.h"
#include "tools/model_loader.h"
#include "vulkan_core/vulkan_acceleration_structure.h"
#include <memory>

struct model_data
{
	alignas(16) glm::mat4 model_matrix = glm::mat4(1.f); // std430 layout
	alignas(8) uint32_t material_index = std::numeric_limits<uint32_t>::max();
	alignas(8) vk::DeviceAddress vertex_address = 0;
	alignas(8) vk::DeviceAddress index_address = 0;
};

struct model_infomation
{
	size_t vertex_offset = 0;
	size_t index_offset = 0;
	std::vector<model_vertex> vertices;
	std::vector<uint32_t> indices;
	vulkan_acceleration_structure blas_info;
};

class scene_model
{
public:
	scene_model(std::shared_ptr<model_infomation> _model_info) noexcept;
	~scene_model() = default;

	vk::AccelerationStructureInstanceKHR get_blas_instance() const noexcept;

public:
	std::shared_ptr<model_infomation> model_info = nullptr;
	std::shared_ptr<scene_material> material = nullptr;

	bool is_show = true;
	uint32_t custom_index = 0;

	glm::mat4 model_matrix = glm::mat4(1.0f);
};
