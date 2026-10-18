/**
 * @file window.h
 * @brief GLFW 窗口与帧循环：持有 application / scene / ui 并规定每帧调用顺序。
 * @details 构造按 GLFW → 实例与设备 → scene_manager → ui_manager 的顺序建立对象图；
 *          render 是唯一帧循环，顺序为 begin_frame → ui->update → scene->update → ui->render →
 *          scene->render → app->render，窗口关闭或异常时退出并按 ui → scene → app 逆序销毁。
 */
#pragma once

#include <GLFW/glfw3.h>
#include <array>
#include <cstdint>
#include <exception>
#include <memory>

class scene_manager;
class ui_manager;
class vulkan_application;

/**
 * @brief 主窗口与所有顶层对象的拥有者。
 * @details 注册 GLFW 的 framebuffer / cursor / mouse / key 回调，并用 window user pointer 回到本对象；
 *          回调里不抛异常，错误存进 callback_error，由帧循环在主线程重抛。窗口尺寸变化只置
 *          resize_requested，真正的 swapchain 重建合并到下一帧开头。
 * @note GLFW 与 ImGui 都要求在主线程使用；成员声明顺序保证异常展开时 scene / ui 先于 app 销毁。
 */
class glfw_window
{
public:
    /**
     * @brief 初始化 GLFW、Vulkan 应用、场景管理器与 UI 管理器，并绑定两张离屏图。
     * @throws std::runtime_error GLFW 初始化、窗口创建或 surface 创建失败时抛出。
     * @throws vk::SystemError 实例、设备、交换链等 Vulkan 对象创建失败时由 Vulkan-Hpp 抛出。
     * @note 窗口固定 800x600 起步、不使用图形 API（GLFW_NO_API）；构造末尾才把 scene/UI 输出图交给合成。
     */
    glfw_window();
    /**
     * @brief 等 GPU 空闲、保存管线缓存，再按 ui → scene → app → GLFW 的顺序释放。
     * @note 必须先于 glfwDestroyWindow 销毁 ui：ImGui 的 GLFW 回调仍引用该窗口。
     */
    ~glfw_window();

    /**
     * @brief 主帧循环：轮询事件、合并 resize、按固定顺序推进一帧。
     * @throws std::runtime_error acquire 或 present 返回 fatal 时抛出。
     * @note 回调里缓存的异常以及 scene/ui 更新、提交失败的异常会原样传播。
     * @note 最小化或零尺寸时只等 0.05 秒，不 acquire 也不推进帧槽。
     */
    void render();

    /**
     * @brief GLFW framebuffer 尺寸回调：只登记 resize 请求，实际重建在下一帧开头。
     * @param _window 触发回调的 GLFW 窗口。
     * @param _width  新帧缓冲宽度（像素）。
     * @param _height 新帧缓冲高度（像素）。
     * @note 静态回调不能抛异常：内部错误存进 callback_error，由 render 重抛。
     */
    static void glfw_resize_callback(GLFWwindow* _window, int _width, int _height) noexcept;
    /**
     * @brief GLFW 光标位置回调：转发到成员函数，当前为空实现。
     * @param _window 触发回调的 GLFW 窗口。
     * @param _xpos   相对窗口内容区左上角的光标 x 坐标，使用 GLFW 屏幕坐标单位，不一定是 framebuffer 像素。
     * @param _ypos   相对窗口内容区左上角的光标 y 坐标，与 _xpos 单位相同。
     * @note 成员实现是 noexcept，因此这里不捕获异常。
     */
    static void glfw_cursor_position_callback(GLFWwindow* _window, double _xpos, double _ypos) noexcept;
    /**
     * @brief GLFW 鼠标按键回调：转发到成员函数，当前为空实现。
     * @param _window 触发回调的 GLFW 窗口。
     * @param _button 鼠标按键编号。
     * @param _action 按键动作，取 GLFW_PRESS / GLFW_RELEASE。
     * @param _mods   修饰键位掩码。
     * @note 成员实现是 noexcept，因此这里不捕获异常。
     */
    static void glfw_mouse_button_callback(GLFWwindow* _window, int _button, int _action, int _mods) noexcept;
    /**
     * @brief GLFW 键盘回调：先交给 UI，再按 ImGui 是否捕获键盘决定是否传给场景与截图逻辑。
     * @param _window   触发回调的 GLFW 窗口。
     * @param _key      GLFW 键码。
     * @param _scancode GLFW 扫描码。
     * @param _action   GLFW 动作，取 GLFW_PRESS / GLFW_REPEAT / GLFW_RELEASE。
     * @param _mods     GLFW 修饰键位掩码。
     * @note 静态回调不能抛异常：ui/scene 的异常存进 callback_error，由帧循环重抛。
     */
    static void glfw_key_callback(GLFWwindow* _window, int _key, int _scancode, int _action, int _mods) noexcept;

private:
    /** @brief 置位 resize_requested；下一帧开头统一重建 swapchain 与离屏图。 */
    void resize_callback(GLFWwindow* _window, int _width, int _height);
    /** @brief 光标位置入口，当前为空实现。 */
    void cursor_position_callback(GLFWwindow* _window, double _xpos, double _ypos) const noexcept;
    /** @brief 鼠标按键入口，当前为空实现。 */
    void mouse_button_callback(GLFWwindow* _window, int _button, int _action, int _mods) const noexcept;
    /**
     * @brief 把按键先转给 ui_manager，再决定是否传给 scene_manager，并识别截图键 C。
     * @param _window   触发回调的 GLFW 窗口。
     * @param _key      GLFW 键码。
     * @param _scancode GLFW 扫描码。
     * @param _action   GLFW 动作，取 GLFW_PRESS / GLFW_REPEAT / GLFW_RELEASE。
     * @param _mods     GLFW 修饰键位掩码。
     * @note ImGui 捕获键盘时直接返回，避免输入框打字同时推进回放或触发截图。
     */
    void key_callback(GLFWwindow* _window, int _key, int _scancode, int _action, int _mods);

private:
    /** @brief 按 C 键后置位，本帧 scene->render 以 need_capture 触发一次截图。 */
    bool need_capture = false;
    /** @brief 回调中捕获的异常，由帧循环在主线程重抛；resize/key 回调入口会检查它并提前返回。 */
    std::exception_ptr callback_error;
    /** @brief framebuffer 回调置位，帧循环开头重建 swapchain、scene、ui 并重新绑定图像。 */
    bool resize_requested = false;

    /** @brief 标题栏刷新计时累计秒数，达到 0.5 秒时重算 fps。 */
    float                 title_timer  = 0.f;
    /** @brief 当前统计窗口内累计的帧数，与 title_timer 一起算 fps 后清零。 */
    uint32_t              title_frames = 0;
    /** @brief 标题字符串缓冲，format_to_n 写入后手动补 '\0' 截断。 */
    std::array<char, 128> window_title = {};

    /** @brief GLFW 窗口句柄；构造成功后才非空。 */
    GLFWwindow*                         window = nullptr;
    /** @brief Vulkan 应用；必须最后销毁，scene/ui 都引用它。 */
    std::unique_ptr<vulkan_application> app    = nullptr;
    /** @brief UI 管理器；持有 ImGui 回调，必须先于窗口与 GLFW 终止销毁。 */
    std::unique_ptr<ui_manager>         ui     = nullptr;
    /** @brief 场景管理器；在 ui 之后、app 之前销毁。 */
    std::unique_ptr<scene_manager>      scene  = nullptr;

#ifndef NDEBUG
    // for renderdoc capture
    /** @brief 调试构建下标记首帧：RenderDoc 不能从首帧开始捕获，首帧跳过。 */
    bool first_frame   = true;
    /** @brief 本帧是否已在 scene->update 前开始 RenderDoc 捕获，帧末负责结束。 */
    bool begin_capture = false;
#endif  // !NDEBUG
};
