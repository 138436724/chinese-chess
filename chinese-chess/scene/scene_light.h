#pragma once

#include <glm/glm.hpp>
#include <variant>

enum class light_type :uint32_t
{
	directional,
	point,
	spot
};

struct light_data
{
	alignas(8) uint32_t active_type = static_cast<uint32_t>(light_type::directional);
	glm::vec3 color = glm::vec3(1.f);
	alignas(8) float intensity = 1.f;
	glm::vec3 direction = glm::vec3(0.f, -1.f, 0.f);
	alignas(8) glm::vec3 position = glm::vec3(0.f);
	float range = 10.f;
	alignas(8) float inner_cone_angle = glm::radians(15.f);
	float outer_cone_angle = glm::radians(30.f);
};


struct directional_light
{
	glm::vec3 color = glm::vec3(1.f);
	float intensity = 1.f;
	glm::vec3 direction = glm::vec3(0.f, -1.f, 0.f);
};

struct point_light
{
	glm::vec3 color = glm::vec3(1.f);
	float intensity = 1.f;
	glm::vec3 position = glm::vec3(0.f);
	float range = 10.f;
};

struct spot_light
{
	glm::vec3 color = glm::vec3(1.f);
	float intensity = 1.f;
	glm::vec3 direction = glm::vec3(0.f, -1.f, 0.f);
	glm::vec3 position = glm::vec3(0.f);
	float range = 10.f;
	float inner_cone_angle = glm::radians(15.f);
	float outer_cone_angle = glm::radians(30.f);
};

struct scene_light 
{
	light_type active_type = light_type::directional;
	std::variant<directional_light, point_light, spot_light> light;
};
