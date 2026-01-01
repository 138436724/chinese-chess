module scene_camera;

void scene_camera::set_ortho_projection(float _left, float _right, float _bottom, float _top, float _near, float _far)
{
	left = _left;
	right = _right;
	bottom = _bottom;
	top = _top;
	near = _near;
	far = _far;

	update_camera_matrix();
}

void scene_camera::set_position(const glm::vec3& _position)
{
	position = _position;
	update_camera_axis();
}

void scene_camera::set_direction(const glm::vec3& _direction)
{
	direction = glm::normalize(_direction);
	update_camera_axis();
}

void scene_camera::set_world_up(const glm::vec3& _world_up)
{
	world_up = _world_up;
	update_camera_axis();
}

glm::vec3 scene_camera::get_position() const
{
	return position;
}

glm::mat4 scene_camera::get_projection_matrix() const
{
	return projection_matrix;
}

glm::mat4 scene_camera::get_view_matrix() const
{
	return view_matrix;
}

void scene_camera::update_camera_axis()
{
	right_axis = glm::normalize(glm::cross(direction, world_up));
	up_axis = glm::normalize(glm::cross(right_axis, direction));
	update_camera_matrix();
}

void scene_camera::update_camera_matrix()
{
	projection_matrix = glm::ortho(left, right, bottom, top, near, far);
	view_matrix = glm::lookAt(position, position + direction, up_axis);
}