#pragma once

#include <Windows.h>
#include <vulkan/vulkan.h>

#define RENDERDOC_CAPTURE renderdoc_capture::get_renderdoc_capture()

struct RENDERDOC_API_1_7_0;

class renderdoc_capture
{
public:
    // Begin capturing API calls on the given Vulkan instance/window.
    void begin_capture(VkInstance _vk_instance, HWND _hwnd = nullptr);

    // End the current capture
    void end_capture(VkInstance _vk_instance, HWND _hwnd = nullptr);

    /// Set the window handle used for capture (optional — nullptr is a valid wildcard).
    void set_window_handle(HWND _hwnd) noexcept;

    [[nodiscard]] static renderdoc_capture& get_renderdoc_capture() noexcept;

private:
    renderdoc_capture();
    ~renderdoc_capture() = default;

    renderdoc_capture(const renderdoc_capture&)            = delete;
    renderdoc_capture& operator=(const renderdoc_capture&) = delete;
    renderdoc_capture(renderdoc_capture&&)                 = delete;
    renderdoc_capture& operator=(renderdoc_capture&&)      = delete;

    static renderdoc_capture capture;

    RENDERDOC_API_1_7_0* m_api  = nullptr;
    HWND                 m_hwnd = nullptr;
};
