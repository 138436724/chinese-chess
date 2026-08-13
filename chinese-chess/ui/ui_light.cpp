#include "ui_light.h"

#include "scene/scene_manager.h"

#include <glm/gtc/type_ptr.hpp>


namespace {
constexpr std::string_view LIGHT_MANAGER     = "灯光管理";
constexpr std::string_view LIGHT_TYPE        = "灯光类型";
constexpr std::string_view DIRECTIONAL_LIGHT = "平行光";
constexpr std::string_view POINT_LIGHT       = "点光源";
constexpr std::string_view SPOT_LIGHT        = "聚光灯";
constexpr std::string_view ADD_LIGHT         = "添加灯光";
constexpr std::string_view DELETE_LIGHT      = "删除灯光";
constexpr std::string_view NO_LIGHT          = "无灯光";
constexpr std::string_view LIGHT_COLOR       = "灯光颜色";
constexpr std::string_view LIGHT_INTENSITY   = "灯光强度";
constexpr std::string_view LIGHT_DIRECTION   = "灯光方向";
constexpr std::string_view LIGHT_POSITION    = "灯光位置";
constexpr std::string_view LIGHT_RANGE       = "灯光范围";
constexpr std::string_view LIGHT_INNER_CONE  = "内锥角";
constexpr std::string_view LIGHT_OUTER_CONE  = "外锥角";
}  // namespace


ui_light::ui_light(scene_manager& _manager)
    : manager(_manager)
{
    auto light = manager.create<scene_light>();
    *light     = scene_light{.active_type = light_type::directional,
                             .color       = glm::vec3(1.0f, 0.95f, 0.85f),
                             .intensity   = 5.f,
                             .direction   = glm::vec3(1.f, 1.f, 10.f)};
    lights.push_back(std::move(light));
}

void ui_light::resize(uint32_t /* _width*/, uint32_t /* _height*/) noexcept {}

void ui_light::update()
{
    ImGui::SeparatorText(LIGHT_MANAGER.data());

    const std::array all_light_types = {DIRECTIONAL_LIGHT.data(), POINT_LIGHT.data(), SPOT_LIGHT.data()};
    ImGui::Combo(LIGHT_TYPE.data(), &add_light_type, all_light_types.data(), static_cast<int>(all_light_types.size()));

    if (ImGui::Button(ADD_LIGHT.data()))
    {
        auto light         = manager.create<scene_light>();
        light->active_type = static_cast<light_type>(add_light_type);
        lights.push_back(std::move(light));
    }

    std::optional<size_t> delete_index = std::nullopt;
    for (size_t i = 0; i < lights.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));

        auto& light_ptr = lights.at(i);

        std::string header_label;
        switch (light_ptr->active_type)
        {
            case light_type::directional:
                header_label = std::format("{} {}", DIRECTIONAL_LIGHT.data(), i);
                break;
            case light_type::point:
                header_label = std::format("{} {}", POINT_LIGHT.data(), i);
                break;
            case light_type::spot:
                header_label = std::format("{} {}", SPOT_LIGHT.data(), i);
                break;
            default:
                std::unreachable();
        }

        bool modified = false;
        if (ImGui::CollapsingHeader(header_label.c_str(), ImGuiTreeNodeFlags_None))
        {
            modified |= ImGui::ColorEdit3(LIGHT_COLOR.data(), glm::value_ptr(light_ptr->color));
            modified |= ImGui::DragFloat(LIGHT_INTENSITY.data(), &light_ptr->intensity, 0.1f, 0.0f, 100.0f);

            switch (light_ptr->active_type)
            {
                case light_type::directional:
                    modified |= ImGui::DragFloat3(LIGHT_DIRECTION.data(), glm::value_ptr(light_ptr->direction), 0.01f);
                    break;
                case light_type::point:
                    modified |= ImGui::DragFloat3(LIGHT_POSITION.data(), glm::value_ptr(light_ptr->position), 0.1f);
                    modified |= ImGui::DragFloat(LIGHT_RANGE.data(), &light_ptr->range, 0.1f, 0.1f, 1000.0f);
                    break;
                case light_type::spot:
                    modified |= ImGui::DragFloat3(LIGHT_DIRECTION.data(), glm::value_ptr(light_ptr->direction), 0.01f);
                    modified |= ImGui::DragFloat3(LIGHT_POSITION.data(), glm::value_ptr(light_ptr->position), 0.1f);
                    modified |= ImGui::DragFloat(LIGHT_RANGE.data(), &light_ptr->range, 0.1f, 0.1f, 1000.0f);
                    if (ImGui::SliderAngle(LIGHT_INNER_CONE.data(), &light_ptr->inner_cone_angle, 0.f, 90.f))
                    {
                        modified |= true;
                        if (glm::degrees(light_ptr->inner_cone_angle) >= glm::degrees(light_ptr->outer_cone_angle))
                        {
                            light_ptr->inner_cone_angle = light_ptr->outer_cone_angle - 1.f;
                        }
                    }
                    if (ImGui::SliderAngle(LIGHT_OUTER_CONE.data(), &light_ptr->outer_cone_angle, 0.f, 90.f))
                    {
                        modified |= true;
                        if (glm::degrees(light_ptr->outer_cone_angle) <= glm::degrees(light_ptr->inner_cone_angle))
                        {
                            light_ptr->outer_cone_angle = light_ptr->inner_cone_angle + 1.f;
                        }
                    }
                    break;
                default:
                    std::unreachable();
            }

            if (modified)
            {
                manager.need_light_update();
            }

            if (ImGui::Button(DELETE_LIGHT.data()))
            {
                delete_index = i;
            }
        }

        ImGui::PopID();
    }

    if (delete_index.has_value())
    {
        std::erase_if(lights, [&](const auto& p) { return p == lights.at(delete_index.value()); });
        manager.need_light_update();
    }

    if (lights.empty())
    {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), NO_LIGHT.data());
    }
}

void ui_light::handle(int /*_glfw_key*/) noexcept {}
