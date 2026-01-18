module;

#include <unicode/utypes.h>

module record_loader;

record_loader record_loader::loader;

std::vector<board_state> record_loader::load_records(const std::filesystem::path& _record_path)
{
	std::vector<board_state> all_board_state = { init_board_state };

	auto all_records = std::move(record_loader::get_record_loader().load_records<std::wstring>(_record_path));

	for (auto& w_field : all_records)
	{
		board_state now_board_state = all_board_state.back();
		bool use_red = is_chinese_number(w_field.at(3));
		std::wstring& now_names = now_board_state.at(static_cast<size_t>(use_red)).name;
		std::string& now_positions = now_board_state.at(static_cast<size_t>(use_red)).position;

		std::optional<uint8_t> now_piece = std::nullopt;

		if (is_number(w_field.at(1)))
		{
			std::vector<uint8_t> pieces_on_x = find_piece_on_x(use_red, to_number(w_field.at(1)), now_board_state);
			auto iter = std::ranges::find_if(pieces_on_x,
				[&](const auto& _x) {
					return to_character(now_names.at(_x)) == to_character(w_field.at(0));
				});

			if (iter != pieces_on_x.end())
			{
				now_piece = *iter;
			}
		}
		else
		{
			std::vector<uint8_t> pieces = find_piece_by_name(use_red, w_field.at(1), now_board_state);
			if (w_field.at(0) == L'前')
			{
				auto iter = std::ranges::max_element(pieces, {}, [&](const auto& _piece) { return now_positions.at(_piece) & 0x0F; });
				if (iter != pieces.end())
				{
					now_piece = *iter;
				}
			}
			else if (w_field.at(0) == L'后')
			{
				auto iter = std::ranges::min_element(pieces, {}, [&](const auto& _piece) { return now_positions.at(_piece) & 0x0F; });
				if (iter != pieces.end())
				{
					now_piece = *iter;
				}
			}
		}


		if (!now_piece.has_value())
		{
			continue;
		}


		uint8_t now_piece_position = now_positions.at(now_piece.value());
		uint8_t now_piece_position_x = (now_piece_position >> 4) & 0x0F;
		uint8_t now_piece_position_y = now_piece_position & 0x0F;

		now_positions.at(now_piece.value()) = move(now_names.at(now_piece.value()), now_piece_position, w_field.at(2), to_number(w_field.at(3)));

		now_piece_position = now_positions.at(now_piece.value());
		now_piece_position_x = (now_piece_position >> 4) & 0x0F;
		now_piece_position_y = now_piece_position & 0x0F;

		std::optional<uint8_t> piece = find_piece_on_x_y(!use_red, 10 - now_piece_position_x, 9 - now_piece_position_y, now_board_state); // 红方和黑方的Y是相反的
		if (piece.has_value())
		{
			now_board_state.at(static_cast<size_t>(!use_red)).name.erase(piece.value(), 1);
			now_board_state.at(static_cast<size_t>(!use_red)).position.erase(piece.value(), 1);
		}


		all_board_state.push_back(std::move(now_board_state));
	}

	return all_board_state;
}

uint8_t record_loader::move(wchar_t _character, uint8_t _now_position, wchar_t _move_direction, uint8_t _number)
{
	switch (_character)
	{
	case L'士':
		switch (_move_direction)
		{
		case L'进':
			_now_position = (_number << 4) | ((_now_position & 0x0F) + 1);
			break;
		case L'退':
			_now_position = (_number << 4) | ((_now_position & 0x0F) - 1);
			break;
		default:
			break;
		}
		break;
	case L'象':
		switch (_move_direction)
		{
		case L'进':
			_now_position = (_number << 4) | ((_now_position & 0x0F) + 2);
			break;
		case L'退':
			_now_position = (_number << 4) | ((_now_position & 0x0F) - 2);
			break;
		default:
			break;
		}
		break;
	case L'马':
		switch (_move_direction)
		{
		case L'进':
			_now_position = (_number << 4) | ((_now_position & 0x0F) + (3 - std::abs((_now_position >> 4) - _number)));
			break;
		case L'退':
			_now_position = (_number << 4) | ((_now_position & 0x0F) - (3 - std::abs((_now_position >> 4) - _number)));
			break;
		default:
			break;
		}
		break;
	case L'帅':
	case L'将':
	case L'车':
	case L'炮':
	case L'兵':
	case L'卒':
		switch (_move_direction)
		{
		case L'进':
			_now_position = (_now_position & 0xF0) | ((_now_position & 0x0F) + _number);
			break;
		case L'退':
			_now_position = (_now_position & 0xF0) | ((_now_position & 0x0F) - _number);
			break;
		case L'平':
			_now_position = (_number << 4) | (_now_position & 0x0F);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return _now_position;
}

std::vector<uint8_t> record_loader::find_piece_by_name(bool _use_color, wchar_t _name, const board_state& _board_state)
{
	return _board_state.at(static_cast<size_t>(_use_color)).name
		| std::views::enumerate
		| std::views::filter([&](const auto& pair)
			{
				return to_character(std::get<1>(pair)) == to_character(_name);
			})
		| std::views::transform([&](const auto& pair) { return static_cast<uint8_t>(std::get<0>(pair)); })
		| std::ranges::to<std::vector>();
}

std::vector<uint8_t> record_loader::find_piece_on_x(bool _use_color, uint8_t _x, const board_state& _board_state)
{
	return _board_state.at(static_cast<size_t>(_use_color)).position
		| std::views::enumerate
		| std::views::filter([&](const auto& pair)
			{
				uint8_t x = (std::get<1>(pair) >> 4) & 0x0F;
				return x == _x;
			})
		| std::views::transform([&](const auto& pair) { return static_cast<uint8_t>(std::get<0>(pair)); })
		| std::ranges::to<std::vector>();
}

std::vector<uint8_t> record_loader::find_piece_on_y(bool _use_color, uint8_t _y, const board_state& _board_state)
{
	return _board_state.at(static_cast<size_t>(_use_color)).position
		| std::views::enumerate
		| std::views::filter([&](const auto& pair)
			{
				uint8_t y = std::get<1>(pair) & 0x0F;
				return y == _y;
			})
		| std::views::transform([&](const auto& pair) { return static_cast<uint8_t>(std::get<0>(pair)); })
		| std::ranges::to<std::vector>();
}

std::optional<uint8_t> record_loader::find_piece_on_x_y(bool _use_color, uint8_t _x, uint8_t _y, const board_state& _board_state)
{
	auto index = _board_state.at(static_cast<size_t>(_use_color)).position
		| std::views::enumerate
		| std::views::filter([&](const auto& pair)
			{
				uint8_t y = std::get<1>(pair) & 0x0F;
				uint8_t x = (std::get<1>(pair) >> 4) & 0x0F;
				return x == _x && y == _y;
			})
		| std::views::transform([&](const auto& pair) { return static_cast<uint8_t>(std::get<0>(pair)); });

	return index.empty() ? std::optional<uint8_t>(std::nullopt) : std::optional<uint8_t>(index.front());
}

bool record_loader::is_digit_number(wchar_t _character)
{
	switch (_character)
	{
	case L'1':
	case L'１':
	case L'2':
	case L'２':
	case L'3':
	case L'３':
	case L'4':
	case L'４':
	case L'5':
	case L'５':
	case L'6':
	case L'６':
	case L'7':
	case L'７':
	case L'8':
	case L'８':
	case L'9':
	case L'９':
		return true;
	default:
		return false;
	}
}

bool record_loader::is_chinese_number(wchar_t _character)
{
	switch (_character)
	{
	case L'一':
	case L'二':
	case L'三':
	case L'四':
	case L'五':
	case L'六':
	case L'七':
	case L'八':
	case L'九':
		return true;
	default:
		return false;
	}
}

bool record_loader::is_number(wchar_t _character)
{
	return is_digit_number(_character) || is_chinese_number(_character);
}

uint8_t record_loader::to_number(wchar_t _character)
{
	switch (_character)
	{
	case L'1':
	case L'１':
	case L'一':
		return 1;
	case L'2':
	case L'２':
	case L'二':
		return 2;
	case L'3':
	case L'３':
	case L'三':
		return 3;
	case L'4':
	case L'４':
	case L'四':
		return 4;
	case L'5':
	case L'５':
	case L'五':
		return 5;
	case L'6':
	case L'６':
	case L'六':
		return 6;
	case L'7':
	case L'７':
	case L'七':
		return 7;
	case L'8':
	case L'８':
	case L'八':
		return 8;
	case L'9':
	case L'９':
	case L'九':
		return 9;
	default:
		return 0;
	}
}

wchar_t record_loader::to_character(wchar_t _character)
{
	switch (_character)
	{
	case L'帅':
	case L'帥':
		return L'帅';
	case L'将':
	case L'將':
		return L'将';
	case L'士':
	case L'仕':
		return L'士';
	case L'相':
	case L'象':
		return L'象';
	case L'马':
	case L'馬':
	case L'傌':
		return L'马';
	case L'车':
	case L'車':
	case L'俥':
		return L'车';
	case L'炮':
	case L'砲':
		return L'炮';
	case L'兵':
		return L'兵';
	case L'卒':
		return L'卒';
	default:
		return L'\0';
	}
}

const std::vector<std::array<wchar_t, 4>> record_loader::load_record(const std::filesystem::path& _record_path)
{
	std::ifstream f(_record_path.generic_string());
	if (!f.is_open())
	{
		throw std::runtime_error("Can not open file!");
	}

	std::vector<std::array<wchar_t, 4>> all_records;

	std::string line;
	while (std::getline(f, line))
	{
		std::wstring w_field = std::filesystem::path(line).generic_wstring();
		std::wstringstream w_ss(w_field);

		while (std::getline(w_ss, w_field, L'，'))
		{
			auto start = std::find_if(w_field.begin(), w_field.end(), [](const auto& _c) {return !std::iswspace(_c); });
			auto end = std::find_if(w_field.rbegin(), w_field.rend(), [](const auto& _c) {return !std::iswspace(_c); }).base();
			w_field.erase(w_field.begin(), start);
			w_field.erase(end, w_field.end());

			std::array<wchar_t, 4> w_record;
			std::copy(w_field.begin(), w_field.end(), w_record.begin());
			all_records.push_back(std::move(w_record));
		}
	}

	f.close();

	return all_records;
}

record_loader& record_loader::get_record_loader() noexcept
{
	return loader;
}
