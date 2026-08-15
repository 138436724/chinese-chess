#include "window.h"

#include "scene/scene_manager.h"
#include "ui/ui_manager.h"
#include "vulkan_core/vulkan_application.h"

#ifndef NDEBUG
#include "tools/renderdoc_capture.h"
#endif  // !NDEBUG

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <chrono>
#include <format>
#include <stdexcept>
#include <vector>

glfw_window::glfw_window()
{
    constexpr uint32_t WIDTH  = 800;
    constexpr uint32_t HEIGHT = 600;

    // init window
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window = glfwCreateWindow(WIDTH, HEIGHT, "Chinese Chess", nullptr, nullptr);

#ifndef NDEBUG
    RENDERDOC_CAPTURE.set_window_handle(glfwGetWin32Window(window));
#endif  // !NDEBUG

    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, glfw_resize_callback);
    glfwSetCursorPosCallback(window, glfw_cursor_position_callback);
    glfwSetMouseButtonCallback(window, glfw_mouse_button_callback);
    glfwSetKeyCallback(window, glfw_key_callback);


    // init application
    uint32_t                       count           = 0;
    const auto                     glfw_extensions = glfwGetRequiredInstanceExtensions(&count);
    const std::vector<const char*> extensions(glfw_extensions, glfw_extensions + count);
    app = std::make_unique<vulkan_application>();
    app->init({}, extensions);

    VkSurfaceKHR _surface;
    if (glfwCreateWindowSurface(*(app->get_instance()), window, nullptr, &_surface))
    {
        throw std::runtime_error("failed to create window surface!");
    }

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    app->create(_surface, static_cast<uint32_t>(width), static_cast<uint32_t>(height));


    // create scene manager
    scene = std::make_unique<scene_manager>(*app, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    // create ui
    ui = std::make_unique<ui_manager>(window, *app, *scene, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    // bind
    app->bind_image(scene->get_render_image(), ui->get_render_image());
}

glfw_window::~glfw_window()
{
    app->wait();

    ui->destroy();
    scene->destroy();

    glfwDestroyWindow(window);
    glfwTerminate();
}

void glfw_window::render()
{
    // render loop
    while (!glfwWindowShouldClose(window))
    {
        while (glfwGetWindowAttrib(window, GLFW_ICONIFIED))
        {
            glfwWaitEvents();
        }

        glfwPollEvents();

        const auto start = std::chrono::high_resolution_clock::now();

        ui->update();

#ifndef NDEBUG
        // Renderdoc cannot start with capture first frame, so skip the first frame
        if (!first_frame && scene->get_need_update())
        {
            begin_capture = true;
            RENDERDOC_CAPTURE.begin_capture(*(app->get_instance()));
        }
#endif  // !NDEBUG

        scene->update();

        const auto ui_wait_info    = ui->render();
        const auto scene_wait_info = scene->render();
        app->render(ui_wait_info, scene_wait_info);

        if (need_save)
        {
            scene->save_image();
            need_save = false;
        }

#ifndef NDEBUG
        if (begin_capture)
        {
            RENDERDOC_CAPTURE.end_capture(*(app->get_instance()));
            begin_capture = false;
        }

        first_frame = false;
#endif  // !NDEBUG

        const auto end      = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration<float>(end - start).count();

        constexpr float update_interval = 0.5f;
        title_timer += duration;
        ++title_frames;

        if (title_timer >= update_interval)
        {
            const auto result = std::format_to_n(window_title.data(), window_title.size() - 1, "Chinese Chess {} fps",
                                                 static_cast<float>(title_frames) / title_timer);
            *result.out       = '\0';
            glfwSetWindowTitle(window, window_title.data());

            title_timer  = 0.f;
            title_frames = 0;
        }
    }
}

void glfw_window::glfw_resize_callback(GLFWwindow* _window, int _width, int _height) noexcept
{
    static_cast<glfw_window*>(glfwGetWindowUserPointer(_window))->resize_callback(_window, _width, _height);
}

void glfw_window::glfw_cursor_position_callback(GLFWwindow* _window, double _xpos, double _ypos) noexcept
{
    static_cast<glfw_window*>(glfwGetWindowUserPointer(_window))->cursor_position_callback(_window, _xpos, _ypos);
}

void glfw_window::glfw_mouse_button_callback(GLFWwindow* _window, int _button, int _action, int _mods) noexcept
{
    static_cast<glfw_window*>(glfwGetWindowUserPointer(_window))->mouse_button_callback(_window, _button, _action, _mods);
}

void glfw_window::glfw_key_callback(GLFWwindow* _window, int _key, int _scancode, int _action, int _mods) noexcept
{
    static_cast<glfw_window*>(glfwGetWindowUserPointer(_window))->key_callback(_window, _key, _scancode, _action, _mods);
}

void glfw_window::resize_callback(GLFWwindow* /*_window*/, int _width, int _height)
{
    if (_width > 0 && _height > 0)
    {
        app->wait();

        app->resize(static_cast<uint32_t>(_width), static_cast<uint32_t>(_height));
        scene->resize(static_cast<uint32_t>(_width), static_cast<uint32_t>(_height));
        ui->resize(static_cast<uint32_t>(_width), static_cast<uint32_t>(_height));

        app->bind_image(scene->get_render_image(), ui->get_render_image());
    }
}

void glfw_window::cursor_position_callback(GLFWwindow* /*_window*/, double /*_xpos*/, double /*_ypos*/) const noexcept
{
}

void glfw_window::mouse_button_callback(GLFWwindow* /*_window*/, int /*_button*/, int /*_action*/, int /*_mods*/) const noexcept
{
}

void glfw_window::key_callback(GLFWwindow* /*_window*/, int _key, int, int _action, int)
{
    if (_action == GLFW_PRESS || _action == GLFW_REPEAT)
    {
        ui->handle(_key);
        scene->handle(_key);

        // WASD处理
        switch (_key)
        {
            case GLFW_KEY_C:
                need_save = true;
                break;
            default:
                break;
        }
    }
}
