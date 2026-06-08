#pragma once
#include "scene_material.h"
#include "tools/model_loader.h"
#include "vulkan_core/vulkan_acceleration_structure.h"
#include <memory>

class scene_node
{
public:
	scene_node(std::shared_ptr<model_infomation> _model_info, std::shared_ptr<vulkan_acceleration_structure> _blas_info) noexcept;
	~scene_node() = default;

	vk::AccelerationStructureInstanceKHR get_blas_instance() const noexcept;

public:
	std::shared_ptr<model_infomation> model_info = nullptr;
	std::shared_ptr<vulkan_acceleration_structure> blas_info = nullptr;
	std::shared_ptr<scene_material> material = nullptr;

	bool is_show = true;
	uint32_t custom_index = 0;

	glm::mat4 model_matrix = glm::mat4(1.0f);
};
