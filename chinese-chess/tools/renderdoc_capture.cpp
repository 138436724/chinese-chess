/**
 * @file renderdoc_capture.cpp
 * @brief renderdoc_capture 的两套实现：HAS_RENDERDOC 下真正探测注入并调 RenderDoc API，否则全部空实现。
 * @details 构造函数用 GetModuleHandleA("renderdoc.dll") 判断 RenderDoc 是否注入了本进程，再取
 *          RENDERDOC_GetAPI 的 1.7.0 接口并设置抓帧文件模板；任何一步失败都保持 m_api 为空，
 *          begin_capture / end_capture 随之退化成 no-op。set_window_handle 与单例 getter 在两个
 *          分支之外，保证调用方不必关心编译开关。
 */

#include "renderdoc_capture.h"

#ifdef HAS_RENDERDOC

#include "renderdoc_app.h"

#include <cstdint>
#include <string_view>

namespace {
constexpr std::string_view RENDERDOC_DLL = "renderdoc.dll";
constexpr std::string_view CAPTURES_PATH = "resources\\captures\\";
}  // namespace

renderdoc_capture renderdoc_capture::capture;

/**
 * @brief 探测已注入的 renderdoc.dll，取 1.7.0 API 并设置抓帧文件模板。
 * @note 只认「RenderDoc 已注入」这一种情况：GetModuleHandleA 拿不到模块就直接放弃，不尝试 LoadLibrary。
 * @note 请求固定的 1.7.0 接口；不是运行时查询最新 API 版本。
 */
renderdoc_capture::renderdoc_capture()
{
    // RenderDoc injects renderdoc.dll into the process. Try to locate it.
    const HMODULE mod = GetModuleHandleA(RENDERDOC_DLL.data());
    if (!mod)
    {
        return;
    }

    const pRENDERDOC_GetAPI RENDERDOC_GetAPI = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
    if (!RENDERDOC_GetAPI)
    {
        return;
    }

    // 请求固定的 1.7.0 接口；支持该接口的 RenderDoc 版本即可使用。
    void*     api = nullptr;
    const int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_7_0, &api);
    if (ret != 1 || !api)
    {
        return;
    }
    m_api = static_cast<RENDERDOC_API_1_7_0*>(api);

    // Set a default capture file path so captures go to a known location.
    m_api->SetCaptureFilePathTemplate(CAPTURES_PATH.data());
}

/**
 * @brief 取得设备指针后开始抓帧，必要时先激活窗口。
 * @note 没有窗口句柄时跳过 SetActiveWindow（它要求两个参数都非空），只调 StartFrameCapture。
 */
void renderdoc_capture::begin_capture(VkInstance _vk_instance, HWND _hwnd)
{
    if (!m_api)
    {
        return;
    }

    void*      device_ptr = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(_vk_instance);
    const HWND wnd        = _hwnd ? _hwnd : m_hwnd;

    // SetActiveWindow requires both parameters to be non-NULL.
    // Skip it when no window handle is available.
    if (wnd)
    {
        m_api->SetActiveWindow(device_ptr, reinterpret_cast<RENDERDOC_WindowHandle>(wnd));
    }

    m_api->StartFrameCapture(device_ptr, reinterpret_cast<RENDERDOC_WindowHandle>(wnd));
}

/**
 * @brief 结束抓帧；返回值不是 1 就丢弃这次捕获。
 * @note 丢弃是为了让失败帧不进入 RenderDoc 的捕获列表。
 */
void renderdoc_capture::end_capture(VkInstance _vk_instance, HWND _hwnd)
{
    if (!m_api)
    {
        return;
    }

    void*      device_ptr = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(_vk_instance);
    const HWND wnd        = _hwnd ? _hwnd : m_hwnd;

    const uint32_t result = m_api->EndFrameCapture(device_ptr, reinterpret_cast<RENDERDOC_WindowHandle>(wnd));

    // Discard failed captures so they don't clutter the capture list.
    if (result != 1)
    {
        m_api->DiscardFrameCapture(device_ptr, reinterpret_cast<RENDERDOC_WindowHandle>(wnd));
    }
}

#else

renderdoc_capture renderdoc_capture::capture;

/** @brief 未找到 renderdoc_app.h 的构建：构造即空实现，m_api 恒为 nullptr。 */
renderdoc_capture::renderdoc_capture() = default;

/** @brief 空实现：该构建没有 RenderDoc，抓帧请求被静默忽略。 */
void renderdoc_capture::begin_capture(VkInstance /*_vk_instance*/, HWND /*_hwnd*/) {}

/** @brief 空实现：该构建没有 RenderDoc，结束抓帧同样是空操作。 */
void renderdoc_capture::end_capture(VkInstance /*_vk_instance*/, HWND /*_hwnd*/) {}

#endif  // HAS_RENDERDOC

void renderdoc_capture::set_window_handle(HWND _hwnd) noexcept
{
    m_hwnd = _hwnd;
}

renderdoc_capture& renderdoc_capture::get_renderdoc_capture() noexcept
{
    return capture;
}
