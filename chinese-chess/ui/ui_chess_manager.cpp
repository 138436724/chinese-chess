#include "tools/font_loader.h"
#include "tools/shader_compiler.h"
#include "tools/string_helper.h"
#include "ui_chess_manager.h"
#include "vulkan_core/vulkan_common.h"
#include <algorithm>
#include <ranges>
#include <unordered_set>

void ui_chess_manager::create(scene_manager* _manager)
{
	manager = _manager;


	// create board
	auto chess_board_material = manager->create_material(L"楚河汉界");
	chess_board_material.lock()->background_color = glm::vec3(0.87843, 0.69020, 0.48627);
	chess_board_material.lock()->foreground_color = glm::vec3(0., 0., 0.);

	chess_board = manager->create_node(u8"chess_board.glb");
	chess_board.lock()->custom_index = 0;
	chess_board.lock()->material = std::shared_ptr(chess_board_material);


	// create board line
	chess_board_line = manager->create_node(u8"chess_board_line.glb");
	chess_board_line.lock()->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 0.15f));
	chess_board_line.lock()->custom_index = 1;


	// create all pieces and all materials
	std::ranges::for_each(all_chess_pieces, [this](auto& p)
		{
			p = manager->create_node(u8"chess_piece.glb");
			p.lock()->custom_index = 2u;
		});


	std::ranges::for_each(std::views::zip(std::wstring_view(L"帥仕相傌俥炮兵"), std::u16string_view(u"帥仕相傌俥炮兵")), [this](const auto& _pair)
		{
			const auto& [chw, chu] = _pair;
			auto piece_material = manager->create_material(std::wstring(1, chw));
			piece_material.lock()->background_color = glm::vec3(1.0, 0.85, 0.75);
			piece_material.lock()->foreground_color = glm::vec3(0.6, 0.1, 0.1);
			red_chess_piece_materials.emplace(RECORD_LOADER.get_piece_type(chu), piece_material);
		});

	std::ranges::for_each(std::views::zip(std::wstring_view(L"將士象馬車砲卒"), std::u16string_view(u"將士象馬車砲卒")), [this](const auto& _pair)
		{
			const auto& [chw, chu] = _pair;
			auto piece_material = manager->create_material(std::wstring(1, chw));
			piece_material.lock()->background_color = glm::vec3(0.85, 0.75, 0.65);
			piece_material.lock()->foreground_color = glm::vec3(0.1, 0.1, 0.1);
			black_chess_piece_materials.emplace(RECORD_LOADER.get_piece_type(chu), piece_material);
		});


	// init
	set_now_record_index(0);
}

bool ui_chess_manager::load_record(const std::filesystem::path& _record_path)
{
	board_state = RECORD_LOADER.load_record(_record_path);
	return board_state.size() > 1;
}

void ui_chess_manager::set_now_record_index(uint32_t _index) noexcept
{
	if (0 <= _index && _index < board_state.size())
	{
		now_record_index = _index;
		restore_board_state(board_state.at(now_record_index));
	}
}

//all_board_state ui_chess_manager::capture_board_state()
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

void ui_chess_manager::restore_board_state(const all_board_state& _state)
{
	std::ranges::for_each(all_chess_pieces, [](const auto& p)
		{
			p.lock()->is_show = false;
		});

	size_t index_offset = 0;
	std::ranges::for_each(_state.at(static_cast<size_t>(PIECE_COLOR::BLACK)) | std::views::enumerate, [&](const auto& _pair)
		{
			const auto& [index, state] = _pair;
			const auto& sp = all_chess_pieces.at(index + index_offset).lock();
			sp->material = std::shared_ptr<scene_material>(black_chess_piece_materials.at(state.piece_type));
			sp->is_show = true;
			sp->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(location_transform(PIECE_COLOR::RED, PIECE_COLOR::BLACK, state.x, state.y), 0.3f));
		});

	index_offset = _state.at(static_cast<size_t>(PIECE_COLOR::BLACK)).size();
	std::ranges::for_each(_state.at(static_cast<size_t>(PIECE_COLOR::RED)) | std::views::enumerate, [&](const auto& _pair)
		{
			const auto& [index, state] = _pair;
			const auto& sp = all_chess_pieces.at(index + index_offset).lock();
			sp->material = std::shared_ptr<scene_material>(red_chess_piece_materials.at(state.piece_type));
			sp->is_show = true;
			sp->model_matrix = glm::translate(glm::mat4(1.f), glm::vec3(location_transform(PIECE_COLOR::RED, PIECE_COLOR::RED, state.x, state.y), 0.3f));
		});

	manager->need_update();
}

glm::vec2 ui_chess_manager::location_transform(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, uint8_t _x, uint8_t _y) noexcept
{
	// 棋盘中心为坐标的(0, 0)点，而右下和左上作为双方棋子的定位原点
	glm::vec2 location;
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
	location = (location - glm::vec2(4, 4.5)) * board_unit_distance;
	return location;
}