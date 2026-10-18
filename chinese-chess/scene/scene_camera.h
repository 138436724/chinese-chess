/**
 * @file scene_camera.h
 * @brief 相机状态与投影/视图矩阵：保存正交与透视两套参数，切换时只重建矩阵。
 * @details 投影类型由 projection_type 决定；set_* 修改参数后立即重算 projection / view 与它们的逆矩阵，
 *          位置或方向变化还会先重建相机基向量。类不持有 Vulkan 对象，矩阵由渲染器每帧写进 UBO。
 * @note setter / getter 与两个 update_* 都标 noexcept；方向退化由 safe_normalize 与参考 up 兜底。
 *       投影范围、视场角、宽高比与位置仍需调用方提供合法有限值，不能保证任意输入都生成有限矩阵。
 */
#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

/**
 * @brief 投影类型，决定 update_camera_matrix 用哪一套参数建投影矩阵。
 * @details 两套参数各自独立保存，切换类型不会覆盖对方，切回去仍是原来的视锥。
 */
enum class projection_type : uint32_t
{
    orthographic, ///< 正交投影：使用 left/right/bottom/top 与 orthographic_near/far。
    perspective   ///< 透视投影：使用 fov_y/aspect 与 perspective_near/far。
};

/**
 * @brief 场景相机：保存两套投影参数、位置/朝向与由此派生的四个矩阵。
 * @details 默认是正交投影（-10..10、near 0.1、far 100），位置原点、朝 -Z、上方向 +Y。
 *          set_position / set_direction / set_world_up 先重建 right_axis / up_axis，再重算 view 与逆矩阵；
 *          两个 set_*_projection 与 set_projection_type 只触发矩阵重算，不改变位置与朝向。
 * @note 默认成员初始化器调用 glm 初始化矩阵，因此默认状态即可渲染；类不等待 GPU，也不做任何同步。
 */
class scene_camera
{
public:
    /**
     * @brief 构造相机：使用默认正交参数、曝光 1.0 与默认位置/朝向，并立即建好四个矩阵。
     */
    scene_camera()  = default;
    /**
     * @brief 默认析构：只含数值与 glm 矩阵，没有资源需要释放。
     */
    ~scene_camera() = default;

    /**
     * @brief 写入正交投影的六面参数并切换到正交投影。
     * @param _left 视锥左边界，世界单位。
     * @param _right 视锥右边界，世界单位。
     * @param _bottom 视锥下边界，世界单位。
     * @param _top 视锥上边界，世界单位。
     * @param _near 近平面距离，必须为正。
     * @param _far 远平面距离，必须大于 _near。
     * @note 只重算矩阵，不改变位置、朝向与曝光。
     */
    void set_orthographic_projection(float _left, float _right, float _bottom, float _top, float _near, float _far) noexcept;
    /**
     * @brief 写入透视投影参数并切换到透视投影。
     * @param _fov_y 垂直视场角，弧度。
     * @param _aspect 宽高比，通常取输出宽 / 高。
     * @param _near 近平面距离，必须为正。
     * @param _far 远平面距离，必须大于 _near。
     * @note 只重算矩阵，不改变位置、朝向与曝光。
     */
    void set_perspective_projection(float _fov_y, float _aspect, float _near, float _far) noexcept;

    /**
     * @brief 切换投影类型并按新类型重算投影矩阵。
     * @param _type 目标投影类型；两套参数各自保留，切回去仍是原来的视锥。
     */
    void set_projection_type(projection_type _type) noexcept;
    /**
     * @brief 设置曝光倍数。
     * @param _exposure 曝光值，只存下来供渲染器写进 UBO，不参与矩阵计算。
     */
    void set_exposure(float _exposure) noexcept;
    /**
     * @brief 设置相机位置并重建基向量与视图矩阵。
     * @param _position 世界空间位置。
     */
    void set_position(const glm::vec3& _position) noexcept;
    /**
     * @brief 设置观察方向并重建基向量与视图矩阵。
     * @param _direction 期望的朝向；内部归一化，零向量或非有限值时保留上一条有效方向。
     */
    void set_direction(const glm::vec3& _direction) noexcept;
    /**
     * @brief 设置参考上方向并重建基向量与视图矩阵。
     * @param _world_up 期望的上方向；内部归一化，退化输入保留旧值。
     * @note 与 direction 平行时实现会换用参考轴，避免基向量归零。
     */
    void set_world_up(const glm::vec3& _world_up) noexcept;

    /**
     * @brief 取当前投影类型。
     * @return active_type，默认 orthographic。
     */
    [[nodiscard]] projection_type  get_projection_type() const noexcept;
    /**
     * @brief 取曝光倍数。
     * @return 上次 set_exposure 写入的值，默认 1.0。
     */
    [[nodiscard]] float            get_exposure() const noexcept;
    /**
     * @brief 取相机位置。
     * @return 世界空间位置（按值返回）。
     */
    [[nodiscard]] glm::vec3        get_position() const noexcept;
    /**
     * @brief 取观察方向。
     * @return 归一化后的方向（按值返回）。
     */
    [[nodiscard]] glm::vec3        get_direction() const noexcept;
    /**
     * @brief 取当前投影矩阵。
     * @return 投影矩阵的常量引用，随 set_*_projection 与 set_projection_type 更新。
     */
    [[nodiscard]] const glm::mat4& get_projection_matrix() const noexcept;
    /**
     * @brief 取视图矩阵。
     * @return view 矩阵的常量引用，随位置/方向/上方向更新。
     */
    [[nodiscard]] const glm::mat4& get_view_matrix() const noexcept;
    /**
     * @brief 取投影矩阵的逆。
     * @return 与 get_projection_matrix 同步更新的逆矩阵。
     * @note 着色器用它从深度反投影，正交与透视共用同一份逆。
     */
    [[nodiscard]] const glm::mat4& get_inv_projection_matrix() const noexcept;
    /**
     * @brief 取视图矩阵的逆。
     * @return 与 get_view_matrix 同步更新的逆矩阵，即相机到世界的变换。
     */
    [[nodiscard]] const glm::mat4& get_inv_view_matrix() const noexcept;

private:
    /**
     * @brief 归一化 direction/world_up，重建 right_axis/up_axis，然后重算矩阵。
     * @note direction 与 world_up 平行时换用参考 up，避免叉乘归零把整个基变成 NaN。
     */
    void update_camera_axis() noexcept;
    /**
     * @brief 按 active_type 重建投影矩阵，并重算 view 与两个逆矩阵。
     * @note 正交走 glm::ortho，透视走 glm::perspective；两者都用 near/far 各自的参数。
     */
    void update_camera_matrix() noexcept;

    /** @brief 当前生效的投影类型，默认正交。 */
    projection_type active_type = projection_type::orthographic;
    /** @brief 曝光倍数，默认 1.0；只供渲染器读取，不参与矩阵。 */
    float           exposure    = 1.0f;

    // orthographic parameters
    /** @brief 正交投影左边界，默认 -10。 */
    float left              = -10.f;
    /** @brief 正交投影右边界，默认 10。 */
    float right             = 10.f;
    /** @brief 正交投影下边界，默认 -10。 */
    float bottom            = -10.f;
    /** @brief 正交投影上边界，默认 10。 */
    float top               = 10.f;
    /** @brief 正交投影近平面，默认 0.1。 */
    float orthographic_near = 0.1f;
    /** @brief 正交投影远平面，默认 100。 */
    float orthographic_far  = 100.f;

    // perspective parameters
    /** @brief 透视投影垂直视场角，弧度，默认 90 度。 */
    float fov_y            = glm::radians(90.f);
    /** @brief 透视投影宽高比，默认 16:9。 */
    float aspect           = 16.f / 9.f;
    /** @brief 透视投影近平面，默认 0.1。 */
    float perspective_near = 0.1f;
    /** @brief 透视投影远平面，默认 100。 */
    float perspective_far  = 100.f;

    /** @brief 相机世界空间位置，默认原点。 */
    glm::vec3 position   = glm::vec3(0.f);
    /** @brief 归一化后的观察方向，默认 -Z。 */
    glm::vec3 direction  = glm::vec3(0.f, 0.f, -1.f);
    /** @brief 参考上方向，默认 +Y；仅在重建基向量时使用。 */
    glm::vec3 world_up   = glm::vec3(0.f, 1.f, 0.f);
    /** @brief 相机右轴，由 direction 与有效 up 叉乘并归一化得到。 */
    glm::vec3 right_axis = glm::normalize(glm::cross(direction, world_up));
    /** @brief 相机上轴，由 right_axis 与 direction 叉乘并归一化得到。 */
    glm::vec3 up_axis    = glm::normalize(glm::cross(right_axis, direction));

    /** @brief 当前投影矩阵，默认由正交初始化参数计算。 */
    glm::mat4 projection_matrix     = glm::ortho(left, right, bottom, top, orthographic_near, orthographic_far);
    /** @brief 当前视图矩阵，默认由位置、方向与 up_axis 计算。 */
    glm::mat4 view_matrix           = glm::lookAt(position, position + direction, up_axis);
    /** @brief projection_matrix 的逆，与它同步更新。 */
    glm::mat4 inv_projection_matrix = glm::inverse(projection_matrix);
    /** @brief view_matrix 的逆，与它同步更新。 */
    glm::mat4 inv_view_matrix       = glm::inverse(view_matrix);
};
