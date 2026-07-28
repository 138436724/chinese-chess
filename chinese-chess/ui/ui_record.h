#pragma once

#include "scene/scene_manager.h"
#include "tools/record_loader.h"

#include <imgui.h>

class ui_record
{
public:
    explicit ui_record(scene_manager& _manager);
    ~ui_record() = default;

    void resize(uint32_t _width, uint32_t _height);
    void update();
    void handle(int _glfw_key) noexcept;

private:
    void load_records(const std::filesystem::path& _record_path);
    //all_board_state capture_board_state();
    void restore_board_state(uint32_t _index) noexcept;

    void prev_step() noexcept;
    void next_step() noexcept;

    [[nodiscard]] static glm::vec2 location_transform(PIECE_COLOR _use_color, PIECE_COLOR _piece_color, uint8_t _x, uint8_t _y) noexcept;

private:
    uint32_t width  = 0;
    uint32_t height = 0;

    int                      now_record_index = 0;
    std::vector<std::string> all_records;
    std::vector<const char*> all_records_c_str;

    scene_manager& manager;

    std::shared_ptr<scene_model>                 chess_board;
    std::shared_ptr<scene_model>                 chess_board_line;
    std::array<std::shared_ptr<scene_model>, 32> all_chess_pieces;

    std::vector<all_board_state>                                    board_state = {record_loader::get_init_all_board()};
    std::unordered_map<PIECE_TYPE, std::shared_ptr<scene_material>> red_chess_piece_materials;
    std::unordered_map<PIECE_TYPE, std::shared_ptr<scene_material>> black_chess_piece_materials;
};
