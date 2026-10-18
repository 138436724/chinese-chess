#pragma once

#include "scene/scene_manager.h"
#include "ui_base.h"
#include "vulkan_core/vulkan_image.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"
#include "vulkan_core/vulkan_submission_token.h"

#include <GLFW/glfw3.h>
#include <cstdint>
#include <vector>
#include <memory>

class vulkan_application;
class vulkan_commandbuffer;
struct ImDrawData;
class ui_viewport_renderer;

class ui_manager
{
public:
    ui_manager(GLFWwindow* _window, vulkan_application& _app, scene_manager& _manager, uint32_t _width, uint32_t _height);
    ~ui_manager();

    void                                  resize(uint32_t _width, uint32_t _height);
    void                                  update();
    [[nodiscard]] vulkan_submission_token render(uint32_t frame_slot);
    void                                  handle(int _key, int _scancode, int _action, int _mods) noexcept;

    // ImGui 是否正在接收键盘（文本框、下拉、快捷键捕获）——
    // 窗口层用它决定"要不要把事件继续喂给回放 / 截图 / 抓帧热键"
    [[nodiscard]] bool wants_keyboard() const noexcept;

    [[nodiscard]] vulkan_image& get_render_image() noexcept;

private:
    void render_mode_ui();

private:
    vk::Format color_format = vk::Format::eUndefined;

    vulkan_application&      app;
    vulkan_queue&            graphic_queue;
    vulkan_recycle_bin       recycle_bin;
    vk::raii::DescriptorPool descriptor_pool = nullptr;

    ImDrawData* draw_data = nullptr;

    vulkan_image render_output;
    vulkan_image color_image;
    std::unique_ptr<ui_viewport_renderer> viewport_renderer;

    scene_manager&                   manager;
    std::vector<pro::proxy<ui_base>> ui_managers;
    int                              current_mode = static_cast<int>(render_mode::ray_tracing);

    std::vector<vulkan_commandbuffer> commandbuffers;
};
