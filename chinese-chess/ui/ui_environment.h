/**
 * @file ui_environment.h
 * @brief 环境贴图面板：加载 / 移除 HDR 天空盒并编辑环境强度。
 * @details 构造时把 TEXTURES_PATH 下的 cracked ground.hdr 设为环境图；之后可用文件对话框换图，
 *          或移除后退回着色器的程序化天空。image 是场景里的引用边，写入一律经
 *          scene_manager::set_environment_image()，强度则直接写 scene_environment_data::intensity 后调用 need_environment_update。
 */
#pragma once

#include <cstdint>
#include <filesystem>

class scene_manager;

/**
 * @brief 环境管理器面板。
 * @details 提供“加载环境贴图”“移除环境贴图”两个按钮和强度滑块；无有效环境图时只显示占位文案。
 *          update 中返回无效句柄或取消选图时保留当前环境；构造时会直接写入默认图的创建结果。
 * @note 面板不拥有贴图；贴图由 scene_texture_manager 的槽持有，引用计数经 set_environment_image 维护。
 */
class ui_environment
{
public:
    /**
     * @brief 绑定场景管理器，并把默认的 cracked ground.hdr 设为环境贴图。
     * @param _manager 面板长期引用的场景管理器，生命周期必须覆盖本对象。
     * @note 构造期间创建 environment 用途的贴图并写入环境引用；返回无效句柄时使用程序化天空，未捕获异常仍会传播。
     */
    explicit ui_environment(scene_manager& _manager);
    /** @brief 默认析构：不释放环境贴图，引用由场景管理器维护。 */
    ~ui_environment() = default;

    /**
     * @brief 空实现：环境面板不随窗口尺寸调整任何状态。
     * @param _width  新帧缓冲宽度（像素），未使用。
     * @param _height 新帧缓冲高度（像素），未使用。
     */
    void resize(uint32_t _width, uint32_t _height) noexcept;
    /**
     * @brief 构建环境控件：加载按钮、当前贴图状态、移除按钮与强度滑块。
     * @note 换图或移除可能改变贴图紧凑绑定；只拖强度时调用 need_environment_update，置场景与贴图脏；无其他变化时不重建材质 SSBO。
     */
    void update();
    /**
     * @brief 键盘事件入口，当前为空实现。
     * @param _key      GLFW 键码。
     * @param _scancode GLFW 扫描码。
     * @param _action   GLFW 动作，取 GLFW_PRESS / GLFW_REPEAT / GLFW_RELEASE。
     * @param _mods     GLFW 修饰键位掩码。
     * @note 环境面板没有快捷键，保留该入口只为满足 ui_base 的 handle 约定。
     */
    void handle(int _key, int _scancode, int _action, int _mods) noexcept;

private:
    scene_manager& manager;  ///< @brief 场景管理器引用，环境图与强度都经它读写
};
