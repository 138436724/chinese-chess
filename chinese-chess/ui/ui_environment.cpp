/**
 * @file ui_environment.cpp
 * @brief ui_environment 的实现：HDR 文件对话框、环境图替换与强度编辑。
 * @details pick_environment_file 用 Win32 的 GetOpenFileName 选 .hdr（非 Windows 返回空路径），
 *          构造函数在启动时加载资源目录里的默认 HDR；update 根据环境句柄是否有效切换
 *          “可移除”与“程序化天空”两种 UI 状态。
 */
#include "ui_environment.h"

#include "scene/scene_data_type.h"
#include "scene/scene_handle.h"
#include "scene/scene_manager.h"
#include "tools/image_helper.h"

#include <filesystem>
#include <imgui.h>
#include <iterator>
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

/**
 * @brief 弹出 Win32 打开文件对话框，过滤 .hdr；取消或非 Windows 平台返回空路径。
 * @return 选中的路径；用户取消时为空。
 */
[[nodiscard]] std::filesystem::path pick_environment_file()
{
    std::filesystem::path file_path;

#ifdef _WIN32
    TCHAR szFile[MAX_PATH] = {0};

    OPENFILENAME ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.lpstrFile    = szFile;
    ofn.nMaxFile     = static_cast<DWORD>(std::size(szFile));
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

/**
 * @brief 加载资源目录里的默认 HDR 并设为环境图。
 * @note 路径拼接依赖运行目录为 chinese-chess/；创建出的贴图句柄直接交给 set_environment_image。
 */
ui_environment::ui_environment(scene_manager& _manager)
    : manager(_manager)
{
    manager.set_environment_image(manager.create_texture(std::filesystem::path(TEXTURES_PATH) / u8"cracked ground.hdr",
                                                         texture_usage::environment));
}

void ui_environment::resize(uint32_t /* _width*/, uint32_t /* _height*/) noexcept {}

/**
 * @brief 构建环境控件：加载、移除与强度；无有效环境图时提前返回。
 * @note 强度直接改 scene_environment_data::intensity 再置环境脏，不维护本地副本。
 */
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
