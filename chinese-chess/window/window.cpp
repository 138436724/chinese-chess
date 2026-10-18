#include "window.h"

#include "scene/scene_manager.h"
#include "ui/ui_manager.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_common.h"

#ifndef NDEBUG
#include "tools/renderdoc_capture.h"
#endif  // !NDEBUG

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <memory>
#include <stdexcept>
#include <vector>

glfw_window::glfw_window()
{
    constexpr uint32_t WIDTH  = 800;
    constexpr uint32_t HEIGHT = 600;

    // init window
    if (glfwInit() != GLFW_TRUE)
    {
        throw std::runtime_error("failed to initialize GLFW!");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window = glfwCreateWindow(WIDTH, HEIGHT, "Chinese Chess", nullptr, nullptr);
    if (window == nullptr)
    {
        glfwTerminate();
        throw std::runtime_error("failed to create GLFW window!");
    }

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
    app->wait_idle_for_cleanup();
    try { app->get_pipeline_cache().save(); }
    catch (const std::exception& error) { std::fprintf(stderr, "Pipeline cache save failed: %s\n", error.what()); }

    // delete before destroy window
    ui.reset();
    scene.reset();
    app.reset();

    glfwDestroyWindow(window);
    glfwTerminate();
}

void glfw_window::render()
{
    // render loop
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        if (callback_error) std::rethrow_exception(callback_error);
        if (glfwWindowShouldClose(window)) break;

        int framebuffer_width, framebuffer_height;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) || framebuffer_width <= 0 || framebuffer_height <= 0)
        {
            glfwWaitEventsTimeout(0.05);
            continue;
        }
        if (resize_requested)
        {
            if (!app->resize(static_cast<uint32_t>(framebuffer_width), static_cast<uint32_t>(framebuffer_height))) continue;
            const auto extent = app->get_swapchain().get_extent();
            scene->resize(extent.width, extent.height);
            ui->resize(extent.width, extent.height);
            app->bind_image(scene->get_render_image(), ui->get_render_image());
            resize_requested = false;
        }

        const auto start = std::chrono::high_resolution_clock::now();

        const auto acquired = app->begin_frame();
        if (acquired.status == vulkan_frame_status::recreate) { resize_requested = true; continue; }
        if (acquired.status == vulkan_frame_status::skip) continue;
        if (acquired.status == vulkan_frame_status::fatal)
            throw std::runtime_error(std::format("Acquire failed: {}", vk::to_string(acquired.reason)));

        try
        {
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

            const auto ui_wait_info    = ui->render(acquired.frame.slot);
            const auto scene_wait_info = scene->render(acquired.frame.slot, need_capture);
            const auto presented = app->render(acquired.frame, ui_wait_info, scene_wait_info);
            if (presented.status == vulkan_frame_status::recreate) resize_requested = true;
            if (presented.status == vulkan_frame_status::fatal)
                throw std::runtime_error(std::format("Present failed: {}", vk::to_string(presented.reason)));
        }
        catch (...)
        {
            const auto failure = std::current_exception();
            try { app->cancel_frame(); }
            catch (const std::exception& error) { std::fprintf(stderr, "Frame cancellation failed: %s\n", error.what()); }
            std::rethrow_exception(failure);
        }


        need_capture = false;

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
    auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(_window));
    if (self->callback_error) return;
    try { self->resize_callback(_window, _width, _height); }
    catch (...) { self->callback_error = std::current_exception(); }
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
    auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(_window));
    if (self->callback_error) return;
    try { self->key_callback(_window, _key, _scancode, _action, _mods); }
    catch (...) { self->callback_error = std::current_exception(); }
}

void glfw_window::resize_callback(GLFWwindow* /*_window*/, int /*_width*/, int /*_height*/)
{
    resize_requested = true;
}

void glfw_window::cursor_position_callback(GLFWwindow* /*_window*/, double /*_xpos*/, double /*_ypos*/) const noexcept
{
}

void glfw_window::mouse_button_callback(GLFWwindow* /*_window*/, int /*_button*/, int /*_action*/, int /*_mods*/) const noexcept
{
}

void glfw_window::key_callback(GLFWwindow* /*_window*/, int _key, int _scancode, int _action, int _mods)
{
    ui->handle(_key, _scancode, _action, _mods);

    // ImGui 正在接收键盘（文本框 / 下拉）时不再往下传：否则打字会连带推进回放、触发截图
    if (ui->wants_keyboard())
    {
        return;
    }

    scene->handle(_key, _scancode, _action, _mods);

    if (_key == GLFW_KEY_C && _action == GLFW_PRESS)
    {
        need_capture = true;
    }
}
