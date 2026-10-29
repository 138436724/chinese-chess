/**
 * @file ui_manager.h
 * @brief UI 管理器：初始化 ImGui 双 backend，持有全部面板与主 UI 的离屏渲染目标。
 * @details 构造体先创建五个面板（可能加载场景资源），再依次创建 ImGui context → 中文字体 → GLFW backend → 描述符池 → Vulkan backend →
 *          每帧槽命令缓冲 → 离屏图 → 副窗口 WSI 接管器；update 构建“场景设置”窗口并汇总各面板，
 *          render 把 draw data 画进 MSAA 附件、解析到 render_output 后提交。析构先等 GPU 空闲，
 *          再销毁平台窗口、两个 backend 与 context。
 */
#pragma once

#include "scene/scene_manager.h"
#include "ui_base.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <GLFW/glfw3.h>
#include <cstdint>
#include <vector>
#include <memory>

class vulkan_application;
class vulkan_commandbuffer;
struct ImDrawData;
class ui_viewport_renderer;

/**
 * @brief ImGui 生命周期与主 UI 渲染的唯一所有者。
 * @details 面板以 pro::proxy<ui_base> 存放在 vector 中，按摄像机、灯光、物体、棋局、环境的固定顺序
 *          update / handle；渲染模式下拉直接调 scene_manager::set_render_mode()。
 *          主 UI 用 MSAA color_image 渲染、average 解析到 render_output，后者再交给 application 合成。
 * @note 必须在 GLFW 窗口与 scene_manager 之后构造、在它们之前析构：ImGui 的 GLFW 回调依赖窗口，
 *       面板与 draw data 依赖场景管理器；backend 初始化期间还会改全局 ImGui 回调。
 */
class ui_manager
{
public:
    /**
     * @brief 建 ImGui context、字体、描述符池与两个 backend，并创建离屏目标与副窗口渲染器。
     * @param _window  GLFW 主窗口，GLFW backend 安装回调并把它作为主视口平台句柄。
     * @param _app     提供实例、设备、交换链格式与图形队列的应用对象。
     * @param _manager 场景管理器，面板通过它读写场景。
     * @param _width   初始离屏图宽度（像素）。
     * @param _height  初始离屏图高度（像素）。
     * @throws std::logic_error 当前线程已存在另一个副窗口渲染上下文时抛出。
     * @throws vk::SystemError 描述符池创建失败时由 Vulkan-Hpp 抛出。
     * @note 构造函数末尾调用 resize，离屏图在构造期间就已按宽高创建。
     */
    ui_manager(GLFWwindow* _window, vulkan_application& _app, scene_manager& _manager, uint32_t _width, uint32_t _height);
    /**
     * @brief 先 wait_idle，再按平台窗口 → 副窗口渲染器 → 两个 backend → context 的顺序销毁。
     * @note DestroyPlatformWindows 会触发副窗口的 destroy 回调，必须先于 backend Shutdown。
     */
    ~ui_manager();

    /**
     * @brief 按新尺寸重建 render_output 与 MSAA color_image，并转发给所有面板。
     * @param _width  新宽度（像素）。
     * @param _height 新高度（像素）。
     * @note 旧图经 recycle_bin.retire_after_submitted 延迟回收；两图格式都取自主交换链。
     */
    void                                  resize(uint32_t _width, uint32_t _height);
    /**
     * @brief 开始新 ImGui 帧，构建“场景设置”窗口与全部面板，最后记录 draw data。
     * @note 每帧调用一次后由 render 消费；draw_data 在下一次 ImGui::NewFrame 前有效。
     */
    void                                  update();
    /**
     * @brief 录制并提交主 UI：MSAA 渲染 → 解析到 render_output → 驱动平台窗口路径。
     * @param frame_slot 当前帧槽，同时也是命令缓冲下标。
     * @return 含副窗口路径在内的图形队列最新提交凭据，作为 application 合成时的 UI 依赖。
     * @throws std::out_of_range frame_slot 超出命令缓冲数量时由 vector::at 抛出。
     * @note 录 ImGui draw data 时持有图形队列的提交锁，因为上游字体/纹理上传可能在同一队列提交并等待。
     * @note 副窗口的 acquire / present 由 ui_viewport_renderer 在 UpdatePlatformWindows 与
     *       RenderPlatformWindowsDefault 中接管。
     */
    [[nodiscard]] vulkan_submission_token render(uint32_t frame_slot);
    /**
     * @brief 把 GLFW 键盘事件转发给全部面板；各面板自行决定是否响应，不返回消费标志。
     * @param _key      GLFW 键码。
     * @param _scancode GLFW 扫描码。
     * @param _action   GLFW 动作，取 GLFW_PRESS / GLFW_REPEAT / GLFW_RELEASE。
     * @param _mods     GLFW 修饰键位掩码。
     * @note 窗口层仅在 wants_keyboard() 为 false 时调用本函数，再转发到 scene_manager。
     * @note 本函数为 noexcept，面板 handle 若抛出异常会触发 terminate。
     */
    void                                  handle(int _key, int _scancode, int _action, int _mods) noexcept;

    // ImGui 是否正在接收键盘（文本框、下拉、快捷键捕获）——
    // 窗口层在分发应用快捷键前检查它，统一拦截面板回放、场景热键与截图。
    /**
     * @brief 查询 ImGui 当前是否捕获键盘。
     * @return ImGuiIO::WantCaptureKeyboard 的当前值。
     */
    [[nodiscard]] bool wants_keyboard() const noexcept;

    /**
     * @brief 取主 UI 的离屏输出图。
     * @return render_output 的引用，application 用它做屏幕合成采样；所有权仍在本管理器。
     */
    [[nodiscard]] vulkan_image& get_render_image() noexcept;

private:
    /** @brief 构建“场景管理”区的渲染模式下拉，切换时调 scene_manager::set_render_mode。 */
    void render_mode_ui();

private:
    vk::Format color_format = vk::Format::eUndefined;  ///< @brief 主 UI 离屏图与交换链共用的颜色格式，构造时固定

    vulkan_application&      app;  ///< @brief 应用对象引用；实例、设备、分配器与交换链都经它取用
    vulkan_queue&            graphic_queue;  ///< @brief 主 UI 提交所用的图形队列，与 application 的图形队列是同一份
    vulkan_recycle_bin       recycle_bin;  ///< @brief 延迟回收离屏旧图与相关资源的回收站，登记在图形队列上
    vk::raii::DescriptorPool descriptor_pool = nullptr;  ///< @brief ImGui Vulkan backend 用的描述符池，两类 descriptorCount 取 backend 最小值，仅 maxSets 使用两者之和乘帧槽数

    ImDrawData* draw_data = nullptr;  ///< @brief 本帧 ImGui::GetDrawData 的结果，update 时刷新、render 时消费

    vulkan_image render_output;  ///< @brief 单 sample 的主 UI 输出图，供 application 合成采样
    vulkan_image color_image;  ///< @brief MSAA 颜色附件，每帧 average 解析到 render_output
    std::unique_ptr<ui_viewport_renderer> viewport_renderer;  ///< @brief 副窗口 WSI 接管器；在 ImGui 平台窗口销毁之后、backend Shutdown 之前 reset

    scene_manager&                   manager;  ///< @brief 场景管理器引用，面板与渲染模式切换都经它转发
    std::vector<pro::proxy<ui_base>> ui_managers;  ///< @brief 五个面板的 proxy，按固定顺序 update / handle
    int                              current_mode = static_cast<int>(render_mode::ray_tracing);  ///< @brief 渲染模式下拉的当前下标，初值对应 render_mode::ray_tracing

    std::vector<vulkan_commandbuffer> commandbuffers;  ///< @brief 每个 frame slot 一条主 UI 命令缓冲
};
