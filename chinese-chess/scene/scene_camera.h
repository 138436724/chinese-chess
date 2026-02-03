#pragma once
#define GLM_FORCE_RADIANS
#define GLM_ENABLE_EXPERIMENTAL
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class scene_camera
{
public:
	scene_camera() = default;
	~scene_camera() = default;

	void set_ortho_projection(float _left, float _right, float _bottom, float _top, float _near, float _far) noexcept;
	void set_position(const glm::vec3& _position) noexcept;
	void set_direction(const glm::vec3& _direction) noexcept;
	void set_world_up(const glm::vec3& _world_up) noexcept;

	glm::vec3 get_position() const noexcept;
	glm::mat4 get_projection_matrix() const noexcept;
	glm::mat4 get_view_matrix() const noexcept;

private:
	void update_camera_axis() noexcept;
	void update_camera_matrix() noexcept;

	float left = -10.f;
	float right = 10.f;
	float bottom = -10.f;
	float top = 10.f;
	float near = 0.1f;
	float far = 100.f;

	glm::vec3 position = glm::vec3(0.f);
	glm::vec3 direction = glm::vec3(0.f, 0.f, -1.f);
	glm::vec3 world_up = glm::vec3(0.f, 1.f, 0.f);
	glm::vec3 right_axis = glm::normalize(glm::cross(direction, world_up));
	glm::vec3 up_axis = glm::normalize(glm::cross(right_axis, direction));

	glm::mat4 projection_matrix = glm::ortho(left, right, bottom, top, near, far);
	glm::mat4 view_matrix = glm::lookAt(position, position + direction, up_axis);
};