/**
 * @file string_helper.h
 * @brief 字符串裁剪、ICU 编码转换与源文件编码探测，全部以模板 / inline 形式在头文件内实现。
 * @details trim 按 std::iswspace 原地擦除两端空白；convert_to 以 icu::UnicodeString 为中转，
 *          在 std::string / std::u8string / std::wstring 之间按 constexpr 分支选择 ICU 入口，
 *          只接受源与目标互不相同的类型组合。get_file_encoding 用 ICU 的 UCharsetDetector
 *          嗅探整份文件的编码名，是 record_loader 解析棋谱前的第一步。三个接口都不做缓存，
 *          同一文件重复探测就会重复读盘。
 */

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unicode/errorcode.h>
#include <unicode/ucsdet.h>
#include <unicode/unistr.h>
#include <unicode/ustring.h>
#include <vector>

namespace string_helper {

/**
 * @brief 按 std::iswspace 原地裁剪首尾空白，保留中间的字符。
 * @param s 待裁剪的字符串，只接受 std::string 或 std::wstring。
 * @note 空白判定逐码元进行，受当前 C locale 影响；std::string 不会先按 UTF-8 解码。
 * @note 删除前导空白后重新查找尾部边界；空串和全空白串都得到空串。
 */
template <typename string_class>
    requires(std::same_as<string_class, std::string> || std::same_as<string_class, std::wstring>)
constexpr void trim(string_class& s) noexcept
{
    const auto start = std::ranges::find_if(s, [](const auto& _c) { return !std::iswspace(_c); });
    s.erase(s.begin(), start);

    // erase 可能使旧迭代器失效，尾部边界必须从裁剪后的字符串重新取得。
    const auto end = std::ranges::find_if(s | std::views::reverse, [](const auto& _c) { return !std::iswspace(_c); }).base();
    s.erase(end, s.end());
}

/**
 * @brief 在 std::string / std::u8string / std::wstring 之间做编码转换。
 * @param _string 源字符串；std::string 按 _encoding 解释，std::u8string 固定按 UTF-8，std::wstring 走 u_strFromWCS。
 * @param _encoding std::string 一侧的编码名，默认 utf8；源为 string 时控制解码，目标为 string 时控制编码。
 *        u8string 与 wstring 直接互转时不用此参数。
 * @return 目标类型 new_string_class 的字符串，内容取自 icu::UnicodeString 的转出结果。
 * @note 源与目标类型必须不同，同类型转换会被 requires 拒绝（同类型直接用拷贝即可）。
 * @note 未检查 ICU 状态或所需缓冲长度，不能保证非法输入和转换失败可恢复；结果可能不完整。
 * @note noexcept 仅禁止异常向外传播，字符串分配等异常若逸出会触发 std::terminate。
 */
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
        icu_string = icu::UnicodeString(_string.c_str(), static_cast<int32_t>(_string.size()), _encoding);
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
        const int32_t s_len = icu_string.extract(0, icu_string.length(), s.data(), static_cast<uint32_t>(s.size()), _encoding);
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

/**
 * @brief 用 ICU 的 UCharsetDetector 探测文本文件的字符集。
 * @param _file_path 待探测的文件，整份读入内存后再交给探测器。
 * @return 编码名（例如 GB18030、UTF-8）；string_class 为 std::wstring / std::u8string 时再经 convert_to 转一次。
 * @throws std::runtime_error 文件打不开（Cannot open file!），或探测器给不出匹配结果 / 探测过程报错。
 * @note 字符集探测是推测，可能误判；返回编码名不保证还原出原作者期望的文本，尤其是短文本或歧义编码。
 * @note std::ifstream 以 ate 模式打开并读完即关闭，文件大小完全由 tellg 决定。
 */
template <typename string_class>
    requires(std::same_as<string_class, std::string> || std::same_as<string_class, std::u8string>
             || std::same_as<string_class, std::wstring>)
[[nodiscard]] inline string_class get_file_encoding(const std::filesystem::path& _file_path)
{
    std::ifstream in_file(_file_path, std::ios::ate | std::ios::binary);
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
