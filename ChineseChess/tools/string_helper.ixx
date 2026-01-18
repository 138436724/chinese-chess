module;

#include <unicode/ucsdet.h>
#include <unicode/ucnv.h>
#include <unicode/utypes.h>
#include <unicode/unistr.h>
#include <unicode/ustring.h>
#include <unicode/uchar.h>

export module string_helper;

import std;

export namespace string_helper
{
	void trim(std::string& s);
	void trim(std::wstring& ws);

	std::string get_file_encoding(const std::filesystem::path& _file_path);

	template<typename old_string, typename new_string>
		requires (std::same_as<old_string, std::string> || std::same_as<old_string, std::u8string> || std::same_as<old_string, std::wstring>)
	&& (std::same_as<new_string, std::string> || std::same_as<new_string, std::u8string> || std::same_as<new_string, std::wstring>)
		&& (!std::same_as<old_string, new_string>)
		new_string convert_to(const old_string& _string, const char* _encoding)
	{
		icu::UnicodeString icu_string;

		if constexpr (std::same_as<old_string, std::string>)
		{
			icu_string = std::move(icu::UnicodeString(_string.c_str(), static_cast<int32_t>(_string.size()), _encoding));
		}
		else if constexpr (std::same_as<old_string, std::u8string>)
		{
			icu_string = std::move(icu::UnicodeString::fromUTF8(icu::StringPiece(reinterpret_cast<const char*>(_string.data()), static_cast<int32_t>(_string.size()))));
		}
		else if constexpr (std::same_as<old_string, std::wstring>)
		{
			UChar* buffer = icu_string.getBuffer(static_cast<int32_t>(_string.size() * 2));

			int32_t icu_len;
			UErrorCode error = U_ZERO_ERROR;

			u_strFromWCS(buffer, static_cast<int32_t>(_string.size() * 2), &icu_len, _string.data(), static_cast<int32_t>(_string.size()), &error);
			icu_string.releaseBuffer(icu_len);
		}

		if constexpr (std::same_as<new_string, std::string>)
		{
			std::string s;
			s.resize(static_cast<size_t>(icu_string.length() * 4));
			int32_t s_len = icu_string.extract(0, icu_string.length(), s.data(), static_cast<uint32_t>(s.size()), _encoding);
			s.resize(static_cast<size_t>(s_len));
			return s;
		}
		else if constexpr (std::same_as<new_string, std::u8string>)
		{
			return icu_string.toUTF8String<std::u8string>();
		}
		else if constexpr (std::same_as<new_string, std::wstring>)
		{
			std::wstring ws;
			ws.resize(icu_string.length() * 2);

			int32_t ws_len;
			UErrorCode error = U_ZERO_ERROR;

			u_strToWCS(ws.data(), static_cast<int32_t>(ws.size()), &ws_len, icu_string.getBuffer(), icu_string.length(), &error);
			ws.resize(ws_len);

			return ws;
		}
	}
};