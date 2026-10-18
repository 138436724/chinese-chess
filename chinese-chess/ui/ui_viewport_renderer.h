/**
 * @file ui_viewport_renderer.h
 * @brief ImGui 平台副窗口的 WSI 接管器：surface、swapchain、acquire/present 全由应用负责。
 * @details 上游 Vulkan backend 在 1.92.9b 仍自带一套 ImGui_ImplVulkanH_Window 交换链；
 *          本类替换 ImGuiPlatformIO::Renderer_* 五个平台回调，只保留上游的 VBO/IBO 与绘制管线，
 *          把副窗口的 surface、swapchain、fence 与呈现路径换成仓库自己的 vulkan_swapchain。
 *          每个窗口状态按 ImGuiID 存在 implementation 里，回调异常先缓存、由 check_errors 重抛。
 */
#pragma once

#include <memory>

class vulkan_application;

// Public ImGui platform callbacks own the WSI path; the upstream backend still
// owns its vertex/index buffers and performs all ImGui draw commands.
/**
 * @brief 副窗口 WSI 的唯一所有者与 ImGui 平台回调的安装者。
 * @details 构造时保存并替换 Renderer_CreateWindow / DestroyWindow / SetWindowSize /
 *          RenderWindow / SwapBuffers；析构时原样还回。每个副窗口持有一条独立 swapchain、
 *          一组每帧槽命令缓冲和上游共享管线的句柄，resize 延迟到下一次 render 执行。
 * @note 同一线程只允许一个活动实例；必须早于 ImGui::DestroyContext 销毁，否则析构里恢复
 *       回调时 context 已不存在。
 */
class ui_viewport_renderer
{
public:
    /**
     * @brief 安装副窗口平台回调并绑定应用对象。
     * @param app 提供实例、物理设备、逻辑设备、图形队列与主交换链格式的应用对象。
     * @throws std::logic_error 当前线程已有另一个 ui_viewport_renderer 处于活动状态时抛出。
     * @note 构造只替换回调，不创建任何 surface 或 swapchain；窗口在 ImGui 首次创建平台窗口时才建。
     */
    explicit ui_viewport_renderer(vulkan_application& app);
    /**
     * @brief 把构造时保存的五个 Renderer_* 回调原样写回 ImGuiPlatformIO。
     * @note 不主动触发平台窗口的销毁回调；成员窗口表仍会析构并清理 WSI，ui_manager 须先 DestroyPlatformWindows 释放上游数据。
     */
    ~ui_viewport_renderer();
    /** @brief 禁止拷贝：回调安装状态与窗口表必须唯一。 */
    ui_viewport_renderer(const ui_viewport_renderer&) = delete;
    /** @brief 禁止拷贝赋值。 */
    ui_viewport_renderer& operator=(const ui_viewport_renderer&) = delete;
    /**
     * @brief 重新抛出平台回调里捕获的第一个异常。
     * @note 用 std::rethrow_exception 原样重抛缓存异常，无固定异常类型；没有错误时正常返回。
     * @note ui_manager 在 UpdatePlatformWindows 与 RenderPlatformWindowsDefault 之后各调一次，
     *       让本类回调捕获的异常回到主流程；异常槽不会清空，后续调用仍会重抛同一异常。
     */
    void check_errors();
private:
    /**
     * @brief pimpl：窗口表、原回调指针与异常槽都藏在 .cpp，避免头文件依赖 Vulkan/ImGui 平台结构。
     * @details 定义在 ui_viewport_renderer.cpp；头文件只保留指向它的 unique_ptr。
     */
    struct implementation;
    /** @brief 唯一实现对象；析构时先恢复回调再释放窗口表。 */
    std::unique_ptr<implementation> impl;
};
