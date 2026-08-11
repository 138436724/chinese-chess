#include "ui_node.h"

#include "scene/scene_manager.h"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <ranges>
#ifdef _WIN32
#include <commdlg.h>
#endif  // _WIN32


constexpr std::string_view MATERIAL_MANAGER          = "材质管理";
constexpr std::string_view ADD_MATERIAL              = "添加材质";
constexpr std::string_view MATERIAL                  = "材质";
constexpr std::string_view MATERIAL_INDEX            = "材质ID";
constexpr std::string_view MATERIAL_BACKGROUND_COLOR = "背景色";
constexpr std::string_view MATERIAL_FOREGROUND_COLOR = "前景色";
constexpr std::string_view MATERIAL_ROUGHNESS        = "粗糙度";
constexpr std::string_view MATERIAL_METALLIC         = "金属度";
constexpr std::string_view MATERIAL_OPACITY          = "不透明度";
constexpr std::string_view MATERIAL_IOR              = "折射率";
constexpr std::string_view MATERIAL_TRANSMISSION     = "透射强度";
constexpr std::string_view DELETE_MATERIAL           = "删除材质";
constexpr std::string_view NO_MATERIAL               = "无材质";
constexpr std::string_view ADD_IMAGE                 = "添加贴图";

constexpr std::string_view MODEL_MANAGER     = "物体管理";
constexpr std::string_view ADD_MODEL         = "添加物体";
constexpr std::string_view MODEL             = "物体";
constexpr std::string_view SHOW_MODEL        = "可见性";
constexpr std::string_view MODEL_TRANSLATION = "平移";
constexpr std::string_view MODEL_ROTATION    = "旋转";
constexpr std::string_view MODEL_SCALING     = "缩放";
constexpr std::string_view DELETE_MODEL      = "删除物体";
constexpr std::string_view NO_MODEL          = "无物体";


ui_node::ui_node(scene_manager& _manager) noexcept
    : manager(_manager)
{
}

void ui_node::resize(uint32_t /* _width*/, uint32_t /* _height*/) noexcept {}

void ui_node::update()
{
    update_model();
    update_material();
}

void ui_node::handle(int /*_glfw_key*/) noexcept {}

void ui_node::update_material()
{
    ImGui::SeparatorText(MATERIAL_MANAGER.data());

    if (ImGui::Button(ADD_MATERIAL.data()))
    {
        materials.push_back(manager.create<scene_material>());
    }

    std::optional<size_t> delete_index = std::nullopt;
    for (size_t i = 0; i < materials.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));

        auto& material_ptr = materials.at(i);

        if (ImGui::CollapsingHeader(std::format("{} {}", MATERIAL.data(), i).c_str(), ImGuiTreeNodeFlags_None))
        {
            if (ImGui::ColorEdit3(MATERIAL_BACKGROUND_COLOR.data(), glm::value_ptr(material_ptr->background_color)))
            {
                manager.need_material_update();
            }

            if (ImGui::ColorEdit3(MATERIAL_FOREGROUND_COLOR.data(), glm::value_ptr(material_ptr->foreground_color)))
            {
                manager.need_material_update();
            }

            if (!textures.empty())
            {
                ImGui::Text(textures.at(material_ptr->alpha_map).generic_string().c_str());
                ImGui::SameLine();
            }

            if (ImGui::Button(ADD_IMAGE.data()))
            {
                std::filesystem::path file_path;

#ifdef _WIN32
                TCHAR szFile[MAX_PATH] = {0};

                OPENFILENAME ofn;
                ZeroMemory(&ofn, sizeof(ofn));
                ofn.lStructSize  = sizeof(ofn);
                ofn.lpstrFile    = szFile;
                ofn.nMaxFile     = sizeof(szFile);
                ofn.lpstrFilter  = L"image\0*.png\0All\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

                if (GetOpenFileName(&ofn))
                {
                    file_path = szFile;
                }
#endif  // _WIN32

                if (!file_path.empty())
                {
                    if (material_ptr->alpha_map)
                    {
                        std::erase_if(textures, [&](const auto& pair) { return pair.first == material_ptr->alpha_map; });
                    }
                    material_ptr->alpha_map = manager.create<scene_image>(file_path, false);
                    textures.emplace(material_ptr->alpha_map, file_path);
                    manager.need_material_update();
                }
            }

            if (ImGui::DragFloat(MATERIAL_ROUGHNESS.data(), &material_ptr->roughness, 0.01f, 0.f, 1.f))
            {
                manager.need_material_update();
            }

            if (ImGui::DragFloat(MATERIAL_METALLIC.data(), &material_ptr->metallic, 0.01f, 0.f, 1.f))
            {
                manager.need_material_update();
            }

            if (ImGui::DragFloat(MATERIAL_OPACITY.data(), &material_ptr->opacity, 0.01f, 0.f, 1.f))
            {
                manager.need_material_update();
            }

            if (ImGui::DragFloat(MATERIAL_IOR.data(), &material_ptr->ior, 0.01f, 1.f, 3.f))
            {
                manager.need_material_update();
            }

            if (ImGui::DragFloat(MATERIAL_TRANSMISSION.data(), &material_ptr->transmission, 0.01f, 0.f, 1.f))
            {
                manager.need_material_update();
            }

            if (ImGui::Button(DELETE_MATERIAL.data()))
            {
                delete_index = i;
            }
        }

        ImGui::PopID();
    }

    if (delete_index.has_value())
    {
        std::erase_if(materials, [&](const auto& p) { return p == materials.at(delete_index.value()); });
        manager.need_material_update();
    }

    if (materials.empty())
    {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), NO_MATERIAL.data());
    }
}

void ui_node::update_model()
{
    ImGui::SeparatorText(MODEL_MANAGER.data());

    if (ImGui::Button(ADD_MODEL.data()))
    {
        std::filesystem::path file_path;

#ifdef _WIN32
        TCHAR szFile[MAX_PATH] = {0};

        OPENFILENAME ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize  = sizeof(ofn);
        ofn.lpstrFile    = szFile;
        ofn.nMaxFile     = sizeof(szFile);
        ofn.lpstrFilter  = L"gltf model\0*.glb\0All\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileName(&ofn))
        {
            file_path = szFile;
        }
#endif  // _WIN32

        if (!file_path.empty())
        {
            models.push_back(manager.create<scene_model>(file_path));
        }
    }

    std::optional<size_t> delete_index = std::nullopt;
    for (size_t i = 0; i < models.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));

        auto& model_ptr = models.at(i);

        if (ImGui::CollapsingHeader(std::format("{} {}", MODEL.data(), i).c_str(), ImGuiTreeNodeFlags_None))
        {
            if (ImGui::Checkbox(SHOW_MODEL.data(), &model_ptr->is_show))
            {
                manager.need_model_update();
            }

            glm::vec3 translate, scale, skew;
            glm::quat rotate;
            glm::vec4 perspective;
            if (glm::decompose(model_ptr->model_matrix, scale, rotate, translate, skew, perspective))
            {
                glm::vec3 euler_radians = glm::eulerAngles(rotate);
                glm::vec3 euler_degrees = glm::degrees(euler_radians);

                bool modified = false;
                modified |= ImGui::DragFloat3(MODEL_TRANSLATION.data(), glm::value_ptr(translate), 0.1f);
                modified |= ImGui::DragFloat3(MODEL_ROTATION.data(), glm::value_ptr(euler_degrees));
                modified |= ImGui::DragFloat3(MODEL_SCALING.data(), glm::value_ptr(scale), 0.1f, 0.001f, 10000.0f);

                if (modified)
                {
                    const glm::vec3 new_euler_radians = glm::radians(euler_degrees);
                    const glm::quat new_rotate        = glm::quat(new_euler_radians);

                    const glm::mat4 T = glm::translate(glm::mat4(1.0f), translate);
                    const glm::mat4 R = glm::mat4_cast(new_rotate);
                    const glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

                    model_ptr->model_matrix = T * R * S;
                    ;
                    manager.need_model_update();
                }
            }

            if (!materials.empty())
            {
                const auto all_material_index = std::views::iota(0u, materials.size())
                                                | std::views::transform([this](const auto index) {
                                                      return std::format("{} {}", MATERIAL.data(), index);
                                                  })
                                                | std::ranges::to<std::vector>();

                const auto all_material_index_string =
                    all_material_index | std::views::transform([](const auto& s) static { return s.c_str(); })
                    | std::ranges::to<std::vector>();

                int material_index = get_material_index(model_ptr->material).value_or(-1);
                if (ImGui::Combo(MATERIAL_INDEX.data(), &material_index, all_material_index_string.data(),
                                 static_cast<int>(all_material_index_string.size())))
                {
                    model_ptr->material = materials.at(material_index);
                    manager.need_model_update();
                }
            }

            if (ImGui::Button(DELETE_MODEL.data()))
            {
                delete_index = i;
            }
        }

        ImGui::PopID();
    }

    if (delete_index.has_value())
    {
        std::erase_if(models, [&](const auto& p) { return p == models.at(delete_index.value()); });
        manager.need_model_update();
    }

    if (models.empty())
    {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), NO_MODEL.data());
    }
}

std::optional<int> ui_node::get_material_index(const std::weak_ptr<scene_material>& _material) const noexcept
{
    const auto materials_with_index = materials | std::views::enumerate;

    const std::owner_less<void> cmp;
    const auto                  iter = std::ranges::find_if(materials_with_index, [&](const auto& p) {
        return !cmp(std::get<1>(p), _material) && !cmp(_material, std::get<1>(p));
    });

    if (iter != materials_with_index.end())
    {
        return static_cast<int>(std::get<0>(*iter));
    }
    else
    {
        return std::nullopt;
    }
}
