export module scene_camera;

import <glm/gtc/matrix_transform.hpp>;
import glm;

export class scene_camera
{
public:
	scene_camera() = default;
	~scene_camera() = default;

	void set_ortho_projection(float _left, float _right, float _bottom, float _top, float _near, float _far);
	void set_position(const glm::vec3& _position);
	void set_direction(const glm::vec3& _direction);
	void set_world_up(const glm::vec3& _world_up);

	glm::vec3 get_position() const;
	glm::mat4 get_projection_matrix() const;
	glm::mat4 get_view_matrix() const;

private:
	void update_camera_axis();
	void update_camera_matrix();

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