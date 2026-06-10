#pragma once

#include "scene/scene_manager.h"
#include "tools/record_loader.h"

class ui_chess_manager
{
public:
	ui_chess_manager() = default;
	~ui_chess_manager() = default;

	void create(scene_manager* _manager);

	bool load_record(const std::filesystem::path& _record_path);
	void set_now_record_index(uint32_t _index) noexcept;

	//all_board_state capture_board_state();
	void restore_board_state(const all_board_state& _state);

private:
	static glm::vec2 location_transform(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, uint8_t _x, uint8_t _y) noexcept;

private:
	scene_manager* manager;

	std::weak_ptr<scene_model> chess_board;
	std::weak_ptr<scene_model> chess_board_line;

	uint32_t now_record_index = 0;
	std::vector<all_board_state> board_state = { RECORD_LOADER.get_init_all_borad() };
	std::array<std::weak_ptr<scene_model>, 32> all_chess_pieces;
	std::unordered_map<PIECE_TYPE, std::weak_ptr<scene_material>> red_chess_piece_materials;
	std::unordered_map<PIECE_TYPE, std::weak_ptr<scene_material>> black_chess_piece_materials;
};
