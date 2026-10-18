/**
 * @file ui_camera.cpp
 * @brief ui_camera 的实现：摄像机控件、参数回写与 resize 时的投影重置。
 * @details 每个控件的返回值就是“是否被编辑”，被编辑时立刻写回 scene_manager 的活动相机并调用
 *          need_camera_update()；resize 先按新宽高重算正交范围与 aspect，再统一回写两套投影。
 *          面板只持有可编辑副本，不缓存任何 GPU 状态。
 */
#include "ui_camera.h"

#include "scene/scene_manager.h"

#include <array>
#include <cstdint>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <string_view>


namespace {
constexpr std::string_view CAMERA_SETTING        = "摄像机设置";
constexpr std::string_view CAMERA_TYPE           = "摄像机类型";
constexpr std::string_view ORTHOGRAPHIC_CAMERA   = "正交投影";
constexpr std::string_view PERSPECTIVE_CAMERA    = "透视投影";
constexpr std::string_view CAMERA_POSITION       = "摄像机位置";
constexpr std::string_view CAMERA_DIRECTION      = "摄像机前方向";
constexpr std::string_view CAMERA_WORLD_UP       = "摄像机上方向";
constexpr std::string_view LEFT_RIGHT_BOTTOM_TOP = "左右下上";
constexpr std::string_view FOV                   = "视场角";
constexpr std::string_view ASPECT                = "宽高比";
constexpr std::string_view NEAR_FAR              = "近平面和远平面";
constexpr std::string_view EXPOSURE              = "曝光";
}  // namespace


ui_camera::ui_camera(scene_manager& _manager) noexcept
    : manager(_manager)
{
}

/**
 * @brief 按新宽高重算投影默认值并回写场景相机，同时置相机脏。
 * @note 1.3f 是正交范围的半高，宽高比同时决定透视投影的 aspect。
 */
void ui_camera::resize(uint32_t _width, uint32_t _height) noexcept
{
    // reset to default
    constexpr float camera_height = 1.3f;
    orthographic_range =
        glm::vec4(-camera_height * _width / _height, camera_height * _width / _height, -camera_height, camera_height);
    orthographic_near_far = glm::vec2(0.01f, 100.f);

    fov_y                = 90.f;
    aspect               = static_cast<float>(_width) / _height;
    perspective_near_far = glm::vec2(0.01f, 100.f);

    manager.get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x,
                                                           perspective_near_far.y);
    manager.get_active_camera().set_orthographic_projection(orthographic_range.x, orthographic_range.y,
                                                            orthographic_range.z, orthographic_range.w,
                                                            orthographic_near_far.x, orthographic_near_far.y);
    manager.get_active_camera().set_projection_type(static_cast<projection_type>(active_type));

    manager.need_camera_update();
}

/**
 * @brief 逐帧构建摄像机控件：投影类型 → 对应投影参数 → 位姿 → 曝光。
 * @note 只在控件返回 true 时回写，避免每帧都置脏。
 */
void ui_camera::update() noexcept
{
    ImGui::SeparatorText(CAMERA_SETTING.data());

    constexpr std::array all_camera_types = {
        ORTHOGRAPHIC_CAMERA.data(),
        PERSPECTIVE_CAMERA.data(),
    };
    if (ImGui::Combo(CAMERA_TYPE.data(), &active_type, all_camera_types.data(), static_cast<int>(all_camera_types.size())))
    {
        manager.get_active_camera().set_projection_type(static_cast<projection_type>(active_type));
        manager.need_camera_update();
    }

    if (static_cast<projection_type>(active_type) == projection_type::orthographic)
    {
        if (ImGui::DragFloat4(LEFT_RIGHT_BOTTOM_TOP.data(), glm::value_ptr(orthographic_range)))
        {
            manager.get_active_camera().set_orthographic_projection(orthographic_range.x, orthographic_range.y,
                                                                    orthographic_range.z, orthographic_range.w,
                                                                    orthographic_near_far.x, orthographic_near_far.y);
            manager.need_camera_update();
        }

        if (ImGui::DragFloat2(NEAR_FAR.data(), glm::value_ptr(orthographic_near_far)))
        {
            manager.get_active_camera().set_orthographic_projection(orthographic_range.x, orthographic_range.y,
                                                                    orthographic_range.z, orthographic_range.w,
                                                                    orthographic_near_far.x, orthographic_near_far.y);
            manager.need_camera_update();
        }
    }
    else
    {
        if (ImGui::DragFloat(FOV.data(), &fov_y, 0.1f, 0.1f, 179.f))
        {
            manager.get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x,
                                                                   perspective_near_far.y);
            manager.need_camera_update();
        }

        if (ImGui::DragFloat(ASPECT.data(), &aspect, 0.1f))
        {
            manager.get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x,
                                                                   perspective_near_far.y);
            manager.need_camera_update();
        }

        if (ImGui::DragFloat2(NEAR_FAR.data(), glm::value_ptr(perspective_near_far)))
        {
            manager.get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x,
                                                                   perspective_near_far.y);
            manager.need_camera_update();
        }
    }

    if (ImGui::DragFloat3(CAMERA_POSITION.data(), glm::value_ptr(position)))
    {
        manager.get_active_camera().set_position(position);
        manager.need_camera_update();
    }

    if (ImGui::DragFloat3(CAMERA_DIRECTION.data(), glm::value_ptr(direction)))
    {
        manager.get_active_camera().set_direction(direction);
        manager.need_camera_update();
    }

    if (ImGui::DragFloat3(CAMERA_WORLD_UP.data(), glm::value_ptr(world_up)))
    {
        manager.get_active_camera().set_world_up(world_up);
        manager.need_camera_update();
    }

    if (ImGui::DragFloat(EXPOSURE.data(), &exposure, 0.01f, 0.001f, 1000.f, "%.3f", ImGuiSliderFlags_Logarithmic))
    {
        manager.get_active_camera().set_exposure(exposure);
        manager.need_camera_update();
    }
}

void ui_camera::handle(int /*_key*/, int /*_scancode*/, int /*_action*/, int /*_mods*/) noexcept {}
