#pragma once

#include "vulkan_core/vulkan_image.h"
#include <glm/glm.hpp>

struct material_data
{
	alignas(16) glm::vec3 background_color = glm::vec3(1.f, 1.f, 1.f);
	uint32_t texture_index = std::numeric_limits<uint32_t>::max();
	alignas(16) glm::vec3 foreground_color = glm::vec3(1.f, 1.f, 1.f);
	float roughness = 0.5f;
	alignas(16) float metallic = 0.0f;
};

struct scene_material
{
	glm::vec3 background_color = glm::vec3(0.0f, 0.0f, 0.0f);
	glm::vec3 foreground_color = glm::vec3(0.0f, 0.0f, 0.0f);
	std::shared_ptr<vulkan_image> alpha_map = nullptr;
	float roughness = 0.5f;
	float metallic = 0.0f;
};