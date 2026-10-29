/**
 * @file ui_camera.h
 * @brief 摄像机面板：编辑投影类型、投影参数、相机位姿与曝光。
 * @details 面板不直接持有 scene_camera，相机写入经 scene_manager::get_active_camera() 转发；
 *          每次有效编辑后调用 scene_manager::need_camera_update()，置场景脏以重启累积，不把灯光、材质或模型数据置脏。
 *          面板自存一份可编辑副本，resize 时按新宽高重算投影默认值并回写两套投影与投影类型。
 */
#pragma once

#include "scene/scene_camera.h"

#include <cstdint>
#include <glm/glm.hpp>

class scene_manager;

/**
 * @brief 活动相机的设置面板。
 * @details 用 ImGui 组合框切换正交/透视投影，并按当前类型显示对应的范围、视场角、宽高比与
 *          近远平面控件；位置、前方向、上方向与曝光对所有类型都可用。控件被编辑的同一帧即写回
 *          scene_camera，因此本轮随后的场景更新与渲染即可使用新矩阵。
 * @note 只改相机参数，不创建或销毁任何场景实体；场景管理器必须比本对象活得久。
 */
class ui_camera
{
public:
    /**
     * @brief 绑定场景管理器，不读取也不修改任何相机状态。
     * @param _manager 面板长期引用的场景管理器，生命周期必须覆盖本对象。
     */
    explicit ui_camera(scene_manager& _manager) noexcept;
    /** @brief 默认析构：只释放面板自己的可编辑副本。 */
    ~ui_camera() = default;

    /**
     * @brief 按新帧缓冲尺寸重置投影参数并回写活动相机。
     * @param _width  新帧缓冲宽度（像素）。
     * @param _height 新帧缓冲高度（像素），必须非零。
     * @note 正交范围按固定 1.3 的正交半高与宽高比重算；fov 回到 90 度，两套近远平面都回到 0.01/100。
     * @note 末尾调用 need_camera_update()，下一次场景更新会重置累积索引，随后的首样本覆盖旧累积值。
     */
    void resize(uint32_t _width, uint32_t _height) noexcept;
    /**
     * @brief 每帧构建摄像机控件；任一控件被编辑就写回活动相机并置相机脏。
     * @note 只改相机参数，不触发描述符重建或场景 SSBO 重传。
     */
    void update() noexcept;
    /**
     * @brief 键盘事件入口，当前为空实现。
     * @param _key      GLFW 键码。
     * @param _scancode GLFW 扫描码。
     * @param _action   GLFW 动作，取 GLFW_PRESS / GLFW_REPEAT / GLFW_RELEASE。
     * @param _mods     GLFW 修饰键位掩码。
     * @note 摄像机面板没有快捷键，保留该入口只为满足 ui_base 的 handle 约定。
     */
    void handle(int _key, int _scancode, int _action, int _mods) noexcept;

private:
    scene_manager& manager;  ///< @brief 场景管理器引用，所有相机读写都经它转发

    int   active_type = static_cast<int>(projection_type::perspective);  ///< @brief 当前投影类型，存 int 供 ImGui::Combo 直接编辑，取值对应 projection_type
    float exposure    = 1.0f;  ///< @brief 曝光倍数，用对数滑块编辑，写回 scene_camera::set_exposure

    glm::vec4 orthographic_range    = glm::vec4(-10.f, 10.f, -10.f, 10.f);  ///< @brief 正交投影的左右下上范围，x/y/z/w 依次是 left/right/bottom/top
    glm::vec2 orthographic_near_far = glm::vec2(0.1f, 100.f);  ///< @brief 正交投影的近远平面

    float     fov_y                = 90.f;  ///< @brief 透视投影的垂直视场角，单位为度，写回相机前需转成弧度。
    float     aspect               = 16.f / 9.f;  ///< @brief 透视投影的宽高比，resize 时按宽度/高度重算，之后可由 UI 独立编辑
    glm::vec2 perspective_near_far = glm::vec2(0.1f, 100.f);  ///< @brief 透视投影的近远平面

    glm::vec3 position  = glm::vec3(0.f);  ///< @brief 摄像机位置，世界空间
    glm::vec3 direction = glm::vec3(0.f, 0.f, -1.f);  ///< @brief 摄像机前方向，世界空间
    glm::vec3 world_up  = glm::vec3(0.f, 1.f, 0.f);  ///< @brief 摄像机上方向，世界空间
};
