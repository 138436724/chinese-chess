#include "scene_camera.h"

#include "vulkan_core/vulkan_common.h"

#include <cmath>

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

void scene_camera::set_exposure(float _exposure) noexcept
{
    exposure = _exposure;
}

void scene_camera::set_position(const glm::vec3& _position) noexcept
{
    position = _position;
    update_camera_axis();
}

void scene_camera::set_direction(const glm::vec3& _direction) noexcept
{
    direction = vulkan_common::safe_normalize(_direction, direction);  // 零向量：保留上一条有效方向，不写 NaN
    update_camera_axis();
}

void scene_camera::set_world_up(const glm::vec3& _world_up) noexcept
{
    world_up = vulkan_common::safe_normalize(_world_up, world_up);  // 同上，退化输入保留旧值
    update_camera_axis();
}

projection_type scene_camera::get_projection_type() const noexcept
{
    return active_type;
}

float scene_camera::get_exposure() const noexcept
{
    return exposure;
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
    direction = vulkan_common::safe_normalize(direction, glm::vec3(0.f, 0.f, -1.f));
    world_up  = vulkan_common::safe_normalize(world_up, glm::vec3(0.f, 1.f, 0.f));

    // direction 与 world_up 平行时 cross 归零 ⇒ right / up 会双双变成 NaN（lookAt 也跟着坏）。
    // 这种情况换一个不平行的参考 up 来重建基，非退化路径与原来逐位一致。
    const glm::vec3 cross_result = glm::cross(direction, world_up);
    const glm::vec3 reference_up = std::abs(direction.y) < 0.999f ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(0.f, 0.f, 1.f);
    const glm::vec3 effective_up = glm::dot(cross_result, cross_result) > 1e-12f ? world_up : reference_up;

    right_axis = vulkan_common::safe_normalize(glm::cross(direction, effective_up), glm::vec3(1.f, 0.f, 0.f));
    up_axis    = vulkan_common::safe_normalize(glm::cross(right_axis, direction), effective_up);
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
