/**
 * @file ui_node.cpp
 * @brief ui_node 的实现：贴图热重载、材质字段编辑、模型变换分解与文件对话框。
 * @details select_file 封装 Win32 的 GetOpenFileName；update 先用 FILE_WATCHER 检查缓存贴图是否变过，
 *          再清掉已死句柄，随后构建物体区与材质区。模型变换在分解出的平移/欧拉角/缩放上编辑，
 *          改完按 T * R * S 重组成 model_matrix。
 */
#include "ui_node.h"

#include "scene/scene_data_type.h"
#include "scene/scene_manager.h"
#include "tools/file_watcher.h"
#include "vulkan_core/vulkan_data_type.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <commdlg.h>
#endif  // _WIN32


namespace {

constexpr std::string_view MATERIAL_MANAGER           = "材质管理";
constexpr std::string_view ADD_MATERIAL               = "添加材质";
constexpr std::string_view MATERIAL                   = "材质";
constexpr std::string_view MATERIAL_INDEX             = "材质ID";
constexpr std::string_view MATERIAL_ALBEDO            = "反照率";
constexpr std::string_view MATERIAL_ROUGHNESS         = "粗糙度";
constexpr std::string_view MATERIAL_METALLIC          = "金属度";
constexpr std::string_view MATERIAL_REFLECTANCE       = "镜面反射率";
constexpr std::string_view MATERIAL_OPACITY           = "阴影透射率";
constexpr std::string_view MATERIAL_IOR               = "折射率";
constexpr std::string_view MATERIAL_TRANSMISSION      = "透射占比";
constexpr std::string_view MATERIAL_ABSORPTION        = "介质吸收系数";
constexpr std::string_view MATERIAL_EMISSION          = "自发光颜色";
constexpr std::string_view MATERIAL_EMISSION_STRENGTH = "自发光强度";
constexpr std::string_view DELETE_MATERIAL            = "删除材质";
constexpr std::string_view NO_MATERIAL                = "无材质";
constexpr std::string_view ADD_IMAGE                  = "添加贴图";

constexpr std::string_view MODEL_MANAGER     = "物体管理";
constexpr std::string_view ADD_MODEL         = "添加物体";
constexpr std::string_view MODEL             = "物体";
constexpr std::string_view SHOW_MODEL        = "可见性";
constexpr std::string_view MODEL_TRANSLATION = "平移";
constexpr std::string_view MODEL_ROTATION    = "旋转";
constexpr std::string_view MODEL_SCALING     = "缩放";
constexpr std::string_view DELETE_MODEL      = "删除物体";
constexpr std::string_view NO_MODEL          = "无物体";

/** @brief 贴图通道与材质面板按钮文案的对照行，驱动通道按钮的循环。 */
struct texture_channel_name_map
{
    texture_channel  channel;
    std::string_view label;
};
/** @brief 当前参与 UI 的通道表；目前只有 albedo，扩展通道时在此追加一行。 */
constexpr std::array CHANNEL_NAME_MAP = {texture_channel_name_map{texture_channel::albedo, ADD_IMAGE}};

/**
 * @brief 弹出 Win32 打开文件对话框，按调用方给的过滤器选文件；取消或非 Windows 返回空路径。
 * @param _filters 双空字符结尾的 Win32 过滤器串，直接作为 lpstrFilter。
 * @return 选中的路径；用户取消时为空。
 */
[[nodiscard]] std::filesystem::path select_file(std::wstring_view _filters)
{
    std::filesystem::path file_path;

#ifdef _WIN32
    TCHAR szFile[MAX_PATH] = {0};

    OPENFILENAME ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.lpstrFile    = szFile;
    ofn.nMaxFile     = static_cast<DWORD>(std::size(szFile));
    ofn.lpstrFilter  = _filters.data();
    ofn.nFilterIndex = 1;
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileName(&ofn))
    {
        file_path = szFile;
    }
#endif  // _WIN32

    return file_path;
}

}  // namespace


ui_node::ui_node(scene_manager& _manager) noexcept
    : manager(_manager)
{
}

void ui_node::resize(uint32_t /* _width*/, uint32_t /* _height*/) noexcept {}

/**
 * @brief 先热重载被修改的贴图并清理死句柄，再重建物体区与材质区。
 * @note 文件监视只在句柄仍有效时调用 reload_texture，失败结果被忽略、旧纹理保留。
 */
void ui_node::update()
{
    for (const auto& [path, handle] : textures)
    {
        if (manager.get(handle) != nullptr && FILE_WATCHER.is_file_modified(path))
        {
            std::ignore = manager.reload_texture(handle);
        }
    }
    std::erase_if(textures, [this](const auto& _pair) { return manager.get(_pair.second) == nullptr; });

    update_model();
    update_material();
}

void ui_node::handle(int /*_key*/, int /*_scancode*/, int /*_action*/, int /*_mods*/) noexcept {}

/**
 * @brief 构建材质区：添加/删除、albedo、贴图通道按钮与全部 PBR 参数。
 * @note 删除只记下标，循环结束后统一 destroy 并 erase；按钮文案由 CHANNEL_NAME_MAP 驱动。
 */
void ui_node::update_material()
{
    ImGui::SeparatorText(MATERIAL_MANAGER.data());

    if (ImGui::Button(ADD_MATERIAL.data()))
    {
        materials.push_back(manager.create_material());
    }

    std::optional<size_t> delete_index = std::nullopt;
    std::string           header_label;
    for (size_t i = 0; i < materials.size(); ++i)
    {
        auto* material_ptr = manager.get(materials.at(i));
        if (material_ptr == nullptr)
        {
            continue;
        }

        ImGui::PushID(static_cast<int>(i));

        header_label.clear();
        std::format_to(std::back_inserter(header_label), "{} {}", MATERIAL, i);

        if (ImGui::CollapsingHeader(header_label.c_str(), ImGuiTreeNodeFlags_None))
        {
            if (ImGui::ColorEdit3(MATERIAL_ALBEDO.data(), glm::value_ptr(material_ptr->albedo)))
            {
                manager.need_material_update();
            }

            for (const auto& row : CHANNEL_NAME_MAP)
            {
                const auto& channel_map = material_ptr->textures[std::to_underlying(row.channel)];

                if (!textures.empty())
                {
                    const auto texture_path = std::ranges::find_if(textures, [&channel_map](const auto& _pair) {
                        return _pair.second == channel_map;
                    });
                    if (texture_path != textures.end())
                    {
                        ImGui::TextUnformatted(texture_path->first.generic_string().c_str());
                        ImGui::SameLine();
                    }
                }

                ImGui::PushID(static_cast<int>(std::to_underlying(row.channel)));

                if (ImGui::Button(row.label.data()))
                {
                    const std::filesystem::path file_path = select_file(L"image\0*.png\0All\0*.*\0");
                    if (!file_path.empty())
                    {
                        const auto handle = manager.create_texture(file_path, texture_usage::color);
                        if (handle.is_valid())
                        {
                            manager.set_material_texture(materials.at(i), row.channel, handle);
                            textures.insert_or_assign(file_path, handle);
                        }
                    }
                }

                ImGui::PopID();
            }

            // 粗糙度下限交给着色器（alpha 有数值下限），UI 允许到 0（完美镜面）
            if (ImGui::DragFloat(MATERIAL_ROUGHNESS.data(), &material_ptr->roughness, 0.01f, 0.f, 1.f))
            {
                manager.need_material_update();
            }

            if (ImGui::DragFloat(MATERIAL_METALLIC.data(), &material_ptr->metallic, 0.01f, 0.f, 1.f))
            {
                manager.need_material_update();
            }

            // 电介质镜面反射率：F0 = 0.08 · reflectance（0.5 → 4%，金属度 1 时被反照率取代）
            if (ImGui::DragFloat(MATERIAL_REFLECTANCE.data(), &material_ptr->reflectance, 0.01f, 0.f, 1.f))
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

            // 介质吸收系数 σ_a（Beer-Lambert，单位 1/世界单位）：仅 transmission > 0 时生效
            if (ImGui::DragFloat3(MATERIAL_ABSORPTION.data(), glm::value_ptr(material_ptr->absorption), 0.01f, 0.f, 100.f))
            {
                manager.need_material_update();
            }

            if (ImGui::ColorEdit3(MATERIAL_EMISSION.data(), glm::value_ptr(material_ptr->emission)))
            {
                manager.need_material_update();
            }

            if (ImGui::DragFloat(MATERIAL_EMISSION_STRENGTH.data(), &material_ptr->emission_strength, 0.01f, 0.f, 1000.f))
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
        manager.destroy(materials.at(*delete_index));
        materials.erase(materials.begin() + static_cast<std::ptrdiff_t>(*delete_index));
    }

    if (materials.empty())
    {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), NO_MATERIAL.data());
    }
}

/**
 * @brief 构建物体区：添加/删除、可见性、分解后的变换编辑与材质下拉。
 * @note glm::decompose 失败时只跳过变换控件；材质下拉的字符串表每次重建。
 */
void ui_node::update_model()
{
    ImGui::SeparatorText(MODEL_MANAGER.data());

    if (ImGui::Button(ADD_MODEL.data()))
    {
        const std::filesystem::path file_path = select_file(L"gltf model\0*.glb\0All\0*.*\0");
        if (!file_path.empty())
        {
            const auto handle = manager.create_model(file_path);
            if (handle.is_valid())
            {
                models.push_back(handle);
            }
        }
    }

    std::optional<size_t> delete_index = std::nullopt;
    std::string           header_label;
    for (size_t i = 0; i < models.size(); ++i)
    {
        auto* model_ptr = manager.get(models.at(i));
        if (model_ptr == nullptr)
        {
            continue;
        }

        ImGui::PushID(static_cast<int>(i));

        header_label.clear();
        std::format_to(std::back_inserter(header_label), "{} {}", MODEL, i);

        if (ImGui::CollapsingHeader(header_label.c_str(), ImGuiTreeNodeFlags_None))
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
                const glm::vec3 euler_radians = glm::eulerAngles(rotate);
                glm::vec3       euler_degrees = glm::degrees(euler_radians);

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

                    manager.need_model_update();
                }
            }

            if (!materials.empty())
            {
                std::string all_material_items;
                for (size_t j = 0; j < materials.size(); ++j)
                {
                    std::format_to(std::back_inserter(all_material_items), "{} {}", MATERIAL, j);
                    all_material_items.push_back('\0');
                }
                all_material_items.push_back('\0');

                int material_index = material_index_of(model_ptr->material).value_or(-1);
                if (ImGui::Combo(MATERIAL_INDEX.data(), &material_index, all_material_items.c_str()))
                {
                    model_ptr->material = materials.at(static_cast<size_t>(material_index));
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
        manager.destroy(models.at(*delete_index));
        models.erase(models.begin() + static_cast<std::ptrdiff_t>(*delete_index));
    }

    if (models.empty())
    {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), NO_MODEL.data());
    }
}

std::optional<int> ui_node::material_index_of(material_handle _material) const noexcept
{
    const auto iter = std::ranges::find(materials, _material);
    if (iter == materials.end())
    {
        return std::nullopt;
    }
    return static_cast<int>(std::distance(materials.begin(), iter));
}
