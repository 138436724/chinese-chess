#include "scene_material.h"

scene_material::scene_material(const glm::vec3& _background_color, const glm::vec3& _foreground_color, std::shared_ptr<vulkan_image> _alpha_map) noexcept
	: background_color(_background_color),
	foreground_color(_foreground_color),
	alpha_map(_alpha_map)
{
}
