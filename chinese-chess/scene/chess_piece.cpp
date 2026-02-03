#include "chess_piece.h"
#include "tools/font_loader.h"
#include "tools/shader_compiler.h"
#include "vulkan_core/vulkan_common.h"

void chess_piece::create(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, PIECE_TYPE _piece_type)
{
	use_color = _use_color;
	piece_color = _piece_color;
	piece_type = _piece_type;
}

void chess_piece::set_is_on_board(bool _is_on_board) noexcept
{
	is_on_board = _is_on_board;
}

void chess_piece::set_piece_location(const std::pair<uint8_t, uint8_t>& _location) noexcept
{
	piece_x = _location.first;
	piece_y = _location.second;
	model_location = location_transform(use_color, piece_color, piece_x, piece_y);
}

bool chess_piece::get_is_on_board() const noexcept
{
	return is_on_board;
}

PIECE_COLOR chess_piece::get_piece_color() const noexcept
{
	return piece_color;
}

PIECE_TYPE chess_piece::get_piece_type() const noexcept
{
	return piece_type;
}

std::pair<uint8_t, uint8_t> chess_piece::get_piece_location() const noexcept
{
	return std::make_pair(piece_x, piece_y);
}

glm::vec2 chess_piece::get_model_location() const noexcept
{
	return model_location;
}

std::wstring chess_piece::get_piece_name(PIECE_COLOR _piece_color, PIECE_TYPE _piece_type) noexcept
{
	switch (_piece_color)
	{
	case PIECE_COLOR::RED:
		switch (_piece_type)
		{
		case PIECE_TYPE::GENERAL:
			return L"帥";
		case PIECE_TYPE::GUARD:
			return L"仕";
		case PIECE_TYPE::ELEPHANT:
			return L"相";
		case PIECE_TYPE::HORSE:
			return L"傌";
		case PIECE_TYPE::CHARIOT:
			return L"俥";
		case PIECE_TYPE::CANNON:
			return L"炮";
		case PIECE_TYPE::PAWN:
			return L"兵";
		default:
			break;
		}
		break;
	case PIECE_COLOR::BLACK:
		switch (_piece_type)
		{
		case PIECE_TYPE::GENERAL:
			return L"將";
		case PIECE_TYPE::GUARD:
			return L"士";
		case PIECE_TYPE::ELEPHANT:
			return L"象";
		case PIECE_TYPE::HORSE:
			return L"馬";
		case PIECE_TYPE::CHARIOT:
			return L"車";
		case PIECE_TYPE::CANNON:
			return L"砲";
		case PIECE_TYPE::PAWN:
			return L"卒";
		default:
			break;
		}
		break;
	default:
		break;
	}
	return L"";
}

glm::vec2 chess_piece::location_transform(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, uint8_t _x, uint8_t _y) noexcept
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

	location = (location - glm::vec2(4, 4.5)) * board_unit_distance;
	return location;
}