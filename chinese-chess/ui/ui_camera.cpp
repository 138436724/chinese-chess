#include "ui_camera.h"
#include <glm/gtc/type_ptr.hpp>

constexpr std::u8string_view CAMERA_SETTING = u8"摄像机设置";
constexpr std::u8string_view CAMERA_TYPE = u8"摄像机类型";
constexpr std::u8string_view ORTHOGRAPHIC_CAMREA = u8"正交投影";
constexpr std::u8string_view PRESPECTIVE_CAMREA = u8"透视投影";
constexpr std::u8string_view CAMERA_POSITION = u8"摄像机位置";
constexpr std::u8string_view CAMERA_DIRECTION = u8"摄像机前方向";
constexpr std::u8string_view CAMERA_WORLD_UP = u8"摄像机上方向";
constexpr std::u8string_view LEFT_RIGHT_BOTTOM_TOP = u8"左右下上";
constexpr std::u8string_view FOV = u8"视场角";
constexpr std::u8string_view ASPECT = u8"宽高比";
constexpr std::u8string_view NEAR_FAR = u8"近平面和远平面";

void ui_camera::create(scene_manager* _manager)
{
	manager = _manager;
}

void ui_camera::resize(uint32_t _width, uint32_t _height) noexcept
{
	// reset to default
	constexpr float camera_height = 1.3f;
	orthographic_range = glm::vec4(-camera_height * _width / _height, camera_height * _width / _height, -camera_height, camera_height);
	orthographic_near_far = glm::vec2(0.01f, 100.f);

	fov_y = 90.f;
	aspect = static_cast<float>(_width) / _height;
	perspective_near_far = glm::vec2(0.01f, 100.f);

	manager->get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x, perspective_near_far.y);
	manager->get_active_camera().set_orthographic_projection(orthographic_range.x, orthographic_range.y, orthographic_range.z, orthographic_range.w, orthographic_near_far.x, orthographic_near_far.y);
	manager->get_active_camera().set_projection_type(static_cast<projection_type>(active_type));

	manager->need_update();
}

void ui_camera::update() noexcept
{
	ImGui::SeparatorText(reinterpret_cast<const char*>(CAMERA_SETTING.data()));

	const std::array all_camera_types = {
		reinterpret_cast<const char*>(ORTHOGRAPHIC_CAMREA.data()),
		reinterpret_cast<const char*>(PRESPECTIVE_CAMREA.data()),
	};
	if (ImGui::Combo(reinterpret_cast<const char*>(CAMERA_TYPE.data()), &active_type, all_camera_types.data(), static_cast<int>(all_camera_types.size())))
	{
		manager->get_active_camera().set_projection_type(static_cast<projection_type>(active_type));
		manager->need_update();
	}

	if (static_cast<projection_type>(active_type) == projection_type::orthographic)
	{
		if (ImGui::DragFloat4(reinterpret_cast<const char*>(LEFT_RIGHT_BOTTOM_TOP.data()), glm::value_ptr(orthographic_range)))
		{
			manager->get_active_camera().set_orthographic_projection(orthographic_range.x, orthographic_range.y, orthographic_range.z, orthographic_range.w, orthographic_near_far.x, orthographic_near_far.y);
			manager->need_update();
		}

		if (ImGui::DragFloat2(reinterpret_cast<const char*>(NEAR_FAR.data()), glm::value_ptr(orthographic_near_far)))
		{
			manager->get_active_camera().set_orthographic_projection(orthographic_range.x, orthographic_range.y, orthographic_range.z, orthographic_range.w, orthographic_near_far.x, orthographic_near_far.y);
			manager->need_update();
		}
	}
	else
	{
		if (ImGui::DragFloat(reinterpret_cast<const char*>(FOV.data()), &fov_y, 0.1f, 0.1f, 179.f))
		{
			manager->get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x, perspective_near_far.y);
			manager->need_update();
		}

		if (ImGui::DragFloat(reinterpret_cast<const char*>(ASPECT.data()), &aspect, 0.1f))
		{
			manager->get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x, perspective_near_far.y);
			manager->need_update();
		}

		if (ImGui::DragFloat2(reinterpret_cast<const char*>(NEAR_FAR.data()), glm::value_ptr(perspective_near_far)))
		{
			manager->get_active_camera().set_perspective_projection(glm::radians(fov_y), aspect, perspective_near_far.x, perspective_near_far.y);
			manager->need_update();
		}
	}

	if (ImGui::DragFloat3(reinterpret_cast<const char*>(CAMERA_POSITION.data()), glm::value_ptr(position)))
	{
		manager->get_active_camera().set_position(position);
		manager->need_update();
	}

	if (ImGui::DragFloat3(reinterpret_cast<const char*>(CAMERA_DIRECTION.data()), glm::value_ptr(direction)))
	{
		manager->get_active_camera().set_direction(direction);
		manager->need_update();
	}

	if (ImGui::DragFloat3(reinterpret_cast<const char*>(CAMERA_WORLD_UP.data()), glm::value_ptr(world_up)))
	{
		manager->get_active_camera().set_world_up(world_up);
		manager->need_update();
	}
}
