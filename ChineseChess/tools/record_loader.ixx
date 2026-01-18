module;

#include <unicode/ucsdet.h>
#include <unicode/ucnv.h>
#include <unicode/utypes.h>
#include <unicode/unistr.h>
#include <unicode/ustring.h>
#include <unicode/uchar.h>
#include <unicode/regex.h>
#include <unicode/errorcode.h>

export module record_loader;

import <cstdint>;
import std;
import string_helper;

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
	template<typename my_string>
		requires std::same_as<my_string, std::string> || std::same_as<my_string, std::u8string> || std::same_as<my_string, std::wstring>
	std::vector<my_string> load_records(const std::filesystem::path& _record_path)
	{
		std::vector<my_string> all_records;

		std::string encoding = string_helper::get_file_encoding(_record_path);

		std::ifstream in_file(_record_path.generic_string(), std::ios::binary);
		std::string content((std::istreambuf_iterator<char>(in_file)), std::istreambuf_iterator<char>());
		in_file.close();

		icu::UnicodeString records_text(content.c_str(), static_cast<int32_t>(content.size()), encoding.c_str());

		icu::UnicodeString pattern = u"([前后中])?[ ]*([车車俥马馬傌炮砲相象士仕帅帥将將兵卒])[ ]*([一二三四五六七八九]|\\d)?[ ]*([进退平])[ ]*([一二三四五六七八九]|\\d)";
		UParseError pe;
		icu::ErrorCode status;
		icu::RegexPattern* compiled_pattern = icu::RegexPattern::compile(pattern, pe, status);
		if (U_FAILURE(status))
		{
			throw std::runtime_error("compile regex pattern fail!");
		}

		icu::RegexMatcher* matcher = compiled_pattern->matcher(records_text, status);
		while (matcher->find(status))
		{
			icu::UnicodeString match = matcher->group(0, status);
			if constexpr (std::is_same_v<my_string, std::wstring>)
			{
				std::wstring ws;
				ws.resize(match.length() * 2);

				int32_t ws_len;
				UErrorCode error = U_ZERO_ERROR;

				u_strToWCS(ws.data(), static_cast<int32_t>(ws.size()), &ws_len, match.getBuffer(), match.length(), &error);
				if (U_FAILURE(error))
				{
					throw std::runtime_error("Can not convert to wstring.");
				}
				ws.resize(ws_len);

				all_records.push_back(std::move(ws));
			}
			else
			{
				all_records.push_back(std::move(match.toUTF8String<my_string>()));
			}
		}

		return all_records;
	}

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