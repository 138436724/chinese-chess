/**
 * @file renderdoc_capture.h
 * @brief RenderDoc 抓帧的进程级入口：开始 / 结束一帧捕获，未编入 SDK 支持或进程未注入 API 时为 no-op。
 * @details 是否真有实现由 CMake 的 HAS_RENDERDOC 决定：找到 renderdoc_app.h 才编进注入检测与 API 调用，
 *          否则 renderdoc_capture.cpp 里的空实现让调用点一行都不用改。抓帧文件模板固定为
 *          resources/captures/，在取到 API 时设进 RenderDoc。
 */
#pragma once

#ifdef _WIN32
#include <Windows.h>
#endif  // _WIN32
#include <vulkan/vulkan.h>

#define RENDERDOC_CAPTURE renderdoc_capture::get_renderdoc_capture()

/**
 * @brief 前置声明：真正的定义来自 RenderDoc 的 renderdoc_app.h，未安装时保持不完全类型。
 * @details 本工程只把它当作不透明指针使用，所以头文件里不需要 include RenderDoc 的 SDK。
 */
struct RENDERDOC_API_1_7_0;

/**
 * @brief RenderDoc 抓帧单例：持有 1.7.0 版 API 指针与可选窗口句柄。
 * @details 构造时用 GetModuleHandleA("renderdoc.dll") 判断 RenderDoc 是否注入了本进程，取不到 API 就让
 *          m_api 保持空指针，begin_capture / end_capture 随之变成 no-op。抓帧只影响 RenderDoc，
 *          不改变应用的提交或帧序，所以调用点不需要条件编译。
 */
class renderdoc_capture
{
public:
    // Begin capturing API calls on the given Vulkan instance/window.
    /**
     * @brief 在给定 Vulkan instance 上开始一帧抓帧。
     * @param _vk_instance 目标 Vulkan instance，内部用 RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE 换成设备指针。
     * @param _hwnd 窗口句柄；传 nullptr 时用 set_window_handle 记下的那个。
     * @note m_api 为空（RenderDoc 未注入）时直接返回，不做任何事。
     * @note 有窗口句柄时先 SetActiveWindow 再 StartFrameCapture；没有就只调后者（RenderDoc 允许空窗口）。
     */
    void begin_capture(VkInstance _vk_instance, HWND _hwnd = nullptr);

    // End the current capture
    /**
     * @brief 结束当前抓帧，并在失败时丢弃这次捕获。
     * @param _vk_instance 目标 Vulkan instance，同样先换成设备指针。
     * @param _hwnd 窗口句柄；传 nullptr 时用 set_window_handle 记下的那个。
     * @note EndFrameCapture 的返回值不是 1 时调 DiscardFrameCapture，避免失败的捕获污染捕获列表。
     * @note m_api 为空时直接返回。
     */
    void end_capture(VkInstance _vk_instance, HWND _hwnd = nullptr);

    /// Set the window handle used for capture (optional — nullptr is a valid wildcard).
    /**
     * @brief 记下抓帧用的窗口句柄，供后续 begin_capture / end_capture 在未显式传参时使用。
     * @param _hwnd 窗口句柄；nullptr 表示不限定窗口（RenderDoc 接受通配）。
     * @note 只保存指针，不校验窗口是否有效，也不调用 RenderDoc。
     */
    void set_window_handle(HWND _hwnd) noexcept;

    /**
     * @brief 取全局唯一实例。
     * @return 静态实例的引用；构造即尝试连接 RenderDoc，失败时实例仍可用，begin_capture / end_capture 为空操作，窗口句柄仍可设置。
     */
    [[nodiscard]] static renderdoc_capture& get_renderdoc_capture() noexcept;

private:
    /**
     * @brief 尝试从已注入的 renderdoc.dll 取 1.7.0 版 API；任何一步失败都让 m_api 保持空。
     */
    renderdoc_capture();
    /**
     * @brief 默认析构：不调 RenderDoc 的关闭接口，抓帧状态由 RenderDoc 自己管理。
     */
    ~renderdoc_capture() = default;

    /**
     * @brief 禁止拷贝构造：单例与 API 指针都必须唯一。
     */
    renderdoc_capture(const renderdoc_capture&)            = delete;
    renderdoc_capture& operator=(const renderdoc_capture&) = delete;  ///< @brief 禁止拷贝赋值：单例与 API 指针都必须唯一
    /**
     * @brief 禁止移动构造：实例是静态对象，地址必须稳定。
     */
    renderdoc_capture(renderdoc_capture&&)                 = delete;
    renderdoc_capture& operator=(renderdoc_capture&&)      = delete;  ///< @brief 禁止移动赋值：实例是静态对象，地址必须稳定

    static renderdoc_capture capture;  ///< @brief 抓帧单例，静态存储期；构造时完成 DLL 探测与 API 获取

    RENDERDOC_API_1_7_0* m_api  = nullptr;  ///< @brief 1.7.0 版 RenderDoc API 表；未注入时为 nullptr，所有接口据此判空
    HWND                 m_hwnd = nullptr;  ///< @brief set_window_handle 记下的窗口句柄，作为 begin / end 未传参时的默认值
};
