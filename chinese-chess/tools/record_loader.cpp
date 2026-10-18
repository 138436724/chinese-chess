/**
 * @file record_loader.cpp
 * @brief record_loader 的实现：一步着法的坐标换算与整份棋谱的逐帧推演。
 * @details move_piece 在匿名命名空间里，只做纯坐标计算；load_records 负责读文件、跑正则并维护
 *          「轮到谁走」与棋盘快照。显式检查的读取或记谱错误通过 std::expected 返回；
 *          逐步推演的错误附带文件名与步号，内存分配等异常仍可向外传播。
 */

#include "record_loader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <unicode/unistr.h>
#include <utility>
#include <vector>

namespace {
/**
 * @brief 按棋子种类与「进 / 退 / 平」把一个着法换算成新坐标。
 * @param _piece_type 走的棋子种类。
 * @param _now_x 当前列，1..9。
 * @param _now_y 当前行，0..9（己方底线为 0，向前为正）。
 * @param _move_direction 方向字：进、退或平。
 * @param _number 直行类当成步数，斜行类（士 / 象 / 马）当成目标列。
 * @return {新列, 新行}；方向或种类不在分派表里时原样返回当前坐标。
 * @note 马按 |当前列 - 目标列| 修正行号，走的是「日」字；这里只做几何换算，不检查绊马腿、九宫与河界。
 */
[[nodiscard]] constexpr std::pair<uint8_t, uint8_t> move_piece(PIECE_TYPE _piece_type,
                                                               uint8_t    _now_x,
                                                               uint8_t    _now_y,
                                                               UChar      _move_direction,
                                                               uint8_t    _number) noexcept
{
    std::pair new_position{_now_x, _now_y};

    switch (_piece_type)
    {
        case PIECE_TYPE::GENERAL:
        case PIECE_TYPE::CHARIOT:
        case PIECE_TYPE::CANNON:
        case PIECE_TYPE::PAWN:
            switch (_move_direction)
            {
                case u'进':
                    new_position = {_now_x, _now_y + _number};
                    break;
                case u'退':
                    new_position = {_now_x, _now_y - _number};
                    break;
                case u'平':
                    new_position = {_number, _now_y};
                    break;
                default:
                    break;
            }
            break;
        case PIECE_TYPE::GUARD:
            switch (_move_direction)
            {
                case u'进':
                    new_position = {_number, _now_y + 1};
                    break;
                case u'退':
                    new_position = {_number, _now_y - 1};
                    break;
                default:
                    break;
            }
            break;
        case PIECE_TYPE::ELEPHANT:
            switch (_move_direction)
            {
                case u'进':
                    new_position = {_number, _now_y + 2};
                    break;
                case u'退':
                    new_position = {_number, _now_y - 2};
                    break;
                default:
                    break;
            }
            break;
        case PIECE_TYPE::HORSE:
            switch (_move_direction)
            {
                case u'进':
                    new_position = {_number, _now_y + 3 - std::abs(_now_x - _number)};
                    break;
                case u'退':
                    new_position = {_number, _now_y - 3 + std::abs(_now_x - _number)};
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }

    return new_position;
}
}  // namespace

/**
 * @brief 读入棋谱、逐步推演，并返回每一步之后的棋盘快照。
 * @note 正则与 read_record 相同，但这里取 group(0) 的原文去掉空白后，要求有效字符恰好 4 个。
 * @note 红方着法用中文数字、黑方用阿拉伯数字；第 4 个字符的写法与「轮到谁走」不一致就报歧义。
 * @note 走子后按 (10 - x, 9 - y) 的镜像坐标删掉对方对应位置的棋子，即吃子；新坐标越界（x 不在 1..9、
 *       y 大于 9）算记谱错误。
 * @note 「前 / 中 / 后」这类前缀按同种棋子按行号降序排列后的位置取，取不到就报「棋子不在棋盘上」。
 */
std::expected<std::vector<all_board_state>, std::string> record_loader::load_records(const std::filesystem::path& _record_path)
{
    // get file encoding and read file
    const auto encoding = string_helper::get_file_encoding<std::string>(_record_path);

    std::ifstream     in_file(_record_path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in_file)), std::istreambuf_iterator<char>());
    in_file.close();

    const icu::UnicodeString records_text(content.c_str(), static_cast<int32_t>(content.size()), encoding.c_str());


    // regex
    const icu::UnicodeString pattern =
        u"([前后中])?[ ]*([车車俥马馬傌炮砲相象士仕帅帥将將兵卒])[ ]*([一二三四五六七八九]|\\d)?[ ]*([进退平])[ ]*([一二三四五六七八九]|\\d)";

    UParseError    pe{};
    icu::ErrorCode error;
    const auto compiled_pattern = std::unique_ptr<icu::RegexPattern>(icu::RegexPattern::compile(pattern, pe, error));
    if (error.isFailure())
    {
        return std::unexpected(std::format("Failed to compile regex pattern: {}", error.errorName()));
    }


    // generate board state
    std::vector<all_board_state> the_board_state;
    bool                         is_player_red = true;

    const auto matcher = std::unique_ptr<icu::RegexMatcher>(compiled_pattern->matcher(records_text, error));
    while (matcher->find(error))
    {
        const icu::UnicodeString match = matcher->group(0, error);

        icu::UnicodeString result;
        for (const auto& ch : match)
        {
            if (!u_isWhitespace(ch))
            {
                result.append(ch);
            }
        }

        if (result.length() != 4)
        {
            return std::unexpected(std::format("Record {} line {} length != 4.", _record_path.generic_string(),
                                               the_board_state.size() + 1));
        }

        if (is_player_red == is_arabic_digit(result.charAt(3)))
        {
            return std::unexpected(std::format("Ambiguous piece order in record file."));
        }

        if (!is_piece_type(result.charAt(0)) && !is_piece_type(result.charAt(1)))
        {
            return std::unexpected(std::format("Unknown piece type in record file."));
        }

        all_board_state now_board = get_init_all_board();
        if (!the_board_state.empty())
        {
            now_board = the_board_state.back();
        }

        const PIECE_COLOR now_color       = static_cast<PIECE_COLOR>(is_player_red);
        const PIECE_COLOR now_other_color = static_cast<PIECE_COLOR>(!is_player_red);

        const PIECE_TYPE now_type =
            is_piece_type(result.charAt(0)) ? get_piece_type(result.charAt(0)) : get_piece_type(result.charAt(1));

        uint8_t now_x = std::numeric_limits<uint8_t>::max();
        uint8_t now_y = std::numeric_limits<uint8_t>::max();
        if (is_piece_type(result.charAt(0)))
        {
            now_x = get_digit(result.charAt(1));
        }
        else
        {
            auto the_pieces = now_board.at(static_cast<size_t>(now_color))
                              | std::views::filter([&](const auto& _piece) { return _piece.piece_type == now_type; })
                              | std::views::transform([](const auto& _piece) static { return &_piece; })
                              | std::ranges::to<std::vector>();


            std::ranges::sort(the_pieces, [](const auto& _l, const auto& _r) static { return _l->y > _r->y; });

            size_t position_index = 0;
            switch (result.charAt(0))
            {
                case u'前':
                    position_index = 0;
                    break;
                case u'中':
                case u'二':
                    position_index = 1;
                    break;
                case u'三':
                    position_index = 2;
                    break;
                case u'四':
                    position_index = 3;
                    break;
                case u'后':
                    position_index = the_pieces.size() - 1;
                    break;
                default:
                    break;
            }

            if (position_index >= the_pieces.size())
            {
                return std::unexpected(std::format("Record {} line {}: piece not leave on board but also used.",
                                                   _record_path.generic_string(), the_board_state.size() + 1));
            }

            now_y = the_pieces.at(position_index)->y;
        }

        auto now_pieces = now_board.at(static_cast<size_t>(now_color)) | std::views::filter([&](const auto& _piece) {
                              return _piece.piece_type == now_type
                                     && (_piece.x == now_x || now_x == std::numeric_limits<uint8_t>::max())
                                     && (_piece.y == now_y || now_y == std::numeric_limits<uint8_t>::max());
                          });


        if (std::ranges::distance(now_pieces) != 1)
        {
            return std::unexpected(std::format("Ambiguous piece position in record file."));
        }

        auto& now_piece = now_pieces.front();
        const auto [new_x, new_y] =
            move_piece(now_type, now_piece.x, now_piece.y, result.charAt(2), get_digit(result.charAt(3)));

        if (new_x < 1 || new_x > 9 || new_y > 9)
        {
            return std::unexpected(std::format("Record {} line {}: move leaves the board ({},{}).",
                                               _record_path.generic_string(), the_board_state.size() + 1,
                                               static_cast<uint32_t>(new_x), static_cast<uint32_t>(new_y)));
        }

        now_piece.x = new_x;
        now_piece.y = new_y;

        std::erase_if(now_board.at(static_cast<size_t>(now_other_color)), [&](const auto& _piece) {
            return _piece.x == (10 - new_x) && _piece.y == (9 - new_y);  // 红方和黑方的Y是相反的
        });

        the_board_state.push_back(std::move(now_board));

        is_player_red = !is_player_red;
    }

    return the_board_state;
}
