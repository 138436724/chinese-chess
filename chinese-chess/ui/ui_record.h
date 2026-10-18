/**
 * @file ui_record.h
 * @brief 棋局面板：加载棋谱、按步回放，并维护棋盘、棋盘线与 32 个棋子的模型和材质。
 * @details 构造时创建棋盘、棋盘线和 32 个棋子模型；resize 在高度变化时按字号重建棋盘与棋子的
 *          字形图集材质；update 提供加载棋谱、局面列表与上一步/下一步；restore_board_state
 *          先隐藏全部棋子，再按当前局面摆放红黑双方的棋子。
 */
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

/**
 * @brief 棋谱加载与回放面板。
 * @details 棋谱文本与所有历史局面由 record_loader 解析后缓存在 all_records / board_state；
 *          棋盘和棋子模型、两种颜色的材质都登记在 scene_manager 里，面板只持有句柄。
 *          W/A 触发上一步、S/D 触发下一步，其余按键不处理。
 * @note 回放只改模型的可见性、材质与矩阵并置模型脏，不创建或销毁实体。
 */
class ui_record
{
public:
    /**
     * @brief 创建棋盘、棋盘线与 32 个棋子模型，并给棋盘写入默认 PBR 材质。
     * @param _manager 面板长期引用的场景管理器，生命周期必须覆盖本对象。
     * @note 模型路径拼接依赖运行目录为 chinese-chess/；棋子初始全部位于同一位置（z = -1.4）。
     */
    explicit ui_record(scene_manager& _manager);
    /** @brief 默认析构：不销毁棋盘、棋子与材质，实体由场景管理器回收。 */
    ~ui_record() = default;

    /**
     * @brief 记录新尺寸，并在高度变化时重建字形图集材质与当前局面。
     * @param _width  新帧缓冲宽度（像素），只记录，不参与字号计算。
     * @param _height 新帧缓冲高度（像素），决定棋盘与棋子的字号。
     * @note 字号取 floor(height / 9.0 * 2)；宽度变化不会触发重建，但会更新缓存值。
     * @note 末尾 restore_board_state(now_record_index) 保持当前回放位置不变。
     */
    void resize(uint32_t _width, uint32_t _height);
    /**
     * @brief 构建棋局控件：加载棋谱、局面列表、上一步与下一步。
     * @note 选好文件后调用 load_records，再无条件 restore_board_state(0)；成功时第 0 项是第一步走完后的局面。
     */
    void update();
    /**
     * @brief 键盘回放：W/A 上一步，S/D 下一步，只处理 GLFW_PRESS。
     * @param _key      GLFW 键码。
     * @param _scancode GLFW 扫描码，未使用。
     * @param _action   GLFW 动作，非 GLFW_PRESS 直接返回。
     * @param _mods     GLFW 修饰键位掩码，未使用。
     * @note 窗口层在 WantCaptureKeyboard 为 true 时不会转发到本面板，避免输入时触发回放。
     */
    void handle(int _key, int _scancode, int _action, int _mods) noexcept;

private:
    /**
     * @brief 读取并解析棋谱文件，成功后替换文本与局面缓存并把回放位置归零。
     * @param _record_path 棋谱文件路径；解析器返回 expected 错误或空局面时打印 stderr 并保留旧缓存。
     * @note 成功时重建 all_records_c_str；编码探测及分配异常会传播，替换过程中不保证事务回滚。
     */
    void load_records(const std::filesystem::path& _record_path);
    /**
     * @brief 把指定下标的局面写回场景：隐藏全部棋子，再按红黑双方摆放存活棋子。
     * @param _index board_state 的下标；越界或棋子数超过 32 时直接返回。
     * @note 标 noexcept：越界与棋子过多都在进入循环前自查，下标越界静默返回、棋子过多打印后返回；其他异常若逸出会直接 terminate。
     */
    void restore_board_state(uint32_t _index) noexcept;

    /**
     * @brief 为一种颜色的棋子建立或更新材质与字形图集贴图。
     * @param _materials        该颜色的“棋子类型 → 材质句柄”缓存，已存在则复用。
     * @param _characters       按类型顺序排列的宽字符字形，与 _piece_names 一一对应。
     * @param _piece_names      按类型顺序排列的棋子名（UTF-16），用于查 PIECE_TYPE。
     * @param _background_color 图集背景色；材质基色固定为白，颜色全由图集提供。
     * @param _foreground_color 字形前景色。
     * @param _absorption       介质的 RGB 吸收系数，写入材质。
     * @note 已有材质只换贴图与参数，句柄保持稳定；字号由 resize 缓存的 height 推出。
     */
    void rebuild_piece_materials(std::unordered_map<PIECE_TYPE, material_handle>& _materials,
                                 std::wstring_view                                _characters,
                                 std::u16string_view                              _piece_names,
                                 const glm::vec3&                                 _background_color,
                                 const glm::vec3&                                 _foreground_color,
                                 const glm::vec3&                                 _absorption);

    /** @brief 回退到上一个局面；已在第 0 步时不做任何事。 */
    void prev_step() noexcept;
    /** @brief 前进到下一个局面；已在最后一步时不做任何事。 */
    void next_step() noexcept;

private:
    /** @brief 最近一次 resize 记录的帧缓冲宽度（像素），当前不参与字号计算。 */
    uint32_t width  = 0;
    /** @brief 最近一次 resize 记录的帧缓冲高度（像素），决定字形图集字号。 */
    uint32_t height = 0;

    /** @brief 当前回放位置，同时是 ListBox 的选中下标。 */
    int                      now_record_index = 0;
    /** @brief 棋谱文本行，加载成功后整体替换。 */
    std::vector<std::string> all_records;
    /** @brief all_records 每行的 c_str 视图，供 ImGui::ListBox 使用，随加载整体重建。 */
    std::vector<const char*> all_records_c_str;

    /** @brief 场景管理器引用；模型、材质、贴图与脏标记都经它转发。 */
    scene_manager& manager;

    /** @brief 棋盘模型句柄。 */
    model_handle                 chess_board;
    /** @brief 棋盘线模型句柄，z 比棋盘略靠近相机。 */
    model_handle                 chess_board_line;
    /** @brief 32 个棋子模型句柄；回放时按下标依次复用。 */
    std::array<model_handle, 32> all_chess_pieces;

    /** @brief 回放局面：加载前仅有初始局面；加载成功后第 0 项是第一步走完的局面，不额外保留开局项。 */
    std::vector<all_board_state>                    board_state = {record_loader::get_init_all_board()};
    /** @brief 红方棋子的“类型 → 材质”缓存，resize 时复用句柄并重设图集与 PBR 参数。 */
    std::unordered_map<PIECE_TYPE, material_handle> red_chess_piece_materials;
    /** @brief 黑方棋子的“类型 → 材质”缓存，resize 时复用句柄并重设图集与 PBR 参数。 */
    std::unordered_map<PIECE_TYPE, material_handle> black_chess_piece_materials;
};
