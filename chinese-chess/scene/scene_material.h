#pragma once

#include "vulkan_core/vulkan_image.h"
#include <glm/glm.hpp>

struct material_data
{
	alignas(8) glm::vec3 background_color = glm::vec3(1.f, 1.f, 1.f);
	alignas(8) glm::vec3 foreground_color = glm::vec3(1.f, 1.f, 1.f);
	uint32_t texture_index = std::numeric_limits<uint32_t>::max();
};

struct scene_material
{
	glm::vec3 background_color = glm::vec3(0.0f, 0.0f, 0.0f);
	glm::vec3 foreground_color = glm::vec3(0.0f, 0.0f, 0.0f);
	std::shared_ptr<vulkan_image> alpha_map = nullptr;
};