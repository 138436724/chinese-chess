#include "ui_camera.h"

#include "scene/scene_manager.h"

#include <glm/gtc/type_ptr.hpp>


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


ui_camera::ui_camera(scene_manager& _manager) noexcept
    : manager(_manager)
{
}

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

    manager.need_update();
}

void ui_camera::update() noexcept
{
    ImGui::SeparatorText(CAMERA_SETTING.data());

    const std::array all_camera_types = {
        ORTHOGRAPHIC_CAMERA.data(),
        PERSPECTIVE_CAMERA.data(),
    };
    if (ImGui::Combo(CAMERA_TYPE.data(), &active_type, all_camera_types.data(), static_cast<int>(all_camera_types.size())))
    {
        manager.get_active_camera().set_projection_type(static_cast<projection_type>(active_type));
        manager.need_update();
    }

    if (static_cast<projection_type>(active_type) == projection_type::orthographic)
    {
        if (ImGui::DragFloat4(LEFT_RIGHT_BOTTOM_TOP.data(), glm::value_ptr(orthographic_range)))
        {
            manager.get_active_camera().set_orthographic_projection(orthographic_range.x, orthographic_range.y,
                                                                    orthographic_range.z, orthographic_range.w,
                                                                    orthographic_near_far.x, orthographic_near_far.y);
            manager.need_update();
        }

        if (ImGui::DragFloat2(NEAR_FAR.data(), glm::value_ptr(orthographic_near_far)))
        {
            manager.get_active_camera().set_orthographic_projection(orthographic_range.x, orthographic_range.y,
                                                                    orthographic_range.z, orthographic_range.w,
                                                                    orthographic_near_far.x, orthographic_near_far.y);
            manager.need_update();
        }
    }
    else
    {
        if (ImGui::DragFloat(FOV.data(), &fov_y, 0.1f, 0.1f, 179.f))
        {
            manager.get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x,
                                                                   perspective_near_far.y);
            manager.need_update();
        }

        if (ImGui::DragFloat(ASPECT.data(), &aspect, 0.1f))
        {
            manager.get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x,
                                                                   perspective_near_far.y);
            manager.need_update();
        }

        if (ImGui::DragFloat2(NEAR_FAR.data(), glm::value_ptr(perspective_near_far)))
        {
            manager.get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x,
                                                                   perspective_near_far.y);
            manager.need_update();
        }
    }

    if (ImGui::DragFloat3(CAMERA_POSITION.data(), glm::value_ptr(position)))
    {
        manager.get_active_camera().set_position(position);
        manager.need_update();
    }

    if (ImGui::DragFloat3(CAMERA_DIRECTION.data(), glm::value_ptr(direction)))
    {
        manager.get_active_camera().set_direction(direction);
        manager.need_update();
    }

    if (ImGui::DragFloat3(CAMERA_WORLD_UP.data(), glm::value_ptr(world_up)))
    {
        manager.get_active_camera().set_world_up(world_up);
        manager.need_update();
    }
}

void ui_camera::handle(int /*_glfw_key*/) noexcept {}
