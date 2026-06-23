#pragma once

#include <glm/glm.hpp>
#include <variant>

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

using scene_light = std::variant<directional_light, point_light, spot_light>;
