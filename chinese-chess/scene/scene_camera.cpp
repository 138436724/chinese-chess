#include "scene_camera.h"

void scene_camera::set_orthographic_projection(float _left, float _right, float _bottom, float _top, float _near, float _far) noexcept
{
    active_type       = projection_type::orthographic;
    left              = _left;
    right             = _right;
    bottom            = _bottom;
    top               = _top;
    orthographic_near = _near;
    orthographic_far  = _far;
    update_camera_matrix();
}

void scene_camera::set_perspective_projection(float _fov_y, float _aspect, float _near, float _far) noexcept
{
    active_type      = projection_type::perspective;
    fov_y            = _fov_y;
    aspect           = _aspect;
    perspective_near = _near;
    perspective_far  = _far;
    update_camera_matrix();
}

void scene_camera::set_projection_type(projection_type _type) noexcept
{
    active_type = _type;
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
    world_up = glm::normalize(_world_up);
    update_camera_axis();
}

projection_type scene_camera::get_projection_type() const noexcept
{
    return active_type;
}

glm::vec3 scene_camera::get_position() const noexcept
{
    return position;
}

glm::vec3 scene_camera::get_direction() const noexcept
{
    return direction;
}

const glm::mat4& scene_camera::get_projection_matrix() const noexcept
{
    return projection_matrix;
}

const glm::mat4& scene_camera::get_view_matrix() const noexcept
{
    return view_matrix;
}

const glm::mat4& scene_camera::get_inv_projection_matrix() const noexcept
{
    return inv_projection_matrix;
}

const glm::mat4& scene_camera::get_inv_view_matrix() const noexcept
{
    return inv_view_matrix;
}

void scene_camera::update_camera_axis() noexcept
{
    right_axis = glm::normalize(glm::cross(direction, world_up));
    up_axis    = glm::normalize(glm::cross(right_axis, direction));
    update_camera_matrix();
}

void scene_camera::update_camera_matrix() noexcept
{
    if (active_type == projection_type::orthographic)
    {
        projection_matrix = glm::ortho(left, right, bottom, top, orthographic_near, orthographic_far);
    }
    else
    {
        projection_matrix = glm::perspective(fov_y, aspect, perspective_near, perspective_far);
    }

    view_matrix = glm::lookAt(position, position + direction, up_axis);

    inv_projection_matrix = glm::inverse(projection_matrix);
    inv_view_matrix       = glm::inverse(view_matrix);
}
