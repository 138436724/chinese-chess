#include "ui_light.h"
#include <glm/gtc/type_ptr.hpp>

constexpr std::u8string_view LIGHT_MANAGER = u8"灯光管理";
constexpr std::u8string_view LIGHT_TYPE = u8"灯光类型";
constexpr std::u8string_view DIRECTIONAL_LIGHT = u8"平行光";
constexpr std::u8string_view POINT_LIGHT = u8"点光源";
constexpr std::u8string_view SPOT_LIGHT = u8"聚光灯";
constexpr std::u8string_view ADD_LIGHT = u8"添加灯光";
constexpr std::u8string_view DELETE_LIGHT = u8"删除灯光";
constexpr std::u8string_view NO_LIGHT = u8"无灯光";
constexpr std::u8string_view LIGHT_COLOR = u8"灯光颜色";
constexpr std::u8string_view LIGHT_INTENSITY = u8"灯光强度";
constexpr std::u8string_view LIGHT_DIRECTION = u8"灯光方向";
constexpr std::u8string_view LIGHT_POSITION = u8"灯光位置";
constexpr std::u8string_view LIGHT_RANGE = u8"灯光范围";
constexpr std::u8string_view LIGHT_INNER_CONE = u8"内锥角";
constexpr std::u8string_view LIGHT_OUTER_CONE = u8"外锥角";

void ui_light::create(scene_manager* _manager)
{
	manager = _manager;

	auto light = manager->create<directional_light>();
	*light = directional_light{
		.color = glm::vec3(1.0f, 0.95f, 0.85f),
		.intensity = 5.f,
		.direction = glm::vec3(1.f, 1.f, 10.f)
	};
	lights.push_back(std::move(light));
}

void ui_light::resize(uint32_t _width, uint32_t _height) noexcept
{
}

void ui_light::update() noexcept
{
	ImGui::SeparatorText(reinterpret_cast<const char*>(LIGHT_MANAGER.data()));

	const std::array all_light_types = {
		reinterpret_cast<const char*>(DIRECTIONAL_LIGHT.data()),
		reinterpret_cast<const char*>(POINT_LIGHT.data()),
		reinterpret_cast<const char*>(SPOT_LIGHT.data())
	};
	ImGui::Combo(reinterpret_cast<const char*>(LIGHT_TYPE.data()), &add_light_type, all_light_types.data(), static_cast<int>(all_light_types.size()));

	if (ImGui::Button(reinterpret_cast<const char*>(ADD_LIGHT.data())))
	{
		switch (add_light_type)
		{
		case 0:
			lights.push_back(manager->create<std::variant_alternative_t<0u, scene_light>>());
			break;
		case 1:
			lights.push_back(manager->create<std::variant_alternative_t<1u, scene_light>>());
			break;
		case 2:
			lights.push_back(manager->create<std::variant_alternative_t<2u, scene_light>>());
			break;
		default:
			break;
		}
	}

	std::optional<size_t> delete_index = std::nullopt;
	for (size_t i = 0; i < lights.size(); ++i)
	{
		ImGui::PushID(static_cast<int>(i));

		std::visit([&](auto& light_data)
			{
				using T = std::decay_t<decltype(light_data)>;

				std::string header_label;
				if constexpr (std::is_same_v<T, directional_light>)
				{
					header_label = std::format("{} {}", reinterpret_cast<const char*>(DIRECTIONAL_LIGHT.data()), i);
				}
				else if constexpr (std::is_same_v<T, point_light>)
				{
					header_label = std::format("{} {}", reinterpret_cast<const char*>(POINT_LIGHT.data()), i);
				}
				else if constexpr (std::is_same_v<T, spot_light>)
				{
					header_label = std::format("{} {}", reinterpret_cast<const char*>(SPOT_LIGHT.data()), i);
				}

				bool modified = false;
				if (ImGui::CollapsingHeader(header_label.c_str(), ImGuiTreeNodeFlags_None))
				{
					modified |= ImGui::ColorEdit3(reinterpret_cast<const char*>(LIGHT_COLOR.data()), glm::value_ptr(light_data.color));
					modified |= ImGui::DragFloat(reinterpret_cast<const char*>(LIGHT_INTENSITY.data()), &light_data.intensity, 0.1f, 0.0f, 100.0f);

					if constexpr (std::is_same_v<T, directional_light>)
					{
						modified |= ImGui::DragFloat3(reinterpret_cast<const char*>(LIGHT_DIRECTION.data()), glm::value_ptr(light_data.direction), 0.01f);
					}
					else if constexpr (std::is_same_v<T, point_light>)
					{
						modified |= ImGui::DragFloat3(reinterpret_cast<const char*>(LIGHT_POSITION.data()), glm::value_ptr(light_data.position), 0.1f);
						modified |= ImGui::DragFloat(reinterpret_cast<const char*>(LIGHT_RANGE.data()), &light_data.range, 0.1f, 0.1f, 1000.0f);
					}
					else if constexpr (std::is_same_v<T, spot_light>)
					{
						modified |= ImGui::DragFloat3(reinterpret_cast<const char*>(LIGHT_DIRECTION.data()), glm::value_ptr(light_data.direction), 0.01f);
						modified |= ImGui::DragFloat3(reinterpret_cast<const char*>(LIGHT_POSITION.data()), glm::value_ptr(light_data.position), 0.1f);
						modified |= ImGui::DragFloat(reinterpret_cast<const char*>(LIGHT_RANGE.data()), &light_data.range, 0.1f, 0.1f, 1000.0f);
						modified |= ImGui::SliderAngle(reinterpret_cast<const char*>(LIGHT_INNER_CONE.data()), &light_data.inner_cone_angle, glm::radians(1.0f), glm::radians(light_data.outer_cone_angle));
						modified |= ImGui::SliderAngle(reinterpret_cast<const char*>(LIGHT_OUTER_CONE.data()), &light_data.outer_cone_angle, glm::radians(light_data.inner_cone_angle), glm::radians(90.0f));
					}

					if (modified)
					{
						manager->need_update();
					}

					if (ImGui::Button(reinterpret_cast<const char*>(DELETE_LIGHT.data())))
					{
						delete_index = i;
					}
				}
			}, *lights.at(i));

		ImGui::PopID();
	}

	if (delete_index.has_value())
	{
		std::erase_if(lights, [&](const auto& p) { return p == lights.at(delete_index.value()); });
		manager->need_update();
	}

	if (lights.empty())
	{
		ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), reinterpret_cast<const char*>(NO_LIGHT.data()));
	}
}
