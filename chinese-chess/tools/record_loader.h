/**
 * @file record_loader.h
 * @brief 中文记谱解析：识别文件编码、用正则抽出着法，并逐步重放出每一步之后的棋盘状态。
 * @details 记谱是「棋子 + 可选的 前/中/后 前缀 + 进/退/平 + 数字」的文本，本模块只认中文与阿拉伯数字。
 *          棋盘坐标 x 取 1..9（列）、y 取 0..9（行，己方底线为 0，向前为正），红黑各用一套镜像坐标。
 *          解析分两步：read_record 只负责抽字符串（给 UI 显示），load_records 才真正推演棋局。
 */

#pragma once

#include "string_helper.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unicode/errorcode.h>
#include <unicode/parseerr.h>
#include <unicode/regex.h>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr std::string_view RECORDS_PATH = "resources\\records\\";

/**
 * @brief 棋子颜色，同时用作 all_board_state 的下标。
 * @details 数值 0 是黑方、1 是红方，与 is_player_red 的布尔值直接对应（true 走红方那份状态）。
 */
enum class PIECE_COLOR : uint8_t
{
    BLACK = 0,  ///< 黑方，对应 all_board_state 的第 0 份。
    RED   = 1   ///< 红方，对应 all_board_state 的第 1 份。
};

/**
 * @brief 棋子种类，用于查表与按种类分派走子规则。
 * @details NONE 只作默认值，不会出现在开局布局与合法着法里；其余七种经 piece_map 与中文棋子名对应，
 *          红黑两套写法（如「车 / 車 / 俥」）映射到同一个枚举值，分组方式即 move_piece 的分派方式。
 */
enum class PIECE_TYPE : uint8_t
{
    NONE,     ///< 未指定 / 空位，只作默认值，不参与走子规则分派。
    GENERAL,  ///< 帅 / 将，按直行类处理：进 / 退或平到指定列。
    GUARD,    ///< 士 / 仕，进 / 退固定斜一格，数字是目标列。
    ELEPHANT, ///< 相 / 象，进 / 退固定斜两格，数字是目标列。
    HORSE,    ///< 马，走「日」字：数字是目标列，行号按列差修正。
    CHARIOT,  ///< 车 / 車 / 俥，按直行类处理。
    CANNON,   ///< 炮 / 砲，按直行类处理。
    PAWN      ///< 兵 / 卒，按直行类处理。
};

/**
 * @brief 棋盘上的一枚棋子：种类与坐标。
 * @details x 是 1..9 的列、y 是 0..9 的行，两份状态都以己方底线为 y=0；换算对方半场坐标要按
 *          (10 - x, 9 - y) 镜像。默认的三路比较让两枚棋子可以直接比较 / 排序，方便判断回放是否停在同一局面。
 */
struct piece_state
{
    /** @brief 棋子种类，默认 NONE 表示这个槽位还没有意义。 */
    PIECE_TYPE piece_type = PIECE_TYPE::NONE;
    /** @brief 列坐标，1 到 9。 */
    uint8_t    x          = 0;
    /** @brief 行坐标，0 到 9；0 是己方底线，数值越大越靠近对方。 */
    uint8_t    y          = 0;

    /**
     * @brief 按成员逐项比较的默认三路比较，供容器排序或局面判等使用。
     * @param _other 被比较的另一枚棋子。
     * @return 三路比较结果。
     */
    auto operator<=>(const piece_state& _other) const = default;
};

/** @brief 一方的棋子列表；开局 16 枚，被吃掉的棋子直接从这个列表里 erase。 */
using half_board_state = std::vector<piece_state>;
/** @brief 完整棋局：两份半场状态，下标与 PIECE_COLOR 对应。 */
using all_board_state  = std::array<half_board_state, 2>;

namespace record_loader {

/** @brief 中文数字（含大写与「〇 / 零」）到数值的映射；「十」与「拾」都按 10 处理。 */
inline const std::unordered_map<UChar, uint8_t> chinese_digits = {
    {u'〇', 0}, {u'一', 1}, {u'二', 2},  {u'三', 3}, {u'四', 4}, {u'五', 5}, {u'六', 6}, {u'七', 7},
    {u'八', 8}, {u'九', 9}, {u'十', 10}, {u'零', 0}, {u'壹', 1}, {u'贰', 2}, {u'叁', 3}, {u'肆', 4},
    {u'伍', 5}, {u'陆', 6}, {u'柒', 7},  {u'捌', 8}, {u'玖', 9}, {u'拾', 10}};

/**
 * @brief 判断字符是否是表里的中文数字。
 * @param ch UTF-16 码元（icu::UnicodeString 的字符类型）。
 * @return 在 chinese_digits 里返回 true。
 */
[[nodiscard]] inline bool is_chinese_digit(UChar ch) noexcept
{
    return chinese_digits.contains(ch);
}

/**
 * @brief 判断字符是否是 ICU 认定的十进制数字。
 * @param ch UTF-16 码元。
 * @return u_isdigit 的结果（ASCII 的 '0'..'9' 与其它 Unicode 十进制数字都算）。
 */
[[nodiscard]] inline bool is_arabic_digit(UChar ch) noexcept
{
    return u_isdigit(ch);
}

/**
 * @brief 把数字字符转成 0..10 的数值。
 * @param ch UTF-16 码元，必须是阿拉伯数字或表里的中文数字。
 * @return 阿拉伯数字取 u_charDigitValue，其余查 chinese_digits。
 * @note 标了 noexcept，但中文分支用的是 .at()：表里没有的字符会抛 std::out_of_range 并直接 terminate，
 *       调用方必须先确认 is_chinese_digit / is_arabic_digit。
 */
[[nodiscard]] inline uint8_t get_digit(UChar ch) noexcept
{
    return u_isdigit(ch) ? static_cast<uint8_t>(u_charDigitValue(ch)) : chinese_digits.at(ch);
}

/** @brief 棋子名到种类的映射，红黑两套写法（帅 / 帥、车 / 車 / 俥 等）都在同一张表里。 */
inline const std::unordered_map<UChar, PIECE_TYPE> piece_map = {
    {u'帅', PIECE_TYPE::GENERAL},  {u'帥', PIECE_TYPE::GENERAL},  {u'将', PIECE_TYPE::GENERAL},
    {u'將', PIECE_TYPE::GENERAL},  {u'士', PIECE_TYPE::GUARD},    {u'仕', PIECE_TYPE::GUARD},
    {u'相', PIECE_TYPE::ELEPHANT}, {u'象', PIECE_TYPE::ELEPHANT}, {u'马', PIECE_TYPE::HORSE},
    {u'馬', PIECE_TYPE::HORSE},    {u'傌', PIECE_TYPE::HORSE},    {u'车', PIECE_TYPE::CHARIOT},
    {u'車', PIECE_TYPE::CHARIOT},  {u'俥', PIECE_TYPE::CHARIOT},  {u'炮', PIECE_TYPE::CANNON},
    {u'砲', PIECE_TYPE::CANNON},   {u'兵', PIECE_TYPE::PAWN},     {u'卒', PIECE_TYPE::PAWN}};

/**
 * @brief 判断字符是否是认得的中文棋子名。
 * @param ch UTF-16 码元。
 * @return 在 piece_map 里返回 true。
 */
[[nodiscard]] inline bool is_piece_type(UChar ch) noexcept
{
    return piece_map.contains(ch);
}

/**
 * @brief 取字符对应的棋子种类。
 * @param ch UTF-16 码元，必须是 piece_map 里的字。
 * @return 对应的 PIECE_TYPE。
 * @note 同样是 noexcept + .at()：查不到会抛 std::out_of_range 并 terminate，调用前须先过 is_piece_type。
 */
[[nodiscard]] inline PIECE_TYPE get_piece_type(UChar ch) noexcept
{
    return piece_map.at(ch);
}

/**
 * @brief 读入整份记谱，按正则抽出每一条着法原文。
 * @tparam string_class 结果字符串类型，只接受 std::string 或 std::wstring。
 * @param _record_path 记谱文件路径。
 * @return 抽到的着法文本数组，顺序与文件中的出现顺序一致；正则编译失败或宽字符转换失败返回错误串。
 * @throws std::runtime_error 文件打不开——由 string_helper::get_file_encoding 内部抛出，本函数不捕获。
 * @note 先探测文件编码，再用该编码把内容转成 icu::UnicodeString；正则允许「前 / 后 / 中」前缀与任意空格。
 * @note 只匹配形状像着法的片段，所以标题、注解、空行会被自然跳过；这里不校验棋盘合法性。
 */
template <typename string_class>
    requires std::same_as<string_class, std::string> || std::same_as<string_class, std::wstring>
[[nodiscard]] std::expected<std::vector<string_class>, std::string> read_record(const std::filesystem::path& _record_path)
{
    std::vector<string_class> all_records;

    const auto encoding = string_helper::get_file_encoding<std::string>(_record_path);

    std::ifstream     in_file(_record_path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in_file)), std::istreambuf_iterator<char>());
    in_file.close();

    const icu::UnicodeString records_text(content.c_str(), static_cast<int32_t>(content.size()), encoding.c_str());

    const icu::UnicodeString pattern =
        u"([前后中])?[ ]*([车車俥马馬傌炮砲相象士仕帅帥将將兵卒])[ ]*([一二三四五六七八九]|\\d)?[ ]*([进退平])[ ]*([一二三四五六七八九]|\\d)";
    UParseError    pe{};
    icu::ErrorCode status;
    const auto compiled_pattern = std::unique_ptr<icu::RegexPattern>(icu::RegexPattern::compile(pattern, pe, status));
    if (status.isFailure())
    {
        return std::unexpected("Failed to compile regex pattern!");
    }

    const auto matcher = std::unique_ptr<icu::RegexMatcher>(compiled_pattern->matcher(records_text, status));
    while (matcher->find(status))
    {
        const icu::UnicodeString match = matcher->group(0, status);
        if constexpr (std::is_same_v<string_class, std::wstring>)
        {
            std::wstring ws;
            ws.resize(static_cast<size_t>(match.length()) * 2);

            int32_t    ws_len;
            UErrorCode error = U_ZERO_ERROR;

            u_strToWCS(ws.data(), static_cast<int32_t>(ws.size()), &ws_len, match.getBuffer(), match.length(), &error);
            if (U_FAILURE(error))
            {
                return std::unexpected("Cannot convert to wstring.");
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

/**
 * @brief 解析记谱并重放出每一步之后的棋盘状态。
 * @param _record_path 记谱文件路径。
 * @return 快照数组，第 i 项是第 i+1 步走完后的完整棋局；解析失败返回错误串。
 * @throws std::runtime_error 文件打不开——由 string_helper::get_file_encoding 内部抛出，本函数不捕获。
 * @note 每步都从上一份快照复制，红黑交替（is_player_red 每步翻转）；吃子按对方半场的镜像坐标从对方列表 erase。
 * @note 长度不是 4、棋子不在棋盘上、按已定位的列 / 行筛不出唯一一枚同种棋子等情况都判为记谱错误。
 */
[[nodiscard]] std::expected<std::vector<all_board_state>, std::string> load_records(const std::filesystem::path& _record_path);

/**
 * @brief 构造开局棋局：红黑各 16 枚棋子，两边用同一套坐标布局。
 * @return all_board_state，下标 0 黑方、1 红方。
 * @note 两份布局数值完全相同，方向差异由「y 从各自底线起算」这一定义吸收，走子时不需要按颜色翻转。
 */
[[nodiscard]] constexpr all_board_state get_init_all_board() noexcept
{
    half_board_state red;
    red.emplace_back(PIECE_TYPE::GENERAL, 5, 0);
    red.emplace_back(PIECE_TYPE::GUARD, 4, 0);
    red.emplace_back(PIECE_TYPE::GUARD, 6, 0);
    red.emplace_back(PIECE_TYPE::ELEPHANT, 3, 0);
    red.emplace_back(PIECE_TYPE::ELEPHANT, 7, 0);
    red.emplace_back(PIECE_TYPE::HORSE, 2, 0);
    red.emplace_back(PIECE_TYPE::HORSE, 8, 0);
    red.emplace_back(PIECE_TYPE::CHARIOT, 1, 0);
    red.emplace_back(PIECE_TYPE::CHARIOT, 9, 0);
    red.emplace_back(PIECE_TYPE::CANNON, 2, 2);
    red.emplace_back(PIECE_TYPE::CANNON, 8, 2);
    red.emplace_back(PIECE_TYPE::PAWN, 1, 3);
    red.emplace_back(PIECE_TYPE::PAWN, 3, 3);
    red.emplace_back(PIECE_TYPE::PAWN, 5, 3);
    red.emplace_back(PIECE_TYPE::PAWN, 7, 3);
    red.emplace_back(PIECE_TYPE::PAWN, 9, 3);

    half_board_state black;
    black.emplace_back(PIECE_TYPE::GENERAL, 5, 0);
    black.emplace_back(PIECE_TYPE::GUARD, 4, 0);
    black.emplace_back(PIECE_TYPE::GUARD, 6, 0);
    black.emplace_back(PIECE_TYPE::ELEPHANT, 3, 0);
    black.emplace_back(PIECE_TYPE::ELEPHANT, 7, 0);
    black.emplace_back(PIECE_TYPE::HORSE, 2, 0);
    black.emplace_back(PIECE_TYPE::HORSE, 8, 0);
    black.emplace_back(PIECE_TYPE::CHARIOT, 1, 0);
    black.emplace_back(PIECE_TYPE::CHARIOT, 9, 0);
    black.emplace_back(PIECE_TYPE::CANNON, 2, 2);
    black.emplace_back(PIECE_TYPE::CANNON, 8, 2);
    black.emplace_back(PIECE_TYPE::PAWN, 1, 3);
    black.emplace_back(PIECE_TYPE::PAWN, 3, 3);
    black.emplace_back(PIECE_TYPE::PAWN, 5, 3);
    black.emplace_back(PIECE_TYPE::PAWN, 7, 3);
    black.emplace_back(PIECE_TYPE::PAWN, 9, 3);

    all_board_state board{std::move(red), std::move(black)};

    return board;
}

}  // namespace record_loader
