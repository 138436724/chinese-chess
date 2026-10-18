/**
 * @file window.cpp
 * @brief glfw_window 的实现：GLFW 初始化、对象图建立、帧循环与输入回调。
 * @details 构造建立 GLFW 无 API 窗口，用 glfwGetRequiredInstanceExtensions 拿 WSI 扩展后依次创建
 *          application、scene_manager 与 ui_manager；render 在每帧开头合并 resize，再按固定顺序
 *          推进 UI、场景与合成，并在 Debug 下用 RenderDoc 捕获有变化的帧。
 *          四个 GLFW 回调均经 window user pointer 回到本对象；resize/key 捕获可传播异常并存入 callback_error。
 */
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

/**
 * @brief 建窗口、实例与设备，再建场景与 UI，并把两张离屏图交给 application 合成。
 * @note 固定 800x600、GLFW_NO_API；窗口 user pointer 在注册回调前指向本对象。
 */
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

/**
 * @brief 等 GPU 空闲、保存管线缓存，再显式按 ui → scene → app 释放并终止 GLFW。
 * @note 显式 reset 保证 ui、scene 都在 app 之前释放，最后才销毁窗口并终止 GLFW。
 */
glfw_window::~glfw_window()
{
    app->wait_idle();
    try { app->get_pipeline_cache().save(); }
    catch (const std::exception& error) { std::fprintf(stderr, "Pipeline cache save failed: %s\n", error.what()); }

    // delete before destroy window
    ui.reset();
    scene.reset();
    app.reset();

    glfwDestroyWindow(window);
    glfwTerminate();
}

/**
 * @brief 主循环：轮询事件、跳过最小化、合并 resize，再推进一帧并统计 fps。
 * @note acquire/present 的 fatal 转成 runtime_error 上抛；acquire 成功后的帧体异常会尝试 cancel_frame，再重抛原异常。
 * @note Debug 下在 scene->update 前判断是否需要 RenderDoc 捕获，帧末 end_capture。
 */
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
            // 自动捕获从第二个可渲染帧开始；这是本项目的抓帧策略。
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


/**
 * @brief 先查 callback_error 再进 try，把 resize 错误转存到 callback_error，绝不从 C 回调抛出。
 */
void glfw_window::glfw_resize_callback(GLFWwindow* _window, int _width, int _height) noexcept
{
    auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(_window));
    if (self->callback_error) return;
    try { self->resize_callback(_window, _width, _height); }
    catch (...) { self->callback_error = std::current_exception(); }
}

/** @brief 光标回调转发；成员函数为 noexcept，因此这里不捕获异常。 */
void glfw_window::glfw_cursor_position_callback(GLFWwindow* _window, double _xpos, double _ypos) noexcept
{
    static_cast<glfw_window*>(glfwGetWindowUserPointer(_window))->cursor_position_callback(_window, _xpos, _ypos);
}

/** @brief 鼠标按键回调转发；成员函数为 noexcept，因此这里不捕获异常。 */
void glfw_window::glfw_mouse_button_callback(GLFWwindow* _window, int _button, int _action, int _mods) noexcept
{
    static_cast<glfw_window*>(glfwGetWindowUserPointer(_window))->mouse_button_callback(_window, _button, _action, _mods);
}

/**
 * @brief 先查 callback_error 再进 try；键盘处理错误同样转存到 callback_error。
 */
void glfw_window::glfw_key_callback(GLFWwindow* _window, int _key, int _scancode, int _action, int _mods) noexcept
{
    auto* self = static_cast<glfw_window*>(glfwGetWindowUserPointer(_window));
    if (self->callback_error) return;
    try { self->key_callback(_window, _key, _scancode, _action, _mods); }
    catch (...) { self->callback_error = std::current_exception(); }
}

/** @brief 只置 resize_requested，真正的重建在 render 开头合并执行。 */
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

/**
 * @brief ImGui 未捕获键盘时，依次处理 UI 面板、场景热键与 C 键截图请求。
 * @note ImGui 捕获键盘时直接返回，阻止回放、场景热键和截图；ImGui backend 自身仍接收 GLFW 事件。
 */
void glfw_window::key_callback(GLFWwindow* /*_window*/, int _key, int _scancode, int _action, int _mods)
{
    // UI 面板的 handle 处理应用快捷键，必须和场景热键一起受键盘捕获状态约束。
    if (ui->wants_keyboard())
    {
        return;
    }

    ui->handle(_key, _scancode, _action, _mods);
    scene->handle(_key, _scancode, _action, _mods);

    if (_key == GLFW_KEY_C && _action == GLFW_PRESS)
    {
        need_capture = true;
    }
}
