#pragma once

#include "vulkan_core/vulkan_image.h"
#include <glm/glm.hpp>

class scene_material
{
public:
	scene_material(const glm::vec3& _background_color, const glm::vec3& _foreground_color, std::shared_ptr<vulkan_image> _alpha_map) noexcept;
	~scene_material() = default;

public:
	glm::vec3 background_color = glm::vec3(0.0f, 0.0f, 0.0f);
	glm::vec3 foreground_color = glm::vec3(0.0f, 0.0f, 0.0f);
	std::shared_ptr<vulkan_image> alpha_map = nullptr;
};