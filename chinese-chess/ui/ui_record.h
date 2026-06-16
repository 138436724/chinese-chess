#pragma once

#include "scene/scene_manager.h"
#include "tools/record_loader.h"
#include <imgui.h>

class ui_record
{
public:
	ui_record() = default;
	~ui_record() = default;

	void create(scene_manager* _manager);
	void update() noexcept;

	void load_previous() noexcept;
	void load_next() noexcept;

private:
	void load_records(const std::filesystem::path& _record_path);

	//all_board_state capture_board_state();
	void restore_board_state(uint32_t _index) noexcept;

	static glm::vec2 location_transform(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, uint8_t _x, uint8_t _y) noexcept;

private:
	int now_record_index = 0;
	std::vector<std::u8string> all_records;
	std::vector<const char*> all_records_c_str;

	scene_manager* manager = nullptr;

	std::weak_ptr<scene_model> chess_board;
	std::weak_ptr<scene_model> chess_board_line;

	std::vector<all_board_state> board_state = { RECORD_LOADER.get_init_all_borad() };
	std::array<std::shared_ptr<scene_model>, 32> all_chess_pieces;
	std::unordered_map<PIECE_TYPE, std::weak_ptr<scene_material>> red_chess_piece_materials;
	std::unordered_map<PIECE_TYPE, std::weak_ptr<scene_material>> black_chess_piece_materials;
};
