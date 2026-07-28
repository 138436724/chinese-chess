#include "renderdoc_capture.h"

#include "renderdoc_app.h"

#include <string>

constexpr std::string_view RENDERDOC_DLL = "renderdoc.dll";
constexpr std::string_view CAPTURES_PATH = "resources\\captures\\";

renderdoc_capture renderdoc_capture::capture;

renderdoc_capture::renderdoc_capture()
{
    // RenderDoc injects renderdoc.dll into the process. Try to locate it.
    HMODULE mod = GetModuleHandleA(RENDERDOC_DLL.data());
    if (!mod)
    {
        return;
    }

    pRENDERDOC_GetAPI RENDERDOC_GetAPI = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
    if (!RENDERDOC_GetAPI)
    {
        return;
    }

    // Request the latest API version. RenderDoc may return a newer version if backward-compatible.
    int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_7_0, reinterpret_cast<void**>(&m_api));
    if (ret != 1 || !m_api)
    {
        m_api = nullptr;
        return;
    }

    // Set a default capture file path so captures go to a known location.
    m_api->SetCaptureFilePathTemplate(CAPTURES_PATH.data());
}

bool renderdoc_capture::is_available() const noexcept
{
    return m_api != nullptr;
}

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

void renderdoc_capture::end_capture(VkInstance _vk_instance, HWND _hwnd)
{
    if (!m_api)
    {
        return;
    }

    void*      device_ptr = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(_vk_instance);
    const HWND wnd        = _hwnd ? _hwnd : m_hwnd;

    uint32_t result = m_api->EndFrameCapture(device_ptr, reinterpret_cast<RENDERDOC_WindowHandle>(wnd));

    // Discard failed captures so they don't clutter the capture list.
    if (result != 1)
    {
        m_api->DiscardFrameCapture(device_ptr, reinterpret_cast<RENDERDOC_WindowHandle>(wnd));
    }
}

void renderdoc_capture::set_window_handle(HWND _hwnd) noexcept
{
    m_hwnd = _hwnd;
}

renderdoc_capture& renderdoc_capture::get_renderdoc_capture() noexcept
{
    return capture;
}
