#pragma once

#include <glm/glm.hpp>

enum class light_type
{
	directional,
	point,
	spot
};

class scene_light
{
public:
	scene_light() = default;
	~scene_light() = default;

	void set_light_type(light_type _type) noexcept;
	void set_color(const glm::vec3& _color) noexcept;
	void set_intensity(float _intensity) noexcept;

	// directional / spot
	void set_direction(const glm::vec3& _direction) noexcept;

	// point / spot
	void set_position(const glm::vec3& _position) noexcept;
	void set_range(float _range) noexcept;

	// spot only
	void set_inner_cone_angle(float _radians) noexcept;
	void set_outer_cone_angle(float _radians) noexcept;

	light_type get_light_type() const noexcept;
	glm::vec3 get_color() const noexcept;
	float get_intensity() const noexcept;
	glm::vec3 get_direction() const noexcept;
	glm::vec3 get_position() const noexcept;
	float get_range() const noexcept;
	float get_inner_cone_angle() const noexcept;
	float get_outer_cone_angle() const noexcept;

private:
	light_type active_type = light_type::directional;

	glm::vec3 color = glm::vec3(1.f);
	float intensity = 1.f;

	// directional / spot
	glm::vec3 direction = glm::vec3(0.f, -1.f, 0.f);

	// point / spot
	glm::vec3 position = glm::vec3(0.f);
	float range = 10.f;

	// spot only
	float inner_cone_angle = glm::radians(15.f);
	float outer_cone_angle = glm::radians(30.f);
};