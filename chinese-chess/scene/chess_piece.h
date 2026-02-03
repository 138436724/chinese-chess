#pragma once

#include "scene/scene_camera.h"
#include "tools/record_loader.h"

class chess_piece
{
public:
	chess_piece() = default;
	~chess_piece() = default;

	void create(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, PIECE_TYPE _piece_type);

	void set_is_on_board(bool _is_on_board) noexcept;
	void set_piece_location(const std::pair<uint8_t, uint8_t>& _location) noexcept;

	bool get_is_on_board() const noexcept;
	PIECE_COLOR get_piece_color() const noexcept;
	PIECE_TYPE get_piece_type() const noexcept;
	std::pair<uint8_t, uint8_t> get_piece_location() const noexcept;
	glm::vec2 get_model_location() const noexcept;

private:
	static std::wstring get_piece_name(PIECE_COLOR _piece_color, PIECE_TYPE _piece_type) noexcept;

	bool is_on_board = true;
	PIECE_COLOR use_color = PIECE_COLOR::RED; // play chess in `color` side
	PIECE_COLOR piece_color = PIECE_COLOR::RED; // piece color
	PIECE_TYPE piece_type = PIECE_TYPE::NONE;
	uint8_t piece_x = 0;
	uint8_t piece_y = 0;

	glm::vec2 model_location = { 0,0 };

	inline static const float board_unit_distance = 0.25f;
	static glm::vec2 location_transform(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, uint8_t _x, uint8_t _y) noexcept;
};