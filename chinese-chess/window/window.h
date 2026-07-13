#pragma once
#include "scene/scene_manager.h"
#include "ui/ui_manager.h"
#include "vulkan_core/vulkan_application.h"

#include <GLFW/glfw3.h>

class glfw_window
{
public:
    glfw_window();
    ~glfw_window();

    void render() noexcept;

    static void glfw_resize_callback(GLFWwindow* _window, int _width, int _height) noexcept;
    static void glfw_cursor_position_callback(GLFWwindow* _window, double _xpos, double _ypos) noexcept;
    static void glfw_mouse_button_callback(GLFWwindow* _window, int _button, int _action, int _mods) noexcept;
    static void glfw_key_callback(GLFWwindow* _window, int _key, int _scancode, int _action, int _mods) noexcept;

private:
    void resize_callback(GLFWwindow* _window, int _width, int _height) noexcept;
    void cursor_position_callback(GLFWwindow* _window, double _xpos, double _ypos) noexcept;
    void mouse_button_callback(GLFWwindow* _window, int _button, int _action, int _mods) noexcept;
    void key_callback(GLFWwindow* _window, int _key, int /*_scancode*/, int _action, int /*_mods*/) noexcept;

private:
    bool need_save = false;

    float                 title_timer  = 0.f;
    uint32_t              title_frames = 0;
    std::array<char, 128> window_title = {};

    GLFWwindow*                         window = nullptr;
    std::unique_ptr<vulkan_application> app    = nullptr;
    std::unique_ptr<ui_manager>         ui     = nullptr;
    std::unique_ptr<scene_manager>      scene  = nullptr;
};
