#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

enum class projection_type : uint32_t
{
    orthographic,
    perspective
};

class scene_camera
{
public:
    scene_camera()  = default;
    ~scene_camera() = default;

    void set_orthographic_projection(float _left, float _right, float _bottom, float _top, float _near, float _far) noexcept;
    void set_perspective_projection(float _fov_y, float _aspect, float _near, float _far) noexcept;

    void set_projection_type(projection_type _type) noexcept;
    void set_position(const glm::vec3& _position) noexcept;
    void set_direction(const glm::vec3& _direction) noexcept;
    void set_world_up(const glm::vec3& _world_up) noexcept;

    [[nodiscard]] projection_type  get_projection_type() const noexcept;
    [[nodiscard]] glm::vec3        get_position() const noexcept;
    [[nodiscard]] glm::vec3        get_direction() const noexcept;
    [[nodiscard]] const glm::mat4& get_projection_matrix() const noexcept;
    [[nodiscard]] const glm::mat4& get_view_matrix() const noexcept;
    [[nodiscard]] const glm::mat4& get_inv_projection_matrix() const noexcept;
    [[nodiscard]] const glm::mat4& get_inv_view_matrix() const noexcept;

private:
    void update_camera_axis() noexcept;
    void update_camera_matrix() noexcept;

    projection_type active_type = projection_type::orthographic;

    // orthographic parameters
    float left              = -10.f;
    float right             = 10.f;
    float bottom            = -10.f;
    float top               = 10.f;
    float orthographic_near = 0.1f;
    float orthographic_far  = 100.f;

    // perspective parameters
    float fov_y            = glm::radians(90.f);
    float aspect           = 16.f / 9.f;
    float perspective_near = 0.1f;
    float perspective_far  = 100.f;

    glm::vec3 position   = glm::vec3(0.f);
    glm::vec3 direction  = glm::vec3(0.f, 0.f, -1.f);
    glm::vec3 world_up   = glm::vec3(0.f, 1.f, 0.f);
    glm::vec3 right_axis = glm::normalize(glm::cross(direction, world_up));
    glm::vec3 up_axis    = glm::normalize(glm::cross(right_axis, direction));

    glm::mat4 projection_matrix     = glm::ortho(left, right, bottom, top, orthographic_near, orthographic_far);
    glm::mat4 view_matrix           = glm::lookAt(position, position + direction, up_axis);
    glm::mat4 inv_projection_matrix = glm::inverse(projection_matrix);
    glm::mat4 inv_view_matrix       = glm::inverse(view_matrix);
};
