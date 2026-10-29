/**
 * @file ui_light.h
 * @brief 灯光面板：创建 / 删除灯光，并按类型编辑颜色、强度、方向、位置、范围与锥角。
 * @details 构造时创建一个暖色平行光作为默认照明；update 里先选类型再点“添加灯光”，
 *          每盏灯用可折叠头展示适用于当前 light_type 的字段。面板只保存 light_handle 列表，
 *          灯光实体归 scene_light_manager 所有，删除经 scene_manager::destroy()。
 */
#pragma once

#include "scene/scene_handle.h"

#include <cstdint>
#include <vector>

class scene_manager;

/**
 * @brief 灯光管理面板。
 * @details 下拉框决定新灯的类型，添加后立即按该类型初始化 active_type；已有灯光按
 *          directional / point / spot 三种分支显示不同字段，聚光灯会实时钳制内外锥角，
 *          让内锥角不超过外锥角（余量 1 度，受 90 度上限约束）。
 * @note 面板持有的是句柄视图，不是所有权；场景里灯被别处销毁后对应行会被跳过。
 */
class ui_light
{
public:
    /**
     * @brief 绑定场景管理器并创建一个默认平行光。
     * @param _manager 面板长期引用的场景管理器，生命周期必须覆盖本对象。
     * @note 默认灯为暖白色、强度 5、方向 (1, 1, 10)；句柄同时记入面板的灯光列表。
     */
    explicit ui_light(scene_manager& _manager);
    /** @brief 默认析构：不销毁场景灯光，实体由场景管理器回收。 */
    ~ui_light() = default;

    /**
     * @brief 空实现：灯光面板不随窗口尺寸调整状态。
     * @param _width  新帧缓冲宽度（像素），未使用。
     * @param _height 新帧缓冲高度（像素），未使用。
     */
    void resize(uint32_t _width, uint32_t _height) noexcept;
    /**
     * @brief 构建灯光控件：类型下拉、添加按钮与每盏灯的可折叠参数区。
     * @note 删除按钮只记录下标，遍历结束后才 destroy 并从列表移除，避免遍历中改容器。
     * @note 聚光灯的内外锥角在拖动时互相钳制，余量 1 度，受 90 度上限约束。
     */
    void update();
    /**
     * @brief 键盘事件入口，当前为空实现。
     * @param _key      GLFW 键码。
     * @param _scancode GLFW 扫描码。
     * @param _action   GLFW 动作，取 GLFW_PRESS / GLFW_REPEAT / GLFW_RELEASE。
     * @param _mods     GLFW 修饰键位掩码。
     * @note 灯光面板没有快捷键，保留该入口只为满足 ui_base 的 handle 约定。
     */
    void handle(int _key, int _scancode, int _action, int _mods) noexcept;

private:
    scene_manager& manager;  ///< @brief 场景管理器引用；灯光创建、查询与销毁都经它转发

    int                       add_light_type = 0;  ///< @brief “添加灯光”下拉框的当前选项，取值对应 light_type 的底层值
    std::vector<light_handle> lights;  ///< @brief 面板展示的灯光句柄列表；只是视图，不拥有灯光实体
};
