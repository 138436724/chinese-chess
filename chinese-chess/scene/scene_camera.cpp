#include "scene_camera.h"

void scene_camera::set_ortho_projection(float _left, float _right, float _bottom, float _top, float _near, float _far) noexcept
{
	left = _left;
	right = _right;
	bottom = _bottom;
	top = _top;
	near = _near;
	far = _far;

	update_camera_matrix();
}

void scene_camera::set_position(const glm::vec3& _position) noexcept
{
	position = _position;
	update_camera_axis();
}

void scene_camera::set_direction(const glm::vec3& _direction) noexcept
{
	direction = glm::normalize(_direction);
	update_camera_axis();
}

void scene_camera::set_world_up(const glm::vec3& _world_up) noexcept
{
	world_up = _world_up;
	update_camera_axis();
}

glm::vec3 scene_camera::get_position() const noexcept
{
	return position;
}

glm::mat4 scene_camera::get_projection_matrix() const noexcept
{
	return projection_matrix;
}

glm::mat4 scene_camera::get_view_matrix() const noexcept
{
	return view_matrix;
}

void scene_camera::update_camera_axis() noexcept
{
	right_axis = glm::normalize(glm::cross(direction, world_up));
	up_axis = glm::normalize(glm::cross(right_axis, direction));
	update_camera_matrix();
}

void scene_camera::update_camera_matrix() noexcept
{
	projection_matrix = glm::ortho(left, right, bottom, top, near, far);
	view_matrix = glm::lookAt(position, position + direction, up_axis);
}