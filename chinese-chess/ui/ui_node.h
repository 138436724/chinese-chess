/**
 * @file ui_node.h
 * @brief 材质与物体面板：创建 / 删除材质与模型，并编辑 PBR 参数和模型变换。
 * @details 面板只保存 material_handle / model_handle 列表和一个“贴图路径 → 句柄”的缓存，
 *          实体所有权都在 scene_manager。update 先按缓存路径做文件监视重载、删掉已死句柄，
 *          再依次构建物体区与材质区，修改只调用 need_material_update / need_model_update 置脏。
 */
#pragma once

#include "scene/scene_handle.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

class scene_manager;

/**
 * @brief 材质管理与物体管理面板。
 * @details 材质区按 texture_channel 的对照表生成贴图按钮（目前只列 albedo），
 *          其余控件覆盖 albedo、粗糙度、金属度、反射率、透射、吸收、自发光等字段；
 *          物体区用 glm::decompose 从模型矩阵拆出平移/欧拉角/缩放，编辑后按 T * R * S 重组。
 * @note 面板持有的是句柄视图；模型和材质由场景管理器销毁后，对应行在下一次 update 被跳过。
 */
class ui_node
{
public:
    /**
     * @brief 绑定场景管理器，不创建任何材质或模型。
     * @param _manager 面板长期引用的场景管理器，生命周期必须覆盖本对象。
     */
    explicit ui_node(scene_manager& _manager) noexcept;
    /** @brief 默认析构：不销毁材质与模型，实体由场景管理器回收。 */
    ~ui_node() = default;

    /**
     * @brief 空实现：物体面板不随窗口尺寸调整状态。
     * @param _width  新帧缓冲宽度（像素），未使用。
     * @param _height 新帧缓冲高度（像素），未使用。
     */
    void resize(uint32_t _width, uint32_t _height) noexcept;
    /**
     * @brief 先做贴图文件监视与死句柄清理，再依次构建物体区与材质区。
     * @note 监视命中时调用 reload_texture；路径缓存里指向已死贴图的项会被 erase_if 删除。
     */
    void update();
    /**
     * @brief 键盘事件入口，当前为空实现。
     * @param _key      GLFW 键码。
     * @param _scancode GLFW 扫描码。
     * @param _action   GLFW 动作，取 GLFW_PRESS / GLFW_REPEAT / GLFW_RELEASE。
     * @param _mods     GLFW 修饰键位掩码。
     * @note 物体面板没有快捷键，保留该入口只为满足 ui_base 的 handle 约定。
     */
    void handle(int _key, int _scancode, int _action, int _mods) noexcept;

private:
    /** @brief 构建“材质管理”区：材质列表、贴图按钮与全部 PBR 字段。 */
    void update_material();
    /** @brief 构建“物体管理”区：模型列表、可见性、变换分解与材质选择。 */
    void update_model();

    /**
     * @brief 在面板的材质列表中查找句柄下标。
     * @param _material 要查找的材质句柄。
     * @return 找到时返回下标，不在列表中返回 std::nullopt。
     */
    [[nodiscard]] std::optional<int> material_index_of(material_handle _material) const noexcept;

    /** @brief 场景管理器引用；材质、模型、贴图与脏标记都经它转发。 */
    scene_manager& manager;

    /** @brief 面板展示的材质句柄列表；只是视图，不拥有材质实体。 */
    std::vector<material_handle> materials;
    /** @brief 面板展示的模型句柄列表；只是视图，不拥有模型实体。 */
    std::vector<model_handle>    models;

    /** @brief 已加载贴图的路径到句柄映射，用于文件监视重载与按钮旁显示路径。 */
    std::unordered_map<std::filesystem::path, texture_handle> textures;
};
