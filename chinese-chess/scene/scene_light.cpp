#include "scene_light.h"

void scene_light::set_light_type(light_type _type) noexcept
{
	active_type = _type;
}

void scene_light::set_color(const glm::vec3& _color) noexcept
{
	color = _color;
}

void scene_light::set_intensity(float _intensity) noexcept
{
	intensity = _intensity;
}

void scene_light::set_direction(const glm::vec3& _direction) noexcept
{
	direction = glm::normalize(_direction);
}

void scene_light::set_position(const glm::vec3& _position) noexcept
{
	position = _position;
}

void scene_light::set_range(float _range) noexcept
{
	range = _range;
}

void scene_light::set_inner_cone_angle(float _radians) noexcept
{
	inner_cone_angle = _radians;
}

void scene_light::set_outer_cone_angle(float _radians) noexcept
{
	outer_cone_angle = _radians;
}

light_type scene_light::get_light_type() const noexcept
{
	return active_type;
}

glm::vec3 scene_light::get_color() const noexcept
{
	return color;
}

float scene_light::get_intensity() const noexcept
{
	return intensity;
}

glm::vec3 scene_light::get_direction() const noexcept
{
	return direction;
}

glm::vec3 scene_light::get_position() const noexcept
{
	return position;
}

float scene_light::get_range() const noexcept
{
	return range;
}

float scene_light::get_inner_cone_angle() const noexcept
{
	return inner_cone_angle;
}

float scene_light::get_outer_cone_angle() const noexcept
{
	return outer_cone_angle;
}