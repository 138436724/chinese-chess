#ifdef __INTELLISENSE__
#include <ranges>
#include <algorithm>
#include <chrono>
#include  <array>
#endif // __INTELLISENSE__


module piece_horse;

void piece_horse::create(const vulkan_application* _app, bool _use_color, bool _piece_color)
{
	piece_base::create(_app, _use_color, _piece_color);

	if (piece_color)
	{
		piece_name = L'傌';
	}
	else
	{
		piece_name = L'馬';
	}
}

glm::u8vec2 piece_horse::move_by(wchar_t _direction, uint8_t _move_number)
{
	uint8_t move_x = std::abs(piece_location.x - _move_number);

	piece_location.x = _move_number;

	switch (_direction)
	{
	case L'进':
		piece_location.y += (3 - move_x);
		break;
	case L'退':
		piece_location.y -= (3 - move_x);
		break;
	default:
		break;
	}

	return piece_location;
}

std::pair<wchar_t, uint8_t> piece_horse::move_to(const glm::u8vec2& _new_location)
{
	std::pair<wchar_t, uint8_t> record;

	if (piece_location.y < _new_location.y)
	{
		record.first = L'进';
		record.second = _new_location.x;
	}
	else if (piece_location.y > _new_location.y)
	{
		record.first = L'退';
		record.second = _new_location.x;
	}

	piece_location = _new_location;

	return record;
}