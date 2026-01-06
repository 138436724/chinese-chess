export module record_loader;

import <cstdint>;
import std;

export constexpr std::string_view RECORDS_PATH = "resources\\records\\";


export struct half_board_state
{
	std::wstring name;
	std::string position;
};

export using board_state = std::array<half_board_state, 2>;

export class record_loader
{
public:
	std::vector<board_state> load_records(const std::filesystem::path& _record_path);

	static uint8_t move(wchar_t _character, uint8_t _now_position, wchar_t _move_direction, uint8_t _number);

	static std::vector<uint8_t> find_piece_by_name(bool _use_color, wchar_t _name, const board_state& _board_state);
	static std::vector<uint8_t> find_piece_on_x(bool _use_color, uint8_t _x, const board_state& _board_state);
	static std::vector<uint8_t> find_piece_on_y(bool _use_color, uint8_t _y, const board_state& _board_state);
	static std::optional<uint8_t> find_piece_on_x_y(bool _use_color, uint8_t _x, uint8_t _y, const board_state& _board_state);

	static bool is_digit_number(wchar_t _character);
	static bool is_chinese_number(wchar_t _character);
	static bool is_number(wchar_t _character);

	static uint8_t to_number(wchar_t _character);
	static wchar_t to_character(wchar_t _character);

	const std::vector<std::array<wchar_t, 4>> load_record(const std::filesystem::path& _record_path);

	static record_loader& get_record_loader() noexcept;

	inline const static board_state init_board_state = {
		L"将士士象象马马车车炮炮卒卒卒卒卒","\x50\x40\x60\x30\x70\x20\x80\x10\x90\x22\x82\x13\x33\x53\x73\x93",
		L"帅士士象象马马车车炮炮兵兵兵兵兵","\x50\x40\x60\x30\x70\x20\x80\x10\x90\x22\x82\x13\x33\x53\x73\x93",
	};

private:
	record_loader() = default;
	~record_loader() = default;

	record_loader(const record_loader&) = delete;
	record_loader& operator=(const record_loader&) = delete;
	record_loader(const record_loader&&) = delete;
	record_loader& operator=(const record_loader&&) = delete;

	static record_loader loader;
};