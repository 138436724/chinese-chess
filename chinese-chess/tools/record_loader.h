#pragma once

#include "string_helper.h"

#include <array>
#include <memory>
#include <unicode/errorcode.h>
#include <unicode/parseerr.h>
#include <unicode/regex.h>
#include <unordered_map>

#define RECORD_LOADER record_loader::get_record_loader()
constexpr std::u8string_view RECORDS_PATH = u8"resources\\records\\";

enum class PIECE_COLOR : uint8_t
{
    BLACK = 0,
    RED   = 1
};

enum class PIECE_TYPE : uint8_t
{
    NONE,
    GENERAL,
    GUARD,
    ELEPHANT,
    HORSE,
    CHARIOT,
    CANNON,
    PAWN
};

struct piece_state
{
    PIECE_TYPE piece_type = PIECE_TYPE::NONE;
    uint8_t    x          = 0;
    uint8_t    y          = 0;

    auto operator<=>(const piece_state& _other) const = default;
};

using half_board_state = std::vector<piece_state>;
using all_board_state  = std::array<half_board_state, 2>;

class record_loader
{
public:
    template <typename string_class>
        requires std::same_as<string_class, std::string> || std::same_as<string_class, std::u8string>
                 || std::same_as<string_class, std::wstring>
    std::vector<string_class> read_record(const std::filesystem::path& _record_path);

    static std::pair<uint8_t, uint8_t> move_piece(PIECE_TYPE _piece_type, uint8_t _now_x, uint8_t _now_y, UChar _move_direction, uint8_t _number);

    inline const static std::unordered_map<UChar, uint8_t> chinese_digits = {
        {u'〇', 0}, {u'一', 1}, {u'二', 2},  {u'三', 3}, {u'四', 4}, {u'五', 5}, {u'六', 6}, {u'七', 7},
        {u'八', 8}, {u'九', 9}, {u'十', 10}, {u'零', 0}, {u'壹', 1}, {u'贰', 2}, {u'叁', 3}, {u'肆', 4},
        {u'伍', 5}, {u'陆', 6}, {u'柒', 7},  {u'捌', 8}, {u'玖', 9}, {u'拾', 10}};
    bool    is_chinese_digit(UChar ch) noexcept { return chinese_digits.find(ch) != chinese_digits.end(); };
    bool    is_arabic_digit(UChar ch) noexcept { return u_isdigit(ch); };
    uint8_t get_digit(UChar ch) noexcept
    {
        return u_isdigit(ch) ? static_cast<uint8_t>(u_charDigitValue(ch)) : chinese_digits.at(ch);
    };


    inline const static std::unordered_map<UChar, PIECE_TYPE> piece_map = {
        {u'帅', PIECE_TYPE::GENERAL},  {u'帥', PIECE_TYPE::GENERAL},  {u'将', PIECE_TYPE::GENERAL},
        {u'將', PIECE_TYPE::GENERAL},  {u'士', PIECE_TYPE::GUARD},    {u'仕', PIECE_TYPE::GUARD},
        {u'相', PIECE_TYPE::ELEPHANT}, {u'象', PIECE_TYPE::ELEPHANT}, {u'马', PIECE_TYPE::HORSE},
        {u'馬', PIECE_TYPE::HORSE},    {u'傌', PIECE_TYPE::HORSE},    {u'车', PIECE_TYPE::CHARIOT},
        {u'車', PIECE_TYPE::CHARIOT},  {u'俥', PIECE_TYPE::CHARIOT},  {u'炮', PIECE_TYPE::CANNON},
        {u'砲', PIECE_TYPE::CANNON},   {u'炮', PIECE_TYPE::CANNON},   {u'兵', PIECE_TYPE::PAWN},
        {u'卒', PIECE_TYPE::PAWN}};
    bool       is_piece_type(UChar ch) noexcept { return piece_map.find(ch) != piece_map.end(); }
    PIECE_TYPE get_piece_type(UChar ch) noexcept { return piece_map.at(ch); };


    std::vector<all_board_state>     load_records(const std::filesystem::path& _record_path);
    static constexpr all_board_state get_init_all_borad() noexcept
    {
        half_board_state red;
        red.emplace_back(piece_state(PIECE_TYPE::GENERAL, 5, 0));
        red.emplace_back(piece_state(PIECE_TYPE::GUARD, 4, 0));
        red.emplace_back(piece_state(PIECE_TYPE::GUARD, 6, 0));
        red.emplace_back(piece_state(PIECE_TYPE::ELEPHANT, 3, 0));
        red.emplace_back(piece_state(PIECE_TYPE::ELEPHANT, 7, 0));
        red.emplace_back(piece_state(PIECE_TYPE::HORSE, 2, 0));
        red.emplace_back(piece_state(PIECE_TYPE::HORSE, 8, 0));
        red.emplace_back(piece_state(PIECE_TYPE::CHARIOT, 1, 0));
        red.emplace_back(piece_state(PIECE_TYPE::CHARIOT, 9, 0));
        red.emplace_back(piece_state(PIECE_TYPE::CANNON, 2, 2));
        red.emplace_back(piece_state(PIECE_TYPE::CANNON, 8, 2));
        red.emplace_back(piece_state(PIECE_TYPE::PAWN, 1, 3));
        red.emplace_back(piece_state(PIECE_TYPE::PAWN, 3, 3));
        red.emplace_back(piece_state(PIECE_TYPE::PAWN, 5, 3));
        red.emplace_back(piece_state(PIECE_TYPE::PAWN, 7, 3));
        red.emplace_back(piece_state(PIECE_TYPE::PAWN, 9, 3));

        half_board_state black;
        black.emplace_back(piece_state(PIECE_TYPE::GENERAL, 5, 0));
        black.emplace_back(piece_state(PIECE_TYPE::GUARD, 4, 0));
        black.emplace_back(piece_state(PIECE_TYPE::GUARD, 6, 0));
        black.emplace_back(piece_state(PIECE_TYPE::ELEPHANT, 3, 0));
        black.emplace_back(piece_state(PIECE_TYPE::ELEPHANT, 7, 0));
        black.emplace_back(piece_state(PIECE_TYPE::HORSE, 2, 0));
        black.emplace_back(piece_state(PIECE_TYPE::HORSE, 8, 0));
        black.emplace_back(piece_state(PIECE_TYPE::CHARIOT, 1, 0));
        black.emplace_back(piece_state(PIECE_TYPE::CHARIOT, 9, 0));
        black.emplace_back(piece_state(PIECE_TYPE::CANNON, 2, 2));
        black.emplace_back(piece_state(PIECE_TYPE::CANNON, 8, 2));
        black.emplace_back(piece_state(PIECE_TYPE::PAWN, 1, 3));
        black.emplace_back(piece_state(PIECE_TYPE::PAWN, 3, 3));
        black.emplace_back(piece_state(PIECE_TYPE::PAWN, 5, 3));
        black.emplace_back(piece_state(PIECE_TYPE::PAWN, 7, 3));
        black.emplace_back(piece_state(PIECE_TYPE::PAWN, 9, 3));

        all_board_state borad{std::move(red), std::move(black)};

        return borad;
    }

    static record_loader& get_record_loader() noexcept;

private:
    record_loader()                                 = default;
    ~record_loader()                                = default;
    record_loader(const record_loader&)             = delete;
    record_loader& operator=(const record_loader&)  = delete;
    record_loader(const record_loader&&)            = delete;
    record_loader& operator=(const record_loader&&) = delete;

    static record_loader loader;
};


template <typename string_class>
    requires std::same_as<string_class, std::string> || std::same_as<string_class, std::u8string>
             || std::same_as<string_class, std::wstring>
std::vector<string_class> record_loader::read_record(const std::filesystem::path& _record_path)
{
    std::vector<string_class> all_records;

    auto encoding = STRING_HELPER::get_file_encoding<std::string>(_record_path);

    std::ifstream in_file(_record_path, std::ios::binary);
    std::string   content((std::istreambuf_iterator<char>(in_file)), std::istreambuf_iterator<char>());
    in_file.close();

    icu::UnicodeString records_text(content.c_str(), static_cast<int32_t>(content.size()), encoding.c_str());

    icu::UnicodeString pattern =
        u"([前后中])?[ ]*([车車俥马馬傌炮砲相象士仕帅帥将將兵卒])[ ]*([一二三四五六七八九]|\\d)?[ ]*([进退平])[ ]*([一二三四五六七八九]|\\d)";
    UParseError    pe{};
    icu::ErrorCode status;
    auto compiled_pattern = std::unique_ptr<icu::RegexPattern>(icu::RegexPattern::compile(pattern, pe, status));
    if (status.isFailure())
    {
        throw std::runtime_error("compile regex pattern fail!");
    }

    auto matcher = std::unique_ptr<icu::RegexMatcher>(compiled_pattern->matcher(records_text, status));
    while (matcher->find(status))
    {
        icu::UnicodeString match = matcher->group(0, status);
        if constexpr (std::is_same_v<string_class, std::wstring>)
        {
            std::wstring ws;
            ws.resize(match.length() * 2);

            int32_t    ws_len;
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
            all_records.push_back(std::move(match.toUTF8String<string_class>()));
        }
    }

    return all_records;
}
