#include "ui_record.h"

#include "tools/font_loader.h"
#include "tools/model_loader.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <ranges>
#ifdef _WIN32
#include <commdlg.h>
#endif  // _WIN32


constexpr std::u8string_view RECORDS_MANAGER = u8"棋局管理";
constexpr std::u8string_view RECORDS_LIST    = u8"棋谱列表";
constexpr std::u8string_view LOAD_RECORDS    = u8"加载棋谱";
constexpr std::u8string_view LAST_STEP       = u8"上一步";
constexpr std::u8string_view NEXT_STEP       = u8"下一步";


ui_record::ui_record(scene_manager& _manager)
    : manager(_manager)
{
    // create board
    auto chess_board_material              = manager.create<scene_material>();
    chess_board_material->background_color = glm::vec3(0.87843, 0.69020, 0.48627);
    chess_board_material->foreground_color = glm::vec3(0., 0., 0.);

    chess_board               = manager.create<scene_model>(std::u8string(MODELS_PATH) + u8"chess_board.glb");
    chess_board->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, -7.f));
    chess_board->custom_index = 0u;
    chess_board->material     = std::move(chess_board_material);


    // create board line
    chess_board_line               = manager.create<scene_model>(std::u8string(MODELS_PATH) + u8"chess_board_line.glb");
    chess_board_line->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, -5.f));
    chess_board_line->custom_index = 1u;


    // create all pieces and all materials
    std::ranges::for_each(all_chess_pieces, [this](auto& p) {
        p               = manager.create<scene_model>(std::u8string(MODELS_PATH) + u8"chess_piece.glb");
        p->custom_index = 2u;
    });
}

void ui_record::resize(uint32_t _width, uint32_t _height) noexcept
{
    width  = _width;
    height = _height;

    chess_board->material->alpha_map = manager.create<scene_image>(std::u8string(FONTS_PATH) + u8"LXGWWenKaiGB-Medium.ttf",
                                                                   static_cast<uint32_t>(height / 9.0 * 2), L"楚河汉界");

    std::ranges::for_each(
        std::views::zip(std::wstring_view(L"帥仕相傌俥炮兵"), std::u16string_view(u"帥仕相傌俥炮兵")), [this](const auto& _pair) {
            const auto& [chw, chu]           = _pair;
            auto piece_material              = manager.create<scene_material>();
            piece_material->background_color = glm::vec3(1.0, 0.85, 0.75);
            piece_material->foreground_color = glm::vec3(0.6, 0.1, 0.1);

            auto piece_image = manager.create<scene_image>(std::u8string(FONTS_PATH) + u8"LXGWWenKaiGB-Medium.ttf",
                                                           static_cast<uint32_t>(height / 9.0 * 2), std::wstring(1, chw));
            piece_material->alpha_map = piece_image;

            red_chess_piece_materials.emplace(record_loader::get_piece_type(chu), std::move(piece_material));
        });

    std::ranges::for_each(
        std::views::zip(std::wstring_view(L"將士象馬車砲卒"), std::u16string_view(u"將士象馬車砲卒")), [this](const auto& _pair) {
            const auto& [chw, chu]           = _pair;
            auto piece_material              = manager.create<scene_material>();
            piece_material->background_color = glm::vec3(0.85, 0.75, 0.65);
            piece_material->foreground_color = glm::vec3(0.1, 0.1, 0.1);

            auto piece_image = manager.create<scene_image>(std::u8string(FONTS_PATH) + u8"LXGWWenKaiGB-Medium.ttf",
                                                           static_cast<uint32_t>(height / 9.0 * 2), std::wstring(1, chw));
            piece_material->alpha_map = piece_image;

            black_chess_piece_materials.emplace(record_loader::get_piece_type(chu), std::move(piece_material));
        });

    // init
    restore_board_state(0);
}

void ui_record::update() noexcept
{
    ImGui::SeparatorText(reinterpret_cast<const char*>(RECORDS_MANAGER.data()));

    if (ImGui::Button(reinterpret_cast<const char*>(LOAD_RECORDS.data())))
    {
        std::filesystem::path file_path;

#ifdef _WIN32
        TCHAR szFile[MAX_PATH] = {0};

        OPENFILENAME ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize  = sizeof(ofn);
        ofn.lpstrFile    = szFile;
        ofn.nMaxFile     = sizeof(szFile);
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
        if (ImGui::ListBox(reinterpret_cast<const char*>(RECORDS_LIST.data()), &now_record_index,
                           all_records_c_str.data(), static_cast<int>(all_records_c_str.size())))
        {
            restore_board_state(static_cast<uint32_t>(now_record_index));
        }
        if (ImGui::Button(reinterpret_cast<const char*>(LAST_STEP.data())))
        {
            prev_step();
        }
        ImGui::SameLine();
        if (ImGui::Button(reinterpret_cast<const char*>(NEXT_STEP.data())))
        {
            next_step();
        }
    }
}

void ui_record::handle(int _glfw_key) noexcept
{
    switch (_glfw_key)
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
    all_records = record_loader::read_record<std::u8string>(_record_path);

    all_records_c_str =
        all_records
        | std::views::transform([](const auto& _record) { return reinterpret_cast<const char*>(_record.data()); })
        | std::ranges::to<std::vector>();

    now_record_index = 0;

    board_state = record_loader::load_records(_record_path);
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
    std::ranges::for_each(all_chess_pieces, [](const auto& p) { p->is_show = false; });

    const all_board_state& state        = board_state.at(_index);
    size_t                 index_offset = 0;
    std::ranges::for_each(state.at(static_cast<size_t>(PIECE_COLOR::BLACK)) | std::views::enumerate, [&](const auto& _pair) {
        const auto& [index, state] = _pair;
        const auto& sp             = all_chess_pieces.at(index + index_offset);
        sp->material               = std::shared_ptr<scene_material>(black_chess_piece_materials.at(state.piece_type));
        sp->is_show                = true;
        sp->model_matrix =
            glm::translate(glm::mat4(1.f),
                           glm::vec3(location_transform(PIECE_COLOR::RED, PIECE_COLOR::BLACK, state.x, state.y), -3.f));
    });

    index_offset = state.at(static_cast<size_t>(PIECE_COLOR::BLACK)).size();
    std::ranges::for_each(state.at(static_cast<size_t>(PIECE_COLOR::RED)) | std::views::enumerate, [&](const auto& _pair) {
        const auto& [index, state] = _pair;
        const auto& sp             = all_chess_pieces.at(index + index_offset);
        sp->material               = std::shared_ptr<scene_material>(red_chess_piece_materials.at(state.piece_type));
        sp->is_show                = true;
        sp->model_matrix =
            glm::translate(glm::mat4(1.f),
                           glm::vec3(location_transform(PIECE_COLOR::RED, PIECE_COLOR::RED, state.x, state.y), -3.f));
    });

    manager.need_update();
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
    if (now_record_index < board_state.size() - 1)
    {
        now_record_index++;
        restore_board_state(static_cast<uint32_t>(now_record_index));
    }
}

glm::vec2 ui_record::location_transform(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, uint8_t _x, uint8_t _y) noexcept
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
