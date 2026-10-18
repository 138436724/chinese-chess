/**
 * @file scene_camera.cpp
 * @brief 相机参数 setter、相机基重建与投影/视图矩阵计算的实现。
 * @details 两个 set_*_projection 与 set_projection_type 只触发矩阵重算；位置/方向/上方向的 setter
 *          先重建 right_axis/up_axis，再重算全部矩阵。所有函数都是 noexcept，方向与上向量的退化输入由
 *          vulkan_common::safe_normalize 与参考 up 兜底；位置与投影参数仍须调用方保证合法。getter 按值返回数值或返回矩阵引用。
 */
#include "scene_camera.h"

#include "vulkan_core/vulkan_common.h"

#include <cmath>

/**
 * @brief 写入正交六面参数、切到正交投影并重算矩阵。
 * @note 只改投影相关状态；位置、朝向与曝光不受影响。
 */
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

/**
 * @brief 写入透视四参数、切到透视投影并重算矩阵。
 * @note fov_y 以弧度保存，调用方负责从角度换算；近远平面的合法性不做校验。
 */
void scene_camera::set_perspective_projection(float _fov_y, float _aspect, float _near, float _far) noexcept
{
    active_type      = projection_type::perspective;
    fov_y            = _fov_y;
    aspect           = _aspect;
    perspective_near = _near;
    perspective_far  = _far;
    update_camera_matrix();
}

/**
 * @brief 切换投影类型并按新类型重算投影矩阵。
 * @note 两套参数各自保留，切换不会覆盖对方；位置与朝向不变。
 */
void scene_camera::set_projection_type(projection_type _type) noexcept
{
    active_type = _type;
    update_camera_matrix();
}

void scene_camera::set_exposure(float _exposure) noexcept
{
    exposure = _exposure;
}

/**
 * @brief 更新位置后重建相机基与视图矩阵。
 * @note 位置本身不做校验，投影矩阵不受影响。
 */
void scene_camera::set_position(const glm::vec3& _position) noexcept
{
    position = _position;
    update_camera_axis();
}

/**
 * @brief 写入归一化后的方向，并重建基向量与视图矩阵。
 * @note 零向量或非有限输入时保留上一条有效方向，不把 NaN 写进矩阵。
 */
void scene_camera::set_direction(const glm::vec3& _direction) noexcept
{
    direction = vulkan_common::safe_normalize(_direction, direction);  // 零向量：保留上一条有效方向，不写 NaN
    update_camera_axis();
}

/**
 * @brief 写入归一化后的参考上方向，并重建基向量与视图矩阵。
 * @note 退化输入保留旧值；与 direction 平行时由 update_camera_axis 换用参考轴。
 */
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

/**
 * @brief 归一化输入、挑选有效参考 up 并重建 right_axis/up_axis，最后重算矩阵。
 * @note direction 与 world_up 平行时叉乘趋零，实现改用另一个参考轴重建，非退化路径保持原值。
 */
void scene_camera::update_camera_axis() noexcept
{
    direction = vulkan_common::safe_normalize(direction, glm::vec3(0.f, 0.f, -1.f));
    world_up  = vulkan_common::safe_normalize(world_up, glm::vec3(0.f, 1.f, 0.f));

    // direction 与 world_up 平行时 cross 归零 ⇒ right / up 会双双变成 NaN（lookAt 也跟着坏）。
    // 这种情况换一个不平行的参考 up 来重建基，非退化时继续采用 world_up。
    const glm::vec3 cross_result = glm::cross(direction, world_up);
    const glm::vec3 reference_up = std::abs(direction.y) < 0.999f ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(0.f, 0.f, 1.f);
    const glm::vec3 effective_up = glm::dot(cross_result, cross_result) > 1e-12f ? world_up : reference_up;

    right_axis = vulkan_common::safe_normalize(glm::cross(direction, effective_up), glm::vec3(1.f, 0.f, 0.f));
    up_axis    = vulkan_common::safe_normalize(glm::cross(right_axis, direction), effective_up);
    update_camera_matrix();
}

/**
 * @brief 按 active_type 生成投影矩阵，并同步重算 view 与两个逆矩阵。
 * @note 逆矩阵用 glm::inverse 直接求；不检查投影参数，非法 near/far 等输入可能产生非有限矩阵。
 */
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
