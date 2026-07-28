#pragma once

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <unicode/errorcode.h>
#include <unicode/ucsdet.h>
#include <unicode/unistr.h>
#include <unicode/ustring.h>

namespace string_helper {

template <typename string_class>
    requires(std::same_as<string_class, std::string> || std::same_as<string_class, std::wstring>)
constexpr void trim(string_class& s) noexcept
{
    const auto start = std::ranges::find_if(s, [](const auto& _c) { return !std::iswspace(_c); });
    const auto end = std::ranges::find_if(s | std::views::reverse, [](const auto& _c) { return !std::iswspace(_c); }).base();
    s.erase(s.begin(), start);
    s.erase(end, s.end());
}

template <typename new_string_class, typename old_string_class>
    requires(std::same_as<old_string_class, std::string> || std::same_as<old_string_class, std::u8string>
             || std::same_as<old_string_class, std::wstring>)
            && (std::same_as<new_string_class, std::string> || std::same_as<new_string_class, std::u8string>
                || std::same_as<new_string_class, std::wstring>)
            && (!std::same_as<old_string_class, new_string_class>)
[[nodiscard]] inline new_string_class convert_to(const old_string_class& _string, const char* _encoding = "utf8") noexcept
{
    icu::UnicodeString icu_string;

    if constexpr (std::same_as<old_string_class, std::string>)
    {
        icu_string = std::move(icu::UnicodeString(_string.c_str(), static_cast<int32_t>(_string.size()), _encoding));
    }
    else if constexpr (std::same_as<old_string_class, std::u8string>)
    {
        icu_string = icu::UnicodeString::fromUTF8(
            icu::StringPiece(reinterpret_cast<const char*>(_string.data()), static_cast<int32_t>(_string.size())));
    }
    else if constexpr (std::same_as<old_string_class, std::wstring>)
    {
        UChar* buffer = icu_string.getBuffer(static_cast<int32_t>(_string.size() * 2));

        int32_t        icu_len;
        icu::ErrorCode error;

        u_strFromWCS(buffer, static_cast<int32_t>(_string.size() * 2), &icu_len, _string.data(),
                     static_cast<int32_t>(_string.size()), error);
        icu_string.releaseBuffer(icu_len);
    }

    if constexpr (std::same_as<new_string_class, std::string>)
    {
        std::string s;
        s.resize(static_cast<size_t>(icu_string.length() * 4));
        int32_t s_len = icu_string.extract(0, icu_string.length(), s.data(), static_cast<uint32_t>(s.size()), _encoding);
        s.resize(static_cast<size_t>(s_len));
        return s;
    }
    else if constexpr (std::same_as<new_string_class, std::u8string>)
    {
        return icu_string.toUTF8String<std::u8string>();
    }
    else if constexpr (std::same_as<new_string_class, std::wstring>)
    {
        std::wstring ws;
        ws.resize(static_cast<size_t>(icu_string.length()) * 2);

        int32_t        ws_len;
        icu::ErrorCode error;

        u_strToWCS(ws.data(), static_cast<int32_t>(ws.size()), &ws_len, icu_string.getBuffer(), icu_string.length(), error);
        ws.resize(ws_len);

        return ws;
    }
}

template <typename string_class>
    requires(std::same_as<string_class, std::string> || std::same_as<string_class, std::u8string>
             || std::same_as<string_class, std::wstring>)
[[nodiscard]] inline string_class get_file_encoding(const std::filesystem::path& _file_path)
{
    std::ifstream in_file(_file_path.generic_string(), std::ios::ate | std::ios::binary);
    if (!in_file.is_open())
    {
        throw std::runtime_error("Cannot open file!");
    }

    std::vector<char> buffer(in_file.tellg());
    in_file.seekg(0, std::ios::beg);
    in_file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    in_file.close();

    icu::ErrorCode error;
    const auto detector_guard = std::unique_ptr<UCharsetDetector, decltype(&ucsdet_close)>(ucsdet_open(error), ucsdet_close);

    ucsdet_setText(detector_guard.get(), buffer.data(), static_cast<int32_t>(buffer.size()), error);

    const UCharsetMatch* match = ucsdet_detect(detector_guard.get(), error);
    if (match == nullptr || error.isFailure())
    {
        throw std::runtime_error("Encoding detection failed!");
    }

    const std::string encoding = std::string(ucsdet_getName(match, error));

    if constexpr (std::same_as<string_class, std::string>)
    {
        return encoding;
    }
    else
    {
        return convert_to<string_class, std::string>(encoding);
    }
}

}  // namespace string_helper
