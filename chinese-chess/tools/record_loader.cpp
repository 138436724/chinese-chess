#include "record_loader.h"

#include <algorithm>
#include <fstream>
#include <ranges>

std::pair<uint8_t, uint8_t> record_loader::move_piece(PIECE_TYPE _piece_type, uint8_t _now_x, uint8_t _now_y, UChar _move_direction, uint8_t _number) noexcept
{
    std::pair<uint8_t, uint8_t> new_position = std::make_pair(_now_x, _now_y);

    switch (_piece_type)
    {
        case PIECE_TYPE::GENERAL:
        case PIECE_TYPE::CHARIOT:
        case PIECE_TYPE::CANNON:
        case PIECE_TYPE::PAWN:
            switch (_move_direction)
            {
                case u'进':
                    new_position = std::make_pair(_now_x, _now_y + _number);
                    break;
                case u'退':
                    new_position = std::make_pair(_now_x, _now_y - _number);
                    break;
                case u'平':
                    new_position = std::make_pair(_number, _now_y);
                    break;
                default:
                    break;
            }
            break;
        case PIECE_TYPE::GUARD:
            switch (_move_direction)
            {
                case u'进':
                    new_position = std::make_pair(_number, _now_y + 1);
                    break;
                case u'退':
                    new_position = std::make_pair(_number, _now_y - 1);
                    break;
                default:
                    break;
            }
            break;
        case PIECE_TYPE::ELEPHANT:
            switch (_move_direction)
            {
                case u'进':
                    new_position = std::make_pair(_number, _now_y + 2);
                    break;
                case u'退':
                    new_position = std::make_pair(_number, _now_y - 2);
                    break;
                default:
                    break;
            }
            break;
        case PIECE_TYPE::HORSE:
            switch (_move_direction)
            {
                case u'进':
                    new_position = std::make_pair(_number, _now_y + 3 - std::abs(_now_x - _number));
                    break;
                case u'退':
                    new_position = std::make_pair(_number, _now_y - 3 + std::abs(_now_x - _number));
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

std::vector<all_board_state> record_loader::load_records(const std::filesystem::path& _record_path)
{
    // get file encoding and read file
    const auto encoding = string_helper::get_file_encoding<std::string>(_record_path);

    std::ifstream     in_file(_record_path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in_file)), std::istreambuf_iterator<char>());
    in_file.close();

    icu::UnicodeString records_text(content.c_str(), static_cast<int32_t>(content.size()), encoding.c_str());


    // regex
    const icu::UnicodeString pattern =
        u"([前后中])?[ ]*([车車俥马馬傌炮砲相象士仕帅帥将將兵卒])[ ]*([一二三四五六七八九]|\\d)?[ ]*([进退平])[ ]*([一二三四五六七八九]|\\d)";

    UParseError    pe{};
    icu::ErrorCode error;
    const auto compiled_pattern = std::unique_ptr<icu::RegexPattern>(icu::RegexPattern::compile(pattern, pe, error));
    if (error.isFailure())
    {
        throw std::runtime_error("Failed to compile regex pattern!");
    }


    // generate board state
    std::vector<all_board_state> the_board_state;
    bool                         is_player_red = true;

    const auto matcher = std::unique_ptr<icu::RegexMatcher>(compiled_pattern->matcher(records_text, error));
    while (matcher->find(error))
    {
        icu::UnicodeString match = matcher->group(0, error);

        icu::UnicodeString result;
        for (auto& ch : match)
        {
            if (!u_isWhitespace(ch))
            {
                result.append(ch);
            }
        }

        if (result.length() != 4)
        {
            throw std::runtime_error("Not a valid chess record file.");
        }

        if (is_player_red == is_arabic_digit(result[3]))
        {
            throw std::runtime_error("Not a valid chess record file.");
        }

        if (!is_piece_type(result[0]) && !is_piece_type(result[1]))
        {
            throw std::runtime_error("Not a valid chess record file.");
        }

        all_board_state now_board = get_init_all_board();
        if (!the_board_state.empty())
        {
            now_board = the_board_state.back();
        }

        const PIECE_COLOR now_color       = static_cast<PIECE_COLOR>(is_player_red);
        const PIECE_COLOR now_other_color = static_cast<PIECE_COLOR>(!is_player_red);

        const PIECE_TYPE now_type = is_piece_type(result[0]) ? get_piece_type(result[0]) : get_piece_type(result[1]);

        uint8_t now_x = std::numeric_limits<uint8_t>::max();
        uint8_t now_y = std::numeric_limits<uint8_t>::max();
        if (is_piece_type(result[0]))
        {
            now_x = get_digit(result[1]);
        }
        else
        {
            auto the_pieces = now_board.at(static_cast<size_t>(now_color))
                              | std::views::filter([&](const auto& _piece) { return _piece.piece_type == now_type; })
                              | std::views::transform([](const auto& _piece) static { return &_piece; })
                              | std::ranges::to<std::vector>();


            std::ranges::sort(the_pieces, [](const auto& _l, const auto& _r) static { return _l->y > _r->y; });

            switch (result[0])
            {
                case u'前':
                    now_y = the_pieces.front()->y;
                    break;
                case u'中':
                    now_y = the_pieces.at(1)->y;
                    break;
                case u'后':
                    now_y = the_pieces.back()->y;
                    break;
                case u'二':
                    now_y = the_pieces.at(1)->y;
                    break;
                case u'三':
                    now_y = the_pieces.at(2)->y;
                    break;
                case u'四':
                    now_y = the_pieces.at(3)->y;
                    break;
                default:
                    break;
            }
        }

        auto now_pieces = now_board.at(static_cast<size_t>(now_color))
                          | std::views::filter([&](const auto& _piece) { return _piece.piece_type == now_type; })
                          | std::views::filter([&](const auto& _piece) {
                                return _piece.x == now_x || now_x == std::numeric_limits<uint8_t>::max();
                            })
                          | std::views::filter([&](const auto& _piece) {
                                return _piece.y == now_y || now_y == std::numeric_limits<uint8_t>::max();
                            });


        if (std::ranges::distance(now_pieces) != 1)
        {
            throw std::runtime_error("Not a valid chess record file.");
        }

        auto& now_piece           = now_pieces.front();
        const auto [new_x, new_y] = move_piece(now_type, now_piece.x, now_piece.y, result[2], get_digit(result[3]));
        now_piece.x               = new_x;
        now_piece.y               = new_y;

        std::erase_if(now_board.at(static_cast<size_t>(now_other_color)), [&](const auto& _piece) {
            return _piece.x == (10 - new_x) && _piece.y == (9 - new_y);  // 红方和黑方的Y是相反的
        });

        the_board_state.push_back(std::move(now_board));

        is_player_red = !is_player_red;
    }

    return the_board_state;
}
