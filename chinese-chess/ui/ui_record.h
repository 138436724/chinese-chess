#pragma once

#include "scene/scene_handle.h"
#include "tools/record_loader.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class scene_manager;

class ui_record
{
public:
    explicit ui_record(scene_manager& _manager);
    ~ui_record() = default;

    void resize(uint32_t _width, uint32_t _height);
    void update();
    void handle(int _key, int _scancode, int _action, int _mods) noexcept;

private:
    void load_records(const std::filesystem::path& _record_path);
    //all_board_state capture_board_state();
    void restore_board_state(uint32_t _index) noexcept;

    void rebuild_piece_materials(std::unordered_map<PIECE_TYPE, material_handle>& _materials,
                                 std::wstring_view                                _characters,
                                 std::u16string_view                              _piece_names,
                                 const glm::vec3&                                 _background_color,
                                 const glm::vec3&                                 _foreground_color,
                                 const glm::vec3&                                 _absorption);

    void prev_step() noexcept;
    void next_step() noexcept;

private:
    uint32_t width  = 0;
    uint32_t height = 0;

    int                      now_record_index = 0;
    std::vector<std::string> all_records;
    std::vector<const char*> all_records_c_str;

    scene_manager& manager;

    model_handle                 chess_board;
    model_handle                 chess_board_line;
    std::array<model_handle, 32> all_chess_pieces;

    std::vector<all_board_state>                    board_state = {record_loader::get_init_all_board()};
    std::unordered_map<PIECE_TYPE, material_handle> red_chess_piece_materials;
    std::unordered_map<PIECE_TYPE, material_handle> black_chess_piece_materials;
};
