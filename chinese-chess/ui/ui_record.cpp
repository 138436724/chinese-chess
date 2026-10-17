#include "ui_record.h"

#include "scene/scene_data_type.h"
#include "scene/scene_manager.h"
#include "tools/font_loader.h"
#include "tools/model_loader.h"
#include "vulkan_core/vulkan_data_type.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <imgui.h>
#include <iostream>
#include <iterator>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <commdlg.h>
#endif  // _WIN32


namespace {

constexpr std::string_view RECORDS_MANAGER = "棋局管理";
constexpr std::string_view RECORDS_LIST    = "棋谱列表";
constexpr std::string_view LOAD_RECORDS    = "加载棋谱";
constexpr std::string_view LAST_STEP       = "上一步";
constexpr std::string_view NEXT_STEP       = "下一步";

constexpr uint32_t board_font_padding = 2;

constexpr float chess_board_z      = -1.5f;
constexpr float chess_board_line_z = -1.45f;
constexpr float chess_piece_z      = -1.4f;

constexpr glm::vec3 board_background_color = glm::vec3(0.87843f, 0.69020f, 0.48627f);
constexpr glm::vec3 board_foreground_color = glm::vec3(0.0f, 0.0f, 0.0f);

constexpr glm::vec3 red_piece_background_color = glm::vec3(0.95f, 0.92f, 0.85f);
constexpr glm::vec3 red_piece_foreground_color = glm::vec3(0.45f, 0.08f, 0.06f);
constexpr glm::vec3 red_piece_absorption       = glm::vec3(1.5f, 1.2f, 1.0f);

constexpr glm::vec3 black_piece_background_color = glm::vec3(0.15f, 0.45f, 0.32f);
constexpr glm::vec3 black_piece_foreground_color = glm::vec3(0.02f, 0.10f, 0.06f);
constexpr glm::vec3 black_piece_absorption       = glm::vec3(4.0f, 1.6f, 1.0f);

[[nodiscard]] constexpr glm::vec2 location_transform(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, uint8_t _x, uint8_t _y) noexcept
{
    // 棋盘中心为坐标的(0, 0)点，而右下和左上作为双方棋子的定位原点
    glm::vec2 location{};
    if (_use_color == PIECE_COLOR::RED)
    {
        if (_piece_color == PIECE_COLOR::RED)
        {
            // 红方红子从右到左是一到九，先将_x映射到坐标对应的位置，然后-1计算格子数
            location.x = static_cast<float>(10 - _x - 1);
            location.y = static_cast<float>(9 - _y);
        }
        else
        {
            // 红方黑子从左到右是1到9，先将_x映射到坐标对应的位置，然后-1计算格子数
            location.x = static_cast<float>(_x - 1);
            location.y = static_cast<float>(_y);
        }
    }
    else
    {
        if (_piece_color == PIECE_COLOR::RED)
        {
            // 黑方红子从左到右是一到九，先将_x映射到坐标对应的位置，然后-1计算格子数
            location.x = static_cast<float>(_x - 1);
            location.y = static_cast<float>(_y);
        }
        else
        {
            // 黑方黑子从右到左是1到9，先将_x映射到坐标对应的位置，然后-1计算格子数
            location.x = static_cast<float>(10 - _x - 1);
            location.y = static_cast<float>(9 - _y);
        }
    }

    constexpr float board_unit_distance = 0.25f;
    location                            = (location - glm::vec2(4, 4.5)) * board_unit_distance;
    return location;
}
}  // namespace


ui_record::ui_record(scene_manager& _manager)
    : manager(_manager)
{
    // create board
    const auto chess_board_material = manager.create_material();
    if (auto* material = manager.get(chess_board_material))
    {
        material->albedo      = glm::vec3(1.f);
        material->roughness   = 0.4f;
        material->reflectance = 0.4f;
    }

    chess_board = manager.create_model(std::string(MODELS_PATH) + "chess_board.glb");
    if (auto* model = manager.get(chess_board))
    {
        model->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, chess_board_z));
        model->material     = chess_board_material;
    }


    // create board line
    chess_board_line = manager.create_model(std::string(MODELS_PATH) + "chess_board_line.glb");
    if (auto* model = manager.get(chess_board_line))
    {
        model->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, chess_board_line_z));
    }


    // create all pieces
    for (auto& piece : all_chess_pieces)
    {
        piece = manager.create_model(std::string(MODELS_PATH) + "chess_piece.glb");
        if (auto* model = manager.get(piece))
        {
            model->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, chess_piece_z));
        }
    }
}

void ui_record::resize(uint32_t _width, uint32_t _height)
{
    const bool size_changed = _height != height;
    width                   = _width;
    height                  = _height;

    if (!size_changed)
    {
        return;
    }

    const std::string font_path = std::string(FONTS_PATH) + "LXGWWenKaiGB-Medium.ttf";

    if (auto* board = manager.get(chess_board); board != nullptr)
    {
        manager.set_material_texture(board->material, texture_channel::albedo,
                                     manager.create_texture(font_path, static_cast<uint32_t>(height / 9.0 * 2), L"楚河汉界",
                                                            board_font_padding, board_foreground_color, board_background_color));
    }

    rebuild_piece_materials(red_chess_piece_materials, L"帥仕相傌俥炮兵", u"帥仕相傌俥炮兵", red_piece_background_color,
                            red_piece_foreground_color, red_piece_absorption);
    rebuild_piece_materials(black_chess_piece_materials, L"將士象馬車砲卒", u"將士象馬車砲卒",
                            black_piece_background_color, black_piece_foreground_color, black_piece_absorption);

    manager.need_material_update();

    restore_board_state(static_cast<uint32_t>(now_record_index));
}

void ui_record::rebuild_piece_materials(std::unordered_map<PIECE_TYPE, material_handle>& _materials,
                                        std::wstring_view                                _characters,
                                        std::u16string_view                              _piece_names,
                                        const glm::vec3&                                 _background_color,
                                        const glm::vec3&                                 _foreground_color,
                                        const glm::vec3&                                 _absorption)
{
    const std::string font_path = std::string(FONTS_PATH) + "LXGWWenKaiGB-Medium.ttf";
    const uint32_t    font_size = static_cast<uint32_t>(height / 9.0 * 2);

    std::ranges::for_each(std::views::zip(_characters, _piece_names), [&](const auto& _pair) {
        const auto& [character, piece_name] = _pair;
        const auto type                     = record_loader::get_piece_type(piece_name);

        material_handle handle;
        if (const auto iter = _materials.find(type); iter != _materials.end())
        {
            handle = iter->second;
        }
        else
        {
            handle = manager.create_material();
            _materials.emplace(type, handle);
        }

        auto* material = manager.get(handle);
        if (material == nullptr)
        {
            return;
        }

        material->albedo       = glm::vec3(1.f);
        material->roughness    = 0.25f;
        material->reflectance  = 0.5f;
        material->metallic     = 0.0f;
        material->ior          = 1.5f;
        material->transmission = 0.35f;
        material->absorption   = _absorption;
        material->opacity      = 1.0f;

        manager.set_material_texture(handle, texture_channel::albedo,
                                     manager.create_texture(font_path, font_size, std::wstring(1, character),
                                                            board_font_padding, _foreground_color, _background_color));
    });
}

void ui_record::update()
{
    ImGui::SeparatorText(RECORDS_MANAGER.data());

    if (ImGui::Button(LOAD_RECORDS.data()))
    {
        std::filesystem::path file_path;

#ifdef _WIN32
        TCHAR szFile[MAX_PATH] = {0};

        OPENFILENAME ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize  = sizeof(ofn);
        ofn.lpstrFile    = szFile;
        ofn.nMaxFile     = static_cast<DWORD>(std::size(szFile));
        ofn.lpstrFilter  = L"Text\0*.txt\0All\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileName(&ofn))
        {
            file_path = szFile;
        }
#endif  // _WIN32

        if (!file_path.empty())
        {
            load_records(file_path);
            restore_board_state(0);
        }
    }

    if (!all_records.empty())
    {
        if (ImGui::ListBox(RECORDS_LIST.data(), &now_record_index, all_records_c_str.data(),
                           static_cast<int>(all_records_c_str.size())))
        {
            restore_board_state(static_cast<uint32_t>(now_record_index));
        }
        if (ImGui::Button(LAST_STEP.data()))
        {
            prev_step();
        }
        ImGui::SameLine();
        if (ImGui::Button(NEXT_STEP.data()))
        {
            next_step();
        }
    }
}

void ui_record::handle(int _key, int /*_scancode*/, int _action, int /*_mods*/) noexcept
{
    if (_action != GLFW_PRESS)
    {
        return;
    }

    switch (_key)
    {
        case GLFW_KEY_W:
        case GLFW_KEY_A:
            prev_step();
            break;
        case GLFW_KEY_S:
        case GLFW_KEY_D:
            next_step();
            break;
        default:
            break;
    }
}

void ui_record::load_records(const std::filesystem::path& _record_path)
{
    auto records = record_loader::read_record<std::string>(_record_path);
    if (!records)
    {
        std::println(std::cerr, "Cannot read record file {}: {}", _record_path.generic_string(), records.error());
        return;
    }

    auto states = record_loader::load_records(_record_path);
    if (!states)
    {
        std::println(std::cerr, "Cannot read record file {}: {}", _record_path.generic_string(), states.error());
        return;
    }

    if (states->empty())
    {
        std::println(std::cerr, "Record file {} contains no valid move.", _record_path.generic_string());
        return;
    }

    all_records       = std::move(*records);
    all_records_c_str = all_records | std::views::transform([](const auto& _record) static { return _record.data(); })
                        | std::ranges::to<std::vector>();

    board_state = std::move(*states);

    now_record_index = 0;
}

//all_board_state ui_record::capture_board_state()
//{
//	all_board_state state;
//
//	for (auto [_color, _pieces] : all_chess_pieces | std::views::enumerate)
//	{
//		for (auto& _piece : _pieces)
//		{
//			if (_piece.get_is_on_board())
//			{
//				state.at(_color).emplace_back(piece_state(_piece.get_piece_type(), _piece.get_piece_location().first, _piece.get_piece_location().second));
//			}
//		}
//	}
//
//	return state;
//}

void ui_record::restore_board_state(uint32_t _index) noexcept
{
    if (_index >= board_state.size())
    {
        return;
    }

    std::ranges::for_each(all_chess_pieces, [this](const auto& _handle) {
        if (auto* model = manager.get(_handle); model != nullptr)
        {
            model->is_show = false;
        }
    });

    const all_board_state& state        = board_state.at(_index);
    size_t                 index_offset = 0;
    std::ranges::for_each(state.at(static_cast<size_t>(PIECE_COLOR::BLACK)) | std::views::enumerate, [&](const auto& _pair) {
        const auto& [index, piece] = _pair;
        const auto& handle         = all_chess_pieces.at(index + index_offset);
        auto*       sp             = manager.get(handle);
        if (sp == nullptr)
        {
            return;
        }
        sp->material = black_chess_piece_materials.at(piece.piece_type);
        sp->is_show  = true;
        sp->model_matrix =
            glm::translate(glm::mat4(1.f),
                           glm::vec3(location_transform(PIECE_COLOR::RED, PIECE_COLOR::BLACK, piece.x, piece.y), chess_piece_z));
    });

    index_offset = state.at(static_cast<size_t>(PIECE_COLOR::BLACK)).size();
    std::ranges::for_each(state.at(static_cast<size_t>(PIECE_COLOR::RED)) | std::views::enumerate, [&](const auto& _pair) {
        const auto& [index, piece] = _pair;
        const auto& handle         = all_chess_pieces.at(index + index_offset);
        auto*       sp             = manager.get(handle);
        if (sp == nullptr)
        {
            return;
        }
        sp->material = red_chess_piece_materials.at(piece.piece_type);
        sp->is_show  = true;
        sp->model_matrix =
            glm::translate(glm::mat4(1.f),
                           glm::vec3(location_transform(PIECE_COLOR::RED, PIECE_COLOR::RED, piece.x, piece.y), chess_piece_z));
    });

    manager.need_model_update();
}

void ui_record::prev_step() noexcept
{
    if (now_record_index > 0)
    {
        now_record_index--;
        restore_board_state(static_cast<uint32_t>(now_record_index));
    }
}

void ui_record::next_step() noexcept
{
    if (std::cmp_less(now_record_index + 1ull, board_state.size()))
    {
        now_record_index++;
        restore_board_state(static_cast<uint32_t>(now_record_index));
    }
}
