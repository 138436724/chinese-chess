#include "ui_environment.h"

#include "scene/scene_data_type.h"
#include "scene/scene_handle.h"
#include "scene/scene_manager.h"
#include "tools/image_helper.h"

#include <filesystem>
#include <imgui.h>
#include <string_view>

#ifdef _WIN32
#include <commdlg.h>
#endif  // _WIN32

namespace {

constexpr std::string_view ENVIRONMENT_MANAGER   = "环境管理";
constexpr std::string_view ADD_ENVIRONMENT       = "加载环境贴图";
constexpr std::string_view REMOVE_ENVIRONMENT    = "移除环境贴图";
constexpr std::string_view NO_ENVIRONMENT        = "无环境贴图（程序化天空）";
constexpr std::string_view ENVIRONMENT_INTENSITY = "环境强度";

[[nodiscard]] std::filesystem::path pick_environment_file()
{
    std::filesystem::path file_path;

#ifdef _WIN32
    TCHAR szFile[MAX_PATH] = {0};

    OPENFILENAME ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.lpstrFile    = szFile;
    ofn.nMaxFile     = sizeof(szFile);
    ofn.lpstrFilter  = L"HDR environment\0*.hdr\0All\0*.*\0";
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

ui_environment::ui_environment(scene_manager& _manager)
    : manager(_manager)
{
    manager.set_environment_image(manager.create_texture(std::filesystem::path(TEXTURES_PATH) / u8"cracked ground.hdr",
                                                         texture_usage::environment));
}

void ui_environment::resize(uint32_t /* _width*/, uint32_t /* _height*/) noexcept {}

void ui_environment::update()
{
    ImGui::SeparatorText(ENVIRONMENT_MANAGER.data());

    scene_environment_data& environment = manager.get_environment();

    if (ImGui::Button(ADD_ENVIRONMENT.data()))
    {
        const std::filesystem::path file_path = pick_environment_file();
        if (!file_path.empty())
        {
            const auto handle = manager.create_texture(file_path, texture_usage::environment);
            if (handle.is_valid())
            {
                manager.set_environment_image(handle);
            }
        }
    }

    if (manager.get(environment.image) == nullptr)
    {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), NO_ENVIRONMENT.data());
        return;
    }

    ImGui::SameLine();
    if (ImGui::Button(REMOVE_ENVIRONMENT.data()))
    {
        manager.set_environment_image({});
        return;
    }

    if (ImGui::DragFloat(ENVIRONMENT_INTENSITY.data(), &environment.intensity, 0.01f, 0.f, 100.f))
    {
        manager.need_environment_update();
    }
}

void ui_environment::handle(int /*_key*/, int /*_scancode*/, int /*_action*/, int /*_mods*/) noexcept {}
