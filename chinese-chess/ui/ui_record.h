#pragma once

#include "tools/record_loader.h"

#include <array>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

struct scene_model;
struct scene_material;
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

    void prev_step() noexcept;
    void next_step() noexcept;

    // 白炉测试球体阵:roughness(列) × metallic(行) 的球体网格。
    // 每格一个材质(共用同一份球体网格),配合 debug_flags 的
    // "只强制 albedo 为白" 即可逐格核查环境光下的方向反射率。
    void rebuild_furnace_grid();
    void set_furnace_grid_visible(bool _visible) noexcept;
    // 白炉模式下隐藏棋盘与棋子(隔离测试场景);非白炉模式为空操作
    void apply_furnace_isolation() noexcept;
    // 白炉球体阵开关:UI 勾选与 `G` 热键共用这一条路径
    void set_furnace_grid_enabled(bool _enabled);
    // 白炉预设(关全部钳制 + 反照率强制为白):UI 按钮与 `H` 热键共用
    void toggle_furnace_preset() noexcept;

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

    // 白炉球体阵(懒创建;隐藏时不参与渲染)
    std::vector<std::shared_ptr<scene_model>> furnace_spheres;
    bool                                      furnace_grid_enabled = false;
    bool                                      furnace_grid_built   = false;

    std::vector<all_board_state>                                    board_state = {record_loader::get_init_all_board()};
    std::unordered_map<PIECE_TYPE, std::shared_ptr<scene_material>> red_chess_piece_materials;
    std::unordered_map<PIECE_TYPE, std::shared_ptr<scene_material>> black_chess_piece_materials;
};
