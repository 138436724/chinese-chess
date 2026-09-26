#include "ui_record.h"

#include "scene/scene_manager.h"
#include "scene/scene_material.h"
#include "scene/scene_model.h"
// 仅为白炉预设(furnace_preset)与 debug_flags 位定义
#include "scene/scene_raytracing_render.h"
#include "tools/font_loader.h"
#include "tools/model_loader.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <exception>
#include <glm/glm.hpp>
#include <imgui.h>
#include <print>
#include <ranges>
#include <string_view>
#include <utility>
#ifdef _WIN32
#include <commdlg.h>
#endif  // _WIN32


namespace {

constexpr std::string_view RECORDS_MANAGER = "棋局管理";
constexpr std::string_view RECORDS_LIST    = "棋谱列表";
constexpr std::string_view LOAD_RECORDS    = "加载棋谱";
constexpr std::string_view LAST_STEP       = "上一步";
constexpr std::string_view NEXT_STEP       = "下一步";

// 白炉测试球体阵
constexpr std::string_view FURNACE_GRID        = "白炉测试球体阵";
constexpr std::string_view FURNACE_GRID_TOGGLE = "显示球体阵(roughness × metallic)";
constexpr std::string_view FURNACE_GRID_HINT =
    "热键 G 切球体阵、H 切白炉预设;两者配合「PBR 物理调试」可逐格核查方向反射率。";

constexpr uint32_t board_font_padding = 2;

constexpr float chess_board_z      = -1.5f;
constexpr float chess_board_line_z = -1.45f;
constexpr float chess_piece_z      = -1.4f;

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
    auto chess_board_material              = manager.create<scene_material>();
    chess_board_material->background_color = glm::vec3(0.87843, 0.69020, 0.48627);
    chess_board_material->foreground_color = glm::vec3(0., 0., 0.);

    chess_board               = manager.create<scene_model>(std::string(MODELS_PATH) + "chess_board.glb");
    chess_board->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, chess_board_z));
    chess_board->custom_index = 0u;
    chess_board->material     = std::move(chess_board_material);


    // create board line
    chess_board_line               = manager.create<scene_model>(std::string(MODELS_PATH) + "chess_board_line.glb");
    chess_board_line->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, chess_board_line_z));
    chess_board_line->custom_index = 1u;


    // create all pieces and all materials
    std::ranges::for_each(all_chess_pieces, [this](auto& p) {
        p               = manager.create<scene_model>(std::string(MODELS_PATH) + "chess_piece.glb");
        p->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, chess_piece_z));
        p->custom_index = 2u;
    });
}

void ui_record::resize(uint32_t _width, uint32_t _height)
{
    width  = _width;
    height = _height;

    chess_board->material->alpha_map =
        manager.create<scene_image>(std::string(FONTS_PATH) + "LXGWWenKaiGB-Medium.ttf",
                                    static_cast<uint32_t>(height / 9.0 * 2), L"楚河汉界", board_font_padding);

    std::ranges::for_each(std::views::zip(std::wstring_view(L"帥仕相傌俥炮兵"), std::u16string_view(u"帥仕相傌俥炮兵")),
                          [this](const auto& _pair) {
                              const auto& [chw, chu]           = _pair;
                              auto piece_material              = manager.create<scene_material>();
                              piece_material->background_color = glm::vec3(0.95f, 0.92f, 0.85f);
                              piece_material->foreground_color = glm::vec3(0.45f, 0.08f, 0.06f);
                              piece_material->roughness        = 0.3f;
                              // 透射由 transmission 表达;opacity 只用于介质内的概率穿透,
                              // 不再拿它当"透射开关"(否则会产生非物理的软阴影)
                              piece_material->opacity     = 1.0f;
                              piece_material->ior         = 1.5f;
                              piece_material->transmission = 1.0f;
                              // 白玉:各波长近似等吸收,微微偏暖;刻字为深红且吸收更强
                              piece_material->absorption_coefficient = glm::vec3(20.0f, 22.0f, 26.0f);
                              piece_material->engrave_absorption     = glm::vec3(20.0f, 55.0f, 45.0f);

                              auto piece_image = manager.create<scene_image>(std::string(FONTS_PATH) + "LXGWWenKaiGB-Medium.ttf",
                                                                             static_cast<uint32_t>(height / 9.0 * 2),
                                                                             std::wstring(1, chw), board_font_padding);
                              piece_material->alpha_map = piece_image;

                              red_chess_piece_materials.emplace(record_loader::get_piece_type(chu), std::move(piece_material));
                          });

    std::ranges::for_each(std::views::zip(std::wstring_view(L"將士象馬車砲卒"), std::u16string_view(u"將士象馬車砲卒")),
                          [this](const auto& _pair) {
                              const auto& [chw, chu]           = _pair;
                              auto piece_material              = manager.create<scene_material>();
                              piece_material->background_color = glm::vec3(0.15f, 0.45f, 0.32f);
                              piece_material->foreground_color = glm::vec3(0.02f, 0.10f, 0.06f);
                              piece_material->roughness        = 0.3f;
                              piece_material->opacity          = 1.0f;
                              piece_material->ior              = 1.5f;
                              piece_material->transmission     = 1.0f;
                              // 青玉:红光被强烈吸收、绿光相对透过 → 绿色由物理涌现,
                              // 而不是靠 background_color 直接"涂"成绿色
                              piece_material->absorption_coefficient = glm::vec3(30.0f, 12.0f, 22.0f);
                              piece_material->engrave_absorption     = glm::vec3(40.0f, 60.0f, 50.0f);

                              auto piece_image = manager.create<scene_image>(std::string(FONTS_PATH) + "LXGWWenKaiGB-Medium.ttf",
                                                                             static_cast<uint32_t>(height / 9.0 * 2),
                                                                             std::wstring(1, chw), board_font_padding);
                              piece_material->alpha_map = piece_image;

                              black_chess_piece_materials.emplace(record_loader::get_piece_type(chu), std::move(piece_material));
                          });

    // init
    restore_board_state(0);
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

    // ---- 白炉测试球体阵 ----
    // 与"PBR 物理调试"面板的"一键白炉"配合使用:后者只把 albedo 强制为白,
    // 保留每格自己的 roughness/metallic,于是每格的方向反射率可逐格核查。
    ImGui::SeparatorText(FURNACE_GRID.data());
    bool grid_enabled = furnace_grid_enabled;
    if (ImGui::Checkbox(FURNACE_GRID_TOGGLE.data(), &grid_enabled))
    {
        set_furnace_grid_enabled(grid_enabled);
    }
    ImGui::TextUnformatted(FURNACE_GRID_HINT.data());
}

// ============================================================================
// 白炉测试球体阵
//
// 布局:x 轴 = roughness(0.05 → 1.0),y 轴 = metallic(0 → 1)。
// 每个球一个材质(粗糙度/金属度不同),但**共用同一份球体网格**,
// 所以整个 6×5 网格只有一份顶点/索引数据。
//
// 物理用途:配合"只强制 albedo 为白"的调试开关,每格的方向反射率
// 应只由 (roughness, metallic) 决定。若某格出现非物理的亮斑/暗块,
// 或整列随粗糙度出现系统性偏移,就是能量补偿或 FRESNEL 项出了问题。
// ============================================================================
void ui_record::rebuild_furnace_grid()
{
    constexpr uint32_t cols = 6;  // roughness
    constexpr uint32_t rows = 5;  // metallic
    constexpr float    radius  = 0.22f;
    // 间距从 0.62 放宽到 0.85:球之间的**相互遮挡**会压低入射辐照度,
    // 让白炉读数偏低(实测间距 0.62 时相邻球可挡掉约 20%~40% 半球)。
    // 0.85 在 z=-2.0 处仍完整落在视锥内(6 列半宽 2.125+0.22 < 2.667)。
    // 注意:改这里必须同步 tools/furnace_probe.ps1 与 tools/dark_probe.ps1 里的 spacing。
    constexpr float    spacing = 0.85f;
    // 相机在原点朝 -Z,FOV_y = 90°(可视半高 = 距离)。
    // 网格铺在**屏幕平面 XY** 上、放在 z = -2.0:
    //   · z 必须为负 —— 正值在相机背后,根本看不见(踩过这个错)
    //   · 行不能沿 z 铺开 —— 那样会向远处堆叠、近排遮远排
    // z=-2.0 时可视半高 2.0 / 半宽 3.56,而网格半高 1.24 / 半宽 1.55,留有余量。
    constexpr float    grid_z = -2.0f;

    furnace_spheres.clear();
    furnace_spheres.reserve(static_cast<size_t>(cols) * rows);

    for (uint32_t row = 0; row < rows; ++row)
    {
        const float metallic = static_cast<float>(row) / static_cast<float>(rows - 1);

        for (uint32_t col = 0; col < cols; ++col)
        {
            // 粗糙度从 0.05(近镜面)到 1.0(完全粗糙);下限避开 delta 化
            const float roughness = 0.05f + (1.0f - 0.05f) * static_cast<float>(col) / static_cast<float>(cols - 1);

            auto sphere = manager.create_procedural_sphere(radius, 48u, 24u);
            sphere->model_matrix =
                glm::translate(glm::mat4(1.0f),
                               glm::vec3((static_cast<float>(col) - static_cast<float>(cols - 1) * 0.5f) * spacing,
                                         (static_cast<float>(row) - static_cast<float>(rows - 1) * 0.5f) * spacing,
                                         grid_z));

            auto material              = manager.create<scene_material>();
            material->background_color = glm::vec3(1.0f);  // 白炉:反照率本身就设白
            material->roughness        = roughness;
            material->metallic         = metallic;
            sphere->material           = std::move(material);

            sphere->is_show = furnace_grid_enabled;
            furnace_spheres.push_back(std::move(sphere));
        }
    }

    manager.need_model_update();
    manager.need_material_update();
}

void ui_record::apply_furnace_isolation() noexcept
{
    // 白炉模式 = 隔离测试场景:隐掉棋盘与棋子,避免遮挡球体阵。
    // 关闭白炉时不在这里恢复 —— 由 restore_board_state 按当前棋局状态重建。
    if (!furnace_grid_enabled)
    {
        return;
    }

    chess_board->is_show      = false;
    chess_board_line->is_show = false;
    for (auto& piece : all_chess_pieces)
    {
        piece->is_show = false;
    }
}

void ui_record::set_furnace_grid_visible(bool _visible) noexcept
{
    for (auto& sphere : furnace_spheres)
    {
        sphere->is_show = _visible;
    }

    if (_visible)
    {
        apply_furnace_isolation();
        manager.need_model_update();
    }
    else
    {
        // 关闭白炉:按当前棋局状态把棋盘与棋子重新点亮
        restore_board_state(static_cast<uint32_t>(now_record_index));
    }
}

void ui_record::set_furnace_grid_enabled(bool _enabled)
{
    furnace_grid_enabled = _enabled;

    if (furnace_grid_enabled && !furnace_grid_built)
    {
        rebuild_furnace_grid();
        furnace_grid_built = true;
    }

    set_furnace_grid_visible(furnace_grid_enabled);
}

void ui_record::toggle_furnace_preset() noexcept
{
    constexpr uint32_t preset = furnace_preset();
    const uint32_t     flags  = manager.get_debug_flags();

    // 已完整处于白炉预设 → 关闭;否则打开(并补齐缺失位)
    const bool already_on = (flags & preset) == preset;
    manager.set_debug_flags(already_on ? (flags & ~preset) : (flags | preset));
}

void ui_record::handle(int _key, int /*_scancode*/, int _action, int /*_mods*/) noexcept
{
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
        // ---- 调试热键(便于脚本化验证:可用 PostMessage 投递按键,不抢焦点)----
        // 只在按下时响应:GLFW 还会发 GLFW_REPEAT,不过滤的话按住键会反复切换。
        // W/A/S/D 保持原有行为不变 —— 那里的自动重复是"连续翻步"的有意特性。
        case GLFW_KEY_G:
            if (_action == GLFW_PRESS)
            {
                // 建网格会分配内存/创建对象,可能抛异常;而 handle 是 noexcept
                // (ui_base facade 的约定),noexcept 里抛出会直接 std::terminate,
                // 所以必须在这里兜住。
                try
                {
                    set_furnace_grid_enabled(!furnace_grid_enabled);
                }
                catch (const std::exception& _error)
                {
                    std::println(std::cerr, "furnace grid toggle failed: {}", _error.what());
                }
            }
            break;
        case GLFW_KEY_H:
            if (_action == GLFW_PRESS)
            {
                toggle_furnace_preset();
            }
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

    std::ranges::for_each(all_chess_pieces, [](const auto& p) static { p->is_show = false; });

    const all_board_state& state        = board_state.at(_index);
    size_t                 index_offset = 0;
    std::ranges::for_each(state.at(static_cast<size_t>(PIECE_COLOR::BLACK)) | std::views::enumerate, [&](const auto& _pair) {
        const auto& [index, piece] = _pair;
        const auto& sp             = all_chess_pieces.at(index + index_offset);
        sp->material               = std::shared_ptr<scene_material>(black_chess_piece_materials.at(piece.piece_type));
        sp->is_show                = true;
        sp->model_matrix =
            glm::translate(glm::mat4(1.f),
                           glm::vec3(location_transform(PIECE_COLOR::RED, PIECE_COLOR::BLACK, piece.x, piece.y), chess_piece_z));
    });

    index_offset = state.at(static_cast<size_t>(PIECE_COLOR::BLACK)).size();
    std::ranges::for_each(state.at(static_cast<size_t>(PIECE_COLOR::RED)) | std::views::enumerate, [&](const auto& _pair) {
        const auto& [index, piece] = _pair;
        const auto& sp             = all_chess_pieces.at(index + index_offset);
        sp->material               = std::shared_ptr<scene_material>(red_chess_piece_materials.at(piece.piece_type));
        sp->is_show                = true;
        sp->model_matrix =
            glm::translate(glm::mat4(1.f),
                           glm::vec3(location_transform(PIECE_COLOR::RED, PIECE_COLOR::RED, piece.x, piece.y), chess_piece_z));
    });

    // 棋局恢复会把棋子重新点亮 —— 若当前处于白炉模式,要再隐回去,
    // 否则翻棋谱时棋子会突然出现在球体阵前面。
    apply_furnace_isolation();

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
    if (std::cmp_less(now_record_index + 1ull, board_state.size()))
    {
        now_record_index++;
        restore_board_state(static_cast<uint32_t>(now_record_index));
    }
}
